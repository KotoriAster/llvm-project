//===- Finalizer.cpp ------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ConcatOutputSection.h"
#include "Config.h"
#include "OutputSegment.h"
#include "Sections.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "SyntheticSections.h"
#include "Target.h"
#include "lld/Common/CommonLinkerContext.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/TimeProfiler.h"

#define DEBUG_TYPE "lld-macho-branch-islands"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

using namespace llvm;
using namespace llvm::MachO;
using namespace lld;
using namespace lld::macho;

namespace {
constexpr uint64_t maxVA = std::numeric_limits<uint64_t>::max();
constexpr uint32_t noExtender = std::numeric_limits<uint32_t>::max();

struct ExtensionPolicy {
  StringLiteral name;
  uint32_t maxHops;
};

constexpr ExtensionPolicy extensionPolicy(BranchRangeExtensionMode mode) {
  switch (mode) {
  case BranchRangeExtensionMode::hybrid:
    return {"hybrid", 2};
  case BranchRangeExtensionMode::islandsSlopFree:
    return {"islands-slop-free", noExtender};
  default:
    llvm_unreachable("invalid exact-layout branch-extension mode");
  }
}

struct Callsite {
  Callsite(Relocation *reloc, uint32_t inputIdx)
      : reloc(reloc), inputIdx(inputIdx) {}

  Relocation *reloc;
  uint32_t inputIdx;
  bool forceExtender = false;
};

struct Callee {
  Symbol *target = nullptr;
  int64_t addend = 0;
  uint32_t targetInputIdx = noExtender;
  SmallVector<Callsite, 1> callsites;
};

struct OwnerInputSlice {
  TextOutputSection *section;
  uint32_t firstInput;
  uint32_t lastInput;
};

struct FinalizerContext {
  FinalizerContext(TextOutputSection &first, BranchRangeExtensionMode mode);

  const ExtensionPolicy policy;
  SmallVector<ConcatInputSection *, 0> inputs;
  SmallVector<OwnerInputSlice, 4> ownerRuns;
  SmallVector<Callee, 0> callees;
  std::vector<uint32_t> reservedExtra;
  size_t callsiteCount = 0;
};

using CalleeIdentity = PointerUnion<const Symbol *, const InputSection *>;
using CalleeKey = std::tuple<CalleeIdentity, uint64_t, int64_t>;

static std::optional<uint64_t> tryAddVA(uint64_t va, int64_t addend) {
  if (addend >= 0) {
    uint64_t value = addend;
    return va > maxVA - value ? std::nullopt
                              : std::optional<uint64_t>(va + value);
  }
  uint64_t value = uint64_t(-(addend + 1)) + 1;
  return va < value ? std::nullopt : std::optional<uint64_t>(va - value);
}

static CalleeKey makeCalleeKey(Symbol *sym, int64_t addend) {
  if (const auto *defined = dyn_cast<Defined>(sym);
      defined && !needsBinding(sym))
    if (auto value = tryAddVA(defined->value, addend))
      return {defined->isec(), *value, 0};
  return {sym, 0, addend};
}

FinalizerContext::FinalizerContext(TextOutputSection &first,
                                   BranchRangeExtensionMode mode)
    : policy(extensionPolicy(mode)) {
  const auto &sections = first.parent->getSections();
  auto firstIt = llvm::find(sections, &first);
  assert(firstIt != sections.end());
  for (OutputSection *osec : llvm::make_range(firstIt, sections.end())) {
    auto *text = dyn_cast<TextOutputSection>(osec);
    if (!text ||
        !sections::isCodeSection(text->name, first.parent->name, text->flags))
      break;
    uint32_t begin = inputs.size();
    inputs.append(text->inputs.begin(), text->inputs.end());
    ownerRuns.push_back({text, begin, static_cast<uint32_t>(inputs.size())});
  }
  assert(!ownerRuns.empty() && ownerRuns.front().section == &first);
  if (inputs.size() >= noExtender)
    fatal("too many input sections for branch extension");
  reservedExtra.assign(inputs.size(), 0);

  DenseMap<ConcatInputSection *, uint32_t> inputIndex;
  inputIndex.reserve(inputs.size());
  for (uint32_t i = 0; i < inputs.size(); ++i)
    inputIndex.try_emplace(inputs[i], i);

  DenseMap<CalleeKey, uint32_t> calleeByKey;
  auto collect = [&](Relocation &reloc, uint32_t inputIdx) {
    if (!target->hasAttr(reloc.type, RelocAttrBits::BRANCH))
      return;
    Symbol *targetSym = llvm::cast<Symbol *>(reloc.referent);
    bool targetNeedsBinding = needsBinding(targetSym);
    if (policy.maxHops == noExtender && targetNeedsBinding)
      in.stubs->addEntry(targetSym);
    auto [it, inserted] = calleeByKey.try_emplace(
        makeCalleeKey(targetSym, reloc.addend), callees.size());
    uint32_t calleeIdx = it->second;
    if (inserted) {
      if (callees.size() >= noExtender)
        fatal("too many branch-extension targets");
      Callee &callee = callees.emplace_back();
      callee.target = targetSym;
      callee.addend = reloc.addend;
      if (!targetNeedsBinding)
        if (auto *defined = dyn_cast<Defined>(targetSym))
          if (auto *targetIsec =
                  dyn_cast_or_null<ConcatInputSection>(defined->isec()))
            if (auto targetIt = inputIndex.find(targetIsec);
                targetIt != inputIndex.end())
              callee.targetInputIdx = targetIt->second;
    }
    callees[calleeIdx].callsites.emplace_back(&reloc, inputIdx);
    ++callsiteCount;
  };

  for (uint32_t inputIdx = 0; inputIdx < inputs.size(); ++inputIdx) {
    SmallVector<Relocation *, 0> callsites;
    for (Relocation &reloc : inputs[inputIdx]->relocs)
      if (target->hasAttr(reloc.type, RelocAttrBits::BRANCH))
        callsites.push_back(&reloc);
    auto descending = [](const Relocation *lhs, const Relocation *rhs) {
      return lhs->offset > rhs->offset;
    };
    if (llvm::is_sorted(callsites, descending))
      std::reverse(callsites.begin(), callsites.end());
    else
      llvm::sort(callsites,
                 [=](const Relocation *lhs, const Relocation *rhs) {
                   return descending(rhs, lhs);
                 });
    for (Relocation *reloc : callsites)
      collect(*reloc, inputIdx);
  }
}

static uint64_t addVA(uint64_t va, uint64_t offset) {
  if (va > maxVA - offset)
    fatal("virtual address overflow: " + Twine(va) + " + " + Twine(offset));
  return va + offset;
}

static uint64_t addVA(uint64_t va, int64_t addend) {
  if (addend >= 0)
    return addVA(va, uint64_t(addend));
  uint64_t magnitude = uint64_t(-(addend + 1)) + 1;
  if (va < magnitude)
    fatal("virtual address underflow: " + Twine(va) + " + " + Twine(addend));
  return va - magnitude;
}

// Saturating branch-window arithmetic: clamp to [0, UINT64_MAX] instead of
// wrapping when a range offset would overflow.
static uint64_t subSat(uint64_t va, uint64_t range) {
  return va > range ? va - range : 0;
}
static uint64_t addSat(uint64_t va, uint64_t range) {
  return va > maxVA - range ? maxVA : va + range;
}
// Prefer real input boundaries at roughly 1 MiB intervals to reduce search
// cost and cluster extenders for better instruction-cache locality.
constexpr uint64_t coarseBoundarySpacing = 1024 * 1024;
constexpr size_t maxPasses = 30;

enum class ExtenderKind : uint8_t { Island, Thunk };

struct ExtensionNode {
  ExtenderKind kind = ExtenderKind::Island;
  uint32_t calleeIdx = 0;
  // noExtender denotes a terminal island. Thunks also use noExtender because
  // their outgoing relocations are not range-limited BRANCH26 edges.
  uint32_t relayTo = noExtender;
  uint32_t boundaryIdx = 0;
};

struct ExtensionLayout;

// Disposable graph. routing uses callee-major callsite order.
struct ExtensionProposal {
  ExtensionProposal() = default;
  ExtensionProposal(const FinalizerContext &context,
                    ExtensionLayout &planningLayout);
  void plan(FinalizerContext &context, ExtensionLayout &planningLayout);

  SmallVector<ExtensionNode, 0> extenders;
  std::vector<uint32_t> routing;
  std::vector<uint32_t> desiredExtra;
  SmallVector<Callsite *, 0> invalidDirectCallsites;
  SmallVector<uint32_t, 0> byBoundary;
  // A proposal-local, globally shared subset of real boundaries.
  SmallVector<uint32_t, 0> coarseBoundaries;
};

// Addresses derived from either the reservation envelope or one exact
// proposal. Unresolved callee targets are represented by std::nullopt.
struct ExtensionLayout {
  enum class BoundKind : uint8_t { Lower, Upper };

  const FinalizerContext *context = nullptr;
  std::vector<uint64_t> inputVA;
  std::vector<uint64_t> extenderVA;
  std::vector<std::optional<uint64_t>> calleeTargetVA;
  uint64_t textEndVA = 0;

  uint64_t boundaryBaseVA(uint32_t boundary) const {
    return alignToPowerOf2(
        inputVA[boundary] + context->inputs[boundary]->getSize(), 4);
  }
  uint64_t proposedExtenderVA(ArrayRef<uint32_t> desiredExtra,
                              uint32_t boundary) const {
    return addSat(boundaryBaseVA(boundary), desiredExtra[boundary]);
  }
  // Return the index of the first boundary whose base VA is at least `va`, or
  // boundaryCount() if no such boundary exists.
  uint32_t lowerBound(uint64_t va) const {
    return boundaryBound(va, BoundKind::Lower);
  }
  // Return the index of the first boundary whose base VA is greater than `va`,
  // or boundaryCount() if no such boundary exists.
  uint32_t upperBound(uint64_t va) const {
    return boundaryBound(va, BoundKind::Upper);
  }
  uint32_t boundaryBound(uint64_t va, BoundKind kind) const;
  size_t boundaryCount() const { return context->inputs.size(); }
  std::optional<uint64_t> estimateSectionVA(OutputSection *targetSection) const;
  std::optional<uint64_t> nonLocalTargetVA(Symbol *sym, int64_t addend) const;
  std::optional<uint64_t> targetVA(const Callee &callee) const;
  uint64_t callsiteVA(const Callsite &callsite) const {
    return inputVA[callsite.inputIdx] + callsite.reloc->offset;
  }
};

// A callee asks this question for every target-directed island hop. The
// response is always one real input boundary or no valid answer.
struct IslandBoundaryRequest {
  uint64_t callVA;
  uint64_t anchorVA;

  bool isForward() const { return callVA < anchorVA; }
};

struct MaterializedExtender {
  ConcatInputSection *isec = nullptr;
  Defined *sym = nullptr;
};

static bool inBranchRange(uint64_t from, uint64_t to) {
  return (target->backwardBranchRange < from
              ? from - target->backwardBranchRange
              : 0) <= to &&
         to <= addSat(from, target->forwardBranchRange);
}

static size_t extenderSize(const ExtensionNode &extender) {
  return extender.kind == ExtenderKind::Thunk ? target->thunkSize
                                              : target->islandSize;
}

std::optional<uint64_t>
ExtensionLayout::estimateSectionVA(OutputSection *targetSection) const {
  if (!targetSection || !targetSection->isNeeded())
    return std::nullopt;

  uint64_t va = textEndVA;
  bool pastText = false;
  OutputSection *lastText = context->ownerRuns.back().section;
  for (OutputSection *osec :
       context->ownerRuns.front().section->parent->getSections()) {
    if (!pastText) {
      pastText = osec == lastText;
      continue;
    }
    if (!osec->isNeeded())
      continue;
    va = alignToPowerOf2(va, osec->align);
    if (osec == targetSection)
      return va;
    if (auto *concat = dyn_cast<ConcatOutputSection>(osec)) {
      uint64_t size = 0;
      for (ConcatInputSection *isec : concat->inputs) {
        size = alignToPowerOf2(size, isec->align);
        size += isec->getSize();
      }
      va += size;
    } else {
      va += osec->getSize();
    }
  }
  return std::nullopt;
}

std::optional<uint64_t>
ExtensionLayout::nonLocalTargetVA(Symbol *sym, int64_t addend) const {
  if (sym->isInStubs()) {
    if (in.stubs->isFinal)
      return addVA(sym->getStubVA(), addend);
    if (auto stubsVA = estimateSectionVA(in.stubs))
      return addVA(
          addVA(*stubsVA, uint64_t(sym->stubsIndex) * target->stubSize),
          addend);
  }
  auto *defined = dyn_cast<Defined>(sym);
  if (in.objcStubs && in.objcStubs->isNeeded() && defined &&
      defined->isec() == in.objcStubs->isec)
    if (auto objcStubsVA = estimateSectionVA(in.objcStubs))
      return addVA(addVA(*objcStubsVA, defined->value), addend);
  if (defined && defined->isAbsolute())
    return addVA(defined->getVA(), addend);
  return std::nullopt;
}

std::optional<uint64_t> ExtensionLayout::targetVA(const Callee &callee) const {
  if (auto va = nonLocalTargetVA(callee.target, callee.addend))
    return va;
  if (callee.targetInputIdx == noExtender)
    return std::nullopt;
  auto *defined = cast<Defined>(callee.target);
  uint64_t targetInputVA = inputVA[callee.targetInputIdx];
  return addVA(addVA(targetInputVA, defined->value), callee.addend);
}

static void resolveCalleeTargets(const FinalizerContext &context,
                                 ExtensionLayout &layout) {
  auto targetVAs = llvm::map_range(context.callees, [&](const Callee &callee) {
    return layout.targetVA(callee);
  });
  layout.calleeTargetVA.assign(targetVAs.begin(), targetVAs.end());
}

// Apply the common owner/input layout rules. The callback inserts the
// phase-specific reservation or exact extenders after each input.
template <typename PlaceAfterInput>
static ExtensionLayout layoutExtensionInputs(const FinalizerContext &context,
                                             size_t extenderCount,
                                             PlaceAfterInput placeAfterInput) {
  ExtensionLayout layout;
  layout.context = &context;
  layout.inputVA.resize(context.inputs.size());
  layout.extenderVA.resize(extenderCount);
  uint64_t addr = context.ownerRuns.front().section->addr;
  uint64_t groupSize = 0;
  for (const OwnerInputSlice &run : context.ownerRuns) {
    groupSize = alignToPowerOf2(groupSize, run.section->align);
    uint64_t ownerBase = groupSize;
    uint64_t ownerSize = 0;
    for (uint32_t inputIdx = run.firstInput; inputIdx < run.lastInput;
         ++inputIdx) {
      ConcatInputSection *isec = context.inputs[inputIdx];
      ownerSize = alignToPowerOf2(ownerSize, isec->align);
      layout.inputVA[inputIdx] = addr + ownerBase + ownerSize;
      ownerSize += isec->getSize();
      placeAfterInput(layout, inputIdx, addr + ownerBase, ownerSize);
      groupSize = ownerBase + ownerSize;
    }
  }
  layout.textEndVA = addr + groupSize;
  resolveCalleeTargets(context, layout);
  return layout;
}

static ExtensionLayout
layoutReservationEnvelope(const FinalizerContext &context) {
  return layoutExtensionInputs(
      context, 0,
      [&](ExtensionLayout &, uint32_t inputIdx, uint64_t, uint64_t &ownerSize) {
        if (context.reservedExtra[inputIdx] == 0)
          return;
        ownerSize = alignToPowerOf2(ownerSize, 4);
        ownerSize += context.reservedExtra[inputIdx];
      });
}

static void orderExtendersByBoundary(const FinalizerContext &context,
                                     ExtensionProposal &proposal) {
  proposal.byBoundary.resize(proposal.extenders.size());
  std::iota(proposal.byBoundary.begin(), proposal.byBoundary.end(), 0);
  llvm::stable_sort(proposal.byBoundary, [&](uint32_t lhs, uint32_t rhs) {
    assert(proposal.extenders[lhs].boundaryIdx < context.inputs.size());
    return proposal.extenders[lhs].boundaryIdx <
           proposal.extenders[rhs].boundaryIdx;
  });
}

static ExtensionLayout layoutExtensionProposal(const FinalizerContext &context,
                                               ExtensionProposal &proposal) {
  orderExtendersByBoundary(context, proposal);
  proposal.desiredExtra.assign(context.inputs.size(), 0);
  for (const ExtensionNode &extender : proposal.extenders) {
    size_t size = extenderSize(extender);
    if (proposal.desiredExtra[extender.boundaryIdx] >
        std::numeric_limits<uint32_t>::max() - size)
      fatal("too many branch extenders at one input boundary");
    proposal.desiredExtra[extender.boundaryIdx] += size;
  }

  size_t orderIdx = 0;
  ExtensionLayout layout = layoutExtensionInputs(
      context, proposal.extenders.size(),
      [&](ExtensionLayout &layout, uint32_t inputIdx, uint64_t ownerVA,
          uint64_t &ownerSize) {
        while (orderIdx < proposal.byBoundary.size()) {
          uint32_t extenderIdx = proposal.byBoundary[orderIdx];
          const ExtensionNode &extender = proposal.extenders[extenderIdx];
          if (extender.boundaryIdx != inputIdx)
            break;
          ownerSize = alignToPowerOf2(ownerSize, 4);
          layout.extenderVA[extenderIdx] = ownerVA + ownerSize;
          ownerSize += extenderSize(extender);
          ++orderIdx;
        }
      });
  assert(orderIdx == proposal.byBoundary.size());
  return layout;
}

uint32_t ExtensionLayout::boundaryBound(uint64_t va, BoundKind kind) const {
  uint32_t first = 0;
  uint32_t count = context->inputs.size();
  while (count != 0) {
    uint32_t step = count / 2;
    uint32_t mid = first + step;
    uint64_t base = boundaryBaseVA(mid);
    if (base < va || (kind == BoundKind::Upper && base == va)) {
      first = mid + 1;
      count -= step + 1;
    } else {
      count = step;
    }
  }
  return first;
}

// Select one real boundary at each coarse VA seam.  The representative is the
// first boundary at or after that seam, so catalog identity is an input index
// and remains meaningful as extenders move virtual addresses in later passes.
static SmallVector<uint32_t, 0>
buildCoarseBoundaryCatalog(const ExtensionLayout &layout) {
  SmallVector<uint32_t, 0> catalog;
  if (layout.boundaryCount() == 0)
    return catalog;

  uint64_t first = layout.boundaryBaseVA(0);
  uint64_t seam = alignToPowerOf2(first, coarseBoundarySpacing);
  uint64_t last = layout.boundaryBaseVA(layout.boundaryCount() - 1);
  while (seam <= last) {
    uint32_t boundary = layout.lowerBound(seam);
    if (boundary == layout.boundaryCount())
      break;
    if (catalog.empty() || catalog.back() != boundary)
      catalog.push_back(boundary);
    if (seam > maxVA - coarseBoundarySpacing)
      break;
    seam += coarseBoundarySpacing;
  }
  return catalog;
}

static std::optional<uint32_t>
answerIslandBoundaryRequest(const ExtensionLayout &layout,
                            ExtensionProposal &proposal,
                            const IslandBoundaryRequest &request) {
  auto legal = [&](uint32_t boundary) {
    uint64_t va = layout.proposedExtenderVA(proposal.desiredExtra, boundary);
    return request.isForward() ? request.callVA < va && va < request.anchorVA &&
                                     inBranchRange(va, request.anchorVA)
                               : request.anchorVA < va && va < request.callVA &&
                                     inBranchRange(va, request.anchorVA);
  };
  auto findReal = [&](uint32_t begin, uint32_t end) -> std::optional<uint32_t> {
    if (request.isForward()) {
      for (uint32_t boundary = begin; boundary < end; ++boundary)
        if (legal(boundary))
          return boundary;
    } else {
      while (end != begin)
        if (legal(--end))
          return end;
    }
    return std::nullopt;
  };

  if (request.isForward()) {
    for (uint32_t boundary : proposal.coarseBoundaries)
      if (legal(boundary))
        return boundary;
  } else {
    for (uint32_t cursor = proposal.coarseBoundaries.size(); cursor != 0;) {
      uint32_t boundary = proposal.coarseBoundaries[--cursor];
      if (legal(boundary))
        return boundary;
    }
  }

  uint64_t edge =
      request.isForward()
          ? std::max(request.callVA + (request.callVA != maxVA),
                     subSat(request.anchorVA, target->forwardBranchRange))
          : std::min(request.callVA - 1,
                     addSat(request.anchorVA, target->backwardBranchRange));
  uint32_t nearby = layout.lowerBound(edge);
  auto it = llvm::lower_bound(proposal.coarseBoundaries, nearby);
  auto findNeighborhood = [&](size_t pos) {
    uint32_t preferred = proposal.coarseBoundaries[pos];
    uint32_t begin = pos == 0 ? 0 : proposal.coarseBoundaries[pos - 1] + 1;
    uint32_t end = pos + 1 == proposal.coarseBoundaries.size()
                       ? layout.boundaryCount()
                       : proposal.coarseBoundaries[pos + 1];
    return findReal(std::min(begin, preferred), std::max(end, preferred + 1));
  };
  size_t pos = it - proposal.coarseBoundaries.begin();
  if (it != proposal.coarseBoundaries.end())
    if (auto boundary = findNeighborhood(pos))
      return boundary;
  if (it != proposal.coarseBoundaries.begin())
    if (auto boundary = findNeighborhood(pos - 1))
      return boundary;

  uint32_t begin;
  uint32_t end;
  if (request.isForward()) {
    uint64_t low =
        std::max(request.callVA + (request.callVA != maxVA),
                 subSat(request.anchorVA, target->forwardBranchRange));
    begin = layout.lowerBound(low);
    end = layout.lowerBound(request.anchorVA);
  } else {
    if (request.callVA == request.anchorVA)
      return std::nullopt;
    uint64_t high =
        std::min(request.callVA - 1,
                 addSat(request.anchorVA, target->backwardBranchRange));
    begin = layout.upperBound(request.anchorVA);
    end = layout.upperBound(high);
  }
  if (auto boundary = findReal(begin, end))
    return boundary;
  return std::nullopt;
}

static uint32_t createExtender(FinalizerContext &context,
                               ExtensionProposal &proposal,
                               ExtensionLayout &planningLayout,
                               uint32_t calleeIdx, uint32_t boundary,
                               ExtenderKind role, uint32_t relayTo) {
  if (proposal.extenders.size() >= noExtender)
    fatal("too many branch extenders");
  assert(boundary < context.inputs.size());
  assert(role == ExtenderKind::Thunk || relayTo == noExtender ||
         (relayTo < proposal.extenders.size() &&
          proposal.extenders[relayTo].kind == ExtenderKind::Island));

  uint64_t va =
      planningLayout.proposedExtenderVA(proposal.desiredExtra, boundary);
  uint32_t extenderIdx = proposal.extenders.size();
  proposal.extenders.push_back({role, calleeIdx, relayTo, boundary});
  planningLayout.extenderVA.push_back(va);
  size_t size = extenderSize(proposal.extenders.back());
  if (proposal.desiredExtra[boundary] >
      std::numeric_limits<uint32_t>::max() - size)
    fatal("too many branch extenders at one input boundary");
  proposal.desiredExtra[boundary] += size;
  return extenderIdx;
}

static std::optional<uint32_t>
buildIslandChain(FinalizerContext &context, ExtensionProposal &proposal,
                 ExtensionLayout &planningLayout, uint32_t calleeIdx,
                 uint64_t callVA, uint64_t targetVA, bool forceExtender) {
  uint64_t anchorVA = targetVA;
  bool requireFirstIsland = forceExtender;
  if (!requireFirstIsland && inBranchRange(callVA, anchorVA))
    return noExtender;

  SmallVector<uint32_t, 4> boundaries;
  uint32_t depth = 0;
  while (depth != context.policy.maxHops &&
         (requireFirstIsland || !inBranchRange(callVA, anchorVA))) {
    IslandBoundaryRequest request{callVA, anchorVA};
    std::optional<uint32_t> boundary =
        answerIslandBoundaryRequest(planningLayout, proposal, request);
    if (!boundary)
      return std::nullopt;
    boundaries.push_back(*boundary);
    anchorVA =
        planningLayout.proposedExtenderVA(proposal.desiredExtra, *boundary);
    ++depth;
    requireFirstIsland = false;
  }
  if (!inBranchRange(callVA, anchorVA))
    return std::nullopt;

  uint32_t head = noExtender;
  for (uint32_t boundary : boundaries)
    head = createExtender(context, proposal, planningLayout, calleeIdx,
                          boundary, ExtenderKind::Island, head);
  return head;
}

// Return an existing thunk for this callee that is in range of callVA. Keeping
// a callee-local, address-sorted index avoids scanning every extender proposed
// for all preceding callees for each thunk-routed callsite.
static uint32_t reuseCalleeThunk(const ExtensionLayout &planningLayout,
                                 ArrayRef<uint32_t> calleeThunks,
                                 uint64_t callVA) {
  auto thunkVA = [&](uint32_t extenderIdx) {
    return planningLayout.extenderVA[extenderIdx];
  };
  auto it = llvm::lower_bound(calleeThunks, callVA,
                              [&](uint32_t extenderIdx, uint64_t va) {
                                return thunkVA(extenderIdx) < va;
                              });
  uint64_t low = subSat(callVA, target->backwardBranchRange);
  uint64_t high = addSat(callVA, target->forwardBranchRange);
  if (it != calleeThunks.end() && thunkVA(*it) <= high)
    return *it;
  if (it != calleeThunks.begin()) {
    --it;
    if (thunkVA(*it) >= low)
      return *it;
  }
  return noExtender;
}

static std::optional<uint32_t>
findThunkBoundary(const ExtensionLayout &layout,
                  const ExtensionProposal &proposal, uint64_t callVA) {
  uint64_t low = subSat(callVA, target->backwardBranchRange);
  uint64_t high = addSat(callVA, target->forwardBranchRange);
  uint32_t right = layout.lowerBound(callVA);
  uint32_t left = right;
  bool hasLeft = left != 0;
  if (hasLeft)
    --left;

  while (hasLeft || right < layout.boundaryCount()) {
    bool takeRight = right < layout.boundaryCount();
    if (hasLeft && takeRight)
      takeRight = layout.boundaryBaseVA(right) - callVA <=
                  callVA - layout.boundaryBaseVA(left);
    uint32_t boundary = takeRight ? right : left;
    uint64_t base = layout.boundaryBaseVA(boundary);
    if (takeRight) {
      if (base > high) {
        right = layout.boundaryCount();
        continue;
      }
      ++right;
    } else {
      if (base < low) {
        hasLeft = false;
        continue;
      }
      hasLeft = left != 0;
      if (hasLeft)
        --left;
    }
    if (inBranchRange(
            callVA, layout.proposedExtenderVA(proposal.desiredExtra, boundary)))
      return boundary;
  }
  return std::nullopt;
}

static uint32_t placeThunk(FinalizerContext &context,
                           ExtensionProposal &proposal,
                           ExtensionLayout &planningLayout,
                           SmallVectorImpl<uint32_t> &calleeThunks,
                           uint32_t calleeIdx, uint64_t callVA) {
  if (uint32_t reuse = reuseCalleeThunk(planningLayout, calleeThunks, callVA);
      reuse != noExtender)
    return reuse;
  std::optional<uint32_t> boundary =
      findThunkBoundary(planningLayout, proposal, callVA);
  if (!boundary) {
    error("cannot place branch-extension thunk within range for " +
          toString(*context.callees[calleeIdx].target));
    return noExtender;
  }
  uint32_t extenderIdx =
      createExtender(context, proposal, planningLayout, calleeIdx, *boundary,
                     ExtenderKind::Thunk, noExtender);
  auto pos =
      llvm::lower_bound(calleeThunks, planningLayout.extenderVA[extenderIdx],
                        [&](uint32_t existing, uint64_t va) {
                          return planningLayout.extenderVA[existing] < va;
                        });
  calleeThunks.insert(pos, extenderIdx);
  return extenderIdx;
}

// relayTo links already represent an island chain. The callee keeps only its
// head index while planning one side; no duplicate Spine state is needed.
static uint32_t findReachableChainNode(const ExtensionProposal &proposal,
                                       const ExtensionLayout &layout,
                                       uint64_t callVA, uint32_t chainHead) {
  uint32_t best = noExtender;
  uint64_t bestDistance = maxVA;
  for (uint32_t extenderIdx = chainHead; extenderIdx != noExtender;
       extenderIdx = proposal.extenders[extenderIdx].relayTo) {
    uint64_t extenderVA = layout.extenderVA[extenderIdx];
    if (!inBranchRange(callVA, extenderVA))
      continue;
    uint64_t distance =
        callVA >= extenderVA ? callVA - extenderVA : extenderVA - callVA;
    if (distance < bestDistance) {
      best = extenderIdx;
      bestDistance = distance;
    }
  }
  return best;
}

static void planCallsite(FinalizerContext &context, ExtensionProposal &proposal,
                         ExtensionLayout &planningLayout,
                         SmallVectorImpl<uint32_t> &calleeThunks,
                         uint32_t calleeIdx, size_t routeIdx,
                         Callsite &callsite, uint64_t targetVA,
                         uint32_t &chainHead) {
  uint64_t callVA = planningLayout.callsiteVA(callsite);
  if (!callsite.forceExtender && inBranchRange(callVA, targetVA)) {
    proposal.routing[routeIdx] = noExtender;
    return;
  }
  if (uint32_t existing =
          findReachableChainNode(proposal, planningLayout, callVA, chainHead);
      existing != noExtender) {
    proposal.routing[routeIdx] = existing;
    return;
  }
  std::optional<uint32_t> islandHead =
      buildIslandChain(context, proposal, planningLayout, calleeIdx, callVA,
                       targetVA, callsite.forceExtender);
  uint32_t head = islandHead.value_or(noExtender);
  if (head == noExtender && context.policy.maxHops != noExtender)
    head = placeThunk(context, proposal, planningLayout, calleeThunks,
                      calleeIdx, callVA);
  if (head == noExtender) {
    error("cannot build slop-free branch-island chain from " +
          utohexstr(callVA) + " to " +
          toString(*context.callees[calleeIdx].target));
    proposal.routing[routeIdx] = noExtender;
    return;
  }
  proposal.routing[routeIdx] = head;
  if (proposal.extenders[head].kind == ExtenderKind::Island)
    chainHead = head;
}

static void planCallee(FinalizerContext &context, ExtensionProposal &proposal,
                       ExtensionLayout &planningLayout, uint32_t calleeIdx,
                       size_t routeBase) {
  Callee &callee = context.callees[calleeIdx];
  SmallVector<uint32_t, 4> calleeThunks;
  std::optional<uint64_t> targetVA = planningLayout.calleeTargetVA[calleeIdx];
  if (!targetVA) {
    if (context.policy.maxHops == noExtender) {
      error("cannot resolve island-only branch target " +
            toString(*callee.target));
      return;
    }
    for (size_t callsiteIdx = 0; callsiteIdx < callee.callsites.size();
         ++callsiteIdx) {
      uint64_t callVA =
          planningLayout.callsiteVA(callee.callsites[callsiteIdx]);
      proposal.routing[routeBase + callsiteIdx] = placeThunk(
          context, proposal, planningLayout, calleeThunks, calleeIdx, callVA);
    }
    return;
  }

  size_t split =
      std::lower_bound(callee.callsites.begin(), callee.callsites.end(),
                       *targetVA,
                       [&](const Callsite &callsite, uint64_t va) {
                         return planningLayout.callsiteVA(callsite) < va;
                       }) -
      callee.callsites.begin();
  // Process each side from its furthest caller. `relayTo` is the shared chain;
  // the head index gives nearer calls a route into it without a Spine object.
  uint32_t lowerChainHead = noExtender;
  for (size_t callsiteIdx = 0; callsiteIdx < split; ++callsiteIdx)
    planCallsite(context, proposal, planningLayout, calleeThunks, calleeIdx,
                 routeBase + callsiteIdx, callee.callsites[callsiteIdx],
                 *targetVA, lowerChainHead);
  uint32_t upperChainHead = noExtender;
  for (size_t callsiteIdx = callee.callsites.size(); callsiteIdx != split;) {
    --callsiteIdx;
    planCallsite(context, proposal, planningLayout, calleeThunks, calleeIdx,
                 routeBase + callsiteIdx, callee.callsites[callsiteIdx],
                 *targetVA, upperChainHead);
  }
}

ExtensionProposal::ExtensionProposal(const FinalizerContext &context,
                                     ExtensionLayout &planningLayout) {
  routing.assign(context.callsiteCount, noExtender);
  planningLayout.extenderVA.clear();
  desiredExtra.assign(context.inputs.size(), 0);
  coarseBoundaries = buildCoarseBoundaryCatalog(planningLayout);
}

void ExtensionProposal::plan(FinalizerContext &context,
                             ExtensionLayout &planningLayout) {
  size_t routeBase = 0;
  for (uint32_t calleeIdx = 0; calleeIdx < context.callees.size();
       ++calleeIdx) {
    planCallee(context, *this, planningLayout, calleeIdx, routeBase);
    routeBase += context.callees[calleeIdx].callsites.size();
  }
  assert(routeBase == routing.size());
}

static bool isUnresolvedDtrace(const Callee &callee) {
  return callee.target->getName().starts_with("___dtrace_");
}

static bool validateExtensionProposal(FinalizerContext &context,
                                      ExtensionProposal &proposal,
                                      const ExtensionLayout &layout) {
  bool valid = true;
  proposal.invalidDirectCallsites.clear();

  // Each extender must reach its relay island (a lower-indexed island in range)
  // or, as a terminal, its callee's resolved target.
  for (size_t extenderIdx = 0; extenderIdx < proposal.extenders.size();
       ++extenderIdx) {
    const ExtensionNode &extender = proposal.extenders[extenderIdx];
    if (extender.kind == ExtenderKind::Thunk)
      continue;
    if (extender.relayTo != noExtender) {
      if (extender.relayTo >= extenderIdx ||
          proposal.extenders[extender.relayTo].kind != ExtenderKind::Island ||
          !inBranchRange(layout.extenderVA[extenderIdx],
                         layout.extenderVA[extender.relayTo]))
        valid = false;
      continue;
    }
    if (extender.calleeIdx >= layout.calleeTargetVA.size() ||
        !layout.calleeTargetVA[extender.calleeIdx]) {
      if (extender.calleeIdx >= context.callees.size() ||
          !isUnresolvedDtrace(context.callees[extender.calleeIdx]))
        valid = false;
      continue;
    }
    if (!inBranchRange(layout.extenderVA[extenderIdx],
                       *layout.calleeTargetVA[extender.calleeIdx]))
      valid = false;
  }

  // Each callsite must reach its assigned extender or its direct target.
  size_t routeIdx = 0;
  for (size_t calleeIdx = 0; calleeIdx < context.callees.size(); ++calleeIdx) {
    Callee &callee = context.callees[calleeIdx];
    for (Callsite &callsite : callee.callsites) {
      uint32_t extenderIdx = proposal.routing[routeIdx++];
      if (extenderIdx != noExtender) {
        if (extenderIdx >= proposal.extenders.size() ||
            !inBranchRange(layout.callsiteVA(callsite),
                           layout.extenderVA[extenderIdx]))
          valid = false;
      } else if (!layout.calleeTargetVA[calleeIdx]) {
        if (!isUnresolvedDtrace(callee))
          valid = false;
      } else if (!inBranchRange(layout.callsiteVA(callsite),
                                *layout.calleeTargetVA[calleeIdx])) {
        valid = false;
        if (!callsite.forceExtender)
          proposal.invalidDirectCallsites.push_back(&callsite);
      }
    }
  }
  return valid;
}

static size_t findValidExtensionProposal(FinalizerContext &context,
                                         ExtensionProposal &proposal,
                                         ExtensionLayout &proposalLayout) {
  for (size_t pass = 1; pass <= maxPasses; ++pass) {
    ExtensionLayout planningLayout = layoutReservationEnvelope(context);
    proposal = ExtensionProposal(context, planningLayout);
    proposal.plan(context, planningLayout);
    proposalLayout = layoutExtensionProposal(context, proposal);

    log(context.policy.name + " branch extender pass " + std::to_string(pass) +
        ": proposed extenders = " + std::to_string(proposal.extenders.size()));
    if (validateExtensionProposal(context, proposal, proposalLayout))
      return pass;
    log(context.policy.name + " branch extender rejected proposal");

    bool grew = false;
    for (size_t boundary = 0; boundary < context.reservedExtra.size();
         ++boundary) {
      if (context.reservedExtra[boundary] >= proposal.desiredExtra[boundary])
        continue;
      context.reservedExtra[boundary] = proposal.desiredExtra[boundary];
      grew = true;
    }
    if (!grew) {
      size_t promoted = proposal.invalidDirectCallsites.size();
      for (Callsite *callsite : proposal.invalidDirectCallsites)
        callsite->forceExtender = true;
      if (promoted != 0) {
        log(context.policy.name + " branch extender promoted direct calls = " +
            std::to_string(promoted));
        grew = true;
      }
    }
    if (!grew)
      return maxPasses + 1;
  }
  return maxPasses + 1;
}

static SmallVector<MaterializedExtender, 0>
materializeExtenders(const FinalizerContext &context,
                     const ExtensionProposal &proposal) {
  DenseMap<Symbol *, std::pair<size_t, size_t>> sequences;
  SmallVector<MaterializedExtender, 0> materialized(proposal.extenders.size());
  for (size_t extenderIdx = 0; extenderIdx < proposal.extenders.size();
       ++extenderIdx) {
    const ExtensionNode &extender = proposal.extenders[extenderIdx];
    const Callee &callee = context.callees[extender.calleeIdx];
    ConcatInputSection *boundaryIsec = context.inputs[extender.boundaryIdx];
    MaterializedExtender &result = materialized[extenderIdx];
    result.isec = makeSyntheticInputSection(boundaryIsec->getSegName(),
                                            boundaryIsec->getName());
    result.isec->parent = boundaryIsec->parent;
    assert(result.isec->live);

    auto &[islands, thunks] = sequences[callee.target];
    bool isThunk = extender.kind == ExtenderKind::Thunk;
    size_t number = isThunk ? thunks++ : islands++;
    StringRef kind = isThunk ? ".thunk." : ".island.";
    StringRef name =
        saver().save(callee.target->getName() + kind + std::to_string(number));
    size_t size = extenderSize(extender);
    if (!isa<Defined>(callee.target) ||
        cast<Defined>(callee.target)->isExternal()) {
      result.sym = symtab->addDefined(
          name, /*file=*/nullptr, result.isec, /*value=*/0, size,
          /*isWeakDef=*/false, /*isPrivateExtern=*/true,
          /*isReferencedDynamically=*/false, /*noDeadStrip=*/false,
          /*isWeakDefCanBeHidden=*/false);
    } else {
      result.sym = make<Defined>(
          name, /*file=*/nullptr, result.isec, /*value=*/0, size,
          /*isWeakDef=*/false, /*isExternal=*/false,
          /*isPrivateExtern=*/true, /*includeInSymtab=*/true,
          /*isReferencedDynamically=*/false, /*noDeadStrip=*/false,
          /*isWeakDefCanBeHidden=*/false);
    }
    result.sym->used = true;
  }

  for (size_t extenderIdx = 0; extenderIdx < proposal.extenders.size();
       ++extenderIdx) {
    const ExtensionNode &extender = proposal.extenders[extenderIdx];
    const Callee &callee = context.callees[extender.calleeIdx];
    ConcatInputSection *isec = materialized[extenderIdx].isec;
    if (extender.kind == ExtenderKind::Thunk) {
      if (needsBinding(callee.target))
        in.stubs->addEntry(callee.target);
      target->populateThunk(isec, callee.target, callee.addend);
    } else if (extender.relayTo != noExtender) {
      target->populateIsland(isec, materialized[extender.relayTo].sym);
      isec->relocs[0].addend = 0;
    } else {
      target->populateIsland(isec, callee.target);
      isec->relocs[0].addend = callee.addend;
    }
  }
  return materialized;
}

static void rewriteBranches(const FinalizerContext &context,
                            ExtensionProposal &proposal,
                            const ExtensionLayout &layout,
                            ArrayRef<MaterializedExtender> materialized) {
  size_t routeIdx = 0;
  for (size_t calleeIdx = 0; calleeIdx < context.callees.size(); ++calleeIdx) {
    const Callee &callee = context.callees[calleeIdx];
    for (const Callsite &callsite : callee.callsites) {
      uint32_t &extenderIdx = proposal.routing[routeIdx++];
      if (layout.calleeTargetVA[calleeIdx] &&
          inBranchRange(layout.callsiteVA(callsite),
                        *layout.calleeTargetVA[calleeIdx])) {
        extenderIdx = noExtender;
        continue;
      }
      if (extenderIdx == noExtender)
        continue;
      callsite.reloc->referent = materialized[extenderIdx].sym;
      callsite.reloc->addend = 0;
    }
  }
}

} // namespace

void TextOutputSection::finalizeWithExtenders(BranchRangeExtensionMode mode) {
  if (hybridFinalized)
    return;

  TimeTraceScope timeScope("Branch extender", extensionPolicy(mode).name);

  // 1. Freeze the address-ordered topology and callees (branch destinations).
  FinalizerContext context(*this, mode);
  for (const OwnerInputSlice &run : context.ownerRuns)
    assert(!run.section->hybridFinalized);

  // 2. Build disposable graphs until one validates in its own exact layout.
  ExtensionProposal proposal;
  ExtensionLayout proposalLayout;
  size_t passes = findValidExtensionProposal(context, proposal, proposalLayout);
  if (passes > maxPasses)
    fatal(context.policy.name + " branch extender did not converge after " +
          std::to_string(maxPasses) + " passes");

  // 3. Commit only the accepted graph and normalize its final routing.
  SmallVector<MaterializedExtender, 0> materialized =
      materializeExtenders(context, proposal);
  rewriteBranches(context, proposal, proposalLayout, materialized);

  // 4. Emit in the same stable boundary order used by the accepted layout.
  {
    TimeTraceScope timeScope("Branch extension final layout");
    uint64_t groupSize = 0;
    size_t orderIdx = 0;
    for (const OwnerInputSlice &run : context.ownerRuns) {
      TextOutputSection *osec = run.section;
      groupSize = alignToPowerOf2(groupSize, osec->align);
      osec->hybridAddr = addr + groupSize;
      osec->addr = osec->hybridAddr;
      osec->size = 0;
      osec->fileSize = 0;
      auto finalize = [&](ConcatInputSection *isec) {
        osec->finalizeOne(isec);
        groupSize = osec->hybridAddr - addr + osec->size;
      };
      for (uint32_t inputIdx = run.firstInput; inputIdx < run.lastInput;
           ++inputIdx) {
        finalize(context.inputs[inputIdx]);
        assert(context.inputs[inputIdx]->getVA() ==
               proposalLayout.inputVA[inputIdx]);
        while (orderIdx < proposal.byBoundary.size()) {
          uint32_t extenderIdx = proposal.byBoundary[orderIdx];
          if (proposal.extenders[extenderIdx].boundaryIdx != inputIdx)
            break;
          ConcatInputSection *isec = materialized[extenderIdx].isec;
          finalize(isec);
          assert(isec->getVA() == proposalLayout.extenderVA[extenderIdx]);
          osec->thunks.push_back(isec);
          ++orderIdx;
        }
      }
    }
    assert(orderIdx == proposal.byBoundary.size());
  }

  // 5. Publish the finalized owner run.
  log(context.policy.name + " branch extender for " + parent->name + "," +
      name + ": passes = " + std::to_string(passes) +
      ", inputs = " + std::to_string(context.inputs.size()) +
      ", targets = " + std::to_string(context.callees.size()) +
      ", total extenders = " + std::to_string(proposal.extenders.size()));

  for (const OwnerInputSlice &run : context.ownerRuns) {
    run.section->addr = run.section->hybridAddr;
    run.section->hybridFinalized = true;
  }
}

//===- TextOutputSegment.cpp ---------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TextOutputSegment.h"
#include "ConcatOutputSection.h"
#include "Config.h"
#include "OutputSegment.h"
#include "Relocations.h"
#include "Sections.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "SyntheticSections.h"
#include "Target.h"
#include "lld/Common/CommonLinkerContext.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/TimeProfiler.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <optional>
#include <tuple>
#include <utility>

using namespace llvm;
using namespace lld;
using namespace lld::macho;

static uint64_t subSat(uint64_t a, uint64_t b) { return a > b ? a - b : 0; }
static uint64_t addSat(uint64_t a, uint64_t b) {
  return UINT64_MAX - a < b ? UINT64_MAX : a + b;
}

static bool inBranchRange(uint64_t from, uint64_t to) {
  return subSat(from, target->backwardBranchRange) <= to &&
         to <= addSat(from, target->forwardBranchRange);
}

template <typename BoundaryRange, typename Predicate>
static Boundary *findInDirection(BoundaryRange &&boundaries, bool ascending,
                                 Predicate predicate) {
  auto findInRange = [&](auto &&range) -> Boundary * {
    for (auto *boundary : range)
      if (predicate(*boundary))
        return boundary;
    return nullptr;
  };
  return ascending ? findInRange(boundaries)
                   : findInRange(llvm::reverse(boundaries));
}

namespace lld::macho {
uint64_t Boundary::getInputEndVA() const { return inputVA + isec->getSize(); }

uint64_t Boundary::getNextExtenderVA(ExtenderKind kind) const {
  return alignToPowerOf2(getInputEndVA() + plannedSize,
                         target->getExtenderAlign(kind));
}

BoundaryTable::BoundaryTable(ArrayRef<TextOutputSection *> sections) {
  size_t inputCount = 0;
  for (auto *section : sections)
    inputCount += section->inputs.size();
  assert(inputCount && "text output segment must contain an input section");
  // Keep pointers in coarseBoundaries stable while collecting inputs.
  boundaries.reserve(inputCount);

  // Select coarse boundaries once from input sizes and alignment, before any
  // extenders are reserved. Space them 1 MiB apart for performance and
  // locality.
  constexpr uint64_t boundarySpacing = 1024 * 1024;
  uint64_t va = sections.front()->addr;
  uint64_t lastVA = va;
  for (auto *section : sections) {
    va = alignToPowerOf2(va, section->align);
    uint64_t sectionVA = va;
    for (auto *isec : section->inputs) {
      boundaries.emplace_back(isec, section);
      va = sectionVA + alignToPowerOf2(va - sectionVA, isec->align) +
           isec->getSize();
      if (coarseBoundaries.empty() || va - lastVA >= boundarySpacing) {
        coarseBoundaries.push_back(&boundaries.back());
        lastVA = va;
      }
    }
  }
  if (coarseBoundaries.back() != &boundaries.back())
    coarseBoundaries.push_back(&boundaries.back());
}

Boundary *
BoundaryTable::findExtenderPlacementInRange(uint64_t lowVA, uint64_t highVA,
                                            BoundaryPreference preference,
                                            ExtenderKind kind) {
  if (lowVA > highVA)
    return nullptr;

  bool ascending = preference == BoundaryPreference::lowest;
  auto isUsable = [&](const Boundary &boundary) {
    uint64_t va = boundary.getNextExtenderVA(kind);
    return lowVA <= va && va <= highVA;
  };

  auto coarseBegin = llvm::lower_bound(
      coarseBoundaries, lowVA, [](const Boundary *boundary, uint64_t va) {
        return boundary->getInputEndVA() < va;
      });
  auto coarseEnd = llvm::upper_bound(coarseBoundaries, highVA,
                                     [](uint64_t va, const Boundary *boundary) {
                                       return va < boundary->getInputEndVA();
                                     });
  if (auto *boundary = findInDirection(llvm::make_range(coarseBegin, coarseEnd),
                                       ascending, isUsable))
    return boundary;

  auto begin = llvm::partition_point(boundaries, [&](const Boundary &boundary) {
    return boundary.getInputEndVA() < lowVA;
  });
  auto end = llvm::partition_point(boundaries, [&](const Boundary &boundary) {
    return boundary.getInputEndVA() <= highVA;
  });
  return findInDirection(llvm::make_pointer_range(llvm::make_range(begin, end)),
                         ascending, isUsable);
}

Boundary *BoundaryTable::findExtenderPlacementNear(uint64_t callVA,
                                                   ExtenderKind kind) {
  auto baseVA = [kind](const Boundary &boundary) {
    return alignToPowerOf2(boundary.getInputEndVA(),
                           target->getExtenderAlign(kind));
  };
  uint64_t lowVA = subSat(callVA, target->backwardBranchRange);
  uint64_t highVA = addSat(callVA, target->forwardBranchRange);
  auto begin = llvm::partition_point(boundaries, [&](const Boundary &boundary) {
    return baseVA(boundary) < lowVA;
  });
  auto end = llvm::partition_point(boundaries, [&](const Boundary &boundary) {
    return baseVA(boundary) <= highVA;
  });
  auto left = llvm::partition_point(boundaries, [&](const Boundary &boundary) {
    return baseVA(boundary) < callVA;
  });
  auto right = left;
  // Walk dense boundaries in order of distance from the caller. Check the
  // actual placement too, since earlier extenders may occupy this boundary.
  while (left != begin || right != end) {
    Boundary *boundary;
    if (left == begin)
      boundary = &*right++;
    else if (right == end ||
             callVA - baseVA(*std::prev(left)) < baseVA(*right) - callVA)
      boundary = &*--left;
    else
      boundary = &*right++;
    if (inBranchRange(callVA, boundary->getNextExtenderVA(kind)))
      return boundary;
  }
  return nullptr;
}

struct Callsite {
  Relocation &reloc;
  const Boundary &boundary;

  // Selected by the current proposal; null means a direct call.
  Extender *extender = nullptr;

  uint64_t getVA() const { return boundary.inputVA + reloc.offset; }
};

struct Callee {
  Callee(Symbol *sym, int64_t addend,
         const DenseMap<ConcatInputSection *, Boundary *> &inputBoundaries)
      : sym(sym), addend(addend) {
    // A zero-addend callee whose address is resolved at runtime is reached
    // through its stub.
    if (needsBinding(sym) && addend == 0)
      return;

    auto *defined = dyn_cast<Defined>(sym);
    if (!defined)
      return;

    // A definition outside a ConcatInputSection has no modeled input boundary.
    auto *targetIsec = dyn_cast_or_null<ConcatInputSection>(defined->isec());
    if (!targetIsec)
      return;

    // A definition outside this TextOutputSegment has no Boundary in its table.
    Boundary *boundary = inputBoundaries.lookup(targetIsec);
    if (!boundary)
      return;

    // A local definition, or an interior location selected by a nonzero addend,
    // follows the Boundary that models its input section.
    targetBoundary = boundary;
  }

  // Stable target identity and binding shared by all planning passes.
  Symbol *const sym;
  const int64_t addend;
  // Boundary wrapping the callee's input section. Null when the callee uses a
  // stub, lacks a compatible input section, or lies outside this text segment.
  const Boundary *targetBoundary = nullptr;

  // Collected once in input and relocation-offset order. Layout changes
  // preserve this order, so planning can split the callers without sorting.
  SmallVector<Callsite, 1> callsites;
};

struct Extender {
  Extender(Callee &callee, Boundary &boundary, ExtenderKind kind,
           Extender *inwardIsland, uint64_t va)
      : callee(callee), boundary(boundary), kind(kind),
        inwardIsland(inwardIsland), va(va) {}

  size_t size() const { return target->getExtenderSize(kind); }
  uint32_t align() const { return target->getExtenderAlign(kind); }

  // The ultimate destination is needed to materialize the extender body.
  Callee &callee;

  // The insertion boundary connects the proposal to its owning text section.
  Boundary &boundary;
  ExtenderKind kind;

  // The next island toward the callee. A null pointer means that this extender
  // branches directly to the final target.
  Extender *inwardIsland;

  // The proposed or exact address is used for branch-range validation.
  uint64_t va = 0;
};

// Canonical identity for a callee and addend. Non-binding definitions are
// keyed by input location so aliases coalesce; other callees retain symbol
using CalleeKey = std::tuple<const void *, uint64_t, int64_t>;

static CalleeKey makeCalleeKey(Symbol *sym, int64_t addend) {
  if (const auto *defined = dyn_cast<Defined>(sym);
      defined && !needsBinding(sym))
    return {defined->isec(), defined->value + addend, 0};
  return {sym, 0, addend};
}

TextOutputSegment::TextOutputSegment(iterator first, iterator end)
    : next(first) {
  for (; next != end && (*next)->canHostExtenders(); ++next)
    textOutputSections.push_back(cast<TextOutputSection>(*next));
  assert(!textOutputSections.empty());
}

namespace {

class TextOutputSegmentPlanner {
public:
  explicit TextOutputSegmentPlanner(TextOutputSegment &segment);

  void run();

private:
  const uint32_t maxHops;
  const ExtenderKind chainKind;
  const ExtenderKind fallbackKind;
  ArrayRef<TextOutputSection *> textOutputSections;
  BoundaryTable boundaries;
  SmallVector<Callee *, 0> callees;
  // Forced routing is retained only while reservations remain unchanged.
  DenseSet<const Callsite *> forcedExtenderCallsites;
  // Per-pass state is separate from stable callee identity. Resolved VAs are
  // broadly used, islands are common only for callees that need extension, and
  // thunks are rare, so the extender maps are populated lazily.
  DenseMap<const Callee *, uint64_t> calleeVAs;
  DenseMap<const Callee *, SmallVector<Extender *, 4>> calleeIslands;
  DenseMap<const Callee *, SmallVector<Extender *, 1>> calleeThunks;
  // Estimated VAs for following non-hostable sections that contain callees.
  // Their sizes remain stable across planning because they cannot host
  // extenders.
  DenseMap<OutputSection *, uint64_t> postTextOutputSegmentVAs;
  uint64_t textEndVA = 0;

  TextOutputSection &firstSection() const {
    return *textOutputSections.front();
  }
  void updatePostTextOutputSegmentVAs();
  std::optional<uint64_t> getPostTextOutputSegmentVA(OutputSection *) const;
  std::optional<uint64_t> resolveCalleeVA(const Callee &);
  std::optional<uint64_t> getCalleeVA(const Callee &) const;
  // Evaluate the selected segment layout, propagate its end through stable
  // following sections, and resolve callees against the combined result. The
  // reservation layout models reserved space for planning; the proposal layout
  // records candidate VAs for output sections and extenders for validation.
  enum class LayoutKind { reservation, proposal };
  void evaluateLayout(LayoutKind);

  Boundary *findIslandBoundaryTowardCallsite(uint64_t callVA,
                                             uint64_t chainTipVA);
  Extender *planExtenderAtBoundary(Callee &callee, Boundary &boundary,
                                   ExtenderKind kind, Extender *inwardIsland);
  Extender *findReusableExtender(const Callee &callee, uint64_t callVA) const;
  Extender *tryExtendIslandChain(Callee &callee, const Callsite &callsite,
                                 Extender *chainTipIsland);
  Extender *tryPlanIslandChain(Callee &callee, const Callsite &callsite);
  Extender *planFallbackThunk(Callee &callee, uint64_t callVA);
  enum class CallsiteSide { belowTarget, aboveTarget };
  void planCallsite(Callee &callee, Callsite &callsite);
  void planSide(Callee &callee, MutableArrayRef<Callsite> calls,
                CallsiteSide side);
  void planCallee(Callee &callee);
  void buildBranchExtensionProposal();
  // A proposed layout can be invalid because an extender cannot reach its next
  // target or because a callsite cannot reach its selected target. The
  // extenders in a rejected proposal are discarded before replanning, so grow
  // the reservations for those extenders to preserve their space. If a direct
  // callsite-to-callee edge becomes invalid, routing the call through an
  // extender may recover the layout.
  bool recoverFromInvalidProposal();
  bool forceInvalidDirectCalls();
  bool isLayoutValid();
  void materializeBranchExtensionPlan();
};

TextOutputSegmentPlanner::TextOutputSegmentPlanner(TextOutputSegment &segment)
    : maxHops(config->branchRangeExtensionMaxHops),
      chainKind(ExtenderKind::island), fallbackKind(ExtenderKind::thunk),
      textOutputSections(segment.getSections()),
      boundaries(textOutputSections) {
  DenseMap<ConcatInputSection *, Boundary *> inputBoundaries;
  for (auto &boundary : boundaries.entries()) {
    bool inserted =
        inputBoundaries.try_emplace(boundary.isec, &boundary).second;
    assert(inserted && "input section has more than one boundary");
  }

  // Bind every branch target while the temporary input index is available.
  // Callee identity and relocation bindings survive every planning retry.
  DenseMap<CalleeKey, Callee *> calleesByKey;
  SmallVector<Relocation *, 0> branches;
  for (auto &boundary : boundaries.entries()) {
    branches.clear();
    for (auto &reloc : llvm::reverse(boundary.isec->relocs))
      if (target->hasAttr(reloc.type, RelocAttrBits::BRANCH))
        branches.push_back(&reloc);
    auto byOffset = [](const Relocation *lhs, const Relocation *rhs) {
      return lhs->offset < rhs->offset;
    };
    if (!llvm::is_sorted(branches, byOffset))
      llvm::stable_sort(branches, byOffset);
    for (Relocation *reloc : branches) {
      Symbol *sym = cast<Symbol *>(reloc->referent);
      auto &callee = calleesByKey[makeCalleeKey(sym, reloc->addend)];
      if (!callee) {
        callee = make<Callee>(sym, reloc->addend, inputBoundaries);
        callees.push_back(callee);
      }
      callee->callsites.push_back({*reloc, boundary});
    }
  }
}

void TextOutputSegmentPlanner::updatePostTextOutputSegmentVAs() {
  TextOutputSection &first = firstSection();
  const auto &sections = first.parent->getSections();
  auto firstIt = llvm::find(sections, &first);
  assert(firstIt != sections.end());

  assert(textOutputSections.size() <=
         static_cast<size_t>(sections.end() - firstIt));
  auto suffixBegin = std::next(firstIt, textOutputSections.size());
  postTextOutputSegmentVAs.clear();
  uint64_t va = textEndVA;
  for (auto *section : llvm::make_range(suffixBegin, sections.end())) {
    if (!section->isNeeded())
      continue;
    va = alignToPowerOf2(va, section->align);
    postTextOutputSegmentVAs.try_emplace(section, va);
    va += section->getSizeForAddressAssignment();
  }
}

std::optional<uint64_t> TextOutputSegmentPlanner::getPostTextOutputSegmentVA(
    OutputSection *targetSection) const {
  assert(targetSection && targetSection->isNeeded() &&
         "target section must exist and be needed");
  if (auto it = postTextOutputSegmentVAs.find(targetSection);
      it != postTextOutputSegmentVAs.end())
    return it->second;
  return std::nullopt;
}

// Once the current TextOutputSegment and following sections have reserved VAs,
// compute callee VAs to evaluate branch reachability during planning. This
// handles definitions within the segment and callees in following sections
// whose sizes cannot change through extender insertion, such as __stubs.
std::optional<uint64_t>
TextOutputSegmentPlanner::resolveCalleeVA(const Callee &callee) {
  Symbol *sym = callee.sym;
  int64_t addend = callee.addend;

  if (sym->isInStubs() && addend == 0) {
    if (in.stubs->isFinal)
      return sym->getStubVA();
    if (auto sectionVA = getPostTextOutputSegmentVA(in.stubs))
      return *sectionVA + uint64_t(sym->stubsIndex) * target->stubSize;
    return std::nullopt;
  }

  auto *defined = dyn_cast<Defined>(sym);
  if (!defined)
    return std::nullopt;

  if (in.objcStubs && defined->isec() == in.objcStubs->isec &&
      in.objcStubs->isNeeded()) {
    if (in.objcStubs->isec->isFinal)
      return defined->getVA() + addend;
    if (auto sectionVA = getPostTextOutputSegmentVA(in.objcStubs))
      return *sectionVA + defined->value + addend;
    return std::nullopt;
  }

  if (defined->isAbsolute())
    return defined->getVA() + addend;
  if (callee.targetBoundary)
    return callee.targetBoundary->inputVA + defined->value + addend;
  return std::nullopt;
}

std::optional<uint64_t>
TextOutputSegmentPlanner::getCalleeVA(const Callee &callee) const {
  auto it = calleeVAs.find(&callee);
  if (it == calleeVAs.end())
    return std::nullopt;
  return it->second;
}

void TextOutputSegmentPlanner::evaluateLayout(LayoutKind kind) {
  uint64_t va = firstSection().addr;
  MutableArrayRef<Boundary> entries = boundaries.entries();
  auto boundaryIt = entries.begin();
  for (auto *osec : textOutputSections) {
    va = alignToPowerOf2(va, osec->align);
    uint64_t sectionVA = va;
    if (kind == LayoutKind::proposal)
      osec->addr = sectionVA;

    while (boundaryIt != entries.end() && boundaryIt->section == osec) {
      auto &boundary = *boundaryIt++;

      va = sectionVA + alignToPowerOf2(va - sectionVA, boundary.isec->align);
      boundary.inputVA = va;
      va += boundary.isec->getSize();

      if (kind == LayoutKind::reservation) {
        va += boundary.reservedSize;
        continue;
      }
      assert(kind == LayoutKind::proposal);
      for (auto *extender : boundary.plannedExtenders) {
        va = alignToPowerOf2(va, extender->align());
        extender->va = va;
        va += extender->size();
      }
    }
  }
  assert(boundaryIt == entries.end());
  textEndVA = va;
  updatePostTextOutputSegmentVAs();
  calleeVAs.clear();
  for (auto *callee : callees)
    if (std::optional<uint64_t> va = resolveCalleeVA(*callee))
      calleeVAs.try_emplace(callee, *va);
}

Boundary *TextOutputSegmentPlanner::findIslandBoundaryTowardCallsite(
    uint64_t callVA, uint64_t chainTipVA) {
  if (callVA == chainTipVA)
    return nullptr;
  bool upper = callVA > chainTipVA;
  uint64_t low = addSat(chainTipVA, 1);
  uint64_t high =
      std::min(callVA - 1, addSat(chainTipVA, target->backwardBranchRange));
  if (!upper) {
    low = std::max(addSat(callVA, 1),
                   subSat(chainTipVA, target->forwardBranchRange));
    high = chainTipVA - 1;
  }
  return boundaries.findExtenderPlacementInRange(
      low, high,
      upper ? BoundaryPreference::highest : BoundaryPreference::lowest,
      chainKind);
}

Extender *TextOutputSegmentPlanner::planExtenderAtBoundary(
    Callee &callee, Boundary &boundary, ExtenderKind kind,
    Extender *inwardIsland) {
  uint64_t va = boundary.getNextExtenderVA(kind);
  Extender *extender = make<Extender>(callee, boundary, kind, inwardIsland, va);
  boundary.plannedSize = va + extender->size() - boundary.getInputEndVA();
  boundary.plannedExtenders.push_back(extender);
  if (kind == chainKind)
    calleeIslands[&callee].push_back(extender);
  else
    calleeThunks[&callee].push_back(extender);
  return extender;
}

Extender *
TextOutputSegmentPlanner::findReusableExtender(const Callee &callee,
                                               uint64_t callVA) const {
  Extender *nearest = nullptr;
  uint64_t bestDistance = UINT64_MAX;
  auto consider = [&](ArrayRef<Extender *> extenders) {
    for (Extender *extender : extenders) {
      if (!inBranchRange(callVA, extender->va))
        continue;
      uint64_t distance =
          callVA > extender->va ? callVA - extender->va : extender->va - callVA;
      if (distance < bestDistance) {
        nearest = extender;
        bestDistance = distance;
      }
    }
  };
  if (auto it = calleeIslands.find(&callee); it != calleeIslands.end())
    consider(it->second);
  if (auto it = calleeThunks.find(&callee); it != calleeThunks.end())
    consider(it->second);
  return nearest;
}

Extender *TextOutputSegmentPlanner::tryExtendIslandChain(
    Callee &callee, const Callsite &callsite, Extender *chainTipIsland) {
  uint64_t callVA = callsite.getVA();
  uint64_t chainTipVA =
      chainTipIsland ? chainTipIsland->va : *getCalleeVA(callee);
  // Count this chain's actual path, not other islands owned by the callee.
  uint32_t depth = 0;
  for (Extender *island = chainTipIsland; island; island = island->inwardIsland)
    ++depth;
  bool needsIsland =
      forcedExtenderCallsites.contains(&callsite) && !chainTipIsland;

  SmallVector<std::pair<Extender *, uint32_t>, 4> added;
  while (depth < maxHops &&
         (needsIsland || !inBranchRange(callVA, chainTipVA))) {
    Boundary *boundary = findIslandBoundaryTowardCallsite(callVA, chainTipVA);
    if (!boundary)
      break;
    uint32_t previousPlannedSize = boundary->plannedSize;
    Extender *extender =
        planExtenderAtBoundary(callee, *boundary, chainKind, chainTipIsland);
    added.emplace_back(extender, previousPlannedSize);
    chainTipVA = extender->va;
    chainTipIsland = extender;
    ++depth;
    needsIsland = false;
  }
  if (chainTipIsland && inBranchRange(callVA, chainTipVA))
    return chainTipIsland;

  // Undo only this attempt. Previously assigned callers keep their routes.
  for (auto [extender, previousPlannedSize] : llvm::reverse(added)) {
    extender->boundary.plannedSize = previousPlannedSize;
    assert(extender->boundary.plannedExtenders.back() == extender);
    extender->boundary.plannedExtenders.pop_back();
    auto &islands = calleeIslands.find(&callee)->second;
    assert(islands.back() == extender);
    islands.pop_back();
  }
  if (auto it = calleeIslands.find(&callee);
      it != calleeIslands.end() && it->second.empty())
    calleeIslands.erase(it);
  return nullptr;
}

Extender *
TextOutputSegmentPlanner::tryPlanIslandChain(Callee &callee,
                                             const Callsite &callsite) {
  if (!maxHops)
    return nullptr;
  uint64_t callVA = callsite.getVA();
  uint64_t targetVA = *getCalleeVA(callee);
  bool upper = callVA > targetVA;
  Extender *chainTip = nullptr;
  // Prefer extending the outermost island between this caller and its target.
  if (auto it = calleeIslands.find(&callee); it != calleeIslands.end())
    for (Extender *island : it->second) {
      if (upper ? island->va <= targetVA || island->va >= callVA
                : island->va >= targetVA || island->va <= callVA)
        continue;
      if (!chainTip ||
          (upper ? island->va > chainTip->va : island->va < chainTip->va))
        chainTip = island;
    }
  if (chainTip)
    if (Extender *extender = tryExtendIslandChain(callee, callsite, chainTip))
      return extender;

  // A short earlier chain may exhaust the hop budget. Try full-span hops from
  // the target before falling back to a thunk for this caller.
  return tryExtendIslandChain(callee, callsite, nullptr);
}

Extender *TextOutputSegmentPlanner::planFallbackThunk(Callee &callee,
                                                      uint64_t callVA) {
  if (!target->supportsExtender(ExtenderKind::thunk))
    fatal("target does not support thunk fallback for " +
          toString(*callee.sym));
  if (Extender *extender = findReusableExtender(callee, callVA))
    return extender;
  Boundary *boundary =
      boundaries.findExtenderPlacementNear(callVA, fallbackKind);
  if (!boundary)
    fatal("cannot place branch fallback extender for " + toString(*callee.sym));
  return planExtenderAtBoundary(callee, *boundary, fallbackKind,
                                /*inwardIsland=*/nullptr);
}

void TextOutputSegmentPlanner::planCallsite(Callee &callee,
                                            Callsite &callsite) {
  std::optional<uint64_t> calleeVA = getCalleeVA(callee);
  uint64_t callVA = callsite.getVA();
  if (!forcedExtenderCallsites.contains(&callsite) && calleeVA &&
      inBranchRange(callVA, *calleeVA))
    return;

  Extender *extender = findReusableExtender(callee, callVA);
  if (!extender && calleeVA)
    extender = tryPlanIslandChain(callee, callsite);
  if (!extender)
    extender = planFallbackThunk(callee, callVA);
  callsite.extender = extender;
}

void TextOutputSegmentPlanner::planSide(Callee &callee,
                                        MutableArrayRef<Callsite> calls,
                                        CallsiteSide side) {
  // Grow shared chains from the nearest caller toward the furthest caller.
  if (side == CallsiteSide::belowTarget) {
    for (Callsite &callsite : llvm::reverse(calls))
      planCallsite(callee, callsite);
  } else {
    for (Callsite &callsite : calls)
      planCallsite(callee, callsite);
  }
}

void TextOutputSegmentPlanner::planCallee(Callee &callee) {
  std::optional<uint64_t> calleeVA = getCalleeVA(callee);
  if (!calleeVA) {
    for (Callsite &callsite : callee.callsites)
      planCallsite(callee, callsite);
    return;
  }
  auto split = llvm::lower_bound(callee.callsites, *calleeVA,
                                 [](const Callsite &callsite, uint64_t va) {
                                   return callsite.getVA() < va;
                                 });
  MutableArrayRef<Callsite> calls(callee.callsites);
  size_t splitIdx = split - calls.begin();
  planSide(callee, calls.take_front(splitIdx), CallsiteSide::belowTarget);
  planSide(callee, calls.drop_front(splitIdx), CallsiteSide::aboveTarget);
}

void TextOutputSegmentPlanner::buildBranchExtensionProposal() {
  // Routes belong to the previous proposal; identities survive replanning.
  for (Callee *callee : callees)
    for (Callsite &callsite : callee->callsites)
      callsite.extender = nullptr;
  calleeIslands.clear();
  calleeThunks.clear();
  boundaries.resetPlannedSpace();
  evaluateLayout(LayoutKind::reservation);
  for (Callee *callee : callees)
    planCallee(*callee);
  evaluateLayout(LayoutKind::proposal);
}

bool TextOutputSegmentPlanner::isLayoutValid() {
  // Every island must reach its next inward island or the final callee. Thunks
  // need no such check because they can reach any target.
  for (const auto &[callee, islands] : calleeIslands)
    for (const auto *island : islands) {
      std::optional<uint64_t> calleeVA = getCalleeVA(*callee);
      assert(calleeVA && "island target has no VA");
      uint64_t targetVA =
          island->inwardIsland ? island->inwardIsland->va : *calleeVA;
      if (!inBranchRange(island->va, targetVA))
        return false;
    }

  // Every callsite must reach its selected extender or its direct target.
  for (const Callee *callee : callees) {
    std::optional<uint64_t> calleeVA = getCalleeVA(*callee);
    for (const Callsite &callsite : callee->callsites) {
      if (callsite.extender) {
        if (!inBranchRange(callsite.getVA(), callsite.extender->va))
          return false;
      } else if (forcedExtenderCallsites.contains(&callsite) || !calleeVA ||
                 !inBranchRange(callsite.getVA(), *calleeVA))
        return false;
    }
  }
  return true;
}

void TextOutputSegmentPlanner::materializeBranchExtensionPlan() {
  DenseMap<Extender *, TextOutputSection::ExtenderArtifact> artifacts;

  parallelForEach(callees, [&](Callee *callee) {
    auto byVA = [](const Extender *lhs, const Extender *rhs) {
      return lhs->va < rhs->va;
    };
    if (auto it = calleeIslands.find(callee); it != calleeIslands.end())
      llvm::sort(it->second, byVA);
    if (auto it = calleeThunks.find(callee); it != calleeThunks.end())
      llvm::sort(it->second, byVA);
  });

  // Declare every extender before populating any body so island chains may
  // refer an existing extender via the corresponding isec.
  auto declareExtenders = [&](Callee &callee, ArrayRef<Extender *> extenders) {
    Symbol *targetSym = callee.sym;
    int64_t addend = callee.addend;
    bool external = true;
    if (const auto *defined = dyn_cast<Defined>(targetSym))
      external = defined->isExternal();
    for (auto [sequence, extender] : llvm::enumerate(extenders)) {
      SmallString<128> nameStorage(targetSym->getName());
      if (addend != 0) {
        if (addend > 0)
          nameStorage += "+";
        nameStorage += Twine(addend).str();
      }
      nameStorage += target->getExtenderSuffix(extender->kind);
      nameStorage += Twine(sequence).str();
      StringRef name = saver().save(StringRef(nameStorage));
      auto artifact = extender->boundary.section->synthesizeExtender(
          name, extender->size(), external);
      artifacts.insert({extender, artifact});
    }
  };
  for (auto *callee : callees) {
    if (auto it = calleeIslands.find(callee); it != calleeIslands.end())
      declareExtenders(*callee, it->second);
    if (auto it = calleeThunks.find(callee); it != calleeThunks.end())
      declareExtenders(*callee, it->second);
  }

  uint64_t va = firstSection().addr;
  ArrayRef<Boundary> entries = boundaries.entries();
  auto boundaryIt = entries.begin();
  // Boundary entries are grouped by output section. Store each section's
  // accepted placements for Writer to finalize after planning the entire run.
  for (auto *section : textOutputSections) {
    va = alignToPowerOf2(va, section->align);
    assert(section->addr == va);
    SmallVector<TextOutputSection::ExtenderPlacement, 0> placements;
    while (boundaryIt != entries.end() && boundaryIt->section == section) {
      const auto &boundary = *boundaryIt++;
      for (auto *extender : boundary.plannedExtenders) {
        Symbol *bodyTarget = extender->inwardIsland
                                 ? artifacts.lookup(extender->inwardIsland).sym
                                 : extender->callee.sym;
        int64_t bodyAddend =
            extender->inwardIsland ? 0 : extender->callee.addend;
        if (extender->kind == fallbackKind)
          assert(needsBinding(extender->callee.sym) ==
                 extender->callee.sym->isInStubs());
        auto artifact = artifacts.lookup(extender);
        target->populateExtender(artifact.isec, extender->kind, bodyTarget,
                                 bodyAddend);
        placements.push_back({boundary.isec, artifact.isec});
      }
      va = boundary.getInputEndVA() + boundary.plannedSize;
    }
    section->setExtenderPlacements(std::move(placements), va - section->addr);
  }
  assert(boundaryIt == entries.end());
  assert(va == textEndVA);

  for (Callee *callee : callees)
    for (Callsite &callsite : callee->callsites)
      if (callsite.extender) {
        callsite.reloc.referent =
            static_cast<Symbol *>(artifacts.lookup(callsite.extender).sym);
        callsite.reloc.addend = 0;
      }
}

bool TextOutputSegmentPlanner::forceInvalidDirectCalls() {
  bool forcedNewCallsite = false;
  for (Callee *callee : callees) {
    std::optional<uint64_t> calleeVA = getCalleeVA(*callee);
    if (!calleeVA)
      continue;
    for (Callsite &callsite : callee->callsites)
      if (!callsite.extender && !inBranchRange(callsite.getVA(), *calleeVA)) {
        assert(!forcedExtenderCallsites.contains(&callsite) &&
               "direct call already requires an extender");
        forcedExtenderCallsites.insert(&callsite);
        forcedNewCallsite = true;
      }
  }
  return forcedNewCallsite;
}

bool TextOutputSegmentPlanner::recoverFromInvalidProposal() {
  bool grewReservation = false;
  for (auto &boundary : boundaries.entries())
    if (boundary.reservedSize < boundary.plannedSize) {
      boundary.reservedSize = boundary.plannedSize;
      grewReservation = true;
    }
  if (grewReservation) {
    // Reconsider direct calls against the new reservation layout.
    forcedExtenderCallsites.clear();
    return true;
  }
  return forceInvalidDirectCalls();
}

void TextOutputSegmentPlanner::run() {
  TimeTraceScope timeScope("Branch extender");
  constexpr size_t maxPassCount = 30;
  size_t pass = 1;
  for (; pass <= maxPassCount; ++pass) {
    buildBranchExtensionProposal();
    if (isLayoutValid())
      break;
    if (!recoverFromInvalidProposal())
      fatal("branch extension can't recover from failure");
  }
  if (pass > maxPassCount)
    fatal("branch extender did not converge");
  materializeBranchExtensionPlan();
  size_t extenderTargetCount = llvm::count_if(callees, [&](Callee *callee) {
    return calleeIslands.contains(callee) || calleeThunks.contains(callee);
  });
  size_t extenderCount = 0;
  for (const Boundary &boundary : boundaries.entries())
    extenderCount += boundary.plannedExtenders.size();
  auto &first = firstSection();
  log("branch extender for " + first.parent->name + "," + first.name +
      ": passes = " + std::to_string(pass) +
      ", inputs = " + std::to_string(boundaries.size()) +
      ", targets = " + std::to_string(extenderTargetCount) +
      ", total extenders = " + std::to_string(extenderCount));
}

} // namespace

TextOutputSegment::iterator TextOutputSegment::planBranchRangeExtension() {
  TextOutputSegmentPlanner planner(*this);
  planner.run();
  return next;
}
} // namespace lld::macho

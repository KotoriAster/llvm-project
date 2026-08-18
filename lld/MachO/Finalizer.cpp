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
#include "Relocations.h"
#include "Sections.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "SyntheticSections.h"
#include "Target.h"
#include "lld/Common/CommonLinkerContext.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/TimeProfiler.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <tuple>
#include <utility>

using namespace llvm;
using namespace lld;
using namespace lld::macho;

namespace {
constexpr uint64_t boundarySpacing = 1024 * 1024;

static uint64_t subSat(uint64_t a, uint64_t b) { return a > b ? a - b : 0; }
static uint64_t addSat(uint64_t a, uint64_t b) {
  return UINT64_MAX - a < b ? UINT64_MAX : a + b;
}

static bool inBranchRange(uint64_t from, uint64_t to) {
  return subSat(from, target->backwardBranchRange) <= to &&
         to <= addSat(from, target->forwardBranchRange);
}

template <typename Range, typename Predicate>
static std::optional<uint32_t> findInDirection(Range &&range, bool ascending,
                                               Predicate predicate) {
  for (auto idx : llvm::reverse_conditionally(range, !ascending))
    if (predicate(idx))
      return idx;
  return std::nullopt;
}

/* Boundaries are insertion points between isecs :
current isec (i)    planned extenders         alignment padding    next isec (i+1)
|-------------------|-----boundary (i)--------|....................|
inputVA             getVA()                   getExtenderVA()      next inputVA
*/
struct Boundary {
  uint64_t getVA() const {
    return alignToPowerOf2(inputVA + isec->getSize(), 4);
  }
  uint64_t getExtenderVA() const { return getVA() + planned; }

  ConcatInputSection *isec;

  // Extender contribution retained from rejected proposals for convergence.
  uint32_t reserved = 0;

  // Total size of extenders placed at this boundary.
  uint32_t planned = 0;

  // The current layout's input address anchors callers and insertion points.
  uint64_t inputVA = 0;
};

struct Extender;
struct Callsite {
  uint64_t getVA() const { return layoutBoundary->inputVA + reloc->offset; }
  bool operator<(const Callsite &other) const {
    if (layoutBoundary != other.layoutBoundary)
      return layoutBoundary < other.layoutBoundary;
    return reloc->offset < other.reloc->offset;
  }

  // Branch relocation rewritten to the selected extender after materialization.
  Relocation *reloc;

  // Boundary to calculate its tentative VA.
  const Boundary *layoutBoundary;

  // First branch hop, later used to redirect the materialized relocation.
  Extender *extender = nullptr;

  // Exact layout may invalidate a direct call that planning must then route.
  bool forceExtender = false;
};

struct Callee {
  using EstimateSectionVA =
      function_ref<std::optional<uint64_t>(OutputSection *)>;

  void resolveVA(EstimateSectionVA estimateSectionVA) {
    va.reset();
    Symbol *sym = target();
    int64_t targetAddend = addend();

    if (sym->isInStubs() && targetAddend == 0) {
      if (in.stubs->isFinal)
        va = sym->getStubVA();
      else if (auto sectionVA = estimateSectionVA(in.stubs))
        va = *sectionVA +
             uint64_t(sym->stubsIndex) * lld::macho::target->stubSize;
      return;
    }

    auto *defined = dyn_cast<Defined>(sym);
    if (!defined)
      return;

    if (in.objcStubs && defined->isec() == in.objcStubs->isec &&
        in.objcStubs->isNeeded()) {
      if (auto sectionVA = estimateSectionVA(in.objcStubs))
        va = *sectionVA + defined->value + targetAddend;
      return;
    }

    if (defined->isAbsolute()) {
      va = defined->getVA() + targetAddend;
      return;
    }
    if (targetBoundary)
      va = targetBoundary->inputVA + defined->value + targetAddend;
  }

  std::optional<uint64_t> getVA() const { return va; }
  bool promoteCallsites();
  void redirectCallsites();

  SmallVector<Callsite, 1> callsites;
  Symbol *target() const {
    return cast<Symbol *>(callsites[0].reloc->referent);
  }
  int64_t addend() const { return callsites.front().reloc->addend; }
  bool isDtrace() const {
    return target()->getName().starts_with("___dtrace_");
  }

  // Boundary of the input hosting a local target; null for nonlocal targets.
  const Boundary *targetBoundary = nullptr;
  std::optional<uint64_t> va;
};

struct Extender {
  Extender(Callee &callee, uint32_t boundaryIdx, ExtenderKind kind, uint64_t va)
      : callee(&callee), boundaryIdx(boundaryIdx), kind(kind), va(va) {}

  size_t size() const { return target->getExtenderSize(kind); }
  uint32_t align() const { return target->getExtenderAlign(kind); }

  // The ultimate destination is needed to materialize the extender body.
  Callee *callee;

  // The insertion boundary connects the proposal to its owning text section.
  uint32_t boundaryIdx;
  ExtenderKind kind;

  // Only candidates referenced by a call or island chain are materialized.
  bool live = false;

  // The proposed or exact address is used for branch-range validation.
  uint64_t va = 0;

  // Materialization creates the input section that carries the machine code.
  ConcatInputSection *isec = nullptr;

  // Call relocations target this symbol after the proposal is accepted.
  Defined *sym = nullptr;
};

bool Callee::promoteCallsites() {
  std::optional<uint64_t> directTarget = getVA();
  if (!directTarget)
    return false;

  bool promoted = false;
  for (auto &callsite : callsites)
    if (!callsite.extender && !inBranchRange(callsite.getVA(), *directTarget)) {
      callsite.forceExtender = true;
      promoted = true;
    }
  return promoted;
}

void Callee::redirectCallsites() {
  std::optional<uint64_t> directTarget = getVA();
  for (auto &callsite : callsites)
    if (callsite.extender &&
        (!directTarget || !inBranchRange(callsite.getVA(), *directTarget))) {
      callsite.reloc->referent = callsite.extender->sym;
      callsite.reloc->addend = 0;
    }
}

using CalleeKey = std::tuple<const void *, uint64_t, int64_t>;
static CalleeKey makeCalleeKey(Symbol *sym, int64_t addend) {
  if (const auto *defined = dyn_cast<Defined>(sym);
      defined && !needsBinding(sym))
    return {defined->isec(), defined->value + addend, 0};
  return {sym, 0, addend};
}
} // namespace

namespace lld::macho {
class TextOutputSection::Finalizer {
public:
  explicit Finalizer(TextOutputSection &first)
      : first(first), maxHops(config->branchRangeExtensionMaxHops),
        chainKind(target->getChainExtenderKind()),
        fallbackKind(target->getFallbackExtenderKind()) {
    collect();
  }
  void run();

private:
  // The first text output section identifies the contiguous code group being
  // finalized.
  TextOutputSection &first;

  // Maximum number of island hops before falling back to a thunk.
  const uint32_t maxHops;

  // Extender roles are target-defined; the planner only owns the fallback rule.
  const ExtenderKind chainKind;
  const ExtenderKind fallbackKind;

  // Boundaries hold the insertion points and per-pass layout state.
  SmallVector<Boundary, 0> boundaries;

  // Owners are the consecutive text output sections laid out as one group.
  SmallVector<TextOutputSection *, 4> owners;

  // Callees group branches that can share extender chains.
  SmallVector<Callee *, 0> callees;

  // Extenders are the active proposal, later filtered to live entries.
  SmallVector<Extender *, 0> extenders;

  // Sparse placement candidates spread islands through large inputs without
  // considering every boundary for every hop.
  SmallVector<uint32_t, 0> placementBoundaries;

  // Cache post-text section address estimates during a proposed layout.
  DenseMap<OutputSection *, uint64_t> estimatedPostTextSectionVAs;

  // Later output-section estimates begin at the end of the code group.
  uint64_t textEndVA = 0;

  // Until exact layout rejects a direct call, forced-callsite scans are empty.
  bool hasForcedCallsites = false;
  std::string name() const { return "maxHops=" + std::to_string(maxHops); }
  void collect();
  std::optional<uint64_t> estimatePostTextSectionVA(OutputSection *);
  enum class LayoutKind {
    reservation, // Use padding retained from rejected proposals.
    proposal,    // Insert the active proposal without mutating sections.
    finalizable, // Finalize sections with the accepted proposal.
  };
  void walkLayout(LayoutKind);
  // Return the first boundary at `va` or later, or strictly later than `va`.
  uint32_t lowerBoundaryBound(uint64_t) const;
  uint32_t upperBoundaryBound(uint64_t) const;
  enum class BoundaryPreference { lowest, highest };
  std::optional<uint32_t> findBoundary(uint64_t, uint64_t,
                                       BoundaryPreference) const;
  // Keep representative insertion points about one 1 MiB apart. This prevents
  // island chains from concentrating at the edge of their branch window.
  void initializePlacementBoundaries();
  std::optional<uint32_t> findChainBoundary(uint64_t, uint64_t) const;
  Extender *insertExtender(Callee &, uint32_t, ExtenderKind);
  Extender *placeFallbackExtender(SmallVectorImpl<Extender *> &, Callee &,
                                  uint64_t);
  void planSide(Callee &, MutableArrayRef<Callsite>,
                SmallVectorImpl<Extender *> &);
  void planCallee(Callee &);
  void plan();
  bool update();
  // Extenders are boundary-ordered. Walking each side from the target outward
  // makes the previously visited island the current island's next branch hop,
  // so the extender need not store a redundant extender-to-extender edge.
  template <typename Visitor> bool forEachIslandEdge(Visitor);
  bool validate();
  void materialize();
};

void TextOutputSection::Finalizer::collect() {
  const auto &sections = first.parent->getSections();
  auto firstIt = llvm::find(sections, &first);
  for (auto *osec : llvm::make_range(firstIt, sections.end())) {
    auto *text = dyn_cast<TextOutputSection>(osec);
    if (!text ||
        !sections::isCodeSection(text->name, first.parent->name, text->flags))
      break;
    owners.push_back(text);
    for (auto *isec : text->inputs)
      boundaries.push_back({isec});
  }

  DenseMap<ConcatInputSection *, Boundary *> inputBoundaries;
  for (auto &boundary : boundaries)
    inputBoundaries[boundary.isec] = &boundary;

  // Cache the boundary for a locally defined branch target.
  auto cacheTargetInput = [&](Callee &callee, Symbol *sym, int64_t addend) {
    // A branch to an interior offset relies on the locally defined function's
    // layout and therefore cannot be redirected through an interposable stub.
    if (needsBinding(sym) && addend == 0)
      return;
    auto *defined = dyn_cast<Defined>(sym);
    if (!defined)
      return;
    auto *targetIsec = dyn_cast_or_null<ConcatInputSection>(defined->isec());
    if (!targetIsec)
      return;
    auto it = inputBoundaries.find(targetIsec);
    if (it != inputBoundaries.end())
      callee.targetBoundary = it->second;
  };

  DenseMap<CalleeKey, Callee *> calleeByKey;
  // Group a branch relocation with calls to the same effective target.
  auto addCallsite = [&](Callsite &&callsite) {
    const auto &reloc = *callsite.reloc;
    Symbol *sym = cast<Symbol *>(reloc.referent);
    auto &callee = calleeByKey[makeCalleeKey(sym, reloc.addend)];
    if (!callee) {
      callee = make<Callee>();
      callees.push_back(callee);
      cacheTargetInput(*callee, sym, reloc.addend);
    }
    callee->callsites.push_back(std::move(callsite));
  };

  // Collect each boundary's branch relocations
  for (auto &boundary : boundaries)
    for (auto &reloc : llvm::reverse(boundary.isec->relocs))
      if (target->hasAttr(reloc.type, RelocAttrBits::BRANCH))
        addCallsite({&reloc, &boundary});

  // normalize the callsite in ascending order.
  for (auto *callee : callees)
    if (!llvm::is_sorted(callee->callsites))
      llvm::sort(callee->callsites);
}

std::optional<uint64_t> TextOutputSection::Finalizer::estimatePostTextSectionVA(
    OutputSection *targetSection) {
  if (!targetSection || !targetSection->isNeeded())
    return std::nullopt;
  if (auto it = estimatedPostTextSectionVAs.find(targetSection);
      it != estimatedPostTextSectionVAs.end())
    return it->second;

  const auto &sections = first.parent->getSections();

  uint64_t va = textEndVA;
  auto lastOwnerIt = llvm::find(sections, owners.back());
  assert(lastOwnerIt != sections.end());
  // Walk post-text sections to estimate the target's aligned VA.
  for (auto *osec : llvm::make_range(std::next(lastOwnerIt), sections.end())) {
    if (!osec->isNeeded())
      continue;

    va = alignToPowerOf2(va, osec->align);
    if (osec == targetSection) {
      estimatedPostTextSectionVAs[targetSection] = va;
      return va;
    }

    uint64_t size = osec->getSize();
    if (auto *concat = dyn_cast<ConcatOutputSection>(osec)) {
      size = llvm::accumulate(
          concat->inputs, uint64_t{0},
          [](uint64_t size, const ConcatInputSection *isec) {
            return alignToPowerOf2(size, isec->align) + isec->getSize();
          });
    }
    va += size;
  }
  llvm_unreachable("needed post-text section not found");
}

void TextOutputSection::Finalizer::walkLayout(LayoutKind kind) {
  const bool finalizing = kind == LayoutKind::finalizable;
  const uint64_t groupVA = first.addr;
  uint64_t groupSize = 0;
  uint32_t boundaryIdx = 0;
  // Extenders are boundary-ordered, so this cursor visits each exactly once.
  size_t extenderIdx = 0;

  for (auto *owner : owners) {
    groupSize = alignToPowerOf2(groupSize, owner->align);
    const uint64_t sectionVA = groupVA + groupSize;
    uint64_t sectionSize = 0;

    if (finalizing) {
      owner->addr = sectionVA;
      owner->size = owner->fileSize = 0;
    }

    // Allocate one aligned contribution within the current output section.
    auto allocate = [&](uint64_t size, uint32_t align) {
      sectionSize = alignToPowerOf2(sectionSize, align);
      uint64_t va = sectionVA + sectionSize;
      sectionSize += size;
      return va;
    };

    auto finalizeAt = [&](ConcatInputSection *isec, uint64_t expectedVA) {
      if (!finalizing)
        return;
      owner->finalizeOne(isec);
      assert(isec->getVA() == expectedVA);
    };

    for (auto *isec : owner->inputs) {
      Boundary &boundary = boundaries[boundaryIdx];
      boundary.inputVA = allocate(isec->getSize(), isec->align);
      finalizeAt(isec, boundary.inputVA);

      switch (kind) {
      case LayoutKind::reservation: 
        if (boundary.reserved) // Grow `sectionSize` so reservation shift next isec.
          allocate(boundary.reserved, 4);
        break;
      case LayoutKind::proposal:
        [[fallthrough]];
      case LayoutKind::finalizable:
        while (extenderIdx < extenders.size() &&
               extenders[extenderIdx]->boundaryIdx == boundaryIdx) {
          Extender &extender = *extenders[extenderIdx++];
          extender.va = allocate(extender.size(), extender.align());
          finalizeAt(extender.isec, extender.va);
          if (finalizing)
            owner->thunks.push_back(extender.isec);
        }
        break;
      }
      ++boundaryIdx;
    }

    groupSize += sectionSize;
    if (finalizing)
      owner->branchRangeExtensionFinalized = true;
  }

  assert(boundaryIdx == boundaries.size());
  assert(kind == LayoutKind::reservation || extenderIdx == extenders.size());
  textEndVA = groupVA + groupSize;
  estimatedPostTextSectionVAs.clear();
  if (!finalizing)
    for (auto *callee : callees)
      callee->resolveVA(
          [&](OutputSection *osec) { return estimatePostTextSectionVA(osec); });
}

uint32_t TextOutputSection::Finalizer::lowerBoundaryBound(uint64_t va) const {
  auto isBeforeVA = [va](const Boundary &boundary) {
    return boundary.getVA() < va;
  };
  return llvm::partition_point(boundaries, isBeforeVA) - boundaries.begin();
}

uint32_t TextOutputSection::Finalizer::upperBoundaryBound(uint64_t va) const {
  auto isNotAfterVA = [va](const Boundary &boundary) {
    return boundary.getVA() <= va;
  };
  return llvm::partition_point(boundaries, isNotAfterVA) - boundaries.begin();
}

std::optional<uint32_t> TextOutputSection::Finalizer::findBoundary(
    uint64_t lowVA, uint64_t highVA, BoundaryPreference preference) const {
  if (lowVA > highVA)
    return std::nullopt;

  bool ascending = preference == BoundaryPreference::lowest;
  auto isUsable = [&](uint32_t idx) {
    uint64_t va = boundaries[idx].getExtenderVA();
    return lowVA <= va && va <= highVA;
  };

  auto coarseBegin = llvm::lower_bound(
      placementBoundaries, lowVA,
      [&](uint32_t idx, uint64_t va) { return boundaries[idx].getVA() < va; });
  auto coarseEnd = llvm::upper_bound(
      placementBoundaries, highVA,
      [&](uint64_t va, uint32_t idx) { return va < boundaries[idx].getVA(); });
  if (auto idx = findInDirection(llvm::make_range(coarseBegin, coarseEnd),
                                 ascending, isUsable))
    return idx;

  uint32_t begin = lowerBoundaryBound(lowVA);
  uint32_t end = upperBoundaryBound(highVA);
  return findInDirection(llvm::seq(begin, end), ascending, isUsable);
}

void TextOutputSection::Finalizer::initializePlacementBoundaries() {
  if (!placementBoundaries.empty() || boundaries.empty())
    return;

  placementBoundaries.push_back(0);
  uint64_t lastVA = boundaries.front().getVA();
  for (auto [idx, boundary] : llvm::enumerate(boundaries)) {
    if (idx == 0 || idx + 1 == boundaries.size())
      continue;
    uint64_t va = boundary.getVA();
    if (va - lastVA < boundarySpacing)
      continue;
    placementBoundaries.push_back(idx);
    lastVA = va;
  }
  if (boundaries.size() > 1)
    placementBoundaries.push_back(boundaries.size() - 1);
}

std::optional<uint32_t>
TextOutputSection::Finalizer::findChainBoundary(uint64_t callVA,
                                                uint64_t anchorVA) const {
  if (callVA == anchorVA)
    return std::nullopt;
  bool upper = callVA > anchorVA;
  uint64_t low = addSat(anchorVA, 1);
  uint64_t high =
      std::min(callVA - 1, addSat(anchorVA, target->backwardBranchRange));
  if (!upper) {
    low = std::max(addSat(callVA, 1),
                   subSat(anchorVA, target->forwardBranchRange));
    high = anchorVA - 1;
  }
  return findBoundary(low, high,
                      upper ? BoundaryPreference::highest
                            : BoundaryPreference::lowest);
}

Extender *TextOutputSection::Finalizer::insertExtender(Callee &callee,
                                                       uint32_t boundaryIdx,
                                                       ExtenderKind kind) {
  Boundary &boundary = boundaries[boundaryIdx];
  Extender *extender =
      make<Extender>(callee, boundaryIdx, kind, boundary.getExtenderVA());
  boundary.planned += extender->size();
  extenders.push_back(extender);
  return extender;
}

Extender *
TextOutputSection::Finalizer::placeFallbackExtender(
    SmallVectorImpl<Extender *> &fallbacks, Callee &callee, uint64_t callVA) {
  if (auto it = llvm::find_if(fallbacks,
                              [callVA](const Extender *fallback) {
                                return inBranchRange(callVA, fallback->va);
                              });
      it != fallbacks.end())
    return *it; // reuse an existing in-range fallback extender

  // Place the fallback at the inward edge of the callsite's branch window. This
  // maximizes the range that later, nearer callsites can share.
  uint64_t low = subSat(callVA, target->backwardBranchRange);
  uint64_t high = addSat(callVA, target->forwardBranchRange);
  auto targetVA = callee.getVA();
  bool upper = targetVA && callVA >= *targetVA;
  std::optional<uint32_t> boundaryIdx = findBoundary(
      low, high,
      upper ? BoundaryPreference::lowest : BoundaryPreference::highest);
  if (!boundaryIdx)
    fatal("cannot place branch fallback extender for " +
          toString(*callee.target()));
  Extender *fallback = insertExtender(callee, *boundaryIdx, fallbackKind);
  fallbacks.push_back(fallback);
  return fallback;
}

// Plan the callsites on one side of the callee, from the furthest inward.
void TextOutputSection::Finalizer::planSide(
    Callee &callee, MutableArrayRef<Callsite> side,
    SmallVectorImpl<Extender *> &fallbacks) {
  if (side.empty())
    return;
  uint64_t targetVA = *callee.getVA();
  bool upper = side.front().getVA() >= targetVA;
  Callsite *furthest = upper ? &side.back() : &side.front();

  if (inBranchRange(furthest->getVA(), targetVA)) {
    if (!hasForcedCallsites)
      return; // Furthest is range.
    // Otherwise, the furthest should be the one forcing an extender.
    auto outward = llvm::reverse_conditionally(side, upper);
    auto it = llvm::find_if(outward,
                            [](const Callsite &c) { return c.forceExtender; });
    if (it == outward.end())
      return;
    furthest = &*it;
  }

  SmallVector<Extender *, 4> chain;
  uint64_t callVA = furthest->getVA();
  uint64_t anchorVA = targetVA;
  bool forceExtender = furthest->forceExtender;
  // Drive chain growth until `anchorVA` is reachable or maxHops is reached.
  for (uint32_t depth = 0;
       depth < maxHops && (forceExtender || !inBranchRange(callVA, anchorVA));
       ++depth) {
    auto boundaryIdx = findChainBoundary(callVA, anchorVA);
    if (!boundaryIdx)
      break;
    Extender *extender = insertExtender(callee, *boundaryIdx, chainKind);
    chain.push_back(extender);
    anchorVA = extender->va;
    forceExtender = false;
  }
  for (auto &callsite : llvm::reverse_conditionally(side, upper)) {
    uint64_t callVA = callsite.getVA();
    if (!callsite.forceExtender && inBranchRange(callVA, targetVA))
      continue;
    uint64_t bestDistance = UINT64_MAX;
    for (auto *extender : chain) {
      uint64_t distance =
          callVA > extender->va ? callVA - extender->va : extender->va - callVA;
      if (distance < bestDistance && inBranchRange(callVA, extender->va)) {
        callsite.extender = extender;
        bestDistance = distance;
      }
    }
    if (!callsite.extender) // no chain extender is usable.
      callsite.extender = placeFallbackExtender(fallbacks, callee, callVA);
    if (!callsite.extender) // Failed to create a fallback extender.
      fatal("cannot route branch to " + toString(*callee.target()));
    callsite.extender->live = true;
    if (callsite.extender->kind == chainKind) // Inherit liveness of the chain.
      for (auto *extender : chain) {
        extender->live = true;
        if (extender == callsite.extender)
          break;
      }
  }
}

void TextOutputSection::Finalizer::planCallee(Callee &callee) {
  auto target = callee.getVA();
  if (!target) {
    if (callee.isDtrace())
      return;
    SmallVector<Extender *, 4> fallbacks;
    for (auto &callsite : callee.callsites) {
      callsite.extender =
          placeFallbackExtender(fallbacks, callee, callsite.getVA());
      if (callsite.extender)
        callsite.extender->live = true;
    }
    return;
  }
  // Split callsites into the address ranges below and above the target.
  auto split = llvm::lower_bound(callee.callsites, *target,
                                 [&](const Callsite &callsite, uint64_t va) {
                                   return callsite.getVA() < va;
                                 });
  MutableArrayRef<Callsite> calls(callee.callsites);
  size_t splitIdx = split - callee.callsites.begin();
  SmallVector<Extender *, 4> fallbacks;
  planSide(callee, calls.take_front(splitIdx), fallbacks);
  planSide(callee, calls.drop_front(splitIdx), fallbacks);
}

void TextOutputSection::Finalizer::plan() {
  extenders.clear();
  for (auto &boundary : boundaries)
    boundary.planned = 0;
  for (auto *callee : callees)
    for (auto &callsite : callee->callsites)
      callsite.extender = nullptr;
  walkLayout(LayoutKind::reservation);
  initializePlacementBoundaries();
  for (auto *callee : callees)
    planCallee(*callee);
  llvm::erase_if(extenders, [&](Extender *extender) {
    if (extender->live)
      return false;
    boundaries[extender->boundaryIdx].planned -= extender->size();
    return true;
  });
  llvm::stable_sort(extenders, [](Extender *a, Extender *b) {
    return a->boundaryIdx < b->boundaryIdx;
  });
}

template <typename Visitor>
bool TextOutputSection::Finalizer::forEachIslandEdge(Visitor visitor) {
  bool valid = true;
  DenseMap<Callee *, Extender *> inward;
  // Derive an island's next inward hop on the selected target side.
  auto visit = [&](Extender &extender, bool upper) {
    if (extender.kind != chainKind)
      return;
    auto target = extender.callee->getVA();
    uint64_t islandVA = extender.va;
    if (islandVA == *target) {
      valid = false;
      return;
    }
    if ((islandVA > *target) != upper) // check position relativity
      return;
    Extender *nextInward = std::exchange(inward[extender.callee], &extender);
    valid &= visitor(extender, nextInward);
  };
  for (auto *extender : extenders)
    visit(*extender, true);
  inward.clear();
  for (auto *extender : llvm::reverse(extenders))
    visit(*extender, false);
  return valid;
}

bool TextOutputSection::Finalizer::validate() {
  // Every island should be in range.
  bool valid = forEachIslandEdge([&](Extender &extender, Extender *inward) {
    uint64_t targetVA = *extender.callee->getVA();
    return inBranchRange(extender.va, inward ? inward->va : targetVA);
  });
  // Every callsite should be reachable to its extender.
  for (auto *callee : callees) {
    auto target = callee->getVA();
    for (const auto &callsite : callee->callsites) {
      uint64_t callVA = callsite.getVA();
      if (callsite.extender) // An extended call
        valid &= inBranchRange(callVA, callsite.extender->va);
      else if (callsite.forceExtender)
        valid = false;  // A forced call without extender
      else if (!target) // DTrace calls
        valid &= callee->isDtrace();
      else // A direct call
        valid &= inBranchRange(callVA, *target);
    }
  }
  return valid;
}

void TextOutputSection::Finalizer::materialize() {
  assert(
      llvm::all_of(extenders,
                   [](const Extender *extender) { return extender->live; }) &&
      "only live extenders may be materialized");
  DenseMap<Callee *, std::pair<size_t, size_t>> sequences;
  // Create the synthetic input section and symbol for one live extender.
  auto create = [&](Extender &extender, size_t sequence) {
    Callee &callee = *extender.callee;
    ConcatInputSection *boundary = boundaries[extender.boundaryIdx].isec;
    extender.isec =
        makeSyntheticInputSection(boundary->getSegName(), boundary->getName());
    extender.isec->parent = boundary->parent;
    StringRef kind = target->getExtenderSuffix(extender.kind);
    std::string addendSuffix;
    if (callee.addend() != 0)
      addendSuffix = (callee.addend() > 0 ? "+" : "") +
                     std::to_string(callee.addend());
    StringRef name = saver().save(callee.target()->getName() + addendSuffix +
                                  kind + std::to_string(sequence));
    size_t size = extender.size();
    if (!isa<Defined>(callee.target()) ||
        cast<Defined>(callee.target())->isExternal())
      extender.sym = symtab->addDefined(
          name, /*file=*/nullptr, extender.isec, /*value=*/0, /*size=*/size,
          /*isWeakDef=*/false, /*isPrivateExtern=*/true,
          /*isReferencedDynamically=*/false, /*noDeadStrip=*/false,
          /*isWeakDefCanBeHidden=*/false);
    else
      extender.sym = make<Defined>(
          name, /*file=*/nullptr, extender.isec, /*value=*/0, /*size=*/size,
          /*isWeakDef=*/false, /*isExternal=*/false,
          /*isPrivateExtern=*/true, /*includeInSymtab=*/true,
          /*isReferencedDynamically=*/false, /*noDeadStrip=*/false,
          /*canOverrideWeakDef=*/false);
    extender.sym->used = true;
  };
  forEachIslandEdge([&](Extender &extender, Extender *inward) {
    Callee &callee = *extender.callee;
    create(extender, sequences[&callee].first++);
    target->populateExtender(extender.isec, extender.kind,
                             inward ? inward->sym : callee.target(),
                             inward ? 0 : callee.addend());
    return true;
  });
  for (auto *extender : extenders)
    if (extender->kind == fallbackKind) {
      Callee &callee = *extender->callee;
      create(*extender, sequences[&callee].second++);
      if (needsBinding(callee.target()))
        assert(callee.target()->isInStubs() &&
               "stub should have been inserted before finalization");
      target->populateExtender(extender->isec, extender->kind, callee.target(),
                               callee.addend());
    }
  // Direct callsite to its extender
  for (auto *callee : callees)
    callee->redirectCallsites();
}

bool TextOutputSection::Finalizer::update() {
  bool updated = false;
  for (auto &boundary : boundaries)
    if (uint32_t desired = boundary.getExtenderVA() - boundary.getVA();
        boundary.reserved < desired) {
      boundary.reserved = desired;
      updated = true;
    }
  if (updated)
    return true;

  bool promoted = false;
  for (auto *callee : callees)
    promoted |= callee->promoteCallsites();
  if (promoted) {
    hasForcedCallsites = true;
    log(name() + " branch extender promoted direct calls");
  }
  return promoted;
}

void TextOutputSection::Finalizer::run() {
  std::string policyName = name();
  TimeTraceScope timeScope("Branch extender", policyName);
  log("finalization mode for " + first.parent->name + "," + first.name + ": " +
      policyName);

  size_t pass = 1;
  for (; pass <= 30; ++pass) {
    plan();
    walkLayout(LayoutKind::proposal);
    if (validate())
      break;
    log(policyName + " branch extender rejected proposal");
    if (!update())
      pass = 30;
  }
  if (pass > 30)
    fatal(policyName + " branch extender did not converge");
  materialize();
  walkLayout(LayoutKind::finalizable);
  log(policyName + " branch extender for " + first.parent->name + "," +
      first.name + ": passes = " + std::to_string(pass) +
      ", inputs = " + std::to_string(boundaries.size()) +
      ", targets = " + std::to_string(callees.size()) +
      ", total extenders = " + std::to_string(extenders.size()));
}

void TextOutputSection::finalize() {
  if (branchRangeExtensionFinalized)
    return;
  if (target->usesExtenders() &&
      sections::isCodeSection(name, parent->name, flags)) {
    Finalizer finalizer(*this);
    finalizer.run();
    return;
  }
  for (auto *isec : inputs)
    finalizeOne(isec);
}
} // namespace lld::macho

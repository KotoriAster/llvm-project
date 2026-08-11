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

struct Boundary;
struct Extender;
struct Callsite {
  // Branch relocation rewritten to the selected extender after materialization.
  Relocation *reloc;

  // Non-owning layout record for the input section that owns `reloc`.
  Boundary *layoutBoundary;

  // First branch hop, later used to redirect the materialized relocation.
  Extender *extender = nullptr;

  // Exact layout may invalidate a direct call that planning must then route.
  bool forceExtender = false;
};

struct Callee {
  // Index of the input hosting a local target; UINT32_MAX for nonlocal targets.
  uint32_t targetInputIdx = UINT32_MAX;

  // Call sites sharing this effective branch target.
  SmallVector<Callsite, 1> callsites;

  auto target() const { return cast<Symbol *>(callsites[0].reloc->referent); }
  int64_t addend() const { return callsites.front().reloc->addend; }
  bool isDtrace() const {
    return target()->getName().starts_with("___dtrace_");
  }
};

struct Boundary {
  // Borrowed input section whose size and metadata define this insertion point;
  // input sections outlive finalization.
  ConcatInputSection *isec;

  // Extender contribution retained from rejected proposals for convergence.
  uint32_t reserved = 0;

  // Extender contribution from the active proposal at this boundary.
  uint32_t planned = 0;

  // The current layout's input address anchors callers and insertion points.
  uint64_t va = 0;
};

struct Extender {
  Extender(Callee &callee, uint32_t boundaryIdx, bool isThunk, uint64_t va)
      : callee(&callee), boundaryIdx(boundaryIdx), isThunk(isThunk), va(va) {}

  // The ultimate destination is needed to materialize the extender body.
  Callee *callee;

  // The insertion boundary connects the proposal to its owning text section.
  uint32_t boundaryIdx;

  // Thunks and islands use different encodings and routing rules.
  bool isThunk;

  // Only candidates referenced by a call or island chain are materialized.
  bool live = false;

  // The proposed or exact address is used for branch-range validation.
  uint64_t va = 0;

  // Materialization creates the input section that carries the machine code.
  ConcatInputSection *isec = nullptr;

  // Call relocations target this symbol after the proposal is accepted.
  Defined *sym = nullptr;
};

using CalleeKey = std::tuple<const void *, uint64_t, int64_t>;
static CalleeKey makeCalleeKey(Symbol *sym, int64_t addend) {
  if (const auto *defined = dyn_cast<Defined>(sym);
      defined && !needsBinding(sym))
    return {defined->isec(), defined->value + addend, 0};
  return {sym, 0, addend};
}
} // namespace

namespace lld::macho {
class Finalizer {
public:
  Finalizer(TextOutputSection &first, BranchRangeExtensionMode mode)
      : first(first),
        islandOnly(mode == BranchRangeExtensionMode::islandsSlopFree) {
    collect();
  }
  void run();

private:
  // The first text output section identifies the contiguous code group being
  // finalized.
  TextOutputSection &first;

  // Island-only mode forbids thunk fallback when routing long branches.
  bool islandOnly;

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

  // Synthetic target sections share cached address estimates during a layout.
  DenseMap<OutputSection *, uint64_t> estimatedSectionVAs;

  // Later output-section estimates begin at the end of the code group.
  uint64_t textEndVA = 0;

  // Until exact layout rejects a direct call, forced-callsite scans are empty.
  bool hasForcedCallsites = false;
  StringRef name() const { return islandOnly ? "islands-slop-free" : "hybrid"; }
  size_t extenderSize(const Extender &e) const {
    return e.isThunk ? target->thunkSize : target->islandSize;
  }
  void collect();
  std::optional<uint64_t> estimateSectionVA(OutputSection *);
  std::optional<uint64_t> targetVA(const Callee &);
  enum class LayoutKind {
    reservation, // Use padding retained from rejected proposals.
    proposal,    // Insert the active proposal without mutating sections.
    commit,      // Finalize sections with the accepted proposal.
  };
  void walkLayout(LayoutKind);
  uint64_t boundaryBaseVA(uint32_t idx) const {
    return alignToPowerOf2(boundaries[idx].va + boundaries[idx].isec->getSize(),
                           4);
  }
  uint64_t proposedExtenderVA(uint32_t) const;
  uint32_t boundaryBound(uint64_t, bool) const;
  void initializePlacementBoundaries();
  uint64_t callsiteVA(const Callsite &c) const {
    return c.layoutBoundary->va + c.reloc->offset;
  }
  std::optional<uint32_t> findIslandBoundary(uint64_t, uint64_t) const;
  Extender *createExtender(Callee &, uint32_t, bool);
  Extender *placeThunk(SmallVectorImpl<Extender *> &, Callee &, uint64_t);
  void planSide(Callee &, MutableArrayRef<Callsite>, bool,
                SmallVectorImpl<Extender *> &, uint64_t);
  void planCallee(Callee &);
  void plan();
  template <typename Visitor> bool forEachIslandEdge(Visitor);
  bool validate();
  void materialize();
};

void Finalizer::collect() {
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

  DenseMap<ConcatInputSection *, uint32_t> inputIndexes;
  for (auto [idx, boundary] : llvm::enumerate(boundaries))
    inputIndexes[boundary.isec] = idx;

  auto cacheTargetInput = [&](Callee &callee, Symbol *sym) {
    if (needsBinding(sym))
      return;
    auto *defined = dyn_cast<Defined>(sym);
    if (!defined)
      return;
    auto *targetIsec = dyn_cast_or_null<ConcatInputSection>(defined->isec());
    if (!targetIsec)
      return;
    auto it = inputIndexes.find(targetIsec);
    if (it != inputIndexes.end())
      callee.targetInputIdx = it->second;
  };

  DenseMap<CalleeKey, Callee *> calleeByKey;
  auto addCallsite = [&](Relocation &reloc, Boundary &input) {
    Symbol *sym = cast<Symbol *>(reloc.referent);
    bool binds = needsBinding(sym);
    if (islandOnly && binds)
      in.stubs->addEntry(sym);
    auto &callee = calleeByKey[makeCalleeKey(sym, reloc.addend)];
    if (!callee) {
      callee = make<Callee>();
      callees.push_back(callee);
      cacheTargetInput(*callee, sym);
    }
    callee->callsites.push_back({&reloc, &input});
  };

  for (auto &boundary : boundaries) {
    auto &relocs = boundary.isec->relocs;
    if (!llvm::is_sorted(relocs, [](const Relocation &a, const Relocation &b) {
          return a.offset > b.offset;
        })) {
      SmallVector<Relocation *, 0> branches;
      for (auto &reloc : relocs)
        if (target->hasAttr(reloc.type, RelocAttrBits::BRANCH))
          branches.push_back(&reloc);
      llvm::sort(branches, [](const Relocation *a, const Relocation *b) {
        return a->offset < b->offset;
      });
      for (auto *reloc : branches)
        addCallsite(*reloc, boundary);
      continue;
    }

    for (auto &reloc : llvm::reverse(relocs))
      if (target->hasAttr(reloc.type, RelocAttrBits::BRANCH))
        addCallsite(reloc, boundary);
  }
}

std::optional<uint64_t>
Finalizer::estimateSectionVA(OutputSection *targetSection) {
  if (!targetSection || !targetSection->isNeeded())
    return std::nullopt;
  if (auto it = estimatedSectionVAs.find(targetSection);
      it != estimatedSectionVAs.end())
    return it->second;

  uint64_t va = textEndVA;
  const auto &sections = first.parent->getSections();
  auto begin = std::next(llvm::find(sections, owners.back()));
  for (auto *osec : llvm::make_range(begin, sections.end())) {
    if (!osec->isNeeded())
      continue;
    va = alignToPowerOf2(va, osec->align);
    if (osec == targetSection) {
      estimatedSectionVAs[osec] = va;
      return va;
    }
    if (auto *concat = dyn_cast<ConcatOutputSection>(osec)) {
      uint64_t size = llvm::accumulate(
          concat->inputs, uint64_t(0), [](uint64_t size, auto *isec) {
            size = alignToPowerOf2(size, isec->align);
            return size + isec->getSize();
          });
      va += size;
    } else
      va += osec->getSize();
  }
  return std::nullopt;
}

std::optional<uint64_t> Finalizer::targetVA(const Callee &callee) {
  Symbol *sym = callee.target();
  int64_t addend = callee.addend();
  if (sym->isInStubs()) {
    if (in.stubs->isFinal)
      return sym->getStubVA() + addend;
    if (auto va = estimateSectionVA(in.stubs))
      return *va + uint64_t(sym->stubsIndex) * target->stubSize + addend;
  }
  auto *defined = dyn_cast<Defined>(sym);
  bool targetsObjCStubs =
      defined && in.objcStubs && defined->isec() == in.objcStubs->isec;
  if (targetsObjCStubs && in.objcStubs->isNeeded())
    if (auto va = estimateSectionVA(in.objcStubs))
      return *va + defined->value + addend;
  if (defined && defined->isAbsolute())
    return defined->getVA() + addend;
  if (callee.targetInputIdx == UINT32_MAX)
    return std::nullopt;
  return boundaries[callee.targetInputIdx].va + defined->value + addend;
}

void Finalizer::walkLayout(LayoutKind kind) {
  bool useProposal = kind != LayoutKind::reservation;
  bool commit = kind == LayoutKind::commit;
  uint64_t addr = first.addr;
  uint64_t groupSize = 0;
  uint32_t inputIdx = 0;
  size_t extenderIdx = 0;
  for (auto *owner : owners) {
    groupSize = alignToPowerOf2(groupSize, owner->align);
    uint64_t ownerBase = groupSize;
    uint64_t ownerSize = 0;
    if (commit) {
      owner->addr = owner->hybridAddr = addr + ownerBase;
      owner->size = owner->fileSize = 0;
    }
    for (auto *isec : owner->inputs) {
      ownerSize = alignToPowerOf2(ownerSize, isec->align);
      boundaries[inputIdx].va = addr + ownerBase + ownerSize;
      ownerSize += isec->getSize();
      if (commit) {
        owner->finalizeOne(isec);
        assert(isec->getVA() == boundaries[inputIdx].va);
      }
      if (useProposal)
        while (extenderIdx < extenders.size() &&
               extenders[extenderIdx]->boundaryIdx == inputIdx) {
          ownerSize = alignToPowerOf2(ownerSize, 4);
          Extender &extender = *extenders[extenderIdx++];
          extender.va = addr + ownerBase + ownerSize;
          ownerSize += extenderSize(extender);
          if (commit) {
            owner->finalizeOne(extender.isec);
            assert(extender.isec->getVA() == extender.va);
            owner->thunks.push_back(extender.isec);
          }
        }
      else if (boundaries[inputIdx].reserved)
        ownerSize =
            alignToPowerOf2(ownerSize, 4) + boundaries[inputIdx].reserved;
      groupSize = ownerBase + ownerSize;
      ++inputIdx;
    }
    if (commit)
      owner->hybridFinalized = true;
  }
  textEndVA = addr + groupSize;
  estimatedSectionVAs.clear();
}

// Return the first byte available after extenders already assigned to this
// boundary, so a proposal cannot overlap an earlier candidate.
uint64_t Finalizer::proposedExtenderVA(uint32_t boundaryIdx) const {
  return boundaryBaseVA(boundaryIdx) + boundaries[boundaryIdx].planned;
}

// Return the index of the first boundary at `va` or later. The upper form
// instead returns the first index whose boundary is strictly later than `va`.
uint32_t Finalizer::boundaryBound(uint64_t va, bool upper) const {
  va = addSat(va, upper);
  return llvm::partition_point(boundaries,
                               [&](const Boundary &boundary) {
                                 return boundaryBaseVA(&boundary -
                                                       boundaries.data()) < va;
                               }) -
         boundaries.begin();
}

// Keep representative insertion points about one MiB apart. This prevents
// island chains from concentrating at the edge of their branch window.
void Finalizer::initializePlacementBoundaries() {
  if (!placementBoundaries.empty() || boundaries.empty())
    return;

  placementBoundaries.push_back(0);
  uint64_t lastVA = boundaryBaseVA(0);
  for (auto [idx, boundary] : llvm::enumerate(boundaries)) {
    if (idx == 0 || idx + 1 == boundaries.size())
      continue;
    uint64_t va = boundaryBaseVA(idx);
    if (va - lastVA < boundarySpacing)
      continue;
    placementBoundaries.push_back(idx);
    lastVA = va;
  }
  if (boundaries.size() > 1)
    placementBoundaries.push_back(boundaries.size() - 1);
}

std::optional<uint32_t> Finalizer::findIslandBoundary(uint64_t callVA,
                                                      uint64_t anchorVA) const {
  if (callVA == anchorVA)
    return std::nullopt;
  bool forward = callVA < anchorVA;
  uint64_t low = addSat(anchorVA, 1);
  uint64_t high =
      std::min(callVA - 1, addSat(anchorVA, target->backwardBranchRange));
  if (forward) {
    low = std::max(addSat(callVA, 1),
                   subSat(anchorVA, target->forwardBranchRange));
    high = anchorVA - 1;
  }

  auto isUsable = [&](uint32_t idx) {
    uint64_t va = proposedExtenderVA(idx);
    bool liesBetween =
        forward ? callVA < va && va < anchorVA : anchorVA < va && va < callVA;
    return liesBetween && inBranchRange(va, anchorVA);
  };

  auto coarseBegin = llvm::lower_bound(
      placementBoundaries, low,
      [&](uint32_t idx, uint64_t va) { return boundaryBaseVA(idx) < va; });
  auto coarseEnd = llvm::upper_bound(
      placementBoundaries, high,
      [&](uint64_t va, uint32_t idx) { return va < boundaryBaseVA(idx); });
  if (auto idx = findInDirection(llvm::make_range(coarseBegin, coarseEnd),
                                 forward, isUsable))
    return idx;

  uint32_t begin = boundaryBound(low, false);
  uint32_t end = boundaryBound(high, true);
  return findInDirection(llvm::seq(begin, end), forward, isUsable);
}
Extender *Finalizer::createExtender(Callee &callee, uint32_t boundaryIdx,
                                    bool isThunk) {
  Extender *extender = make<Extender>(callee, boundaryIdx, isThunk,
                                      proposedExtenderVA(boundaryIdx));
  boundaries[boundaryIdx].planned += extenderSize(*extender);
  extenders.push_back(extender);
  return extender;
}
Extender *Finalizer::placeThunk(SmallVectorImpl<Extender *> &thunks,
                                Callee &callee, uint64_t callVA) {
  for (auto *thunk : thunks)
    if (inBranchRange(callVA, thunk->va))
      return thunk;
  uint32_t begin =
      boundaryBound(subSat(callVA, target->backwardBranchRange), false);
  uint32_t end =
      boundaryBound(addSat(callVA, target->forwardBranchRange), true);
  uint32_t left = boundaryBound(callVA, false);
  uint32_t right = left;
  auto nextBoundary = [&]() {
    if (left == begin)
      return right++;
    if (right == end)
      return --left;
    if (callVA - boundaryBaseVA(left - 1) < boundaryBaseVA(right) - callVA)
      return --left;
    return right++;
  };

  while (left != begin || right != end) {
    uint32_t idx = nextBoundary();
    if (inBranchRange(callVA, proposedExtenderVA(idx))) {
      Extender *thunk = createExtender(callee, idx, true);
      thunks.push_back(thunk);
      return thunk;
    }
  }
  fatal("cannot place branch thunk for " + toString(*callee.target()));
}

template <typename Fn>
static void forEachOutward(MutableArrayRef<Callsite> side, bool upper, Fn fn) {
  if (upper)
    for (auto &callsite : llvm::reverse(side))
      fn(callsite);
  else
    for (auto &callsite : side)
      fn(callsite);
}

void Finalizer::planSide(Callee &callee, MutableArrayRef<Callsite> side,
                         bool upper, SmallVectorImpl<Extender *> &thunks,
                         uint64_t targetVA) {
  Callsite *furthest = side.empty() ? nullptr : upper ? &side.back() : &side[0];
  if (furthest && !furthest->forceExtender &&
      inBranchRange(callsiteVA(*furthest), targetVA))
    furthest = nullptr;
  if (!furthest && hasForcedCallsites)
    forEachOutward(side, upper, [&](Callsite &callsite) {
      if (!furthest && callsite.forceExtender)
        furthest = &callsite;
    });
  SmallVector<Extender *, 4> islands;
  if (furthest) {
    uint64_t callVA = callsiteVA(*furthest);
    uint64_t anchorVA = targetVA;
    bool forced = furthest->forceExtender;
    for (uint32_t depth = 0; (islandOnly || depth != 2) &&
                             (forced || !inBranchRange(callVA, anchorVA));
         ++depth) {
      auto boundaryIdx = findIslandBoundary(callVA, anchorVA);
      if (!boundaryIdx)
        break;
      Extender *island = createExtender(callee, *boundaryIdx, false);
      islands.push_back(island);
      anchorVA = island->va;
      forced = false;
    }
  }
  forEachOutward(side, upper, [&](Callsite &callsite) {
    uint64_t callVA = callsiteVA(callsite);
    if (!callsite.forceExtender && inBranchRange(callVA, targetVA))
      return;
    uint64_t bestDistance = UINT64_MAX;
    for (auto *island : islands) {
      uint64_t distance =
          callVA > island->va ? callVA - island->va : island->va - callVA;
      if (distance < bestDistance && inBranchRange(callVA, island->va)) {
        callsite.extender = island;
        bestDistance = distance;
      }
    }
    if (!islandOnly)
      if (!callsite.extender)
        callsite.extender = placeThunk(thunks, callee, callVA);
    if (!callsite.extender)
      fatal("cannot route branch to " + toString(*callee.target()));
    callsite.extender->live = true;
    if (!callsite.extender->isThunk)
      for (auto *island : islands) {
        island->live = true;
        if (island == callsite.extender)
          break;
      }
  });
}

void Finalizer::planCallee(Callee &callee) {
  auto target = targetVA(callee);
  if (!target) {
    if (callee.isDtrace())
      return;
    if (islandOnly) {
      error("cannot resolve island-only branch target " +
            toString(*callee.target()));
      return;
    }
    SmallVector<Extender *, 4> thunks;
    for (auto &callsite : callee.callsites) {
      callsite.extender = placeThunk(thunks, callee, callsiteVA(callsite));
      if (callsite.extender)
        callsite.extender->live = true;
    }
    return;
  }
  auto split = llvm::lower_bound(callee.callsites, *target,
                                 [&](const Callsite &callsite, uint64_t va) {
                                   return callsiteVA(callsite) < va;
                                 });
  MutableArrayRef<Callsite> calls(callee.callsites);
  size_t splitIdx = split - callee.callsites.begin();
  SmallVector<Extender *, 4> thunks;
  planSide(callee, calls.take_front(splitIdx), false, thunks, *target);
  planSide(callee, calls.drop_front(splitIdx), true, thunks, *target);
}

void Finalizer::plan() {
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
  for (auto *extender : extenders)
    if (!extender->live)
      boundaries[extender->boundaryIdx].planned -= extenderSize(*extender);
  llvm::erase_if(extenders, [](Extender *extender) { return !extender->live; });
  llvm::stable_sort(extenders, [](Extender *a, Extender *b) {
    return a->boundaryIdx < b->boundaryIdx;
  });
}

// Extenders are boundary-ordered. Walking each side from the target outward
// makes the previously visited island the current island's next branch hop, so
// the extender does not need to store a redundant extender-to-extender edge.
template <typename Visitor> bool Finalizer::forEachIslandEdge(Visitor visitor) {
  bool valid = true;
  DenseMap<Callee *, Extender *> inward;
  auto visit = [&](Extender &extender, bool upper) {
    if (extender.isThunk)
      return;
    auto target = targetVA(*extender.callee);
    uint64_t islandVA = extender.va;
    if (islandVA == *target) {
      valid = false;
      return;
    }
    if ((islandVA > *target) != upper)
      return;
    Extender *nextInward = std::exchange(inward[extender.callee], &extender);
    valid &= visitor(extender, nextInward, *target);
  };
  for (auto *extender : extenders)
    visit(*extender, true);
  inward.clear();
  for (auto *extender : llvm::reverse(extenders))
    visit(*extender, false);
  return valid;
}

bool Finalizer::validate() {
  bool valid = forEachIslandEdge(
      [&](Extender &extender, Extender *inward, uint64_t targetVA) {
        return inBranchRange(extender.va, inward ? inward->va : targetVA);
      });
  for (auto *callee : callees) {
    auto target = targetVA(*callee);
    for (const auto &callsite : callee->callsites) {
      uint64_t callVA = callsiteVA(callsite);
      if (callsite.extender) {
        valid &= inBranchRange(callVA, callsite.extender->va);
      } else if (callsite.forceExtender)
        valid = false;
      else if (!target)
        valid &= callee->isDtrace();
      else
        valid &= inBranchRange(callVA, *target);
    }
  }
  return valid;
}

void Finalizer::materialize() {
  DenseMap<Symbol *, std::pair<size_t, size_t>> sequences;
  auto create = [&](Extender &extender, size_t sequence) {
    Callee &callee = *extender.callee;
    ConcatInputSection *boundary = boundaries[extender.boundaryIdx].isec;
    extender.isec =
        makeSyntheticInputSection(boundary->getSegName(), boundary->getName());
    extender.isec->parent = boundary->parent;
    StringRef kind = extender.isThunk ? ".thunk." : ".island.";
    StringRef name = saver().save(callee.target()->getName() + kind +
                                  std::to_string(sequence));
    size_t size = extenderSize(extender);
    if (!isa<Defined>(callee.target()) ||
        cast<Defined>(callee.target())->isExternal())
      extender.sym = symtab->addDefined(name, nullptr, extender.isec, 0, size,
                                        false, true, false, false, false);
    else
      extender.sym = make<Defined>(name, nullptr, extender.isec, 0, size, false,
                                   false, true, true, false, false, false);
    extender.sym->used = true;
  };
  forEachIslandEdge([&](Extender &extender, Extender *inward, uint64_t) {
    Callee &callee = *extender.callee;
    create(extender, sequences[callee.target()].first++);
    target->populateIsland(extender.isec,
                           inward ? inward->sym : callee.target());
    extender.isec->relocs[0].addend = inward ? 0 : callee.addend();
    return true;
  });
  for (auto *extender : extenders)
    if (extender->isThunk) {
      Callee &callee = *extender->callee;
      create(*extender, sequences[callee.target()].second++);
      if (needsBinding(callee.target()))
        in.stubs->addEntry(callee.target());
      target->populateThunk(extender->isec, callee.target(), callee.addend());
    }
}

void Finalizer::run() {
  TimeTraceScope timeScope("Branch extender", name());
  auto forEachCallsite = [&](auto visitor) {
    for (auto *callee : callees) {
      auto directTarget = targetVA(*callee);
      for (auto &callsite : callee->callsites)
        visitor(callsite, directTarget);
    }
  };

  size_t pass = 1;
  for (; pass <= 30; ++pass) {
    plan();
    walkLayout(LayoutKind::proposal);
    if (validate())
      break;
    log(name() + " branch extender rejected proposal");
    bool updated = false;
    for (auto [idx, boundary] : llvm::enumerate(boundaries))
      if (uint32_t desired = proposedExtenderVA(idx) - boundaryBaseVA(idx);
          boundary.reserved < desired) {
        boundary.reserved = desired;
        updated = true;
      }
    if (updated)
      continue;
    size_t promoted = 0;
    forEachCallsite([&](Callsite &callsite, const auto &directTarget) {
      if (directTarget && !callsite.extender &&
          !inBranchRange(callsiteVA(callsite), *directTarget)) {
        callsite.forceExtender = true;
        hasForcedCallsites = true;
        ++promoted;
      }
    });
    if (promoted)
      log(name() + " branch extender promoted direct calls = " +
          std::to_string(promoted));
  }
  if (pass > 30)
    fatal(name() + " branch extender did not converge");
  materialize();
  forEachCallsite([&](Callsite &callsite, const auto &directTarget) {
    if (callsite.extender &&
        (!directTarget ||
         !inBranchRange(callsiteVA(callsite), *directTarget))) {
      callsite.reloc->referent = callsite.extender->sym;
      callsite.reloc->addend = 0;
    }
  });
  walkLayout(LayoutKind::commit);
  log(name() + " branch extender for " + first.parent->name + "," + first.name +
      ": passes = " + std::to_string(pass) +
      ", inputs = " + std::to_string(boundaries.size()) +
      ", targets = " + std::to_string(callees.size()) +
      ", total extenders = " + std::to_string(extenders.size()));
}
} // namespace lld::macho

void TextOutputSection::finalizeWithExtenders(BranchRangeExtensionMode mode) {
  if (hybridFinalized)
    return;
  Finalizer(*this, mode).run();
}

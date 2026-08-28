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
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
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

namespace {

static uint64_t subSat(uint64_t a, uint64_t b) { return a > b ? a - b : 0; }
static uint64_t addSat(uint64_t a, uint64_t b) {
  return UINT64_MAX - a < b ? UINT64_MAX : a + b;
}

static bool inBranchRange(uint64_t from, uint64_t to) {
  return subSat(from, target->backwardBranchRange) <= to &&
         to <= addSat(from, target->forwardBranchRange);
}

template <typename OrdinalRange, typename Predicate>
static Boundary *findInDirection(MutableArrayRef<Boundary> boundaries,
                                 OrdinalRange &&ordinals, bool ascending,
                                 Predicate predicate) {
  for (uint32_t ordinal : llvm::reverse_conditionally(ordinals, !ascending)) {
    Boundary &boundary = boundaries[ordinal];
    assert(boundary.ordinal == ordinal);
    if (predicate(boundary))
      return &boundary;
  }
  return nullptr;
}

} // namespace

namespace lld::macho {
// Input-backed __TEXT sections such as __cstring and __const are
// TextOutputSections but do not contain instructions. Conversely, synthetic
// sections such as __stubs, __stub_helper, and __objc_stubs contain machine
// code but are SyntheticSections, not TextOutputSections. An extender host
// must be in the intersection: an instruction-bearing TextOutputSection whose
// finalizeWithExtenders() implementation can insert extender input sections.
bool canHostExtenders(const OutputSection *osec) {
  auto *text = dyn_cast<TextOutputSection>(osec);
  return text &&
         sections::isCodeSection(text->name, text->parent->name, text->flags);
}

uint64_t Boundary::getInputEndVA() const { return inputVA + isec->getSize(); }

uint64_t Boundary::getNextExtenderVA(ExtenderKind kind) const {
  return alignToPowerOf2(getInputEndVA() + plannedSize,
                         target->getExtenderAlign(kind));
}

BoundaryTable::BoundaryTable(ArrayRef<TextOutputSection *> sections) {
  for (TextOutputSection *section : sections)
    for (ConcatInputSection *isec : section->inputs)
      boundaries.push_back({isec, static_cast<uint32_t>(boundaries.size())});
}

void BoundaryTable::initializeExtenderPlacementBoundaries() {
  // Currently, we choose boundary spacing to be 1MiB for performance and
  // locality.
  constexpr uint64_t boundarySpacing = 1024 * 1024;
  if (!coarseBoundaryOrdinals.empty() || boundaries.empty())
    return;

  coarseBoundaryOrdinals.push_back(boundaries.front().ordinal);
  uint64_t lastVA = boundaries.front().getInputEndVA();
  for (Boundary &boundary : llvm::drop_begin(boundaries)) {
    if (&boundary == &boundaries.back())
      break;
    uint64_t va = boundary.getInputEndVA();
    if (va - lastVA < boundarySpacing)
      continue;
    coarseBoundaryOrdinals.push_back(boundary.ordinal);
    lastVA = va;
  }
  if (boundaries.size() > 1)
    coarseBoundaryOrdinals.push_back(boundaries.back().ordinal);
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
      coarseBoundaryOrdinals, lowVA, [&](uint32_t ordinal, uint64_t va) {
        return boundaries[ordinal].getInputEndVA() < va;
      });
  auto coarseEnd = llvm::upper_bound(
      coarseBoundaryOrdinals, highVA, [&](uint64_t va, uint32_t ordinal) {
        return va < boundaries[ordinal].getInputEndVA();
      });
  if (Boundary *boundary =
          findInDirection(boundaries, llvm::make_range(coarseBegin, coarseEnd),
                          ascending, isUsable))
    return boundary;

  auto begin = llvm::partition_point(boundaries, [&](const Boundary &boundary) {
    return boundary.getInputEndVA() < lowVA;
  });
  auto end = llvm::partition_point(boundaries, [&](const Boundary &boundary) {
    return boundary.getInputEndVA() <= highVA;
  });
  uint32_t beginOrdinal = static_cast<uint32_t>(begin - boundaries.begin());
  uint32_t endOrdinal = static_cast<uint32_t>(end - boundaries.begin());
  return findInDirection(boundaries, llvm::seq(beginOrdinal, endOrdinal),
                         ascending, isUsable);
}

bool BoundaryTable::growReservationsToPlannedSpace() {
  bool updated = false;
  for (auto &boundary : boundaries)
    if (boundary.reservedSize < boundary.plannedSize) {
      boundary.reservedSize = boundary.plannedSize;
      updated = true;
    }
  return updated;
}

struct Callee {
  using EstimateOutputSectionVA =
      function_ref<std::optional<uint64_t>(OutputSection *)>;

  void resolveVA(EstimateOutputSectionVA estimateOutputSectionVA) {
    va.reset();
    Symbol *sym = target();
    int64_t targetAddend = addend();

    if (sym->isInStubs() && targetAddend == 0) {
      if (in.stubs->isFinal)
        va = sym->getStubVA();
      else if (auto sectionVA = estimateOutputSectionVA(in.stubs))
        va = *sectionVA +
             uint64_t(sym->stubsIndex) * lld::macho::target->stubSize;
      return;
    }

    auto *defined = dyn_cast<Defined>(sym);
    if (!defined)
      return;

    if (in.objcStubs && defined->isec() == in.objcStubs->isec &&
        in.objcStubs->isNeeded()) {
      if (auto sectionVA = estimateOutputSectionVA(in.objcStubs))
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
  Symbol *target() const { return sym; }
  int64_t addend() const { return targetAddend; }

  // null for nonlocal, otherwise, it's the isec containing the target
  Symbol *sym;
  int64_t targetAddend;
  const Boundary *targetBoundary = nullptr;
  std::optional<uint64_t> va;
  SmallVector<Extender *, 4> extenders;
};

struct Extender {
  Extender(Callee &callee, Boundary &boundary, ExtenderKind kind, uint64_t va)
      : callee(callee), boundary(&boundary), kind(kind), va(va) {}

  size_t size() const { return target->getExtenderSize(kind); }
  uint32_t align() const { return target->getExtenderAlign(kind); }

  // The ultimate destination is needed to materialize the extender body.
  Callee &callee;

  // The insertion boundary connects the proposal to its owning text section.
  Boundary *boundary;
  ExtenderKind kind;

  // The proposed or exact address is used for branch-range validation.
  uint64_t va = 0;

  // Materialization creates the input section that carries the machine code.
  ConcatInputSection *isec = nullptr;

  // Call relocations target this symbol after the proposal is accepted.
  Defined *sym = nullptr;
};

struct RelocRewrite {
  RelocRewrite(Relocation &reloc, const Boundary &boundary, Extender &extender)
      : reloc(reloc), boundary(boundary), extender(extender) {}

  Relocation &reloc;
  const Boundary &boundary;
  Extender &extender;
};

static CalleeKey makeCalleeKey(Symbol *sym, int64_t addend) {
  if (const auto *defined = dyn_cast<Defined>(sym);
      defined && !needsBinding(sym))
    return {defined->isec(), defined->value + addend, 0};
  return {sym, 0, addend};
}
SmallVector<TextOutputSection *, 4>
TextOutputSegment::collectConsecutiveOutputSections(TextOutputSection &first) {
  const auto &sections = first.parent->getSections();
  auto firstIt = llvm::find(sections, &first);
  assert(firstIt != sections.end());
  ArrayRef<OutputSection *> remaining(sections);
  remaining = remaining.drop_front(firstIt - sections.begin());
  return llvm::to_vector<4>(llvm::map_range(
      remaining.take_while(canHostExtenders),
      [](OutputSection *osec) { return cast<TextOutputSection>(osec); }));
}

TextOutputSegment::TextOutputSegment(TextOutputSection &first)
    : first(first), maxHops(config->branchRangeExtensionMaxHops),
      chainKind(target->getChainExtenderKind()),
      fallbackKind(target->getFallbackExtenderKind()),
      textOutputSections(collectConsecutiveOutputSections(first)),
      boundaries(textOutputSections) {
  for (Boundary &boundary : boundaries.entries())
    inputBoundaries[boundary.isec] = &boundary;
}

void TextOutputSegment::initializeCallee(Callee &callee, Symbol *sym,
                                         int64_t addend) {
  callee.sym = sym;
  callee.targetAddend = addend;
  // Cache the boundary for a locally defined branch target.
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
}

std::optional<uint64_t>
TextOutputSegment::estimatePostTextSectionVA(OutputSection *targetSection) {
  if (!targetSection || !targetSection->isNeeded())
    return std::nullopt;
  if (auto it = estimatedPostSegmentSecVAs.find(targetSection);
      it != estimatedPostSegmentSecVAs.end())
    return it->second;

  const auto &sections = first.parent->getSections();

  uint64_t va = textEndVA;
  auto lastTextOutputSectionIt =
      llvm::find(sections, textOutputSections.back());
  assert(lastTextOutputSectionIt != sections.end());
  // Walk post-text sections to estimate the target's aligned VA.
  for (auto *osec :
       llvm::make_range(std::next(lastTextOutputSectionIt), sections.end())) {
    if (!osec->isNeeded())
      continue;

    va = alignToPowerOf2(va, osec->align);
    if (osec == targetSection) {
      estimatedPostSegmentSecVAs[targetSection] = va;
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

void TextOutputSegment::walkLayout(LayoutKind kind) {}

Boundary *TextOutputSegment::findChainBoundary(uint64_t callVA,
                                               uint64_t anchorVA) {
  if (callVA == anchorVA)
    return nullptr;
  bool upper = callVA > anchorVA;
  uint64_t low = addSat(anchorVA, 1);
  uint64_t high =
      std::min(callVA - 1, addSat(anchorVA, target->backwardBranchRange));
  if (!upper) {
    low = std::max(addSat(callVA, 1),
                   subSat(anchorVA, target->forwardBranchRange));
    high = anchorVA - 1;
  }
  return boundaries.findExtenderPlacementInRange(
      low, high,
      upper ? BoundaryPreference::highest : BoundaryPreference::lowest,
      chainKind);
}

Extender *TextOutputSegment::planBranchExtensionExtender(Callee &callee,
                                                         Boundary &boundary,
                                                         ExtenderKind kind) {
  uint64_t va = boundary.getNextExtenderVA(kind);
  Extender *extender = make<Extender>(callee, boundary, kind, va);
  boundary.plannedSize = va + extender->size() - boundary.getInputEndVA();
  extenders.push_back(extender);
  callee.extenders.push_back(extender);
  return extender;
}

Extender *TextOutputSegment::findReusableExtender(Callee &callee,
                                                  uint64_t callVA) const {
  Extender *best = nullptr;
  uint64_t bestDistance = UINT64_MAX;
  for (Extender *extender : callee.extenders) {
    uint64_t distance =
        callVA > extender->va ? callVA - extender->va : extender->va - callVA;
    if (distance < bestDistance && inBranchRange(callVA, extender->va)) {
      best = extender;
      bestDistance = distance;
    }
  }
  return best;
}

Extender *TextOutputSegment::placeChainExtender(Callee &callee,
                                                uint64_t callVA) {
  uint64_t targetVA = *callee.getVA();
  bool upper = callVA > targetVA;
  uint64_t anchorVA = targetVA;
  uint32_t depth = 0;

  // Continue from the outermost island already planned between this call and
  // its target. This makes a relocation walk grow one shared chain per callee
  // instead of rebuilding a chain for every callsite.
  for (Extender *extender : callee.extenders) {
    if (extender->kind != chainKind || ((extender->va > targetVA) != upper))
      continue;
    if ((upper && extender->va >= callVA) || (!upper && extender->va <= callVA))
      continue;
    ++depth;
    if ((upper && extender->va > anchorVA) ||
        (!upper && extender->va < anchorVA))
      anchorVA = extender->va;
  }

  SmallVector<std::pair<Extender *, uint32_t>, 4> added;
  while (depth < maxHops && !inBranchRange(callVA, anchorVA)) {
    Boundary *boundary = findChainBoundary(callVA, anchorVA);
    if (!boundary)
      break;
    uint32_t previousPlannedSize = boundary->plannedSize;
    Extender *extender =
        planBranchExtensionExtender(callee, *boundary, chainKind);
    added.emplace_back(extender, previousPlannedSize);
    anchorVA = extender->va;
    ++depth;
  }
  if (inBranchRange(callVA, anchorVA)) {
    if (added.empty())
      return findReusableExtender(callee, callVA);
    return added.back().first;
  }

  // An incomplete chain cannot route this relocation and must not affect the
  // global island-edge sequence. Fall back to a thunk for this callsite.
  for (auto [extender, previousPlannedSize] : llvm::reverse(added)) {
    extender->boundary->plannedSize = previousPlannedSize;
    callee.extenders.pop_back();
    assert(extenders.back() == extender);
    extenders.pop_back();
  }
  return nullptr;
}

Extender *TextOutputSegment::placeFallbackExtender(Callee &callee,
                                                   uint64_t callVA) {
  if (Extender *extender = findReusableExtender(callee, callVA))
    return extender;

  // Place the fallback at the inward edge of the callsite's branch window. This
  // maximizes the range that later, nearer callsites can share.
  uint64_t low = subSat(callVA, target->backwardBranchRange);
  uint64_t high = addSat(callVA, target->forwardBranchRange);
  auto targetVA = callee.getVA();
  bool upper = targetVA && callVA >= *targetVA;
  Boundary *boundary = boundaries.findExtenderPlacementInRange(
      low, high,
      upper ? BoundaryPreference::lowest : BoundaryPreference::highest,
      fallbackKind);
  if (!boundary)
    fatal("cannot place branch fallback extender for " +
          toString(*callee.target()));
  Extender *fallback =
      planBranchExtensionExtender(callee, *boundary, fallbackKind);
  return fallback;
}

void TextOutputSegment::planBranchExtension() {
  extenders.clear();
  relocRewrites.clear();
  calleesByKey.clear();
  boundaries.resetPlannedSpace();
  walkLayout(LayoutKind::reservation);
  boundaries.initializeExtenderPlacementBoundaries();

  // The reserved layout fixes every callsite address, so plan directly while
  // walking the relocations and retain only rewrites that may be committed.
  for (Boundary &boundary : boundaries.entries())
    for (Relocation &reloc : llvm::reverse(boundary.isec->relocs)) {
      if (!target->hasAttr(reloc.type, RelocAttrBits::BRANCH))
        continue;
      Symbol *sym = cast<Symbol *>(reloc.referent);
      Callee transientCallee;
      initializeCallee(transientCallee, sym, reloc.addend);
      transientCallee.resolveVA(
          [&](OutputSection *osec) { return estimatePostTextSectionVA(osec); });
      uint64_t callVA = boundary.inputVA + reloc.offset;
      if (transientCallee.getVA() &&
          inBranchRange(callVA, *transientCallee.getVA()) &&
          !requiredExtenderRelocs.contains(&reloc))
        continue;
      if (!transientCallee.getVA() &&
          transientCallee.target()->getName().starts_with("___dtrace_"))
        continue;

      CalleeKey key = makeCalleeKey(sym, reloc.addend);
      Callee *&callee = calleesByKey[key];
      if (!callee) {
        callee = make<Callee>();
        initializeCallee(*callee, sym, reloc.addend);
        callee->va = transientCallee.va;
      }
      Extender *extender = findReusableExtender(*callee, callVA);
      if (!extender && callee->getVA())
        extender = placeChainExtender(*callee, callVA);
      if (!extender)
        extender = placeFallbackExtender(*callee, callVA);
      relocRewrites.push_back(make<RelocRewrite>(reloc, boundary, *extender));
    }
  llvm::stable_sort(extenders, [](Extender *a, Extender *b) {
    return *a->boundary < *b->boundary;
  });
}

template <typename Visitor>
bool TextOutputSegment::forEachIslandEdge(Visitor visitor) const {
  bool valid = true;
  DenseMap<Callee *, Extender *> inward;
  auto visit = [&](Extender &extender, bool upper) {
    if (extender.kind != chainKind)
      return;
    uint64_t targetVA = *extender.callee.getVA();
    if (extender.va == targetVA) {
      valid = false;
      return;
    }
    if ((extender.va > targetVA) != upper)
      return;
    Extender *nextInward = std::exchange(inward[&extender.callee], &extender);
    valid &= visitor(extender, nextInward);
  };
  for (Extender *extender : extenders)
    visit(*extender, true);
  inward.clear();
  for (Extender *extender : llvm::reverse(extenders))
    visit(*extender, false);
  return valid;
}

bool TextOutputSegment::isLayoutValid() const {
  // Every island should be in range.
  bool valid = forEachIslandEdge([&](Extender &extender, Extender *inward) {
    uint64_t targetVA = *extender.callee.getVA();
    return inBranchRange(extender.va, inward ? inward->va : targetVA);
  });
  // Revisit the planned rewrites instead of retaining callsite state.
  for (const RelocRewrite *rewrite : relocRewrites)
    valid &= inBranchRange(rewrite->boundary.inputVA + rewrite->reloc.offset,
                           rewrite->extender.va);
  return valid;
}

void TextOutputSegment::materializeAcceptedPlan() {}

// There are exactly 4 cases for invalid edges:
// callsite->callee, callsite->extender, extender->extender, extender->callee
// For extenders vanished after the proposal, we expect them to grow the
// reservation. For callsite->call invalid edge, we wish that force an extender
// for such callsite can bail us out.
bool TextOutputSegment::recoverFromRejectedProposal() {
  if (boundaries.growReservationsToPlannedSpace())
    return true;
  return false;
}

size_t TextOutputSegment::finalize() {
  TimeTraceScope timeScope("Branch extender");

  size_t pass = 1;
  for (; pass <= 30; ++pass) {
    planBranchExtension();
    if (isLayoutValid())
      break;
    if (!recoverFromRejectedProposal())
      fatal("branch extension can't recover from failure");
  }
  if (pass > 30)
    fatal("branch extender did not converge");
  materializeAcceptedPlan();
  log("branch extender for " + first.parent->name + "," + first.name +
      ": passes = " + std::to_string(pass) +
      ", inputs = " + std::to_string(boundaries.size()) +
      ", targets = " + std::to_string(calleesByKey.size()) +
      ", total extenders = " + std::to_string(extenders.size()));
  return textOutputSections.size();
}
} // namespace lld::macho

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
  for (auto *boundary : llvm::reverse_conditionally(boundaries, !ascending))
    if (predicate(*boundary))
      return boundary;
  return nullptr;
}

namespace lld::macho {
uint64_t Boundary::getInputEndVA() const { return inputVA + isec->getSize(); }

uint64_t Boundary::getNextExtenderVA(ExtenderKind kind) const {
  return alignToPowerOf2(getInputEndVA() + plannedSize,
                         target->getExtenderAlign(kind));
}

BoundaryTable::BoundaryTable(ArrayRef<TextOutputSection *> sections) {
  for (auto *section : sections)
    for (auto *isec : section->inputs)
      boundaries.emplace_back(isec, section);
}

void BoundaryTable::initializeExtenderPlacementBoundaries() {
  // Boundary spacing is set to be 1MiB for linking performance
  // and locality.
  constexpr uint64_t boundarySpacing = 1024 * 1024;
  if (!coarseBoundaries.empty() || boundaries.empty())
    return;

  coarseBoundaries.push_back(&boundaries.front());
  uint64_t lastVA = boundaries.front().getInputEndVA();
  for (auto &boundary : llvm::drop_begin(boundaries)) {
    if (&boundary == &boundaries.back())
      break;
    uint64_t va = boundary.getInputEndVA();
    if (va - lastVA < boundarySpacing)
      continue;
    coarseBoundaries.push_back(&boundary);
    lastVA = va;
  }
  if (boundaries.size() > 1)
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
  if (Boundary *boundary = findInDirection(
          llvm::make_range(coarseBegin, coarseEnd), ascending, isUsable))
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

struct Callee {
  Callee(Symbol *sym, int64_t addend,
         const DenseMap<ConcatInputSection *, Boundary *> &inputBoundaries)
      : sym(sym), addend(addend) {
    // A branch to an interior offset relies on the locally defined function's
    // layout and therefore cannot be redirected through an interposable stub.
    if (needsBinding(sym) && addend == 0)
      return;
    auto *defined = dyn_cast<Defined>(sym);
    if (!defined)
      return;
    auto *targetIsec = dyn_cast_or_null<ConcatInputSection>(defined->isec());
    if (targetIsec)
      targetBoundary = inputBoundaries.lookup(targetIsec);
  }

  // Stable target identity and binding shared by all planning passes.
  Symbol *const sym;
  const int64_t addend;
  // Null when the target is outside this text segment.
  const Boundary *targetBoundary = nullptr;
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

struct RelocRewrite {
  RelocRewrite(Relocation &reloc, const Boundary &boundary, Extender &extender)
      : reloc(reloc), boundary(boundary), extender(extender) {}

  Relocation &reloc;
  const Boundary &boundary;
  Extender &extender;
};

using CalleeKey = std::tuple<const void *, uint64_t, int64_t>;

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
      remaining.take_while(
          [](OutputSection *osec) { return osec->canHostExtenders(); }),
      [](OutputSection *osec) { return cast<TextOutputSection>(osec); }));
}

TextOutputSegment::TextOutputSegment(TextOutputSection &first)
    : first(first), textOutputSections(collectConsecutiveOutputSections(first)),
      boundaries(textOutputSections) {}

namespace {

class TextOutputSegmentPlanner {
public:
  TextOutputSegmentPlanner(TextOutputSection &first,
                           ArrayRef<TextOutputSection *> textOutputSections,
                           BoundaryTable &boundaries);

  void run();

private:
  enum class LayoutKind { reservation, proposal };

  TextOutputSection &first;
  const uint32_t maxHops;
  const ExtenderKind chainKind;
  const ExtenderKind fallbackKind;
  ArrayRef<TextOutputSection *> textOutputSections;
  BoundaryTable &boundaries;
  DenseMap<CalleeKey, Callee *> calleesByKey;
  SmallVector<Callee *, 0> callees;
  // Per-frame state is separate from stable callee identity. Resolved VAs are
  // broadly used, islands are common only for active callees, and thunks are
  // rare, so the extender maps are populated lazily.
  DenseMap<const Callee *, uint64_t> calleeVAs;
  DenseMap<const Callee *, SmallVector<Extender *, 4>> calleeIslands;
  DenseMap<const Callee *, SmallVector<Extender *, 1>> calleeThunks;
  SmallVector<Callee *, 0> activeCallees;
  SmallVector<std::tuple<const Boundary *, Relocation *, Callee *>, 0>
      directRelocs;
  SmallVector<RelocRewrite *, 0> relocRewrites;
  DenseMap<OutputSection *, uint64_t> outputSectionVAs;
  uint64_t textEndVA = 0;
  DenseSet<const Relocation *> requiredExtenderRelocs;

  void updateOutputSectionVAs();
  std::optional<uint64_t> getOutputSectionVA(OutputSection *) const;
  std::optional<uint64_t> resolveCalleeVA(const Callee &);
  std::optional<uint64_t> getCalleeVA(const Callee &) const;
  void walkLayout(LayoutKind);
  Boundary *findChainBoundary(uint64_t callVA, uint64_t anchorVA);
  Extender *planBranchExtensionExtender(Callee &, Boundary &, ExtenderKind,
                                        Extender *inwardIsland);
  Extender *findReusableExtender(Callee &, uint64_t) const;
  Extender *placeChainExtender(Callee &, uint64_t, bool force);
  Extender *placeFallbackExtender(Callee &, uint64_t);
  void planBranchExtension();
  bool hasInvalidDirectCall();
  bool recoverFromRejectedProposal();
  bool isLayoutValid();
  void materializeAcceptedPlan();
};

TextOutputSegmentPlanner::TextOutputSegmentPlanner(
    TextOutputSection &first, ArrayRef<TextOutputSection *> textOutputSections,
    BoundaryTable &boundaries)
    : first(first), maxHops(config->branchRangeExtensionMaxHops),
      chainKind(ExtenderKind::island), fallbackKind(ExtenderKind::thunk),
      textOutputSections(textOutputSections), boundaries(boundaries) {
  DenseMap<ConcatInputSection *, Boundary *> inputBoundaries;
  for (Boundary &boundary : boundaries.entries()) {
    bool inserted =
        inputBoundaries.try_emplace(boundary.isec, &boundary).second;
    assert(inserted && "input section has more than one boundary");
  }

  // Bind every branch target while the temporary input index is available.
  // Callee identity survives every planning retry; only the three planning
  // maps are rebuilt.
  for (Boundary &boundary : boundaries.entries())
    for (Relocation &reloc : llvm::reverse(boundary.isec->relocs)) {
      if (!target->hasAttr(reloc.type, RelocAttrBits::BRANCH))
        continue;
      Symbol *sym = cast<Symbol *>(reloc.referent);
      Callee *&callee = calleesByKey[makeCalleeKey(sym, reloc.addend)];
      if (!callee) {
        callee = make<Callee>(sym, reloc.addend, inputBoundaries);
        callees.push_back(callee);
      }
    }
}

void TextOutputSegmentPlanner::updateOutputSectionVAs() {
  const auto &sections = first.parent->getSections();
  auto firstIt = llvm::find(sections, &first);
  assert(firstIt != sections.end());

  // Writer has finalized and assigned every section preceding this text run.
  for (OutputSection *section : llvm::make_range(sections.begin(), firstIt))
    if (section->isNeeded())
      outputSectionVAs.try_emplace(section, section->addr);

  assert(textOutputSections.size() <=
         static_cast<size_t>(sections.end() - firstIt));
  auto suffixBegin = std::next(firstIt, textOutputSections.size());
  uint64_t va = textEndVA;
  for (OutputSection *section : llvm::make_range(suffixBegin, sections.end())) {
    if (!section->isNeeded())
      continue;
    va = alignToPowerOf2(va, section->align);
    outputSectionVAs.try_emplace(section, va);
    va += section->getSizeForAddressAssignment();
  }
}

std::optional<uint64_t> TextOutputSegmentPlanner::getOutputSectionVA(
    OutputSection *targetSection) const {
  if (!targetSection || !targetSection->isNeeded())
    return std::nullopt;
  if (auto it = outputSectionVAs.find(targetSection);
      it != outputSectionVAs.end())
    return it->second;
  return std::nullopt;
}

std::optional<uint64_t>
TextOutputSegmentPlanner::resolveCalleeVA(const Callee &callee) {
  Symbol *sym = callee.sym;
  int64_t addend = callee.addend;

  if (sym->isInStubs() && addend == 0) {
    if (auto sectionVA = getOutputSectionVA(in.stubs))
      return *sectionVA + uint64_t(sym->stubsIndex) * target->stubSize;
    return std::nullopt;
  }

  auto *defined = dyn_cast<Defined>(sym);
  if (!defined)
    return std::nullopt;

  if (in.objcStubs && defined->isec() == in.objcStubs->isec &&
      in.objcStubs->isNeeded()) {
    if (auto sectionVA = getOutputSectionVA(in.objcStubs))
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

void TextOutputSegmentPlanner::walkLayout(LayoutKind kind) {
  outputSectionVAs.clear();
  uint64_t va = first.addr;
  MutableArrayRef<Boundary> entries = boundaries.entries();
  auto boundaryIt = entries.begin();
  for (TextOutputSection *section : textOutputSections) {
    va = alignToPowerOf2(va, section->align);
    outputSectionVAs.try_emplace(section, va);
    if (kind == LayoutKind::proposal)
      section->addr = va;

    for (ConcatInputSection *isec : section->inputs) {
      assert(boundaryIt != entries.end());
      Boundary &boundary = *boundaryIt++;
      assert(boundary.section == section && boundary.isec == isec);

      va = alignToPowerOf2(va, isec->align);
      boundary.inputVA = va;
      va += isec->getSize();

      if (kind == LayoutKind::reservation) {
        va += boundary.reservedSize;
        continue;
      }
      assert(kind == LayoutKind::proposal);
      for (Extender *extender : boundary.plannedExtenders) {
        va = alignToPowerOf2(va, extender->align());
        extender->va = va;
        va += extender->size();
      }
    }
  }
  assert(boundaryIt == entries.end());
  textEndVA = va;
  updateOutputSectionVAs();
  calleeVAs.clear();
  for (Callee *callee : callees)
    if (std::optional<uint64_t> va = resolveCalleeVA(*callee))
      calleeVAs.try_emplace(callee, *va);
}

Boundary *TextOutputSegmentPlanner::findChainBoundary(uint64_t callVA,
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

Extender *TextOutputSegmentPlanner::planBranchExtensionExtender(
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
TextOutputSegmentPlanner::findReusableExtender(Callee &callee,
                                               uint64_t callVA) const {
  auto reachable = [callVA](Extender *extender) {
    return inBranchRange(callVA, extender->va);
  };
  Extender *island = nullptr;
  if (auto it = calleeIslands.find(&callee); it != calleeIslands.end())
    if (auto found = llvm::find_if(it->second, reachable);
        found != it->second.end())
      island = *found;
  Extender *thunk = nullptr;
  if (auto it = calleeThunks.find(&callee); it != calleeThunks.end())
    if (auto found = llvm::find_if(it->second, reachable);
        found != it->second.end())
      thunk = *found;
  if (!island)
    return thunk;
  if (!thunk)
    return island;
  auto distance = [callVA](const Extender *extender) {
    return callVA > extender->va ? callVA - extender->va
                                 : extender->va - callVA;
  };
  return distance(island) <= distance(thunk) ? island : thunk;
}

Extender *TextOutputSegmentPlanner::placeChainExtender(Callee &callee,
                                                       uint64_t callVA,
                                                       bool force) {
  uint64_t targetVA = *getCalleeVA(callee);
  bool upper = callVA > targetVA;
  uint64_t anchorVA = targetVA;
  Extender *inwardIsland = nullptr;
  uint32_t depth = 0;

  // Continue from the outermost island already planned between this call and
  // its target. This makes a relocation walk grow one shared chain per callee
  // instead of rebuilding a chain for every callsite.
  if (auto it = calleeIslands.find(&callee); it != calleeIslands.end()) {
    for (Extender *extender : it->second) {
      if ((extender->va > targetVA) != upper)
        continue;
      if ((upper && extender->va >= callVA) ||
          (!upper && extender->va <= callVA))
        continue;
      ++depth;
      if ((upper && extender->va > anchorVA) ||
          (!upper && extender->va < anchorVA)) {
        anchorVA = extender->va;
        inwardIsland = extender;
      }
    }
  }

  SmallVector<std::pair<Extender *, uint32_t>, 4> added;
  while (depth < maxHops && (force || !inBranchRange(callVA, anchorVA))) {
    Boundary *boundary = findChainBoundary(callVA, anchorVA);
    if (!boundary)
      break;
    uint32_t previousPlannedSize = boundary->plannedSize;
    Extender *extender =
        planBranchExtensionExtender(callee, *boundary, chainKind, inwardIsland);
    added.emplace_back(extender, previousPlannedSize);
    anchorVA = extender->va;
    inwardIsland = extender;
    ++depth;
    force = false;
  }
  if (inBranchRange(callVA, anchorVA)) {
    if (added.empty())
      return findReusableExtender(callee, callVA);
    return added.back().first;
  }

  // An incomplete chain cannot route this relocation and must not affect the
  // global island-edge sequence. Fall back to a thunk for this callsite.
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

Extender *TextOutputSegmentPlanner::placeFallbackExtender(Callee &callee,
                                                          uint64_t callVA) {
  if (!target->supportsExtender(ExtenderKind::thunk))
    fatal("target does not support thunk fallback for " +
          toString(*callee.sym));
  if (Extender *extender = findReusableExtender(callee, callVA))
    return extender;

  // Place the fallback at the inward edge of the callsite's branch window. This
  // maximizes the range that later, nearer callsites can share.
  uint64_t low = subSat(callVA, target->backwardBranchRange);
  uint64_t high = addSat(callVA, target->forwardBranchRange);
  auto targetVA = getCalleeVA(callee);
  bool upper = targetVA && callVA >= *targetVA;
  Boundary *boundary = boundaries.findExtenderPlacementInRange(
      low, high,
      upper ? BoundaryPreference::lowest : BoundaryPreference::highest,
      fallbackKind);
  if (!boundary)
    fatal("cannot place branch fallback extender for " + toString(*callee.sym));
  Extender *fallback = planBranchExtensionExtender(
      callee, *boundary, fallbackKind, /*inwardIsland=*/nullptr);
  return fallback;
}

void TextOutputSegmentPlanner::planBranchExtension() {
  directRelocs.clear();
  relocRewrites.clear();
  activeCallees.clear();
  calleeIslands.clear();
  calleeThunks.clear();
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
      Callee *callee = calleesByKey.lookup(makeCalleeKey(sym, reloc.addend));
      assert(callee && "branch target was not prebound");
      std::optional<uint64_t> calleeVA = getCalleeVA(*callee);
      uint64_t callVA = boundary.inputVA + reloc.offset;
      if (calleeVA && inBranchRange(callVA, *calleeVA) &&
          !requiredExtenderRelocs.contains(&reloc)) {
        directRelocs.emplace_back(&boundary, &reloc, callee);
        continue;
      }
      bool firstExtender =
          !calleeIslands.contains(callee) && !calleeThunks.contains(callee);
      if (firstExtender)
        activeCallees.push_back(callee);
      Extender *extender = findReusableExtender(*callee, callVA);
      if (!extender && calleeVA)
        extender = placeChainExtender(*callee, callVA,
                                      requiredExtenderRelocs.contains(&reloc));
      if (!extender)
        extender = placeFallbackExtender(*callee, callVA);
      relocRewrites.push_back(make<RelocRewrite>(reloc, boundary, *extender));
    }
  walkLayout(LayoutKind::proposal);
}

bool TextOutputSegmentPlanner::isLayoutValid() {
  // Every island must reach its next inward island or the final callee. Thunks
  // need no such check because they can reach any target.
  for (const auto &entry : calleeIslands)
    for (const auto *island : entry.second) {
      std::optional<uint64_t> calleeVA = getCalleeVA(island->callee);
      assert(calleeVA && "island target has no VA");
      uint64_t targetVA =
          island->inwardIsland ? island->inwardIsland->va : *calleeVA;
      if (!inBranchRange(island->va, targetVA))
        return false;
    }

  // Every callsite must reach its selected target: an extender for a rewritten
  // relocation or the callee for a direct relocation.
  for (const auto *rewrite : relocRewrites)
    if (!inBranchRange(rewrite->boundary.inputVA + rewrite->reloc.offset,
                       rewrite->extender.va))
      return false;
  for (auto [boundary, reloc, callee] : directRelocs) {
    std::optional<uint64_t> calleeVA = getCalleeVA(*callee);
    if (!calleeVA ||
        !inBranchRange(boundary->inputVA + reloc->offset, *calleeVA))
      return false;
  }
  return true;
}

void TextOutputSegmentPlanner::materializeAcceptedPlan() {
  DenseMap<Extender *, TextOutputSection::ExtenderArtifact> artifacts;
  DenseMap<TextOutputSection *,
           SmallVector<TextOutputSection::MaterializedExtender, 0>>
      bySection;

  parallelForEach(activeCallees, [&](Callee *callee) {
    auto byVA = [](const Extender *lhs, const Extender *rhs) {
      return lhs->va < rhs->va;
    };
    if (auto it = calleeIslands.find(callee); it != calleeIslands.end())
      llvm::sort(it->second, byVA);
    if (auto it = calleeThunks.find(callee); it != calleeThunks.end())
      llvm::sort(it->second, byVA);
  });

  // Declare every extender before populating any body so island chains may
  // refer across output-section boundaries.
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
  for (Callee *callee : activeCallees) {
    if (auto it = calleeIslands.find(callee); it != calleeIslands.end())
      declareExtenders(*callee, it->second);
    if (auto it = calleeThunks.find(callee); it != calleeThunks.end())
      declareExtenders(*callee, it->second);
  }

  for (Boundary &boundary : boundaries.entries()) {
    for (Extender *extender : boundary.plannedExtenders) {
      Symbol *bodyTarget = extender->inwardIsland
                               ? artifacts.lookup(extender->inwardIsland).sym
                               : extender->callee.sym;
      int64_t bodyAddend = extender->inwardIsland ? 0 : extender->callee.addend;
      if (extender->kind == fallbackKind)
        assert(needsBinding(extender->callee.sym) ==
               extender->callee.sym->isInStubs());
      auto artifact = artifacts.lookup(extender);
      target->populateExtender(artifact.isec, extender->kind, bodyTarget,
                               bodyAddend);
      bySection[boundary.section].push_back(
          {boundary.isec, artifact.isec, extender->va});
    }
  }

  for (RelocRewrite *rewrite : relocRewrites) {
    rewrite->reloc.referent =
        static_cast<Symbol *>(artifacts.lookup(&rewrite->extender).sym);
    rewrite->reloc.addend = 0;
  }

  uint64_t va = first.addr;
  for (TextOutputSection *section : textOutputSections) {
    va = alignToPowerOf2(va, section->align);
    uint64_t sectionVA = va;
    assert(section->addr == sectionVA);
    section->finalizeWithExtenders(bySection[section]);
    va += section->getSize();
  }
  assert(va == textEndVA);
}

bool TextOutputSegmentPlanner::hasInvalidDirectCall() {
  bool registeredNewCallsite = false;
  for (auto [boundary, reloc, callee] : directRelocs) {
    std::optional<uint64_t> calleeVA = getCalleeVA(*callee);
    if (!calleeVA)
      continue;

    uint64_t callVA = boundary->inputVA + reloc->offset;
    if (!inBranchRange(callVA, *calleeVA)) {
      requiredExtenderRelocs.insert(reloc);
      registeredNewCallsite = true;
    }
  }
  return registeredNewCallsite;
}

// A proposed layout can fail because an island cannot reach its next target or
// because a callsite cannot reach its selected target. The extenders in a
// rejected proposal are discarded before replanning, so grow the reservations
// for those extenders to preserve their space. If a direct callsite-to-callee
// edge becomes invalid, routing the call through an extender may recover the
// layout.
bool TextOutputSegmentPlanner::recoverFromRejectedProposal() {
  bool grewReservation = false;
  for (auto &boundary : boundaries.entries())
    if (boundary.reservedSize < boundary.plannedSize) {
      boundary.reservedSize = boundary.plannedSize;
      grewReservation = true;
    }
  if (grewReservation) {
    // Registered callsites are specific to a fixed reservation layout.
    // Reconsider direct calls from scratch after growth changes that layout.
    requiredExtenderRelocs.clear();
    return true;
  }
  if (hasInvalidDirectCall())
    return true;
  return false;
}

void TextOutputSegmentPlanner::run() {
  TimeTraceScope timeScope("Branch extender");
  constexpr size_t maxPassCount = 30;
  size_t pass = 1;
  for (; pass <= maxPassCount; ++pass) {
    planBranchExtension();
    if (isLayoutValid())
      break;
    if (!recoverFromRejectedProposal())
      fatal("branch extension can't recover from failure");
  }
  if (pass > maxPassCount)
    fatal("branch extender did not converge");
  materializeAcceptedPlan();
  size_t extenderCount = 0;
  for (const Boundary &boundary : boundaries.entries())
    extenderCount += boundary.plannedExtenders.size();
  log("branch extender for " + first.parent->name + "," + first.name +
      ": passes = " + std::to_string(pass) +
      ", inputs = " + std::to_string(boundaries.size()) +
      ", targets = " + std::to_string(activeCallees.size()) +
      ", total extenders = " + std::to_string(extenderCount));
}

} // namespace

size_t TextOutputSegment::finalize() {
  TextOutputSegmentPlanner planner(first, textOutputSections, boundaries);
  planner.run();
  return size();
}
} // namespace lld::macho

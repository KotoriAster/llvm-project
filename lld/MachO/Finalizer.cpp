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

#include <algorithm>
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

struct Extender;
struct Callsite {
  Relocation *reloc;
  uint32_t inputIdx;
  Extender *extender = nullptr;
  bool forceExtender = false;
};

struct Callee {
  SmallVector<Callsite, 1> callsites;
  auto target() const { return cast<Symbol *>(callsites[0].reloc->referent); }
  int64_t addend() const { return callsites.front().reloc->addend; }
  bool isDtrace() const {
    return target()->getName().starts_with("___dtrace_");
  }
};

struct Boundary {
  ConcatInputSection *isec;
  uint32_t reserved = 0;
  uint64_t va = 0;
};

struct Extender {
  Callee *callee;
  uint32_t boundaryIdx;
  bool isThunk;
  bool live = false;
  uint64_t va = 0;
  ConcatInputSection *isec = nullptr;
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
  TextOutputSection &first;
  bool islandOnly;
  SmallVector<Boundary, 0> boundaries;
  SmallVector<TextOutputSection *, 4> owners;
  SmallVector<Callee *, 0> callees;
  SmallVector<Extender *, 0> extenders;
  uint64_t textEndVA = 0;
  StringRef name() const { return islandOnly ? "islands-slop-free" : "hybrid"; }
  size_t extenderSize(const Extender &e) const {
    return e.isThunk ? target->thunkSize : target->islandSize;
  }
  void collect();
  std::optional<uint64_t> estimateSectionVA(OutputSection *) const;
  std::optional<uint64_t> targetVA(const Callee &) const;
  void layout(bool exact, bool emit = false);
  uint64_t boundaryBaseVA(uint32_t idx) const {
    return alignToPowerOf2(boundaries[idx].va + boundaries[idx].isec->getSize(),
                           4);
  }
  uint64_t proposedExtenderVA(uint32_t) const;
  uint32_t boundaryBound(uint64_t, bool) const;
  uint64_t callsiteVA(const Callsite &c) const {
    return boundaries[c.inputIdx].va + c.reloc->offset;
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
    for (ConcatInputSection *isec : text->inputs)
      boundaries.push_back({isec});
  }
  DenseMap<CalleeKey, Callee *> calleeByKey;
  for (auto [inputIdx, boundary] : llvm::enumerate(boundaries)) {
    SmallVector<Relocation *, 0> branches;
    for (Relocation &reloc : boundary.isec->relocs)
      if (target->hasAttr(reloc.type, RelocAttrBits::BRANCH))
        branches.push_back(&reloc);
    llvm::sort(branches, [](const Relocation *a, const Relocation *b) {
      return a->offset < b->offset;
    });
    for (Relocation *reloc : branches) {
      Symbol *sym = cast<Symbol *>(reloc->referent);
      bool binds = needsBinding(sym);
      if (islandOnly && binds)
        in.stubs->addEntry(sym);
      Callee *&callee = calleeByKey[makeCalleeKey(sym, reloc->addend)];
      if (!callee) {
        callee = make<Callee>();
        callees.push_back(callee);
      }
      callee->callsites.push_back({reloc, static_cast<uint32_t>(inputIdx)});
    }
  }
}

std::optional<uint64_t>
Finalizer::estimateSectionVA(OutputSection *targetSection) const {
  if (!targetSection || !targetSection->isNeeded())
    return std::nullopt;
  uint64_t va = textEndVA;
  const auto &sections = first.parent->getSections();
  auto begin = std::next(llvm::find(sections, owners.back()));
  for (OutputSection *osec : llvm::make_range(begin, sections.end())) {
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
    } else
      va += osec->getSize();
  }
  return std::nullopt;
}

std::optional<uint64_t> Finalizer::targetVA(const Callee &callee) const {
  Symbol *sym = callee.target();
  int64_t addend = callee.addend();
  if (sym->isInStubs()) {
    if (in.stubs->isFinal)
      return sym->getStubVA() + addend;
    if (auto va = estimateSectionVA(in.stubs))
      return *va + uint64_t(sym->stubsIndex) * target->stubSize + addend;
  }
  auto *defined = dyn_cast<Defined>(sym);
  if (in.objcStubs && in.objcStubs->isNeeded() && defined &&
      defined->isec() == in.objcStubs->isec)
    if (auto va = estimateSectionVA(in.objcStubs))
      return *va + defined->value + addend;
  if (defined && defined->isAbsolute())
    return defined->getVA() + addend;
  if (!defined)
    return std::nullopt;
  auto it = llvm::find_if(boundaries, [&](const Boundary &boundary) {
    return boundary.isec == defined->isec();
  });
  if (it == boundaries.end())
    return std::nullopt;
  return it->va + defined->value + addend;
}

void Finalizer::layout(bool exact, bool emit) {
  uint64_t addr = first.addr;
  uint64_t groupSize = 0;
  uint32_t inputIdx = 0;
  size_t extenderIdx = 0;
  for (TextOutputSection *owner : owners) {
    groupSize = alignToPowerOf2(groupSize, owner->align);
    uint64_t ownerBase = groupSize;
    uint64_t ownerSize = 0;
    if (emit) {
      owner->addr = owner->hybridAddr = addr + ownerBase;
      owner->size = owner->fileSize = 0;
    }
    for (ConcatInputSection *isec : owner->inputs) {
      ownerSize = alignToPowerOf2(ownerSize, isec->align);
      boundaries[inputIdx].va = addr + ownerBase + ownerSize;
      ownerSize += isec->getSize();
      if (emit) {
        owner->finalizeOne(isec);
        assert(isec->getVA() == boundaries[inputIdx].va);
      }
      if (exact)
        while (extenderIdx < extenders.size() &&
               extenders[extenderIdx]->boundaryIdx == inputIdx) {
          ownerSize = alignToPowerOf2(ownerSize, 4);
          Extender &extender = *extenders[extenderIdx++];
          extender.va = addr + ownerBase + ownerSize;
          ownerSize += extenderSize(extender);
          if (emit) {
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
    if (emit)
      owner->hybridFinalized = true;
  }
  textEndVA = addr + groupSize;
}

uint64_t Finalizer::proposedExtenderVA(uint32_t boundaryIdx) const {
  uint64_t va = boundaryBaseVA(boundaryIdx);
  for (Extender *extender : extenders)
    if (extender->boundaryIdx == boundaryIdx)
      va += extenderSize(*extender);
  return va;
}
uint32_t Finalizer::boundaryBound(uint64_t va, bool upper) const {
  va = addSat(va, upper);
  return llvm::partition_point(boundaries,
                               [&](const Boundary &boundary) {
                                 return boundaryBaseVA(&boundary -
                                                       boundaries.data()) < va;
                               }) -
         boundaries.begin();
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
  uint32_t begin = boundaryBound(low, false);
  uint32_t end = boundaryBound(high, true);
  for (uint32_t cursor = forward ? begin : end;
       cursor != (forward ? end : begin);) {
    uint32_t idx = forward ? cursor++ : --cursor;
    if (inBranchRange(proposedExtenderVA(idx), anchorVA))
      return idx;
  }
  return std::nullopt;
}
Extender *Finalizer::createExtender(Callee &callee, uint32_t boundaryIdx,
                                    bool isThunk) {
  Extender *extender = make<Extender>();
  extender->callee = &callee;
  extender->boundaryIdx = boundaryIdx;
  extender->isThunk = isThunk;
  extender->va = proposedExtenderVA(boundaryIdx);
  extenders.push_back(extender);
  return extender;
}
Extender *Finalizer::placeThunk(SmallVectorImpl<Extender *> &thunks,
                                Callee &callee, uint64_t callVA) {
  for (Extender *thunk : thunks)
    if (inBranchRange(callVA, thunk->va))
      return thunk;
  uint32_t boundaryIdx = UINT32_MAX;
  uint64_t bestDistance = UINT64_MAX;
  uint32_t begin =
      boundaryBound(subSat(callVA, target->backwardBranchRange), false);
  uint32_t end =
      boundaryBound(addSat(callVA, target->forwardBranchRange), true);
  for (uint32_t idx : llvm::seq(begin, end)) {
    uint64_t base = boundaryBaseVA(idx);
    uint64_t distance = base > callVA ? base - callVA : callVA - base;
    if (distance <= bestDistance &&
        inBranchRange(callVA, proposedExtenderVA(idx))) {
      boundaryIdx = idx;
      bestDistance = distance;
    }
  }
  if (boundaryIdx == UINT32_MAX) {
    error("cannot place branch thunk for " + toString(*callee.target()));
    return nullptr;
  }
  Extender *thunk = createExtender(callee, boundaryIdx, true);
  thunks.push_back(thunk);
  return thunk;
}

template <typename Fn>
static void forEachOutward(MutableArrayRef<Callsite> side, bool upper, Fn fn) {
  if (upper)
    for (Callsite &callsite : llvm::reverse(side))
      fn(callsite);
  else
    for (Callsite &callsite : side)
      fn(callsite);
}

void Finalizer::planSide(Callee &callee, MutableArrayRef<Callsite> side,
                         bool upper, SmallVectorImpl<Extender *> &thunks,
                         uint64_t targetVA) {
  Callsite *furthest = nullptr;
  forEachOutward(side, upper, [&](Callsite &callsite) {
    if (!furthest && (callsite.forceExtender ||
                      !inBranchRange(callsiteVA(callsite), targetVA)))
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
    for (Extender *island : islands) {
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
    if (!callsite.extender) {
      error("cannot route branch to " + toString(*callee.target()));
      return;
    }
    callsite.extender->live = true;
    if (!callsite.extender->isThunk)
      for (Extender *island : islands) {
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
    for (Callsite &callsite : callee.callsites) {
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
  for (Callee *callee : callees)
    for (Callsite &callsite : callee->callsites)
      callsite.extender = nullptr;
  layout(false);
  for (Callee *callee : callees)
    planCallee(*callee);
  llvm::erase_if(extenders, [](Extender *extender) { return !extender->live; });
  llvm::stable_sort(extenders, [](Extender *a, Extender *b) {
    return a->boundaryIdx < b->boundaryIdx;
  });
}

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
    Extender *next = std::exchange(inward[extender.callee], &extender);
    valid &= visitor(extender, next, *target);
  };
  for (Extender *extender : extenders)
    visit(*extender, true);
  inward.clear();
  for (Extender *extender : llvm::reverse(extenders))
    visit(*extender, false);
  return valid;
}

bool Finalizer::validate() {
  bool valid = forEachIslandEdge(
      [&](Extender &extender, Extender *inward, uint64_t targetVA) {
        return inBranchRange(extender.va, inward ? inward->va : targetVA);
      });
  for (Callee *callee : callees) {
    auto target = targetVA(*callee);
    for (const Callsite &callsite : callee->callsites) {
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
  for (Extender *extender : extenders)
    if (extender->isThunk) {
      Callee &callee = *extender->callee;
      create(*extender, sequences[callee.target()].second++);
      if (needsBinding(callee.target()))
        in.stubs->addEntry(callee.target());
      target->populateThunk(extender->isec, callee.target(), callee.addend());
    }
}

void Finalizer::run() {
  size_t pass = 1;
  for (; pass <= 30; ++pass) {
    plan();
    layout(true);
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
    for (Callee *callee : callees)
      if (auto targetAddr = targetVA(*callee))
        for (Callsite &callsite : callee->callsites)
          if (!callsite.extender &&
              !inBranchRange(callsiteVA(callsite), *targetAddr)) {
            callsite.forceExtender = true;
            ++promoted;
          }
    if (promoted)
      log(name() + " branch extender promoted direct calls = " +
          std::to_string(promoted));
  }
  if (pass > 30)
    fatal(name() + " branch extender did not converge");
  materialize();
  for (Callee *callee : callees) {
    auto directTarget = targetVA(*callee);
    for (Callsite &callsite : callee->callsites)
      if (callsite.extender &&
          (!directTarget ||
           !inBranchRange(callsiteVA(callsite), *directTarget))) {
        callsite.reloc->referent = callsite.extender->sym;
        callsite.reloc->addend = 0;
      }
  }
  layout(true, true);
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

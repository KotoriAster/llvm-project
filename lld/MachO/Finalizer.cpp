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
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/TimeProfiler.h"

#define DEBUG_TYPE "lld-macho-branch-islands"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <tuple>
#include <vector>

using namespace llvm;
using namespace llvm::MachO;
using namespace lld;
using namespace lld::macho;

namespace {
// Groups callsites by effective target: (identity, value, addend). A non-binding
// Defined is keyed by its address (isec, value) so aliasing symbols share one
// island; everything else is keyed by the symbol pointer. A Symbol* and an
// InputSection* are always distinct pointers, so the two cases never collide.
using IslandKey = std::tuple<const void *, uint64_t, int64_t>;

static IslandKey makeIslandKey(Symbol *sym, int64_t addend) {
  if (const auto *d = dyn_cast<Defined>(sym); d && !needsBinding(d))
    return {d->isec(), d->value, addend};
  return {sym, 0, addend};
}

} // namespace

static std::optional<uint64_t> addVA(uint64_t va, int64_t addend) {
  if (addend >= 0) {
    uint64_t u = addend;
    if (va > std::numeric_limits<uint64_t>::max() - u)
      return std::nullopt;
    return va + u;
  }
  uint64_t u = uint64_t(-(addend + 1)) + 1;
  if (va < u)
    return std::nullopt;
  return va - u;
}

// Saturating branch-window arithmetic: clamp to [0, UINT64_MAX] instead of
// wrapping when a range offset would overflow.
static uint64_t subSat(uint64_t va, uint64_t range) {
  return va > range ? va - range : 0;
}
static uint64_t addSat(uint64_t va, uint64_t range) {
  return va > std::numeric_limits<uint64_t>::max() - range
             ? std::numeric_limits<uint64_t>::max()
             : va + range;
}

namespace {
constexpr uint32_t noExtender = std::numeric_limits<uint32_t>::max();
constexpr uint64_t boundarySpacing = 1024 * 1024;

enum class ExtenderRole : uint8_t { Terminal, Relay, Thunk };

struct ExtensionPolicy {
  StringLiteral name;
  // Maximum island depth from the callee.
  uint32_t maxHops;
  bool allowThunk;
};

// Immutable per-mode policy table. Each mode maps to a named compile-time
// constant; the fields are, in order: name, maxHops, allowThunk.
constexpr ExtensionPolicy moldPolicy = {"mold", 1, true};
constexpr ExtensionPolicy hybridPolicy = {"hybrid", 2, true};
constexpr ExtensionPolicy islandsSlopFreePolicy = {"islands-slop-free",
                                                   noExtender, false};
constexpr ExtensionPolicy thunkExactPolicy = {"thunk-exact", 0, true};

static constexpr ExtensionPolicy extensionPolicy(BranchRangeExtensionMode mode) {
  switch (mode) {
  case BranchRangeExtensionMode::mold:
    return moldPolicy;
  case BranchRangeExtensionMode::hybrid:
    return hybridPolicy;
  case BranchRangeExtensionMode::islandsSlopFree:
    return islandsSlopFreePolicy;
  case BranchRangeExtensionMode::thunkExact:
    return thunkExactPolicy;
  default:
    llvm_unreachable("invalid unified branch-extension mode");
  }
}

struct ExtensionNode {
  ExtensionNode(ExtenderRole role, uint32_t group, uint32_t relayTo,
                uint32_t bucket, uint64_t layoutVA, uint32_t hopsFromTarget)
      : role(role), group(group), relayTo(relayTo), bucket(bucket),
        layoutVA(layoutVA), hopsFromTarget(hopsFromTarget) {}

  ExtenderRole role = ExtenderRole::Terminal;
  uint32_t group = 0;
  uint32_t relayTo = noExtender;
  uint32_t nextInBucket = noExtender;
  uint32_t bucket = 0;
  uint64_t layoutVA = 0;
  // Zero for thunks; terminal islands are one hop from the real callee and a
  // relay is one hop farther than the island it targets.
  uint32_t hopsFromTarget = 0;
  ConcatInputSection *isec = nullptr;
  Defined *sym = nullptr;
};

struct ExtensionBranch {
  ExtensionBranch(Relocation *reloc, uint32_t inputIdx)
      : reloc(reloc), inputIdx(inputIdx) {}

  Relocation *reloc = nullptr;
  uint32_t inputIdx = 0;
  uint32_t extender = noExtender;
  // Once an exact proposal proves that an envelope-direct branch is actually
  // out of range, keep it on an extender path in subsequent proposals.
  bool forceExtender = false;
};

struct ExtensionGroup {
  Symbol *target = nullptr;
  int64_t addend = 0;
  uint32_t targetInputIdx = noExtender;
  uint64_t targetValue = 0;
  SmallVector<ExtensionBranch, 0> branches;
  // Thunk extenders created for this group, kept sorted by bucket (i.e. by
  // candidateVA). A thunk reaches its target in one hop, so any callsite of
  // this group within a thunk's branch range may reuse it. Sharing is scoped
  // to one group because each thunk carries the group's addend.
  SmallVector<uint32_t, 0> thunks;
};

struct OwnerRun {
  TextOutputSection *owner = nullptr;
  uint32_t begin = 0;
  uint32_t end = 0;
};

struct ExtensionState {
  explicit ExtensionState(BranchRangeExtensionMode mode)
      : policy(extensionPolicy(mode)) {}

  const ExtensionPolicy policy;
  size_t boundaryProbes = 0;
  size_t passes = 0;
  uint64_t plannedTextEndVA = 0;

  SmallVector<TextOutputSection *, 4> sections;
  SmallVector<ConcatInputSection *, 0> inputs;
  SmallVector<OwnerRun, 4> ownerRuns;
  SmallVector<uint32_t, 0> allBuckets;
  SmallVector<uint32_t, 0> placementBuckets;
  std::vector<uint64_t> inputVA;
  std::vector<uint64_t> candidateVA;
  std::vector<uint32_t> desiredExtra;
  std::vector<uint32_t> reservedExtra;
  std::vector<uint32_t> bucketHead;
  std::vector<uint32_t> bucketTail;
  SmallVector<ExtensionNode, 0> extenders;
  SmallVector<ExtensionGroup, 0> groups;
};

struct ExtensionStats {
  size_t branchRelocs = 0;
  size_t islandCalls = 0;
  size_t chainCalls = 0;
  size_t thunkCalls = 0;
  size_t islands = 0;
  size_t thunks = 0;
  size_t verifiedBranches = 0;
};

struct InvalidPlanStats {
  size_t directCalls = 0;
  size_t extenderCalls = 0;
  size_t relays = 0;
  size_t terminals = 0;
  size_t unresolved = 0;

  bool empty() const {
    return directCalls == 0 && extenderCalls == 0 && relays == 0 &&
           terminals == 0 && unresolved == 0;
  }
};

static bool inBranchRange(uint64_t from, uint64_t to) {
  return (target->backwardBranchRange < from
              ? from - target->backwardBranchRange
              : 0) <= to &&
         to <= from + target->forwardBranchRange;
}

static size_t extenderSize(const ExtensionNode &e) {
  return e.role == ExtenderRole::Thunk ? target->thunkSize
                                       : target->islandSize;
}

static void collectExtensionBranches(ExtensionState &s) {
  TimeTraceScope timeScope("Branch extension collect");
  DenseMap<ConcatInputSection *, uint32_t> inputIndex;
  for (uint32_t i = 0; i < s.inputs.size(); ++i)
    inputIndex[s.inputs[i]] = i;

  DenseMap<IslandKey, uint32_t> groupByKey;
  auto collect = [&](Relocation &r, uint32_t inputIdx) {
    if (!target->hasAttr(r.type, RelocAttrBits::BRANCH))
      return;

    Symbol *targetSym = cast<Symbol *>(r.referent);
    if (!s.policy.allowThunk && needsBinding(targetSym))
      in.stubs->addEntry(targetSym);
    IslandKey key = makeIslandKey(targetSym, r.addend);
    auto [it, inserted] = groupByKey.try_emplace(key, s.groups.size());
    if (inserted) {
      if (s.groups.size() >= noExtender)
        fatal("too many branch-extension targets");
      ExtensionGroup &group = s.groups.emplace_back();
      group.target = targetSym;
      group.addend = r.addend;
      if (!needsBinding(targetSym)) {
        if (auto *defined = dyn_cast<Defined>(targetSym)) {
          if (auto *targetIsec =
                  dyn_cast_or_null<ConcatInputSection>(defined->isec())) {
            auto targetIt = inputIndex.find(targetIsec);
            if (targetIt != inputIndex.end()) {
              group.targetInputIdx = targetIt->second;
              group.targetValue = defined->value;
            }
          }
        }
      }
    }
    s.groups[it->second].branches.emplace_back(&r, inputIdx);
  };

  for (uint32_t inputIdx = 0; inputIdx < s.inputs.size(); ++inputIdx) {
    std::vector<Relocation> &relocs = s.inputs[inputIdx]->relocs;
    if (std::is_sorted(relocs.begin(), relocs.end(),
                       [](const Relocation &lhs, const Relocation &rhs) {
                         return lhs.offset > rhs.offset;
                       })) {
      for (Relocation &r : llvm::reverse(relocs))
        collect(r, inputIdx);
      continue;
    }

    SmallVector<Relocation *, 0> branches;
    for (Relocation &r : relocs)
      if (target->hasAttr(r.type, RelocAttrBits::BRANCH))
        branches.push_back(&r);
    llvm::sort(branches, [](const Relocation *lhs, const Relocation *rhs) {
      return lhs->offset < rhs->offset;
    });
    for (Relocation *r : branches)
      collect(*r, inputIdx);
  }
}

static void relayoutExtensions(ExtensionState &s,
                               ArrayRef<uint32_t> reservedExtra = {}) {
  TimeTraceScope timeScope("Branch extension relayout");
  uint64_t addr = s.sections.front()->addr;
  uint64_t groupSize = 0;
  for (const OwnerRun &run : s.ownerRuns) {
    groupSize = alignToPowerOf2(groupSize, run.owner->align);
    uint64_t ownerBase = groupSize;
    uint64_t ownerSize = 0;
    for (uint32_t i = run.begin; i < run.end; ++i) {
      ConcatInputSection *isec = s.inputs[i];
      ownerSize = alignToPowerOf2(ownerSize, isec->align);
      s.inputVA[i] = addr + ownerBase + ownerSize;
      ownerSize += isec->getSize();
      groupSize = ownerBase + ownerSize;
      s.candidateVA[i] =
          alignToPowerOf2(addr + ownerBase + ownerSize, 4);

      if (!reservedExtra.empty()) {
        if (reservedExtra[i] != 0) {
          ownerSize = alignToPowerOf2(ownerSize, 4);
          ownerSize += reservedExtra[i];
          groupSize = ownerBase + ownerSize;
        }
        continue;
      }
      for (uint32_t extenderIdx = s.bucketHead[i];
           extenderIdx != noExtender;
           extenderIdx = s.extenders[extenderIdx].nextInBucket) {
        ExtensionNode &extender = s.extenders[extenderIdx];
        ownerSize = alignToPowerOf2(ownerSize, 4);
        extender.layoutVA = addr + ownerBase + ownerSize;
        ownerSize += extenderSize(extender);
        groupSize = ownerBase + ownerSize;
      }
    }
  }
  s.plannedTextEndVA = addr + groupSize;
}

static void initializePlacementBuckets(ExtensionState &s) {
  if (!s.allBuckets.empty() || s.inputs.empty())
    return;

  s.allBuckets.reserve(s.inputs.size());
  for (uint32_t i = 0; i < s.inputs.size(); ++i)
    s.allBuckets.push_back(i);

  s.placementBuckets.push_back(0);
  uint64_t lastVA = s.candidateVA.front();
  for (uint32_t i = 1; i + 1 < s.inputs.size(); ++i) {
    if (s.candidateVA[i] - lastVA < boundarySpacing)
      continue;
    s.placementBuckets.push_back(i);
    lastVA = s.candidateVA[i];
  }
  if (s.inputs.size() > 1)
    s.placementBuckets.push_back(s.inputs.size() - 1);
}

static std::optional<uint64_t>
estimateSectionVA(const ExtensionState &s, OutputSection *targetSection) {
  if (!targetSection || !targetSection->isNeeded())
    return std::nullopt;

  uint64_t va = s.plannedTextEndVA;
  bool pastText = false;
  for (OutputSection *osec : s.sections.front()->parent->getSections()) {
    if (!pastText) {
      pastText = osec == s.sections.back();
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

static std::optional<uint64_t> nonLocalTargetVA(const ExtensionState &s,
                                                Symbol *sym, int64_t addend) {
  if (sym->isInStubs()) {
    if (in.stubs->isFinal)
      return addVA(sym->getStubVA(), addend);
    if (auto stubsVA = estimateSectionVA(s, in.stubs))
      return addVA(*stubsVA + sym->stubsIndex * target->stubSize, addend);
  }
  auto *defined = dyn_cast<Defined>(sym);
  if (in.objcStubs && in.objcStubs->isNeeded() && defined &&
      defined->isec() == in.objcStubs->isec) {
    if (auto objcStubsVA = estimateSectionVA(s, in.objcStubs))
      return addVA(*objcStubsVA + defined->value, addend);
  }
  if (defined && defined->isAbsolute())
    return addVA(defined->getVA(), addend);
  return std::nullopt;
}

static std::optional<uint64_t> extensionTargetVA(const ExtensionState &s,
                                                 const ExtensionGroup &group) {
  if (auto va = nonLocalTargetVA(s, group.target, group.addend))
    return va;
  if (group.targetInputIdx != noExtender)
    return addVA(s.inputVA[group.targetInputIdx] + group.targetValue,
                 group.addend);
  return std::nullopt;
}

static uint32_t createExtender(ExtensionState &s, uint32_t groupIdx,
                               uint32_t bucket, ExtenderRole role,
                               uint32_t relayTo) {
  if (s.extenders.size() >= noExtender)
    fatal("too many branch extenders");

  uint64_t layoutVA = s.candidateVA[bucket] + s.desiredExtra[bucket];
  if (role == ExtenderRole::Relay) {
    assert(relayTo < s.extenders.size());
    assert(s.extenders[relayTo].role != ExtenderRole::Thunk);
  }
  uint32_t hopsFromTarget = 0;
  if (role == ExtenderRole::Terminal)
    hopsFromTarget = 1;
  else if (role == ExtenderRole::Relay) {
    assert(s.extenders[relayTo].hopsFromTarget < noExtender);
    hopsFromTarget = s.extenders[relayTo].hopsFromTarget + 1;
  }
  uint32_t extenderIdx = s.extenders.size();
  s.extenders.emplace_back(role, groupIdx, relayTo, bucket, layoutVA,
                           hopsFromTarget);
  s.desiredExtra[bucket] += extenderSize(s.extenders.back());

  if (s.bucketTail[bucket] == noExtender)
    s.bucketHead[bucket] = extenderIdx;
  else
    s.extenders[s.bucketTail[bucket]].nextInBucket = extenderIdx;
  s.bucketTail[bucket] = extenderIdx;
  return extenderIdx;
}

static uint64_t plannedExtenderVA(const ExtensionState &s, size_t bucket) {
  return s.candidateVA[bucket] + s.desiredExtra[bucket];
}

static TextOutputSection *ownerForInput(const ExtensionState &s,
                                        uint32_t inputIdx) {
  auto it = llvm::upper_bound(
      s.ownerRuns, inputIdx,
      [](uint32_t index, const OwnerRun &run) { return index < run.end; });
  assert(it != s.ownerRuns.end() && it->begin <= inputIdx);
  return it->owner;
}

static std::optional<size_t>
findExtenderBoundary(ExtensionState &s, ArrayRef<uint32_t> buckets,
                     uint64_t callVA, uint64_t anchorVA) {
  if (callVA < anchorVA) {
    uint64_t afterCall =
        callVA == std::numeric_limits<uint64_t>::max() ? callVA : callVA + 1;
    uint64_t low =
        std::max(afterCall, subSat(anchorVA, target->forwardBranchRange));
    auto it = std::lower_bound(
        buckets.begin(), buckets.end(), low,
        [&](uint32_t bucket, uint64_t va) {
          return s.candidateVA[bucket] < va;
        });
    while (it != buckets.end() && s.candidateVA[*it] < anchorVA) {
      ++s.boundaryProbes;
      size_t bucket = *it;
      uint64_t va = plannedExtenderVA(s, bucket);
      if (callVA < va && va < anchorVA && inBranchRange(va, anchorVA))
        return bucket;
      ++it;
    }
    return std::nullopt;
  }
  if (callVA == anchorVA)
    return std::nullopt;

  uint64_t high = std::min(
      callVA - 1, addSat(anchorVA, target->backwardBranchRange));
  auto it = std::upper_bound(
      buckets.begin(), buckets.end(), high,
      [&](uint64_t va, uint32_t bucket) {
        return va < s.candidateVA[bucket];
      });
  while (it != buckets.begin()) {
    --it;
    if (s.candidateVA[*it] <= anchorVA)
      break;
    ++s.boundaryProbes;
    size_t bucket = *it;
    uint64_t va = plannedExtenderVA(s, bucket);
    if (anchorVA < va && va < callVA && inBranchRange(va, anchorVA))
      return bucket;
  }
  return std::nullopt;
}

static std::optional<size_t>
findExtenderBoundary(ExtensionState &s, uint64_t callVA, uint64_t anchorVA) {
  if (auto bucket =
          findExtenderBoundary(s, s.placementBuckets, callVA, anchorVA))
    return bucket;
  return findExtenderBoundary(s, s.allBuckets, callVA, anchorVA);
}

static uint32_t extendExtenderSpine(ExtensionState &s, uint32_t groupIdx,
                                    uint64_t callVA, uint64_t targetVA,
                                    uint32_t anchorIdx) {
  uint64_t anchorVA = anchorIdx == noExtender
                          ? targetVA
                          : s.extenders[anchorIdx].layoutVA;
  uint32_t hopsFromTarget =
      anchorIdx == noExtender ? 0 : s.extenders[anchorIdx].hopsFromTarget;
  assert(anchorIdx == noExtender ||
         s.extenders[anchorIdx].role != ExtenderRole::Thunk);
  assert(hopsFromTarget <= s.policy.maxHops);
  if (inBranchRange(callVA, anchorVA))
    return anchorIdx;

  SmallVector<size_t, 4> buckets;
  while (hopsFromTarget != s.policy.maxHops &&
         !inBranchRange(callVA, anchorVA)) {
    std::optional<size_t> bucket = findExtenderBoundary(s, callVA, anchorVA);
    if (!bucket)
      return noExtender;
    buckets.push_back(*bucket);
    anchorVA = plannedExtenderVA(s, *bucket);
    ++hopsFromTarget;
  }
  if (!inBranchRange(callVA, anchorVA))
    return noExtender;

  uint32_t next = anchorIdx;
  for (size_t bucket : buckets) {
    ExtenderRole role =
        next == noExtender ? ExtenderRole::Terminal : ExtenderRole::Relay;
    next = createExtender(s, groupIdx, bucket, role, next);
  }
  return next;
}

// Return an existing thunk of this group that reaches callVA in one hop, or
// noExtender. group.thunks is sorted by bucket, hence by candidateVA, so we
// binary-search to the callsite and scan the ~256 MiB reach window on both
// sides, testing the exact layoutVA of each candidate.
static uint32_t reuseGroupThunk(const ExtensionState &s, uint32_t groupIdx,
                                uint64_t callVA) {
  const ExtensionGroup &group = s.groups[groupIdx];
  auto key = [&](uint32_t idx) {
    return s.candidateVA[s.extenders[idx].bucket];
  };
  auto it = llvm::lower_bound(
      group.thunks, callVA,
      [&](uint32_t idx, uint64_t va) { return key(idx) < va; });
  uint64_t low = subSat(callVA, target->backwardBranchRange);
  uint64_t high = addSat(callVA, target->forwardBranchRange);
  for (auto f = it; f != group.thunks.end() && key(*f) <= high; ++f)
    if (inBranchRange(callVA, s.extenders[*f].layoutVA))
      return *f;
  for (auto b = it; b != group.thunks.begin();) {
    --b;
    if (key(*b) < low)
      break;
    if (inBranchRange(callVA, s.extenders[*b].layoutVA))
      return *b;
  }
  return noExtender;
}

// Choose the input-section boundary nearest the callsite. A thunk has no
// BRANCH26 edge to its target, so placing it at the edge of the callsite's
// reach window buys no correctness and makes small layout growth change the
// chosen boundary on subsequent relaxation passes. Search by stable
// candidateVA, but validate the exact proposed address.
static std::optional<size_t> findThunkBoundary(ExtensionState &s,
                                               ArrayRef<uint32_t> buckets,
                                               uint64_t callVA) {
  uint64_t low = subSat(callVA, target->backwardBranchRange);
  uint64_t high = addSat(callVA, target->forwardBranchRange);
  auto right = llvm::lower_bound(
      buckets, callVA,
      [&](uint32_t bucket, uint64_t va) { return s.candidateVA[bucket] < va; });
  auto left = right;
  bool hasLeft = left != buckets.begin();
  if (hasLeft)
    --left;

  while (hasLeft || right != buckets.end()) {
    bool takeRight = right != buckets.end();
    if (hasLeft && takeRight)
      takeRight = s.candidateVA[*right] - callVA <=
                  callVA - s.candidateVA[*left];
    auto candidate = takeRight ? right : left;
    uint64_t candidateVA = s.candidateVA[*candidate];
    if (takeRight) {
      if (candidateVA > high) {
        right = buckets.end();
        continue;
      }
      ++right;
    } else {
      if (candidateVA < low) {
        hasLeft = false;
        continue;
      }
      hasLeft = left != buckets.begin();
      if (hasLeft)
        --left;
    }
    ++s.boundaryProbes;
    if (inBranchRange(callVA, plannedExtenderVA(s, *candidate)))
      return *candidate;
  }
  return std::nullopt;
}

static uint32_t placeThunk(ExtensionState &s, uint32_t groupIdx,
                           uint64_t callVA) {
  // Reuse any existing thunk of this group that already reaches the callsite.
  if (uint32_t reuse = reuseGroupThunk(s, groupIdx, callVA);
      reuse != noExtender)
    return reuse;

  // Otherwise create one near the callsite. Coarse buckets first, then all
  // input-section boundaries. Following callsites can reuse it through the
  // group's complete thunk index above.
  std::optional<size_t> bucket =
      findThunkBoundary(s, s.placementBuckets, callVA);
  if (!bucket)
    bucket = findThunkBoundary(s, s.allBuckets, callVA);
  if (!bucket) {
    error("cannot place branch-extension thunk within range for " +
          toString(*s.groups[groupIdx].target));
    return noExtender;
  }

  uint32_t idx =
      createExtender(s, groupIdx, *bucket, ExtenderRole::Thunk, noExtender);
  auto &thunks = s.groups[groupIdx].thunks;
  auto pos = llvm::lower_bound(
      thunks, s.extenders[idx].bucket, [&](uint32_t existing, uint32_t bkt) {
        return s.extenders[existing].bucket < bkt;
      });
  thunks.insert(pos, idx);
  return idx;
}

static void planBranch(ExtensionState &s, uint32_t groupIdx,
                       ExtensionBranch &branch, uint64_t targetVA,
                       uint32_t &islandHint) {
  uint64_t callVA = s.inputVA[branch.inputIdx] + branch.reloc->offset;
  if (!branch.forceExtender && inBranchRange(callVA, targetVA)) {
    branch.extender = noExtender;
    return;
  }

  uint32_t head =
      extendExtenderSpine(s, groupIdx, callVA, targetVA, islandHint);
  if (head == noExtender && s.policy.allowThunk)
    head = placeThunk(s, groupIdx, callVA);
  if (head == noExtender) {
    error("cannot build slop-free branch-island chain from " +
          utohexstr(callVA) + " to " +
          toString(*s.groups[groupIdx].target));
    branch.extender = noExtender;
    return;
  }
  branch.extender = head;
  if (s.extenders[head].role != ExtenderRole::Thunk)
    islandHint = head;
}

static uint64_t branchVA(const ExtensionState &s,
                         const ExtensionBranch &branch) {
  return s.inputVA[branch.inputIdx] + branch.reloc->offset;
}

static void planGroup(ExtensionState &s, uint32_t groupIdx) {
  ExtensionGroup &group = s.groups[groupIdx];
  std::optional<uint64_t> targetVA = extensionTargetVA(s, group);
  if (!targetVA) {
    if (!s.policy.allowThunk) {
      error("cannot resolve island-only branch target " +
            toString(*group.target));
      for (ExtensionBranch &branch : group.branches)
        branch.extender = noExtender;
      return;
    }
    for (ExtensionBranch &branch : group.branches) {
      uint64_t callVA = s.inputVA[branch.inputIdx] + branch.reloc->offset;
      branch.extender = placeThunk(s, groupIdx, callVA);
    }
    return;
  }

  // Sweep away from the target on each side. planBranch filters genuinely
  // direct branches while retaining any branch that an earlier exact proposal
  // promoted out of the conservative envelope's direct set.
  auto targetSplit = std::lower_bound(
      group.branches.begin(), group.branches.end(), *targetVA,
      [&](const ExtensionBranch &branch, uint64_t va) {
        return branchVA(s, branch) < va;
      });
  uint32_t lowerIsland = noExtender;
  for (auto it = std::make_reverse_iterator(targetSplit);
       it != group.branches.rend(); ++it)
    planBranch(s, groupIdx, *it, *targetVA, lowerIsland);
  uint32_t upperIsland = noExtender;
  for (auto it = targetSplit; it != group.branches.end(); ++it)
    planBranch(s, groupIdx, *it, *targetVA, upperIsland);
}

static InvalidPlanStats validateExtensionPlan(const ExtensionState &s) {
  InvalidPlanStats invalid;
  SmallVector<std::optional<uint64_t>, 0> targetVAs;
  targetVAs.reserve(s.groups.size());
  for (const ExtensionGroup &group : s.groups)
    targetVAs.push_back(extensionTargetVA(s, group));

  for (const ExtensionNode &extender : s.extenders) {
    if (extender.role == ExtenderRole::Thunk)
      continue;
    if (extender.role == ExtenderRole::Relay) {
      if (extender.relayTo >= s.extenders.size() ||
          !inBranchRange(extender.layoutVA,
                         s.extenders[extender.relayTo].layoutVA))
        ++invalid.relays;
      continue;
    }
    std::optional<uint64_t> targetVA = targetVAs[extender.group];
    if (!targetVA) {
      if (!s.groups[extender.group].target->getName().starts_with(
              "___dtrace_"))
        ++invalid.unresolved;
      continue;
    }
    if (!inBranchRange(extender.layoutVA, *targetVA))
      ++invalid.terminals;
  }

  for (size_t groupIdx = 0; groupIdx < s.groups.size(); ++groupIdx) {
    const ExtensionGroup &group = s.groups[groupIdx];
    for (const ExtensionBranch &branch : group.branches) {
      if (branch.extender != noExtender) {
        if (branch.extender >= s.extenders.size() ||
            !inBranchRange(branchVA(s, branch),
                           s.extenders[branch.extender].layoutVA))
          ++invalid.extenderCalls;
        continue;
      }
      if (!targetVAs[groupIdx]) {
        if (!group.target->getName().starts_with("___dtrace_"))
          ++invalid.unresolved;
        continue;
      }
      if (!inBranchRange(branchVA(s, branch), *targetVAs[groupIdx]))
        ++invalid.directCalls;
    }
  }
  return invalid;
}

// The reservation envelope is conservative and can be larger than the exact
// slop-free proposal. That contraction may move a call and its target by
// different amounts, invalidating a branch that planning considered direct.
// Promote those branches monotonically so the next proposal gives them an
// extender instead of repeating the same invalid direct decision.
static size_t forceInvalidDirectBranches(ExtensionState &s) {
  size_t promoted = 0;
  for (ExtensionGroup &group : s.groups) {
    std::optional<uint64_t> targetVA = extensionTargetVA(s, group);
    if (!targetVA)
      continue;
    for (ExtensionBranch &branch : group.branches) {
      if (branch.extender != noExtender ||
          inBranchRange(branchVA(s, branch), *targetVA) ||
          branch.forceExtender)
        continue;
      branch.forceExtender = true;
      ++promoted;
    }
  }
  return promoted;
}

static bool runExtensionFixedPoint(ExtensionState &s) {
  TimeTraceScope timeScope("Branch extension fixed point");
  constexpr size_t maxPasses = 30;
  relayoutExtensions(s, s.reservedExtra);
  initializePlacementBuckets(s);
  for (s.passes = 1; s.passes <= maxPasses; ++s.passes) {
    s.extenders.clear();
    std::fill(s.desiredExtra.begin(), s.desiredExtra.end(), 0);
    std::fill(s.bucketHead.begin(), s.bucketHead.end(), noExtender);
    std::fill(s.bucketTail.begin(), s.bucketTail.end(), noExtender);
    for (ExtensionGroup &group : s.groups) {
      group.thunks.clear();
      for (ExtensionBranch &branch : group.branches)
        branch.extender = noExtender;
    }
    for (uint32_t groupIdx = 0; groupIdx < s.groups.size(); ++groupIdx)
      planGroup(s, groupIdx);

    relayoutExtensions(s);
    log(s.policy.name + " branch extender pass " +
        std::to_string(s.passes) + ": proposed extenders = " +
        std::to_string(s.extenders.size()));
    InvalidPlanStats invalid = validateExtensionPlan(s);
    if (invalid.empty())
      return true;
    log(s.policy.name + " branch extender invalid plan: direct calls = " +
        std::to_string(invalid.directCalls) +
        ", extender calls = " + std::to_string(invalid.extenderCalls) +
        ", relays = " + std::to_string(invalid.relays) +
        ", terminals = " + std::to_string(invalid.terminals) +
        ", unresolved = " + std::to_string(invalid.unresolved));
    bool grew = false;
    for (size_t i = 0; i < s.reservedExtra.size(); ++i) {
      if (s.reservedExtra[i] >= s.desiredExtra[i])
        continue;
      s.reservedExtra[i] = s.desiredExtra[i];
      grew = true;
    }
    if (!grew) {
      size_t promoted = forceInvalidDirectBranches(s);
      if (promoted != 0) {
        log(s.policy.name + " branch extender promoted direct calls = " +
            std::to_string(promoted));
        grew = true;
      }
    }
    if (!grew)
      return false;
    relayoutExtensions(s, s.reservedExtra);
  }
  return false;
}

static void materializeExtenders(ExtensionState &s) {
  TimeTraceScope timeScope("Branch extension materialize");
  size_t sequence = 0;
  for (ExtensionNode &extender : s.extenders) {
    TextOutputSection *owner = ownerForInput(s, extender.bucket);
    extender.isec =
        makeSyntheticInputSection(owner->inputs.front()->getSegName(),
                                  owner->inputs.front()->getName());
    extender.isec->parent = owner;
    assert(extender.isec->live);
    size_t size = extenderSize(extender);
    ExtensionGroup &group = s.groups[extender.group];
    StringRef suffix = extender.role == ExtenderRole::Thunk   ? ".thunk."
                       : extender.role == ExtenderRole::Relay ? ".island.1."
                                                            : ".island.0.";
    StringRef name = saver().save(group.target->getName() + suffix +
                                  std::to_string(sequence++));
    if (!isa<Defined>(group.target) ||
        cast<Defined>(group.target)->isExternal()) {
      extender.sym = symtab->addDefined(
          name, /*file=*/nullptr, extender.isec, /*value=*/0, size,
          /*isWeakDef=*/false, /*isPrivateExtern=*/true,
          /*isReferencedDynamically=*/false, /*noDeadStrip=*/false,
          /*isWeakDefCanBeHidden=*/false);
    } else {
      extender.sym = make<Defined>(
          name, /*file=*/nullptr, extender.isec, /*value=*/0, size,
          /*isWeakDef=*/false, /*isExternal=*/false, /*isPrivateExtern=*/true,
          /*includeInSymtab=*/true, /*isReferencedDynamically=*/false,
          /*noDeadStrip=*/false, /*isWeakDefCanBeHidden=*/false);
    }
    extender.sym->used = true;
  }

  for (ExtensionNode &extender : s.extenders) {
    ExtensionGroup &group = s.groups[extender.group];
    if (extender.role == ExtenderRole::Thunk) {
      if (needsBinding(group.target))
        in.stubs->addEntry(group.target);
      target->populateThunk(extender.isec, group.target, group.addend);
    } else if (extender.role == ExtenderRole::Relay) {
      assert(extender.relayTo != noExtender);
      target->populateIsland(extender.isec,
                             s.extenders[extender.relayTo].sym);
      extender.isec->relocs[0].addend = 0;
    } else {
      target->populateIsland(extender.isec, group.target);
      extender.isec->relocs[0].addend = group.addend;
    }
  }
}

static void rewriteBranches(ExtensionState &s) {
  TimeTraceScope timeScope("Branch extension rewrite");
  for (ExtensionGroup &group : s.groups) {
    std::optional<uint64_t> targetVA = extensionTargetVA(s, group);
    for (ExtensionBranch &branch : group.branches) {
      if (targetVA &&
          inBranchRange(branchVA(s, branch), *targetVA)) {
        branch.extender = noExtender;
        continue;
      }
      if (branch.extender == noExtender)
        continue;
      branch.reloc->referent = s.extenders[branch.extender].sym;
      branch.reloc->addend = 0;
    }
  }
}

static void verifyEdge(const ExtensionState &s, uint64_t fromVA, Symbol *sym,
                       int64_t addend, StringRef what, ExtensionStats &stats) {
  if (sym->getName().starts_with("___dtrace_"))
    return;
  std::optional<uint64_t> toVA = nonLocalTargetVA(s, sym, addend);
  if (!toVA)
    if (auto *defined = dyn_cast<Defined>(sym);
        defined && defined->isec() && defined->isec()->isFinal)
      toVA = addVA(defined->getVA(), addend);
  if (!toVA) {
    error(what + " branch target could not be resolved for " + toString(*sym));
    return;
  }
  ++stats.verifiedBranches;
  if (!inBranchRange(fromVA, *toVA))
    error(what + " branch out of range: from " + utohexstr(fromVA) + " to " +
          utohexstr(*toVA) + " (" + toString(*sym) + ")");
}

static ExtensionStats verifyPlan(const ExtensionState &s) {
  ExtensionStats stats;
  for (const ExtensionNode &extender : s.extenders) {
    if (extender.role == ExtenderRole::Thunk) {
      ++stats.thunks;
      continue;
    }
    ++stats.islands;
    Relocation &r = extender.isec->relocs[0];
    verifyEdge(s, extender.isec->getVA(), cast<Symbol *>(r.referent), r.addend,
               "island", stats);
  }

  for (const ExtensionGroup &group : s.groups) {
    stats.branchRelocs += group.branches.size();
    for (const ExtensionBranch &branch : group.branches) {
      Relocation *r = branch.reloc;
      verifyEdge(s, s.inputs[branch.inputIdx]->getVA() + r->offset,
                 cast<Symbol *>(r->referent), r->addend, "callsite", stats);
      if (branch.extender == noExtender)
        continue;
      switch (s.extenders[branch.extender].role) {
      case ExtenderRole::Terminal:
        ++stats.islandCalls;
        break;
      case ExtenderRole::Relay:
        ++stats.chainCalls;
        break;
      case ExtenderRole::Thunk:
        ++stats.thunkCalls;
        break;
      }
    }
  }
  return stats;
}
} // namespace

void TextOutputSection::finalizeWithExtenders(BranchRangeExtensionMode mode) {
  if (hybridFinalized)
    return;

  ExtensionState state(mode);
  TimeTraceScope timeScope("Branch extender", state.policy.name);
  bool foundThis = false;
  for (OutputSection *osec : parent->getSections()) {
    if (osec == this)
      foundThis = true;
    if (!foundThis)
      continue;
    auto *text = dyn_cast<TextOutputSection>(osec);
    if (!text ||
        !sections::isCodeSection(text->name, parent->name, text->flags))
      break;
    assert(!text->hybridFinalized);
    state.sections.push_back(text);
  }
  assert(!state.sections.empty() && state.sections.front() == this);

  {
    TimeTraceScope timeScope("Chain gather inputs");
    for (TextOutputSection *osec : state.sections) {
      uint32_t begin = state.inputs.size();
      for (ConcatInputSection *isec : osec->inputs)
        state.inputs.push_back(isec);
      state.ownerRuns.push_back(
          {osec, begin, static_cast<uint32_t>(state.inputs.size())});
    }
  }

  if (state.inputs.size() >= noExtender)
    fatal("too many input sections for branch extension");
  state.inputVA.resize(state.inputs.size());
  state.candidateVA.resize(state.inputs.size());
  state.desiredExtra.resize(state.inputs.size());
  state.reservedExtra.resize(state.inputs.size());
  state.bucketHead.assign(state.inputs.size(), noExtender);
  state.bucketTail.assign(state.inputs.size(), noExtender);
  collectExtensionBranches(state);
  if (!runExtensionFixedPoint(state))
    fatal(state.policy.name + " branch extender did not converge after 30 passes");

  materializeExtenders(state);
  rewriteBranches(state);

  {
    TimeTraceScope timeScope("Chain layout");
    uint64_t groupSize = 0;
    for (const OwnerRun &run : state.ownerRuns) {
      TextOutputSection *osec = run.owner;
      groupSize = alignToPowerOf2(groupSize, osec->align);
      osec->hybridAddr = addr + groupSize;
      osec->addr = osec->hybridAddr;
      osec->size = 0;
      osec->fileSize = 0;
      auto finalize = [&](ConcatInputSection *isec) {
        osec->finalizeOne(isec);
        groupSize = osec->hybridAddr - addr + osec->size;
      };
      for (uint32_t inputIdx = run.begin; inputIdx < run.end; ++inputIdx) {
        finalize(state.inputs[inputIdx]);
        for (uint32_t extenderIdx = state.bucketHead[inputIdx];
             extenderIdx != noExtender;
             extenderIdx = state.extenders[extenderIdx].nextInBucket) {
          ExtensionNode &extender = state.extenders[extenderIdx];
          finalize(extender.isec);
          osec->thunks.push_back(extender.isec);
        }
      }
    }
  }

  ExtensionStats stats = verifyPlan(state);
  log(state.policy.name + " branch extender for " + parent->name + "," + name +
      ": passes = " + std::to_string(state.passes) +
      ", branch relocs = " + std::to_string(stats.branchRelocs) +
      ", island calls = " + std::to_string(stats.islandCalls) +
      ", chain calls = " + std::to_string(stats.chainCalls) +
      ", thunk calls = " + std::to_string(stats.thunkCalls) +
      ", islands = " + std::to_string(stats.islands) +
      ", thunks = " + std::to_string(stats.thunks) +
      ", verified branch26 = " + std::to_string(stats.verifiedBranches) +
      ", boundary probes = " + std::to_string(state.boundaryProbes) +
      ", inputs = " + std::to_string(state.inputs.size()) +
      ", targets = " + std::to_string(state.groups.size()) +
      ", total extenders = " + std::to_string(state.extenders.size()));

  for (TextOutputSection *osec : state.sections) {
    osec->addr = osec->hybridAddr;
    osec->hybridFinalized = true;
  }
}

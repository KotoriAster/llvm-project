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
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/TimeProfiler.h"

#define DEBUG_TYPE "lld-macho-branch-islands"

#include <algorithm>
#include <array>
#include <chrono>
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
constexpr uint64_t packingSafetyMargin = 4 * 1024 * 1024;

enum class ExtenderRole : uint8_t { Terminal, Relay, Thunk };

struct ExtensionPolicy {
  StringLiteral name;
  // Maximum island depth from the callee.
  uint32_t maxHops;
  bool allowThunk;
  bool packIslands;
};

static constexpr ExtensionPolicy extensionPolicy(BranchRangeExtensionMode mode) {
  switch (mode) {
  case BranchRangeExtensionMode::mold:
    return {"mold", 1, true, false};
  case BranchRangeExtensionMode::hybrid:
    return {"hybrid", 2, true, false};
  case BranchRangeExtensionMode::islandsSlopFree:
    return {"islands-slop-free", noExtender, false, true};
  case BranchRangeExtensionMode::thunkExact:
    return {"thunk-exact", 0, true, false};
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
  // If a saturated reservation envelope still produces an invalid island
  // graph, policies that permit thunks permanently fall back for this group.
  bool forceThunk = false;
  // Exact-layout contraction can force an otherwise direct branch onto an
  // extender path. Almost every group keeps this false, allowing planning and
  // validation to skip its sorted, provably-direct middle range.
  bool hasForcedExtenders = false;
  SmallVector<ExtensionBranch, 0> branches;
  // Thunk extenders created for this group, kept sorted by bucket (i.e. by
  // candidateVA). A thunk reaches its target in one hop, so any callsite of
  // this group within a thunk's branch range may reuse it. Sharing is scoped
  // to one group because each thunk carries the group's addend.
  SmallVector<uint32_t, 0> thunks;
  // Packing is a placement preference, not a correctness requirement. If an
  // exact-layout pass saturates with an invalid packed graph, exclude the
  // offending preferred boundaries for this group.
  SmallVector<uint32_t, 4> rejectedPackingBuckets;
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
  SmallVector<uint32_t, 0> allBuckets, packingBuckets, placementBuckets;
  std::vector<uint64_t> inputVA, candidateVA;
  std::vector<uint32_t> desiredExtra, reservedExtra, bucketHead, bucketTail;
  SmallVector<ExtensionNode, 0> extenders;
  SmallVector<ExtensionGroup, 0> groups;
};

struct ExtensionStats {
  size_t branchRelocs = 0, islandCalls = 0, chainCalls = 0, thunkCalls = 0;
  size_t islands = 0, thunks = 0, verifiedBranches = 0;
  size_t islandBuckets = 0, islandPages = 0;
};

struct InvalidPlanStats {
  size_t directCalls = 0, extenderCalls = 0, relays = 0, terminals = 0;
  size_t unresolved = 0;
  std::vector<uint8_t> islandGroups;

  bool empty() const {
    return directCalls == 0 && extenderCalls == 0 && relays == 0 &&
           terminals == 0 && unresolved == 0;
  }
};

struct ProposalStats {
  size_t islands = 0, thunks = 0, buckets = 0;
  uint64_t bytes = 0;
};

struct ReservationGrowthStats {
  size_t newBuckets = 0, grownBuckets = 0, totalBuckets = 0;
  uint64_t addedBytes = 0, totalBytes = 0;

  bool grew() const { return newBuckets != 0 || grownBuckets != 0; }
};

static bool inBranchRange(uint64_t from, uint64_t to) {
  return (target->backwardBranchRange < from
              ? from - target->backwardBranchRange
              : 0) <= to &&
         to <= from + target->forwardBranchRange;
}

static bool isDTrace(const Symbol *sym) {
  return sym->getName().starts_with("___dtrace_");
}

static size_t extenderSize(const ExtensionNode &e) {
  return e.role == ExtenderRole::Thunk ? target->thunkSize
                                       : target->islandSize;
}

static void collectExtensionBranches(ExtensionState &s) {
  TimeTraceScope timeScope("Branch extension collect");
  DenseMap<ConcatInputSection *, uint32_t> inputIndex;
  {
    TimeTraceScope timeScope("Branch extension index inputs");
    for (uint32_t i = 0; i < s.inputs.size(); ++i)
      inputIndex[s.inputs[i]] = i;
  }

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
      auto *defined = dyn_cast<Defined>(targetSym);
      auto *targetIsec =
          defined && !needsBinding(defined)
              ? dyn_cast_or_null<ConcatInputSection>(defined->isec())
              : nullptr;
      auto targetIt = inputIndex.find(targetIsec);
      if (targetIt != inputIndex.end()) {
        group.targetInputIdx = targetIt->second;
        group.targetValue = defined->value;
      }
    }
    s.groups[it->second].branches.emplace_back(&r, inputIdx);
  };

  {
    TimeTraceScope timeScope("Branch extension group relocations");
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
      llvm::sort(branches, [](const Relocation *lhs,
                             const Relocation *rhs) {
        return lhs->offset < rhs->offset;
      });
      for (Relocation *r : branches)
        collect(*r, inputIdx);
    }
  }
}

static void relayoutExtensions(ExtensionState &s,
                               ArrayRef<uint32_t> reservedExtra = {}) {
  TimeTraceScope timeScope("Branch extension relayout");
  uint64_t addr = s.sections.front()->addr;
  uint64_t groupSize = 0;
  uint32_t inputIdx = 0;
  for (TextOutputSection *owner : s.sections) {
    groupSize = alignToPowerOf2(groupSize, owner->align);
    uint64_t ownerBase = groupSize;
    uint64_t ownerSize = 0;
    for (ConcatInputSection *isec : owner->inputs) {
      uint32_t i = inputIdx++;
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

  // islands-slop-free first tries globally shared, range-spaced boundaries.
  // These are preferred coordinates only: no space is reserved at them. Do
  // not force either endpoint into a preferred set; allBuckets retains both
  // as exact fallbacks.
  if (s.policy.packIslands && s.inputs.size() > 2) {
    uint64_t branchRange =
        std::min(target->forwardBranchRange, target->backwardBranchRange);
    if (branchRange > packingSafetyMargin) {
      uint64_t stride = branchRange - packingSafetyMargin;
      uint64_t nextVA = addSat(s.candidateVA.front(), stride);
      while (nextVA < s.candidateVA.back()) {
        auto it = llvm::lower_bound(s.candidateVA, nextVA);
        uint32_t bucket = it - s.candidateVA.begin();
        if (it == s.candidateVA.end() || *it > nextVA) {
          if (bucket == 0)
            break;
          --bucket;
        }
        if (bucket != 0 && bucket + 1 < s.inputs.size() &&
            (s.packingBuckets.empty() ||
             s.packingBuckets.back() != bucket))
          s.packingBuckets.push_back(bucket);
        uint64_t followingVA = addSat(nextVA, stride);
        if (followingVA == nextVA)
          break;
        nextVA = followingVA;
      }
    }
  }

  // Keep the existing ~1 MiB fast search without its former unconditional
  // first/last entries. The exhaustive layer supplies those endpoints.
  uint64_t lastVA = s.candidateVA.front();
  for (uint32_t i = 1; i + 1 < s.inputs.size(); ++i) {
    if (s.candidateVA[i] - lastVA < boundarySpacing)
      continue;
    s.placementBuckets.push_back(i);
    lastVA = s.candidateVA[i];
  }
}

static bool isRejectedPackingBucket(const ExtensionState &s,
                                    uint32_t groupIdx, uint32_t bucket) {
  return llvm::binary_search(s.groups[groupIdx].rejectedPackingBuckets,
                             bucket);
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
  uint32_t hopsFromTarget = role == ExtenderRole::Terminal ? 1 : 0;
  if (role == ExtenderRole::Relay) {
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

static std::optional<size_t>
findExtenderBoundary(ExtensionState &s, ArrayRef<uint32_t> buckets,
                     uint64_t callVA, uint64_t anchorVA, uint32_t groupIdx) {
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
      if (isRejectedPackingBucket(s, groupIdx, bucket)) {
        ++it;
        continue;
      }
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
    if (isRejectedPackingBucket(s, groupIdx, bucket))
      continue;
    uint64_t va = plannedExtenderVA(s, bucket);
    if (anchorVA < va && va < callVA && inBranchRange(va, anchorVA))
      return bucket;
  }
  return std::nullopt;
}

static std::optional<size_t>
findExtenderBoundary(ExtensionState &s, uint64_t callVA, uint64_t anchorVA,
                     uint32_t groupIdx) {
  if (s.policy.packIslands)
    if (auto bucket = findExtenderBoundary(
            s, s.packingBuckets, callVA, anchorVA, groupIdx))
      return bucket;
  if (auto bucket = findExtenderBoundary(
          s, s.placementBuckets, callVA, anchorVA, groupIdx))
    return bucket;
  return findExtenderBoundary(s, s.allBuckets, callVA, anchorVA, groupIdx);
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
    std::optional<size_t> bucket =
        findExtenderBoundary(s, callVA, anchorVA, groupIdx);
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

template <class StateT, class GroupT>
static auto directBranchBounds(StateT &s, GroupT &group, uint64_t targetVA) {
  uint64_t firstDirectVA = subSat(targetVA, target->forwardBranchRange);
  uint64_t pastDirectVA = addSat(targetVA, target->backwardBranchRange);
  auto first = llvm::lower_bound(
      group.branches, firstDirectVA,
      [&](const ExtensionBranch &branch, uint64_t va) {
        return branchVA(s, branch) < va;
      });
  auto last = llvm::upper_bound(
      group.branches, pastDirectVA,
      [&](uint64_t va, const ExtensionBranch &branch) {
        return va < branchVA(s, branch);
      });
  return std::make_pair(first, last);
}

static void planGroup(ExtensionState &s, uint32_t groupIdx) {
  ExtensionGroup &group = s.groups[groupIdx];
  std::optional<uint64_t> targetVA = extensionTargetVA(s, group);
  if (!targetVA || group.forceThunk) {
    if (!s.policy.allowThunk) {
      error("cannot resolve island-only branch target " +
            toString(*group.target));
      for (ExtensionBranch &branch : group.branches)
        branch.extender = noExtender;
      return;
    }
    for (ExtensionBranch &branch : group.branches) {
      if (targetVA && !branch.forceExtender &&
          inBranchRange(branchVA(s, branch), *targetVA)) {
        branch.extender = noExtender;
        continue;
      }
      branch.extender = placeThunk(s, groupIdx, branchVA(s, branch));
    }
    return;
  }

  // Sweep only the out-of-range tails. Branches are sorted by address, so the
  // potentially very large middle interval is known to be direct without
  // visiting every relocation again on every fixed-point pass.
  auto [firstDirect, lastDirect] = directBranchBounds(s, group, *targetVA);
  if (group.hasForcedExtenders) {
    firstDirect = lastDirect = llvm::lower_bound(
        group.branches, *targetVA,
        [&](const ExtensionBranch &branch, uint64_t va) {
          return branchVA(s, branch) < va;
        });
  }
  uint32_t lowerIsland = noExtender;
  for (auto it = std::make_reverse_iterator(firstDirect);
       it != group.branches.rend(); ++it)
    planBranch(s, groupIdx, *it, *targetVA, lowerIsland);
  uint32_t upperIsland = noExtender;
  for (auto it = lastDirect; it != group.branches.end(); ++it)
    planBranch(s, groupIdx, *it, *targetVA, upperIsland);
}

static InvalidPlanStats validateExtensionPlan(const ExtensionState &s) {
  InvalidPlanStats invalid;
  invalid.islandGroups.resize(s.groups.size());
  std::vector<std::optional<uint64_t>> targetVAs(s.groups.size());
  parallelFor(0, s.groups.size(), [&](size_t groupIdx) {
    targetVAs[groupIdx] = extensionTargetVA(s, s.groups[groupIdx]);
  });

  for (const ExtensionNode &extender : s.extenders) {
    if (extender.role == ExtenderRole::Thunk)
      continue;
    if (extender.role == ExtenderRole::Relay) {
      if (extender.relayTo >= s.extenders.size() ||
          !inBranchRange(extender.layoutVA,
                         s.extenders[extender.relayTo].layoutVA)) {
        ++invalid.relays;
        invalid.islandGroups[extender.group] = true;
      }
      continue;
    }
    std::optional<uint64_t> targetVA = targetVAs[extender.group];
    if (!targetVA) {
      if (!isDTrace(s.groups[extender.group].target)) {
        ++invalid.unresolved;
        invalid.islandGroups[extender.group] = true;
      }
      continue;
    }
    if (!inBranchRange(extender.layoutVA, *targetVA)) {
      ++invalid.terminals;
      invalid.islandGroups[extender.group] = true;
    }
  }

  struct alignas(64) ValidationShard {
    size_t directCalls = 0;
    size_t extenderCalls = 0;
    size_t unresolved = 0;
  };
  constexpr size_t shardCount = 256;
  std::array<ValidationShard, shardCount> shards;
  parallelFor(0, shardCount, [&](size_t shardIdx) {
    size_t begin = s.groups.size() * shardIdx / shardCount;
    size_t end = s.groups.size() * (shardIdx + 1) / shardCount;
    ValidationShard &shard = shards[shardIdx];
    for (size_t groupIdx = begin; groupIdx < end; ++groupIdx) {
      const ExtensionGroup &group = s.groups[groupIdx];
      auto validate = [&](const ExtensionBranch &branch) {
        if (branch.extender != noExtender) {
          if (branch.extender >= s.extenders.size() ||
              !inBranchRange(branchVA(s, branch),
                             s.extenders[branch.extender].layoutVA)) {
            ++shard.extenderCalls;
            if (branch.extender >= s.extenders.size() ||
                s.extenders[branch.extender].role != ExtenderRole::Thunk)
              invalid.islandGroups[groupIdx] = true;
          }
          return;
        }
        if (!targetVAs[groupIdx]) {
          if (!isDTrace(group.target))
            ++shard.unresolved;
          return;
        }
        if (!inBranchRange(branchVA(s, branch), *targetVAs[groupIdx]))
          ++shard.directCalls;
      };
      if (!targetVAs[groupIdx] || group.hasForcedExtenders) {
        for (const ExtensionBranch &branch : group.branches)
          validate(branch);
        continue;
      }
      auto [firstDirect, lastDirect] =
          directBranchBounds(s, group, *targetVAs[groupIdx]);
      for (auto it = group.branches.begin(); it != firstDirect; ++it)
        validate(*it);
      for (auto it = lastDirect; it != group.branches.end(); ++it)
        validate(*it);
    }
  });
  for (const ValidationShard &shard : shards) {
    invalid.directCalls += shard.directCalls;
    invalid.extenderCalls += shard.extenderCalls;
    invalid.unresolved += shard.unresolved;
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
      group.hasForcedExtenders = true;
      ++promoted;
    }
  }
  return promoted;
}

static size_t forceInvalidIslandGroups(ExtensionState &s,
                                       const InvalidPlanStats &invalid) {
  if (!s.policy.allowThunk)
    return 0;

  size_t forced = 0;
  for (size_t groupIdx = 0; groupIdx < s.groups.size(); ++groupIdx) {
    ExtensionGroup &group = s.groups[groupIdx];
    if (!invalid.islandGroups[groupIdx] || group.forceThunk)
      continue;
    group.forceThunk = true;
    if (forced < 8)
      log(s.policy.name + " branch extender forcing thunk fallback for " +
          toString(*group.target));
    ++forced;
  }
  return forced;
}

static size_t invalidIslandGroupCount(const InvalidPlanStats &invalid) {
  return llvm::count(invalid.islandGroups, uint8_t(1));
}

static size_t rejectInvalidPackingBuckets(ExtensionState &s,
                                          const InvalidPlanStats &invalid) {
  if (!s.policy.packIslands || s.packingBuckets.empty())
    return 0;

  size_t rejected = 0;
  for (const ExtensionNode &extender : s.extenders) {
    if (extender.role == ExtenderRole::Thunk ||
        !invalid.islandGroups[extender.group] ||
        !llvm::binary_search(s.packingBuckets, extender.bucket))
      continue;
    auto &buckets = s.groups[extender.group].rejectedPackingBuckets;
    if (llvm::is_contained(buckets, extender.bucket))
      continue;
    buckets.push_back(extender.bucket);
    ++rejected;
  }
  for (ExtensionGroup &group : s.groups)
    llvm::sort(group.rejectedPackingBuckets);
  return rejected;
}

static void resetExtensionPlan(ExtensionState &s) {
  s.extenders.clear();
  std::fill(s.desiredExtra.begin(), s.desiredExtra.end(), 0);
  std::fill(s.bucketHead.begin(), s.bucketHead.end(), noExtender);
  std::fill(s.bucketTail.begin(), s.bucketTail.end(), noExtender);
  for (ExtensionGroup &group : s.groups) {
    group.thunks.clear();
    for (ExtensionBranch &branch : group.branches)
      branch.extender = noExtender;
  }
}

static ProposalStats summarizeProposal(const ExtensionState &s) {
  ProposalStats stats;
  for (const ExtensionNode &extender : s.extenders) {
    if (extender.role == ExtenderRole::Thunk)
      ++stats.thunks;
    else
      ++stats.islands;
  }
  for (uint32_t bytes : s.desiredExtra) {
    if (bytes == 0)
      continue;
    ++stats.buckets;
    stats.bytes += bytes;
  }
  return stats;
}

static ReservationGrowthStats growReservation(ExtensionState &s) {
  ReservationGrowthStats stats;
  for (size_t i = 0; i != s.reservedExtra.size(); ++i) {
    uint32_t oldBytes = s.reservedExtra[i];
    uint32_t desiredBytes = s.desiredExtra[i];
    if (oldBytes < desiredBytes) {
      if (oldBytes == 0)
        ++stats.newBuckets;
      else
        ++stats.grownBuckets;
      stats.addedBytes += desiredBytes - oldBytes;
      s.reservedExtra[i] = desiredBytes;
    }
    if (s.reservedExtra[i] != 0) {
      ++stats.totalBuckets;
      stats.totalBytes += s.reservedExtra[i];
    }
  }
  return stats;
}

static bool runExtensionFixedPoint(ExtensionState &s) {
  TimeTraceScope timeScope("Branch extension fixed point");
  constexpr size_t maxPasses = 30;
  relayoutExtensions(s, s.reservedExtra);
  initializePlacementBuckets(s);
  for (s.passes = 1; s.passes <= maxPasses; ++s.passes) {
    auto passStart = std::chrono::steady_clock::now();
    size_t probesBefore = s.boundaryProbes;
    {
      TimeTraceScope timeScope("Branch extension plan");
      resetExtensionPlan(s);
      for (uint32_t groupIdx = 0; groupIdx < s.groups.size(); ++groupIdx)
        planGroup(s, groupIdx);
    }

    relayoutExtensions(s);
    InvalidPlanStats invalid;
    {
      TimeTraceScope timeScope("Branch extension validate");
      invalid = validateExtensionPlan(s);
    }
    if (errorHandler().verbose) {
      ProposalStats proposal = summarizeProposal(s);
      auto passMillis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - passStart)
                            .count();
      log(formatv("{0} branch extender pass {1}: time = {2} ms, proposed "
                  "extenders = {3}, islands = {4}, thunks = {5}, proposal "
                  "buckets = {6}, proposal bytes = {7}, boundary probes = {8}",
                  s.policy.name, s.passes, passMillis, s.extenders.size(),
                  proposal.islands, proposal.thunks, proposal.buckets,
                  proposal.bytes, s.boundaryProbes - probesBefore)
              .str());
    }
    if (invalid.empty())
      return true;
    log(formatv("{0} branch extender invalid plan: direct calls = {1}, "
                "extender calls = {2}, relays = {3}, terminals = {4}, "
                "unresolved = {5}",
                s.policy.name, invalid.directCalls, invalid.extenderCalls,
                invalid.relays, invalid.terminals, invalid.unresolved)
            .str());
    ReservationGrowthStats growth;
    {
      TimeTraceScope timeScope("Branch extension converge");
      growth = growReservation(s);
    }
    log(formatv("{0} branch extender reservation growth: new buckets = {1}, "
                "grown buckets = {2}, added bytes = {3}, total buckets = {4}, "
                "total bytes = {5}",
                s.policy.name, growth.newBuckets, growth.grownBuckets,
                growth.addedBytes, growth.totalBuckets, growth.totalBytes)
            .str());
    bool madeProgress = growth.grew();
    // A single bad island group at the tail of hybrid convergence otherwise
    // makes every subsequent pass rescan the complete branch graph while a
    // few unrelated reservation bytes settle. Hybrid is explicitly allowed
    // to use a thunk for this case, and forceInvalidIslandGroups is the same
    // fallback used after reservation growth stops. Apply it as soon as the
    // exact plan's only remaining failure is one island group.
    if (s.policy.allowThunk && invalid.directCalls == 0 &&
        invalid.extenderCalls == 0 && invalid.unresolved == 0 &&
        invalidIslandGroupCount(invalid) == 1) {
      size_t forced = forceInvalidIslandGroups(s, invalid);
      if (forced != 0)
        madeProgress = true;
    }
    if (!madeProgress) {
      size_t promoted = forceInvalidDirectBranches(s);
      if (promoted != 0) {
        log(s.policy.name + " branch extender promoted direct calls = " +
                  std::to_string(promoted));
        madeProgress = true;
      }
      size_t rejected = rejectInvalidPackingBuckets(s, invalid);
      if (rejected != 0) {
        log(s.policy.name + " branch extender rejected packed buckets = " +
            std::to_string(rejected));
        madeProgress = true;
      }
      size_t forced = forceInvalidIslandGroups(s, invalid);
      if (forced != 0) {
        log(s.policy.name + " branch extender forced thunk groups = " +
            std::to_string(forced));
        madeProgress = true;
      }
    }
    if (!madeProgress)
      return false;
    relayoutExtensions(s, s.reservedExtra);
  }
  s.passes = maxPasses;
  return false;
}

static void materializeExtenders(ExtensionState &s) {
  TimeTraceScope timeScope("Branch extension materialize");
  size_t sequence = 0;
  for (ExtensionNode &extender : s.extenders) {
    auto *owner = cast<TextOutputSection>(s.inputs[extender.bucket]->parent);
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
  if (isDTrace(sym))
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
  SmallVector<uint32_t, 0> islandBuckets;
  SmallVector<uint64_t, 0> islandPages;
  uint64_t pageSize = target->getPageSize();
  for (const ExtensionNode &extender : s.extenders) {
    if (extender.role == ExtenderRole::Thunk) {
      ++stats.thunks;
      continue;
    }
    ++stats.islands;
    islandBuckets.push_back(extender.bucket);
    uint64_t begin = extender.isec->getVA();
    uint64_t end = begin + extender.isec->getSize() - 1;
    for (uint64_t page = begin / pageSize; page <= end / pageSize; ++page)
      islandPages.push_back(page);
    Relocation &r = extender.isec->relocs[0];
    verifyEdge(s, extender.isec->getVA(), cast<Symbol *>(r.referent), r.addend,
               "island", stats);
  }
  llvm::sort(islandBuckets);
  stats.islandBuckets = llvm::unique(islandBuckets) - islandBuckets.begin();
  llvm::sort(islandPages);
  stats.islandPages = llvm::unique(islandPages) - islandPages.begin();

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
    for (TextOutputSection *osec : state.sections)
      for (ConcatInputSection *isec : osec->inputs)
        state.inputs.push_back(isec);
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
    fatal(state.policy.name + " branch extender did not converge after " +
          std::to_string(state.passes) + " passes");

  materializeExtenders(state);
  rewriteBranches(state);

  {
    TimeTraceScope timeScope("Chain layout");
    uint64_t groupSize = 0;
    uint32_t inputIdx = 0;
    for (TextOutputSection *osec : state.sections) {
      groupSize = alignToPowerOf2(groupSize, osec->align);
      osec->hybridAddr = addr + groupSize;
      osec->addr = osec->hybridAddr;
      osec->size = 0;
      osec->fileSize = 0;
      auto finalize = [&](ConcatInputSection *isec) {
        osec->finalizeOne(isec);
        groupSize = osec->hybridAddr - addr + osec->size;
      };
      for (ConcatInputSection *isec : osec->inputs) {
        uint32_t i = inputIdx++;
        finalize(isec);
        for (uint32_t extenderIdx = state.bucketHead[i];
             extenderIdx != noExtender;
             extenderIdx = state.extenders[extenderIdx].nextInBucket) {
          ExtensionNode &extender = state.extenders[extenderIdx];
          finalize(extender.isec);
          osec->thunks.push_back(extender.isec);
        }
      }
    }
  }

  // runExtensionFixedPoint already validates the exact proposed layout.
  // Rewalking every branch after materialization is a useful diagnostic audit
  // but duplicates tens of millions of range checks in normal links.
  if (errorHandler().verbose) {
    TimeTraceScope timeScope("Branch extension verify");
    ExtensionStats stats = verifyPlan(state);
    log(formatv("{0} branch extender for {1},{2}: passes = {3}, branch relocs = "
                "{4}, island calls = {5}, chain calls = {6}, thunk calls = {7}, "
                "islands = {8}, thunks = {9}, verified branch26 = {10}, "
                "boundary probes = {11}, inputs = {12}, targets = {13}, total "
                "extenders = {14}, island buckets = {15}, island pages = {16}",
                state.policy.name, parent->name, name, state.passes,
                stats.branchRelocs, stats.islandCalls, stats.chainCalls,
                stats.thunkCalls, stats.islands, stats.thunks,
                stats.verifiedBranches, state.boundaryProbes,
                state.inputs.size(), state.groups.size(), state.extenders.size(),
                stats.islandBuckets, stats.islandPages)
            .str());
  }

  for (TextOutputSection *osec : state.sections) {
    osec->addr = osec->hybridAddr;
    osec->hybridFinalized = true;
  }
}

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
#include "SyntheticSections.h"
#include "Target.h"
#include "lld/Common/CommonLinkerContext.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorHandling.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

using namespace llvm;
using namespace lld;
using namespace lld::macho;

namespace {
constexpr uint64_t maxVA = std::numeric_limits<uint64_t>::max();
constexpr uint32_t noExtender = std::numeric_limits<uint32_t>::max();

struct ExtensionPolicy {
  StringLiteral name;
  uint32_t maxHops;
};

struct Callsite {
  Relocation *reloc = nullptr;
  uint32_t inputIdx = 0;
  bool forceExtender = false;
};

struct Callee {
  Symbol *target = nullptr;
  int64_t addend = 0;
  uint32_t inputIdx = noExtender;
  SmallVector<Callsite, 1> callsites;
};

// Owns inputs in the half-open range [firstInputIdx, lastInputIdx).
struct OwnerInputSlice {
  TextOutputSection *section = nullptr;
  uint32_t firstInputIdx = 0;
  uint32_t lastInputIdx = 0;
};

struct FinalizerContext {
  ExtensionPolicy policy;
  SmallVector<ConcatInputSection *, 0> inputs;
  SmallVector<OwnerInputSlice, 4> ownerRuns;
  SmallVector<Callee, 0> callees;
  std::vector<uint32_t> reservedExtra;
};

enum class ExtenderKind : uint8_t { Island, Thunk };

struct ExtensionNode {
  ExtenderKind kind = ExtenderKind::Island;
  uint32_t calleeIdx = 0;
  uint32_t boundaryIdx = 0;
};

struct ExtensionProposal {
  SmallVector<ExtensionNode, 0> extenders;
  std::vector<uint32_t> routing;
  std::vector<uint32_t> desiredExtra;
  SmallVector<uint32_t, 0> coarseBoundaries;
};

struct ExtensionLayout {
  const FinalizerContext *context = nullptr;
  std::vector<uint64_t> inputVA;
  std::vector<uint64_t> extenderVA;
  std::vector<std::optional<uint64_t>> calleeTargetVA;
  uint64_t textEndVA = 0;
};

struct MaterializedExtender {
  ConcatInputSection *isec = nullptr;
  Defined *sym = nullptr;
};

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

static size_t extenderSize(const ExtensionNode &extender) {
  return extender.kind == ExtenderKind::Thunk ? target->thunkSize
                                              : target->islandSize;
}

static std::optional<uint64_t> estimateSectionVA(const ExtensionLayout &layout,
                                                 OutputSection *targetSection) {
  if (!targetSection || !targetSection->isNeeded())
    return std::nullopt;

  assert(layout.context);
  const FinalizerContext &ctx = *layout.context;
  assert(!ctx.ownerRuns.empty());
  uint64_t va = layout.textEndVA;
  bool pastText = false;
  OutputSection *lastText = ctx.ownerRuns.back().section;
  for (OutputSection *osec :
       ctx.ownerRuns.front().section->parent->getSections()) {
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

static std::optional<uint64_t> nonLocalTargetVA(const ExtensionLayout &layout,
                                                Symbol *sym, int64_t addend) {
  if (sym->isInStubs()) {
    if (in.stubs->isFinal)
      return addVA(sym->getStubVA(), addend);
    if (auto stubsVA = estimateSectionVA(layout, in.stubs))
      return addVA(
          addVA(*stubsVA, uint64_t(sym->stubsIndex) * target->stubSize),
          addend);
  }
  auto *defined = dyn_cast<Defined>(sym);
  if (in.objcStubs && in.objcStubs->isNeeded() && defined &&
      defined->isec() == in.objcStubs->isec)
    if (auto objcStubsVA = estimateSectionVA(layout, in.objcStubs))
      return addVA(addVA(*objcStubsVA, defined->value), addend);
  if (defined && defined->isAbsolute())
    return addVA(defined->getVA(), addend);
  return std::nullopt;
}

static std::optional<uint64_t> targetVA(const ExtensionLayout &layout,
                                        const Callee &callee) {
  if (auto va = nonLocalTargetVA(layout, callee.target, callee.addend))
    return va;
  if (callee.inputIdx == noExtender)
    return std::nullopt;
  assert(callee.inputIdx < layout.inputVA.size());
  auto *defined = cast<Defined>(callee.target);
  return addVA(addVA(layout.inputVA[callee.inputIdx], defined->value),
               callee.addend);
}

static void resolveCalleeTargets(const FinalizerContext &ctx,
                                 ExtensionLayout &layout) {
  layout.calleeTargetVA.reserve(ctx.callees.size());
  for (const Callee &callee : ctx.callees)
    layout.calleeTargetVA.push_back(targetVA(layout, callee));
}

// Apply the common owner/input layout rules. The callback inserts the
// phase-specific reservation or exact extenders after each input.
template <typename PlaceAfterInput>
static ExtensionLayout layoutInputs(const FinalizerContext &ctx,
                                    size_t extenderCount,
                                    PlaceAfterInput placeAfterInput) {
  assert(!ctx.ownerRuns.empty());
  assert(ctx.ownerRuns.front().section);
  assert(ctx.reservedExtra.size() == ctx.inputs.size());

  ExtensionLayout layout;
  layout.context = &ctx;
  layout.inputVA.resize(ctx.inputs.size());
  layout.extenderVA.resize(extenderCount);

  uint64_t addr = ctx.ownerRuns.front().section->addr;
  uint64_t groupSize = 0;
  uint32_t expectedFirstInputIdx = 0;
  for (const OwnerInputSlice &run : ctx.ownerRuns) {
    assert(run.section);
    assert(run.firstInputIdx == expectedFirstInputIdx);
    assert(run.firstInputIdx <= run.lastInputIdx);
    assert(run.lastInputIdx <= ctx.inputs.size());

    groupSize = alignToPowerOf2(groupSize, run.section->align);
    uint64_t ownerBase = groupSize;
    uint64_t ownerSize = 0;
    ArrayRef<ConcatInputSection *> ownedInputs(ctx.inputs);
    ownedInputs = ownedInputs.slice(run.firstInputIdx,
                                    run.lastInputIdx - run.firstInputIdx);
    for (auto [relativeIdx, isec] : llvm::enumerate(ownedInputs)) {
      assert(isec);
      uint32_t inputIdx =
          run.firstInputIdx + static_cast<uint32_t>(relativeIdx);
      ownerSize = alignToPowerOf2(ownerSize, isec->align);
      layout.inputVA[inputIdx] = addr + ownerBase + ownerSize;
      ownerSize += isec->getSize();
      placeAfterInput(layout, inputIdx, addr + ownerBase, ownerSize);
      groupSize = ownerBase + ownerSize;
    }
    expectedFirstInputIdx = run.lastInputIdx;
  }
  assert(expectedFirstInputIdx == ctx.inputs.size());

  layout.textEndVA = addr + groupSize;
  resolveCalleeTargets(ctx, layout);
  return layout;
}

// Lay out the frozen input topology with the current high-water reservation
// inserted after each input boundary.
static ExtensionLayout layoutByReservation(const FinalizerContext &ctx) {
  return layoutInputs(
      ctx, /*extenderCount=*/0,
      [&](ExtensionLayout &, uint32_t inputIdx, uint64_t, uint64_t &ownerSize) {
        uint32_t reserved = ctx.reservedExtra[inputIdx];
        if (reserved == 0)
          return;
        ownerSize = alignToPowerOf2(ownerSize, 4);
        ownerSize += reserved;
      });
}

// Lay out only the boundary-normalized extenders in this proposal.
static ExtensionLayout layoutByProposal(const FinalizerContext &ctx,
                                        const ExtensionProposal &proposal) {
  for (auto [extenderIdx, extender] : llvm::enumerate(proposal.extenders)) {
    assert(extender.boundaryIdx < ctx.inputs.size());
    assert(extenderIdx == 0 ||
           proposal.extenders[extenderIdx - 1].boundaryIdx <=
               extender.boundaryIdx);
  }

  size_t extenderIdx = 0;
  ExtensionLayout layout = layoutInputs(
      ctx, proposal.extenders.size(),
      [&](ExtensionLayout &layout, uint32_t inputIdx, uint64_t ownerVA,
          uint64_t &ownerSize) {
        size_t firstExtenderIdx = extenderIdx;
        for (auto [offset, extender] : llvm::enumerate(
                 llvm::drop_begin(proposal.extenders, firstExtenderIdx))) {
          if (extender.boundaryIdx != inputIdx)
            break;
          extenderIdx = firstExtenderIdx + offset;
          ownerSize = alignToPowerOf2(ownerSize, 4);
          layout.extenderVA[extenderIdx] = ownerVA + ownerSize;
          ownerSize += extenderSize(extender);
          ++extenderIdx;
        }
      });
  assert(extenderIdx == proposal.extenders.size());
  return layout;
}
} // namespace

void TextOutputSection::finalizeWithExtenders(BranchRangeExtensionMode mode) {
  if (hybridFinalized)
    return;
  FinalizerContext ctx(mode);

  for (pass = 0; pass < 30; ++pass) {
    reservedLayout = layoutByReservation(ctx); // loose, padded
    proposal = planExtensionProposal(reservedLayout);
    proposalLayout = layoutByProposal(ctx, proposal); // exact
    if (validateExtensionProposal(proposal, proposalLayout))
      break; // self-valid

    bool updated = ctx.updateReservation(proposal.desiredExtra);
    if (!updated)
      updated = forceInvalidDirectBranches(proposal, proposalLayout);
    if (!updated)
      fail();
  }

  materializeExtenders(proposal); // create synthetic sections/symbols
  rewriteBranches(proposal, proposalLayout);
  emitInBoundaryOrder(proposal); // assert exact-layout addresses match
}

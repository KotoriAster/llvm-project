//===- TextOutputSegment.h --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_MACHO_TEXT_OUTPUT_SEGMENT_H
#define LLD_MACHO_TEXT_OUTPUT_SEGMENT_H

#include "Target.h"
#include "lld/Common/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <tuple>

namespace lld::macho {

class ConcatInputSection;
class OutputSection;
class TextOutputSection;
struct Relocation;
struct Callee;
struct Extender;
struct RelocRewrite;

using CalleeKey = std::tuple<const void *, uint64_t, int64_t>;

bool canHostExtenders(const OutputSection *osec);

/*
Boundaries are insertion points between isecs :
current isec (i)    planned extenders         alignment padding    next isec
(i+1)
|-------------------|-----boundary (i)--------|....................|
inputVA             getInputEndVA()           next extender VA    next inputVA
*/
struct Boundary {
  uint64_t getInputEndVA() const;
  uint64_t getNextExtenderVA(ExtenderKind kind) const;
  bool operator<(const Boundary &rhs) const { return ordinal < rhs.ordinal; }
  ConcatInputSection *isec;
  uint32_t ordinal;
  uint32_t reservedSize = 0;
  uint32_t plannedSize = 0;
  uint64_t inputVA = 0;
};

enum class BoundaryPreference { lowest, highest };

// Encapsulates the physical ordering, lookup rules, and state transitions for
// all insertion seams in one TextOutputSegment.
class BoundaryTable {
public:
  explicit BoundaryTable(ArrayRef<TextOutputSection *> sections);

  ArrayRef<Boundary> entries() const { return boundaries; }
  MutableArrayRef<Boundary> entries() { return boundaries; }
  bool empty() const { return boundaries.empty(); }
  size_t size() const { return boundaries.size(); }

  Boundary *findExtenderPlacementInRange(uint64_t lowVA, uint64_t highVA,
                                         BoundaryPreference preference,
                                         ExtenderKind kind);
  void initializeExtenderPlacementBoundaries();
  void resetPlannedSpace() {
    for (auto &boundary : boundaries)
      boundary.plannedSize = 0;
  }
  bool growReservationsToPlannedSpace();

  void forEachInputBoundary(
      ArrayRef<TextOutputSection *> sections,
      llvm::function_ref<void(TextOutputSection &, Boundary &)> fn);

private:
  SmallVector<Boundary, 0> boundaries;
  SmallVector<uint32_t, 0> coarseBoundaryOrdinals;
};

// The maximal consecutive run of compatible text output sections finalized as
// one branch-range-extension unit.
class TextOutputSegment {
public:
  explicit TextOutputSegment(TextOutputSection &first);

  TextOutputSegment(const TextOutputSegment &) = delete;
  TextOutputSegment &operator=(const TextOutputSegment &) = delete;

  size_t finalize();
  ArrayRef<TextOutputSection *> getSections() const {
    return textOutputSections;
  }

private:
  enum class LayoutKind { reservation, proposal };

  TextOutputSection &first;
  const uint32_t maxHops;
  const ExtenderKind chainKind;
  const ExtenderKind fallbackKind;
  SmallVector<TextOutputSection *, 4> textOutputSections;
  BoundaryTable boundaries;
  llvm::DenseMap<ConcatInputSection *, Boundary *> inputBoundaries;
  llvm::DenseMap<CalleeKey, Callee *> calleesByKey;
  SmallVector<Extender *, 0> extenders;
  SmallVector<RelocRewrite *, 0> relocRewrites;
  llvm::DenseMap<OutputSection *, uint64_t> estimatedPostSegmentSecVAs;
  uint64_t textEndVA = 0;
  llvm::DenseSet<const Relocation *> requiredExtenderRelocs;

  static SmallVector<TextOutputSection *, 4>
  collectConsecutiveOutputSections(TextOutputSection &first);
  void initializeCallee(Callee &, Symbol *, int64_t);
  std::optional<uint64_t> estimatePostTextSectionVA(OutputSection *);
  void walkLayout(LayoutKind);
  Boundary *findChainBoundary(uint64_t callVA, uint64_t anchorVA);
  Extender *planBranchExtensionExtender(Callee &, Boundary &, ExtenderKind);
  Extender *findReusableExtender(Callee &, uint64_t) const;
  Extender *placeChainExtender(Callee &, uint64_t);
  Extender *placeFallbackExtender(Callee &, uint64_t);
  void planBranchExtension();
  bool recoverFromRejectedProposal();
  template <typename Visitor> bool forEachIslandEdge(Visitor) const;
  bool isLayoutValid() const;
  void materializeAcceptedPlan();
};

} // namespace lld::macho

#endif

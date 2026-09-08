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
#include "llvm/ADT/SmallVector.h"

#include <cstddef>
#include <cstdint>

namespace lld::macho {

class ConcatInputSection;
class OutputSection;
class TextOutputSection;
struct Extender;

/// Models an insertion point between input sections.
///
/// \code
/// current isec (i)  planned extenders   padding      next isec (i+1)
/// |----------------|--- boundary (i) ---|............|
/// inputVA          getInputEndVA()     next extender VA  next inputVA
/// \endcode
struct Boundary {
  Boundary(ConcatInputSection *isec, TextOutputSection *section)
      : isec(isec), section(section) {}

  uint64_t getInputEndVA() const;
  uint64_t getNextExtenderVA(ExtenderKind kind) const;
  ConcatInputSection *isec;
  TextOutputSection *section;
  uint32_t reservedSize = 0;
  uint32_t plannedSize = 0;
  uint64_t inputVA = 0;
  SmallVector<Extender *, 2> plannedExtenders;
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
  Boundary *findExtenderPlacementNear(uint64_t callVA, ExtenderKind kind);
  void resetPlannedSpace() {
    for (auto &boundary : boundaries) {
      boundary.plannedSize = 0;
      boundary.plannedExtenders.clear();
    }
  }

private:
  // Search coarse boundaries before the full table to concentrate extenders at
  // shared boundaries for locality.
  SmallVector<Boundary, 0> boundaries;
  SmallVector<Boundary *, 0> coarseBoundaries;
};

/// A maximal consecutive run of text output sections that can host extenders.
///
/// The run starts at `first` and ends immediately before the next output
/// section that cannot host extenders.
///
/// Extender insertion can shift every later input and output section in the
/// run, so all member sections are planned as one branch-range-extension unit.
/// `planBranchRangeExtension()` stores the accepted extender placements for
/// Writer to finalize and returns the iterator to the first section after the
/// run, allowing Writer to continue iterating over the remaining output
/// sections.
class TextOutputSegment {
public:
  using iterator = ArrayRef<OutputSection *>::iterator;

  TextOutputSegment(iterator first, iterator end);

  TextOutputSegment(const TextOutputSegment &) = delete;
  TextOutputSegment &operator=(const TextOutputSegment &) = delete;

  iterator planBranchRangeExtension();
  ArrayRef<TextOutputSection *> getSections() const {
    return textOutputSections;
  }
  size_t size() const { return textOutputSections.size(); }

private:
  SmallVector<TextOutputSection *, 4> textOutputSections;

  // Iterator to fist osec behind current TextOutputSegment
  iterator next;
};

} // namespace lld::macho

#endif

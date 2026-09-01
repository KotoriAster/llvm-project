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
class TextOutputSection;
struct Extender;

/*
Boundaries are insertion points between isecs :
current isec (i)    planned extenders         alignment padding    next isec
(i+1)
|-------------------|-----boundary (i)--------|....................|
inputVA             getInputEndVA()           next extender VA    next inputVA
*/
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
  void initializeExtenderPlacementBoundaries();
  void resetPlannedSpace() {
    for (auto &boundary : boundaries) {
      boundary.plannedSize = 0;
      boundary.plannedExtenders.clear();
    }
  }

private:
  // The table is populated once by the constructor and never reordered, so
  // pointers handed to callees and coarse searches remain stable.
  SmallVector<Boundary, 0> boundaries;
  SmallVector<Boundary *, 0> coarseBoundaries;
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
  size_t size() const { return textOutputSections.size(); }

private:
  TextOutputSection &first;
  SmallVector<TextOutputSection *, 4> textOutputSections;
  BoundaryTable boundaries;

  static SmallVector<TextOutputSection *, 4>
  collectConsecutiveOutputSections(TextOutputSection &first);
};

} // namespace lld::macho

#endif

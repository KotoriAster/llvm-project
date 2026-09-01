//===- ConcatOutputSection.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_MACHO_CONCAT_OUTPUT_SECTION_H
#define LLD_MACHO_CONCAT_OUTPUT_SECTION_H

#include "InputSection.h"
#include "OutputSection.h"
#include "lld/Common/LLVM.h"
#include "llvm/ADT/MapVector.h"

namespace lld::macho {

class Defined;

// Linking multiple files will inevitably mean resolving sections in different
// files that are labeled with the same segment and section name. This class
// contains all such sections and writes the data from each section sequentially
// in the final binary.
class ConcatOutputSection : public OutputSection {
public:
  explicit ConcatOutputSection(StringRef name,
                               OutputSection::Kind kind = ConcatKind)
      : OutputSection(kind, name) {}

  const ConcatInputSection *firstSection() const { return inputs.front(); }
  const ConcatInputSection *lastSection() const { return inputs.back(); }
  bool isNeeded() const override { return !inputs.empty(); }

  // These accessors will only be valid after finalizing the section
  uint64_t getSize() const override { return size; }
  uint64_t getFileSize() const override { return fileSize; }

  // Assign values to InputSection::outSecOff. In contrast to TextOutputSection,
  // which does this in its implementation of `finalize()`, we can do this
  // without `finalize()`'s sequential guarantees detailed in the block comment
  // of `OutputSection::finalize()`.
  virtual void finalizeContents();

  void addInput(ConcatInputSection *input);
  void writeTo(uint8_t *buf) const override;

  static bool classof(const OutputSection *sec) {
    return sec->kind() == ConcatKind || sec->kind() == TextKind;
  }

  static ConcatOutputSection *getOrCreateForInput(const InputSection *);

  std::vector<ConcatInputSection *> inputs;

protected:
  size_t size = 0;
  uint64_t fileSize = 0;
  void finalizeOne(ConcatInputSection *);

private:
  void finalizeFlags(InputSection *input);
};

// ConcatOutputSections that contain code (text) require special handling to
// support thunk insertion.
class TextOutputSection : public ConcatOutputSection {
public:
  struct ExtenderArtifact {
    ConcatInputSection *isec;
    Defined *sym;
  };

  struct MaterializedExtender {
    ConcatInputSection *precedingInput;
    ConcatInputSection *isec;
    uint64_t modeledVA;
  };

  explicit TextOutputSection(StringRef name)
      : ConcatOutputSection(name, TextKind) {}
  void finalizeContents() override {}
  uint64_t getSizeForAddressAssignment() const override;
  bool canHostExtenders() const override;
  void finalize() override;
  ExtenderArtifact synthesizeExtender(StringRef name, size_t size,
                                      bool externalSymbol);
  void finalizeWithExtenders(ArrayRef<MaterializedExtender>);
  ArrayRef<ConcatInputSection *> getExtenders() const { return extenders; }
  void writeTo(uint8_t *buf) const override;

  static bool classof(const OutputSection *sec) {
    return sec->kind() == TextKind;
  }

private:
  std::vector<ConcatInputSection *> extenders;
};

NamePair maybeRenameSection(NamePair key);

// Output sections are added to output segments in iteration order
// of ConcatOutputSection, so must have deterministic iteration order.
extern llvm::MapVector<NamePair, ConcatOutputSection *> concatOutputSections;

} // namespace lld::macho

#endif

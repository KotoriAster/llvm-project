//===- ConcatOutputSection.cpp --------------------------------------------===//
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
#include "lld/Common/CommonLinkerContext.h"
#include "llvm/BinaryFormat/MachO.h"

using namespace llvm;
using namespace llvm::MachO;
using namespace lld;
using namespace lld::macho;

MapVector<NamePair, ConcatOutputSection *> macho::concatOutputSections;

void ConcatOutputSection::addInput(ConcatInputSection *input) {
  assert(input->parent == this);
  if (inputs.empty()) {
    align = input->align;
    flags = input->getFlags();
  } else {
    align = std::max(align, input->align);
    finalizeFlags(input);
  }
  inputs.push_back(input);
}

void ConcatOutputSection::finalizeOne(ConcatInputSection *isec) {
  size = alignToPowerOf2(size, isec->align);
  fileSize = alignToPowerOf2(fileSize, isec->align);
  isec->outSecOff = size;
  isec->isFinal = true;
  size += isec->getSize();
  fileSize += isec->getFileSize();
}

void ConcatOutputSection::finalizeContents() {
  for (ConcatInputSection *isec : inputs)
    finalizeOne(isec);
}

TextOutputSection::ExtenderArtifact
TextOutputSection::synthesizeExtender(StringRef name, size_t extenderSize,
                                      bool externalSymbol) {
  auto *isec = makeSyntheticInputSection(parent->name, this->name);
  isec->parent = this;
  Defined *sym;
  if (externalSymbol)
    sym = symtab->addDefined(
        name, /*file=*/nullptr, isec, /*value=*/0, /*size=*/extenderSize,
        /*isWeakDef=*/false, /*isPrivateExtern=*/true,
        /*isReferencedDynamically=*/false, /*noDeadStrip=*/false,
        /*isWeakDefCanBeHidden=*/false);
  else
    sym = make<Defined>(
        name, /*file=*/nullptr, isec, /*value=*/0, /*size=*/extenderSize,
        /*isWeakDef=*/false, /*isExternal=*/false,
        /*isPrivateExtern=*/true, /*includeInSymtab=*/true,
        /*isReferencedDynamically=*/false, /*noDeadStrip=*/false,
        /*canOverrideWeakDef=*/false);
  sym->used = true;
  return {isec, sym};
}

void TextOutputSection::setExtenderPlacements(
    SmallVector<ExtenderPlacement, 0> &&placements, uint64_t expectedSize) {
  assert(extenders.empty() && "extenders have already been finalized");
  assert(!plannedSize && materializedExtenders.empty() &&
         "extender placements have already been set");
  materializedExtenders = std::move(placements);
  plannedSize = expectedSize;
}

void TextOutputSection::finalize() {
  assert(size == 0 && fileSize == 0 && extenders.empty() &&
         "text section has already been finalized");
  auto placementIt = materializedExtenders.begin();
  for (auto *isec : inputs) {
    assert(!isec->isFinal && "input section has already been finalized");
    finalizeOne(isec);
    while (placementIt != materializedExtenders.end() &&
           placementIt->insertAfter == isec) {
      finalizeOne(placementIt->extender);
      extenders.push_back(placementIt->extender);
      ++placementIt;
    }
  }
  assert(placementIt == materializedExtenders.end() &&
         "not all extender placements were consumed");
  assert((!plannedSize || size == *plannedSize) &&
         "finalized section size differs from extender plan");
}

uint64_t TextOutputSection::getSizeForAddressAssignment() const {
  if (!inputs.empty() && inputs.front()->isFinal)
    return getSize();

  uint64_t size = 0;
  for (const ConcatInputSection *isec : inputs) {
    size = alignToPowerOf2(size, isec->align);
    size += isec->getSize();
  }
  return size;
}

bool TextOutputSection::canHostExtenders() const {
  // Some input-backed __TEXT sections, such as __cstring and __const, contain
  // no instructions. Conversely, synthetic code sections cannot interleave
  // extender input sections. Only an instruction-bearing TextOutputSection
  // satisfies both requirements.
  return sections::isCodeSection(name, parent->name, flags);
}

void ConcatOutputSection::writeTo(uint8_t *buf) const {
  for (ConcatInputSection *isec : inputs)
    isec->writeTo(buf + isec->outSecOff);
}

void TextOutputSection::writeTo(uint8_t *buf) const {
  // Merge input sections from extender and ordinary vectors.
  size_t i = 0, ie = inputs.size();
  size_t t = 0, te = extenders.size();
  while (i < ie || t < te) {
    while (i < ie && (t == te || inputs[i]->empty() ||
                      inputs[i]->outSecOff < extenders[t]->outSecOff)) {
      inputs[i]->writeTo(buf + inputs[i]->outSecOff);
      ++i;
    }
    while (t < te &&
           (i == ie || extenders[t]->outSecOff < inputs[i]->outSecOff)) {
      extenders[t]->writeTo(buf + extenders[t]->outSecOff);
      ++t;
    }
  }
}

void ConcatOutputSection::finalizeFlags(InputSection *input) {
  switch (sectionType(input->getFlags())) {
  default /*type-unspec'ed*/:
    // FIXME: Add additional logic here when supporting emitting obj files.
    break;
  case S_4BYTE_LITERALS:
  case S_8BYTE_LITERALS:
  case S_16BYTE_LITERALS:
  case S_CSTRING_LITERALS:
  case S_ZEROFILL:
  case S_LAZY_SYMBOL_POINTERS:
  case S_MOD_TERM_FUNC_POINTERS:
  case S_THREAD_LOCAL_REGULAR:
  case S_THREAD_LOCAL_ZEROFILL:
  case S_THREAD_LOCAL_VARIABLES:
  case S_THREAD_LOCAL_INIT_FUNCTION_POINTERS:
  case S_THREAD_LOCAL_VARIABLE_POINTERS:
  case S_NON_LAZY_SYMBOL_POINTERS:
  case S_SYMBOL_STUBS:
    flags |= input->getFlags();
    break;
  }
}

ConcatOutputSection *
ConcatOutputSection::getOrCreateForInput(const InputSection *isec) {
  NamePair names = maybeRenameSection({isec->getSegName(), isec->getName()});
  ConcatOutputSection *&osec = concatOutputSections[names];
  if (!osec) {
    if (isec->getSegName() == segment_names::text &&
        isec->getName() != section_names::gccExceptTab &&
        isec->getName() != section_names::ehFrame)
      osec = make<TextOutputSection>(names.second);
    else
      osec = make<ConcatOutputSection>(names.second);
  }
  return osec;
}

NamePair macho::maybeRenameSection(NamePair key) {
  auto newNames = config->sectionRenameMap.find(key);
  if (newNames != config->sectionRenameMap.end())
    return newNames->second;
  return key;
}

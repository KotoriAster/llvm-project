# REQUIRES: aarch64

## Check a supported mode and the unknown-value diagnostic.

# RUN: rm -rf %t; mkdir %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t/input.o
# RUN: %lld -arch arm64 -dylib -o %t/out %t/input.o \
# RUN:   --branch-range-extension=hybrid --verbose 2> %t/link.log
# RUN: FileCheck %s --check-prefix=HYBRID --input-file=%t/link.log
# RUN: not %lld -arch arm64 -dylib -o %t/out %t/input.o \
# RUN:   --branch-range-extension=unknown 2>&1 \
# RUN:   | FileCheck %s --check-prefix=UNKNOWN

# HYBRID: finalization mode for __TEXT,__text: hybrid
# UNKNOWN: unknown --branch-range-extension=OPTION `unknown', defaulting to `thunks'

.subsections_via_symbols
.text

.globl _main
_main:
  ret

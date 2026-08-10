# REQUIRES: aarch64

## Supported exact-layout spellings select the requested policy. Removed
## spellings must be diagnosed instead of silently selecting another policy.

# RUN: rm -rf %t; mkdir %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t/input.o
# RUN: %lld -arch arm64 -dylib -o %t/out %t/input.o \
# RUN:   --branch-range-extension=hybrid --verbose 2> %t/link.log
# RUN: FileCheck %s --check-prefix=HYBRID --input-file=%t/link.log
# RUN: not %lld -arch arm64 -dylib -o %t/mold %t/input.o \
# RUN:   --branch-range-extension=mold 2>&1 | FileCheck %s --check-prefix=MOLD
# RUN: not %lld -arch arm64 -dylib -o %t/exact %t/input.o \
# RUN:   --branch-range-extension=thunk-exact 2>&1 \
# RUN:   | FileCheck %s --check-prefix=EXACT

# HYBRID: finalization mode for __TEXT,__text: hybrid
# HYBRID: hybrid branch extender for __TEXT,__text:
# MOLD: unknown --branch-range-extension=OPTION `mold', defaulting to `thunks'
# EXACT: unknown --branch-range-extension=OPTION `thunk-exact', defaulting to `thunks'

.subsections_via_symbols
.text

.globl _main
_main:
  bl _target
  ret

.globl _target
_target:
  ret

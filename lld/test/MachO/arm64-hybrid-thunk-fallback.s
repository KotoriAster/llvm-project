# REQUIRES: aarch64

## Hybrid mode falls back to a 12-byte adrp+add+br x16 thunk when even a
## two-island chain cannot bridge the call. A single atom larger than the
## branch range leaves no usable island boundary between _main and _far, so the
## hybrid planner must complete the link with a universal thunk. The same input
## is intentionally unrouteable in islands-only mode and must fail to link.

# RUN: rm -rf %t; mkdir %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t/input.o
# RUN: %lld -arch arm64 -dylib -undefined dynamic_lookup -o %t/out %t/input.o \
# RUN:   --branch-range-extension=hybrid --verbose 2> %t/link.log
# RUN: FileCheck %s --check-prefix=LOG --input-file=%t/link.log
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t/out | FileCheck %s
# RUN: not %lld -arch arm64 -dylib -undefined dynamic_lookup \
# RUN:   -o %t/islands %t/input.o \
# RUN:   --branch-range-extension=islands-slop-free 2>&1 \
# RUN:   | FileCheck %s --check-prefix=ISLANDS-ERR

# LOG: hybrid branch extender for __TEXT,__text:
# LOG-SAME: total extenders = 1
# LOG-NOT: region overflow

# ISLANDS-ERR: error: cannot route branch to _far

# CHECK: <_main>:
# CHECK-NEXT: bl 0x{{[0-9a-f]+}} <_far.thunk.0>
# CHECK: <_far.thunk.0>:
# CHECK-NEXT: adrp x16
# CHECK-NEXT: add x16, x16
# CHECK-NEXT: br x16

.subsections_via_symbols

.text
.globl _main
.p2align 2
_main:
  bl _far
  ret

## One contiguous atom past the branch range: no input-section boundary exists
## inside it for an island, so neither one island nor a two-island chain can be
## placed.
.globl _big
.p2align 2
_big:
  ret
  .space 0x8100000

.globl _far
.p2align 2
_far:
  ret

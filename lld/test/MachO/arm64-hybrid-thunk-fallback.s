# REQUIRES: aarch64

## The planner falls back to a 12-byte adrp+add+br x16 thunk when an island
## chain cannot bridge the call, regardless of the configured hop budget. A
## single atom larger than the branch range leaves no usable island boundary
## between _main and _far, so both runs must complete with a universal thunk.

# RUN: rm -rf %t; mkdir %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t/input.o
# RUN: %lld -arch arm64 -dylib -undefined dynamic_lookup -o %t/out %t/input.o \
# RUN:   --branch-range-extension-max-hops=2
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t/out | FileCheck %s
# RUN: %lld -arch arm64 -dylib -undefined dynamic_lookup \
# RUN:   -o %t/max-hops %t/input.o \
# RUN:   --branch-range-extension-max-hops=4294967295
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t/max-hops \
# RUN:   | FileCheck %s

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

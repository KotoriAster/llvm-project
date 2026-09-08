# REQUIRES: aarch64

## A branch that remains within the architectural range must stay direct.
## This keeps the planner's exact range check independent of its coarse
## placement-candidate policy.

# RUN: rm -rf %t; mkdir %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t/input.o
# RUN: %lld -arch arm64 -dylib -undefined dynamic_lookup -o %t/out %t/input.o \
# RUN:   --branch-range-extension-max-hops=2
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t/out | FileCheck %s

# CHECK: <_main>:
# CHECK-NEXT: bl 0x{{[0-9a-f]+}} <_far>

.subsections_via_symbols

.text
.globl _main
.p2align 2
_main:
  bl _far
  ret

## _far remains inside the architectural 128 MiB branch range, but close to
## its positive limit.
.space 0x1000
.globl _mid
.p2align 2
_mid:
  ret

.space 0x7fef000
.globl _far
.p2align 2
_far:
  ret

## Raise the input section alignment after the earlier subsections. The
## planner and finalizer must agree on the aligned layout.
.p2align 12
_aligned:
  ret

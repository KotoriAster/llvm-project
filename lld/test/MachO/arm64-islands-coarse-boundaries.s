# REQUIRES: aarch64

## The coarse placement catalog has no legal island position in the target's
## narrow branch window. The planner must fall back to the dense real-input
## boundaries and select the boundary after _dense without adding a relay.

# RUN: rm -rf %t; mkdir %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t/input.o
# RUN: %lld -arch arm64 -dylib -o %t/out %t/input.o \
# RUN:   --branch-range-extension-max-hops=4294967295 --verbose 2> %t/link.log
# RUN: FileCheck %s --check-prefix=LOG --input-file=%t/link.log
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t/out \
# RUN:   | FileCheck %s

# LOG: maxHops=4294967295 branch extender for __TEXT,__text: passes = 1
# LOG-SAME: total extenders = 1

# CHECK-LABEL: <_main>:
# CHECK-NEXT: bl 0x[[#%x, ISLAND:]] <_target.island.0>
# CHECK: [[#ISLAND]] <_target.island.0>:
# CHECK-NEXT: b 0x{{[0-9a-f]+}} <_target>

.subsections_via_symbols
.text

.globl _main
.p2align 2
_main:
  bl _target
  ret

## ARM64's forward BRANCH26 range is 0x7fffffc. _coarse ends 256 KiB before
## the only legal boundary.
.globl _coarse
_coarse:
  .space 0x7fbfff4

.globl _dense
_dense:
  .space 0x40000

## Before insertion, the exact legal window is the final eight bytes of the
## call range. Inserting the island shifts _target by four bytes and leaves a
## final four-byte legal window containing only the _dense boundary.
.globl _tail
_tail:
  .space 0x7fffff4

.globl _target
_target:
  ret

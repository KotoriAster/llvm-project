# REQUIRES: aarch64

## Hybrid is the islands planner capped at depth two. The first caller creates
## a terminal island and the second caller prepends one relay to that shared
## chain. Since the cap is not exceeded, this is the same layout produced by
## islands-slop-free.

# RUN: rm -rf %t; mkdir %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t/input.o
# RUN: %lld -arch arm64 -dylib -undefined dynamic_lookup -o %t/hybrid %t/input.o \
# RUN:   --branch-range-extension=hybrid --verbose 2> %t/link.log
# RUN: %lld -arch arm64 -dylib -undefined dynamic_lookup -o %t/island %t/input.o \
# RUN:   --branch-range-extension=islands-slop-free --verbose 2> %t/islands.log
# RUN: FileCheck %s --check-prefix=LOG --input-file=%t/link.log
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t/hybrid | FileCheck %s
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t/hybrid | tail -n +3 > %t/hybrid.dump
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t/island | tail -n +3 > %t/islands.dump
# RUN: diff %t/hybrid.dump %t/islands.dump

# LOG: hybrid branch extender for __TEXT,__text:
# LOG-SAME: total extenders = 2

# CHECK: <_first_caller>:
# CHECK-NEXT: bl 0x{{[0-9a-f]+}} <_target.island.0>
# CHECK: <_second_caller>:
# CHECK-NEXT: bl 0x{{[0-9a-f]+}} <_target.island.1>
# CHECK-NOT: adrp{{.*}}x16

.subsections_via_symbols

.text
.globl _target
.p2align 2
_target:
  ret

.space 0x6000000
.globl _f0
.p2align 2
_f0:
  ret

## About 144 MiB from _target: one terminal island can bridge the gap.
.space 0x3000000
.globl _first_caller
.p2align 2
_first_caller:
  bl _target
  ret

.space 0x3000000
.globl _f1
.p2align 2
_f1:
  ret

## About 288 MiB from _target: attach one relay to the first caller's terminal
## island, reaching the depth-two cap.
.space 0x6000000
.globl _second_caller
.p2align 2
_second_caller:
  bl _target
  ret

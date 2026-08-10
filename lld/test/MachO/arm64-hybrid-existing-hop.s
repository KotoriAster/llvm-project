# REQUIRES: aarch64

## Hybrid reuses the first caller's two-island chain. The second caller would
## require extending it to depth three, so hybrid falls back to a thunk.

# RUN: rm -rf %t; mkdir %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t/input.o
# RUN: %lld -arch arm64 -dylib -undefined dynamic_lookup -o %t/out %t/input.o \
# RUN:   --branch-range-extension=hybrid --verbose 2> %t/link.log
# RUN: FileCheck %s --check-prefix=LOG --input-file=%t/link.log
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t/out | FileCheck %s

# LOG: hybrid branch extender for __TEXT,__text:
# LOG-SAME: total extenders = 3
# LOG-NOT: region overflow

# CHECK: <_first_caller>:
# CHECK-NEXT: bl 0x{{[0-9a-f]+}} <_target.island.1>
# CHECK: <_target.thunk.{{[0-9]+}}>:
# CHECK-NEXT: adrp x16
# CHECK-NEXT: add x16, x16
# CHECK-NEXT: br x16
# CHECK: <_second_caller>:
# CHECK-NEXT: bl 0x{{[0-9a-f]+}} <_target.thunk.{{[0-9]+}}>

.subsections_via_symbols

.text
.globl _target
.p2align 2
_target:
  ret

## Put _first_caller about 288 MiB after _target. It needs a two-island
## backward chain, creating reusable hops between itself and _target.
.space 0x6000000
.globl _f0
.p2align 2
_f0:
  ret

.space 0x6000000
.globl _f1
.p2align 2
_f1:
  ret

.space 0x6000000
.globl _first_caller
.p2align 2
_first_caller:
  bl _target
  ret

## _second_caller cannot reach the depth-two head and extending the chain would
## exceed hybrid's cap, so it must use a thunk.
.space 0x6000000
.globl _f2
.p2align 2
_f2:
  ret

.space 0x6000000
.globl _second_caller
.p2align 2
_second_caller:
  bl _target
  ret

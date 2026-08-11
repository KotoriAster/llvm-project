# REQUIRES: aarch64

## Callers share the target's two-island spine up to hybrid's depth cap. A
## caller beyond that spine's reach falls back to a thunk instead of extending
## the chain to depth three.

# RUN: rm -rf %t; mkdir %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t/input.o
# RUN: %lld -arch arm64 -dylib -undefined dynamic_lookup -o %t/out %t/input.o \
# RUN:   --branch-range-extension=hybrid --verbose 2> %t/link.log
# RUN: FileCheck %s --check-prefix=LOG --input-file=%t/link.log
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t/out | FileCheck %s

# LOG: hybrid branch extender for __TEXT,__text:
# LOG-SAME: total extenders = 3
# LOG-NOT: region overflow

# CHECK: <_target.island.0>:
# CHECK-NEXT: b 0x{{[0-9a-f]+}} <_target>
# CHECK: <_near_caller>:
# CHECK-NEXT: bl 0x{{[0-9a-f]+}} <_target.island.0>
# CHECK: <_target.island.1>:
# CHECK-NEXT: b 0x{{[0-9a-f]+}} <_target.island.0>
# CHECK: <_first_caller>:
# CHECK-NEXT: bl 0x{{[0-9a-f]+}} <_target.island.1>
# CHECK: <_target.thunk.{{[0-9]+}}>:
# CHECK-NEXT: adrp x16
# CHECK-NEXT: add x16, x16
# CHECK-NEXT: br x16
# CHECK: <_far_caller>:
# CHECK-NEXT: bl 0x{{[0-9a-f]+}} <_target.thunk.{{[0-9]+}}>

.subsections_via_symbols

.text
.globl _target
.p2align 2
_target:
  ret

## The near caller needs the terminal island, and the first caller reaches it
## through a second island.
.space 0x6000000
.globl _f0
.p2align 2
_f0:
  ret

.space 0x3000000
.globl _near_caller
.p2align 2
_near_caller:
  bl _target
  ret

.space 0x3000000
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

## The far caller cannot reach the depth-two head and extending the chain would
## exceed hybrid's cap, so it must use a thunk.
.space 0x6000000
.globl _f2
.p2align 2
_f2:
  ret

.space 0x6000000
.globl _far_caller
.p2align 2
_far_caller:
  bl _target
  ret

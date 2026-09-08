# REQUIRES: aarch64

## Visit callers from the target outward. The nearest caller uses one island,
## and the next caller extends that chain with a second island. The furthest
## caller exceeds the two-hop budget and uses a fallback thunk. Failed attempts
## to extend or rebuild its chain must leave only the two used islands.

# RUN: rm -rf %t; mkdir %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t/input.o
# RUN: %lld -arch arm64 -dylib -undefined dynamic_lookup -o %t/out %t/input.o \
# RUN:   --branch-range-extension-max-hops=2
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t/out | FileCheck %s --implicit-check-not='.island.2'
# RUN: llvm-nm -n %t/out | FileCheck %s --check-prefix=NM

# CHECK: <_far_caller>:
# CHECK-NEXT: bl 0x{{[0-9a-f]+}} <_target.thunk.0>
# CHECK: <_target.thunk.0>:
# CHECK-NEXT: adrp x16
# CHECK-NEXT: add x16, x16
# CHECK-NEXT: br x16
# CHECK: <_first_caller>:
# CHECK-NEXT: bl 0x{{[0-9a-f]+}} <_target.island.0>
# CHECK: <_target.island.0>:
# CHECK-NEXT: b 0x{{[0-9a-f]+}} <_target.island.1>
# CHECK: <_near_caller>:
# CHECK-NEXT: bl 0x{{[0-9a-f]+}} <_target.island.1>
# CHECK: <_target.island.1>:
# CHECK-NEXT: b 0x{{[0-9a-f]+}} <_target>

## Choose the nearest legal boundary, 119 MiB above the furthest callsite,
## even though the boundary at 127 MiB is also reachable.
# NM: [[#%x,FAR:]] T _far_caller
# NM: [[#FAR + 0x7700000]] t _target.thunk.0

.subsections_via_symbols

.text
.globl _far_caller
.p2align 2
_far_caller:
  bl _target
  ret

.space 0x76ffff8
.globl _thunk_boundary
_thunk_boundary:
  ret

.space 0x7ffffc
.globl _later_reachable_boundary
_later_reachable_boundary:
  ret

.space 0x48ffffc
.globl _first_caller
_first_caller:
  bl _target
  ret

.space 0x2dffff8
.globl _outer_island_boundary
_outer_island_boundary:
  ret

.space 0x53ffffc
.globl _near_caller
_near_caller:
  bl _target
  ret

.space 0x2affff8
.globl _inner_island_boundary
_inner_island_boundary:
  ret

.space 0x7effffc
.globl _target
_target:
  ret

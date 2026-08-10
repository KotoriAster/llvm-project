# REQUIRES: aarch64

## Hybrid mode builds a register-safe ladder: a direct branch, then one island,
## then a two-island relay chain for gaps up to ~384 MiB, then a 12-byte thunk.
## Here _far sits far enough that a single island cannot bridge the call, so the
## planner must place a two-island chain (callsite -> islandA -> islandB ->
## _far) while keeping every hop a 4-byte `b` (no x16 clobber).

# RUN: rm -rf %t; mkdir %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t/input.o
# RUN: %lld -arch arm64 -dylib -undefined dynamic_lookup -o %t/out %t/input.o \
# RUN:   --branch-range-extension=hybrid --verbose 2> %t/link.log
# RUN: FileCheck %s --check-prefix=LOG --input-file=%t/link.log
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t/out | FileCheck %s

## One branch, no single-island call, one chain call, two islands, no thunk.
# LOG: hybrid branch extender for __TEXT,__text:
# LOG-SAME: total extenders = 2
# LOG-NOT: region overflow

## The callsite enters the relay chain, and the terminal island reaches _far.
## The first hop is a relay island (.island.1), the second is the terminal
## island (.island.0); the .island.1 symbol proves a two-island chain exists.
## Islands are register-safe direct branches, so no adrp/x16 anywhere.
# CHECK: <_near>:
# CHECK-NEXT: bl 0x{{[0-9a-f]+}} <_far.island.1>
# CHECK-DAG: b 0x{{[0-9a-f]+}} <_far.island.0>
# CHECK-DAG: b 0x{{[0-9a-f]+}} <_far>
# CHECK-NOT: adrp{{.*}}x16

.subsections_via_symbols

.text
.globl _near
.p2align 2
_near:
  bl _far
  ret

## Filler with a symbol boundary every ~96 MiB gives the planner insertion
## points. _far lands ~288 MiB out: beyond one island but within a
## two-island chain.
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
.globl _f2
.p2align 2
_f2:
  ret

.globl _far
.p2align 2
_far:
  ret

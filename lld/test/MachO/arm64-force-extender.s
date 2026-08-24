# REQUIRES: aarch64

## An exact proposal may use less space than its reservation envelope. If an
## alignment boundary absorbs that contraction for the target but not for the
## caller, a branch that was direct in the envelope can be out of range in the
## exact layout. The planner must permanently promote the branch to require an
## extender; otherwise it repeats the same invalid proposal without growing
## any reservation.

# RUN: rm -rf %t; mkdir %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin -defsym MAIN=1 \
# RUN:   %s -o %t/main.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin -defsym CALLERS=1 \
# RUN:   %s -o %t/callers.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin -defsym NEAR=1 \
# RUN:   %s -o %t/near.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin -defsym TARGET=1 \
# RUN:   %s -o %t/target.o
# RUN: %lld -arch arm64 -dylib -undefined dynamic_lookup -o %t/out \
# RUN:   %t/main.o %t/callers.o %t/near.o %t/target.o \
# RUN:   --branch-range-extension-max-hops=2
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t/out \
# RUN:   | FileCheck %s --check-prefix=DIS
# RUN: llvm-nm -n %t/out | FileCheck %s --check-prefix=LAYOUT

# DIS-LABEL: <_near_caller>:
## The promoted branch uses the available one-hop island
# DIS: bl 0x{{[0-9a-f]+}} <_near_target.island.0>
# DIS: <_near_target.island.0>:
# DIS-NEXT: b 0x{{[0-9a-f]+}} <_near_target>

## The direct proposal is four bytes beyond ARM64's maximum positive BRANCH26
## displacement. Promotion makes the planner retain the reachable island.
# LAYOUT: [[#%x,CALLER:]] T _near_caller
# LAYOUT: [[#CALLER + 0x7ffff84]] t _near_target.island.0
# LAYOUT: [[#CALLER + 0x8000000]] T _near_target

.ifdef MAIN
  .subsections_via_symbols
  .text
## `_trigger` first try two islands then one thunk.
.globl _trigger
.p2align 2
_trigger:
  bl _far_target

.globl _padding
_padding:
  .space 0x4000068
.endif

.ifdef CALLERS
  .text

## This always emit a thunk.
  .space 0x3fffffc
  bl _thunk_target
  .space 0x3fffffc
.endif

.ifdef NEAR
  .text

## Second pass update without forcing, third pass force this caller.
.globl _near_caller
_near_caller:
  bl _near_target

## End just below the target's 128-byte alignment boundary. Whether the four
## stale bytes are present decides which side of that boundary the input ends.
  .space 0x7ffff80
.endif

.ifdef TARGET
  .text

.p2align 7
.globl _near_target
_near_target:
  ret

.globl _far_target
_far_target = 0x140002e4

.globl _thunk_target
_thunk_target = 0x100000000
.endif

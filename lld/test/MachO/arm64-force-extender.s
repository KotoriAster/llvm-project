# REQUIRES: aarch64

## An exact proposal may use less space than its reservation envelope. If an
## alignment boundary absorbs that contraction for the target but not for the
## caller, a branch that was direct in the envelope can be out of range in the
## exact layout. The planner must permanently promote the branch to require an
## extender; otherwise it repeats the same invalid proposal without growing
## any reservation.

# RUN: rm -rf %t; mkdir %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t/main.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin -defsym CALLERS=1 \
# RUN:   %s -o %t/callers.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin -defsym TARGET=1 \
# RUN:   %s -o %t/target.o
# RUN: %lld -arch arm64 -dylib -undefined dynamic_lookup -o %t/out \
# RUN:   %t/main.o %t/callers.o %t/target.o \
# RUN:   --branch-range-extension=hybrid --verbose 2> %t/link.log
# RUN: FileCheck %s --check-prefix=LOG --input-file=%t/link.log
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t/out \
# RUN:   | FileCheck %s --check-prefix=DIS
# RUN: llvm-nm -n %t/out | FileCheck %s --check-prefix=LAYOUT

# LOG: hybrid branch extender rejected proposal
# LOG: hybrid branch extender promoted direct calls = 1
# LOG: hybrid branch extender for __TEXT,__text: passes = 4
# LOG-SAME: total extenders = 3

# DIS-LABEL: <_near_caller>:
## The promoted branch uses the one-hop island available to hybrid before
## falling back to a thunk.
# DIS: bl 0x{{[0-9a-f]+}} <_near_target.island.0>
# DIS: <_near_target.island.0>:
# DIS-NEXT: b 0x{{[0-9a-f]+}} <_near_target>

## The accepted artifact proves why promotion is necessary. The direct target
## ends exactly 0x8000000 bytes after the caller, four bytes beyond ARM64's
## maximum positive BRANCH26 displacement. The promoted branch instead reaches
## an island inside that range.
# LAYOUT: [[#%x,CALLER:]] T _near_caller
# LAYOUT: [[#CALLER + 0x7ffffcc]] t _near_target.island.0
# LAYOUT: [[#CALLER + 0x8000000]] T _near_target

.ifdef CALLERS
  .text

## Keep both calls in one input atom. The absolute-target call is almost
## centered: before reservations its nearer thunk boundary is the atom's start;
## after 16 bytes are reserved there, its nearer boundary is the atom's end.
## _near_caller is also inside the atom, so an extender at the end cannot move
## it, while stale capacity at the start can.
  .space 48
.globl _near_caller
_near_caller:
  bl _near_target
  .space 0x3ffffc8
  bl _thunk_target
  .space 0x3fffffc

.else
.ifdef TARGET
  .text

## The caller atom ends 64 bytes before this 128-byte-aligned target input.
## That slack absorbs every proposed extender in the test, so contraction moves
## _near_caller but does not move either target.
.p2align 7
.globl _near_target
_near_target:
  ret
  .space 0x7ffffb8

.globl _far_target
_far_target:
  ret

## No text boundary can build a two-island path to this absolute address, so
## its call always uses a thunk while the chosen thunk boundary may change.
.globl _thunk_target
_thunk_target = 0x100000000

.else
  .subsections_via_symbols
  .text

## The far branch uses a relay at the end of this object and a terminal at the
## end of the caller atom. Their initial distance is exactly one branch range.
## A thunk inserted at the relay bucket shifts only the terminal, rejecting the
## first exact proposal and preserving that bucket's reservation.
.globl _trigger
.p2align 2
_trigger:
  bl _far_target

.globl _padding
_padding:
  .space 0x4000040
.endif
.endif

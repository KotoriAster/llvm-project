# REQUIRES: aarch64

## Regression test for the planner spending more than 40 hours in an O(N^2)
## nested scan over every insertion boundary. A branch beyond the two-island
## limit must fall back to a thunk after probing only a bounded neighborhood.

# RUN: rm -rf %t; mkdir %t
# RUN: split-file %s %t
# RUN: sed -e '/REPEAT/r %t/filler' -e '/REPEAT/d' %t/main.s > %t/input.s
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/input.s -o %t/input.o
# RUN: %lld -arch arm64 -dylib -undefined dynamic_lookup -o %t/out %t/input.o \
# RUN:   --branch-range-extension=hybrid --verbose 2> %t/link.log
# RUN: FileCheck %s --check-prefix=LOG --input-file=%t/link.log
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t/out | FileCheck %s

# LOG: hybrid branch extender for __TEXT,__text:
# LOG-SAME: total extenders = 1

# CHECK: <_main>:
# CHECK-NEXT: bl 0x{{[0-9a-f]+}} <_far.thunk.0>
# CHECK: <_far.thunk.0>:
# CHECK-NEXT: adrp x16
# CHECK-NEXT: add x16, x16
# CHECK-NEXT: br x16

#--- main.s
.subsections_via_symbols
.text
.globl _main
.p2align 2
_main:
  bl _far
  ret
REPEAT
.globl _far
.p2align 2
_far:
  ret

#--- filler
.macro boundary
.space 0x20000
.globl _boundary\@
.p2align 2
_boundary\@:
  ret
.endm
.rept 4096
boundary
.endr

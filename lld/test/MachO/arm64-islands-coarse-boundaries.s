# REQUIRES: aarch64

## Two unrelated callees with the same distant callers should select the
## proposal-wide coarse catalog before dense input boundaries.

# RUN: rm -rf %t; mkdir %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t/input.o
# RUN: %lld -arch arm64 -dylib -undefined dynamic_lookup -o %t/out %t/input.o \
# RUN:   --branch-range-extension=islands-slop-free --verbose 2> %t/link.log
# RUN: FileCheck %s --check-prefix=LOG --input-file=%t/link.log
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t/out | FileCheck %s

# LOG: islands-slop-free branch extender for __TEXT,__text:
# LOG-SAME: total extenders = {{[1-9][0-9]*}}

# CHECK-LABEL: <_main>:
# CHECK: bl 0x{{[0-9a-f]+}} <_far0.island.{{[0-9]+}}>
# CHECK: bl 0x{{[0-9a-f]+}} <_far1.island.{{[0-9]+}}>

.subsections_via_symbols
.text
.globl _main
.p2align 2
_main:
  bl _far0
  bl _far1
  ret

## Real input boundaries at coarse seams give both callee plans the same
## shared placement catalog.
.space 0x4000000
.globl _seam0
_seam0:
  ret
.space 0x4000000
.globl _seam1
_seam1:
  ret
.space 0x4000000
.globl _seam2
_seam2:
  ret
.space 0x4000000
.globl _seam3
_seam3:
  ret
.space 0x4000000
.globl _far0
_far0:
  ret
.globl _far1
_far1:
  ret

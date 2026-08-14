# REQUIRES: aarch64

## _target has a nonzero value in its input section and _alias has the same
## effective address.
##
## Layout of the regression test
##
##   0                 +0x600000c       +0x7fffffc       +0xc000010  +0xc000050
##   |                       |                |                 |          |
## _main             _target.island.0  direct BL limit  target inputVA  targetVA
##   |---------------------->|                                  + 64 B -->|
##   |  both calls, ~96 MiB  |------------------------------------------->|
##                            island branch, ~96 MiB         _target == _alias
##

# RUN: rm -rf %t; mkdir %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin -defsym CALLER=1 \
# RUN:   %s -o %t/caller.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin -defsym PAD=1 \
# RUN:   %s -o %t/pad0.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin -defsym PAD=1 \
# RUN:   %s -o %t/pad1.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin -defsym TARGET=1 \
# RUN:   %s -o %t/target.o
# RUN: %lld -arch arm64 -dylib -o %t/out %t/caller.o %t/pad0.o %t/pad1.o \
# RUN:   %t/target.o --branch-range-extension-max-hops=2 --verbose \
# RUN:   2> %t/link.log
# RUN: FileCheck %s --check-prefix=LOG --input-file=%t/link.log
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t/out \
# RUN:   | FileCheck %s --check-prefix=DIS

# LOG: maxHops=2 branch extender for __TEXT,__text:
# LOG-SAME: targets = 1
# LOG-SAME: total extenders = 1

# DIS-LABEL: <_main>:
# DIS-NEXT: bl 0x[[#%x, ISLAND:]] <_target.island.0>
# DIS-NEXT: bl 0x[[#ISLAND]] <_target.island.0>
# DIS: [[#ISLAND]] <_target.island.0>:
# DIS-NEXT: b 0x{{[0-9a-f]+}} <_target>

.ifdef CALLER
.text
.globl _main
_main:
  bl _target
  bl _alias
  ret
.else
.ifdef PAD
.text
  .space 0x6000000
.else
.ifdef TARGET
.text
  .space 64
.globl _target
_target:
  ret
.globl _alias
_alias = _target
.endif
.endif
.endif

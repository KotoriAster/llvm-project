# REQUIRES: aarch64

## A single atom larger than BRANCH26 leaves no legal island between either
## destination and the callers. Both fallback thunks use the caller boundary,
## whose exact-proposal and final-emission order must match.

# RUN: rm -rf %t; mkdir %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t/input.o
# RUN: %lld -arch arm64 -dylib -o %t/out %t/input.o \
# RUN:   --branch-range-extension=hybrid --verbose 2> %t/link.log
# RUN: FileCheck %s --check-prefix=LOG --input-file=%t/link.log
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t/out \
# RUN:   | FileCheck %s --check-prefix=DIS

# LOG: hybrid branch extender for __TEXT,__text:
# LOG-SAME: total extenders = 2

# DIS-LABEL: <_main>:
# DIS-NEXT: bl 0x{{[0-9a-f]+}} <_far0.thunk.0>
# DIS-NEXT: bl 0x{{[0-9a-f]+}} <_far1.thunk.0>
# DIS: <_far0.thunk.0>:
# DIS-NEXT: adrp x16
# DIS-NEXT: add x16, x16
# DIS-NEXT: br x16
# DIS: <_far1.thunk.0>:
# DIS-NEXT: adrp x16
# DIS-NEXT: add x16, x16
# DIS-NEXT: br x16

.subsections_via_symbols
.text

.globl _main
_main:
  bl _far0
  bl _far1
  ret

.globl _big
_big:
  ret
  .space 0x8100000

.globl _far0
_far0:
  ret

.globl _far1
_far1:
  ret

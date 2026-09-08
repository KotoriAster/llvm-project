# REQUIRES: aarch64

## Thunks use the nearest legal dense boundary, even when it is omitted from
## the coarse catalog. The call is 16 bytes into _main: its own boundary is
## 8 bytes ahead, closer than the coarse boundary immediately before _main.
## The distant coarse and dense boundaries are also reachable candidates.

# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t.o
# RUN: %lld -arch arm64 -dylib -o %t %t.o \
# RUN:   --branch-range-extension-max-hops=0
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t \
# RUN:   | FileCheck %s

# CHECK-LABEL: <_target>:
# CHECK: [[#%x, MAIN:]] <_main>:
# CHECK: bl 0x[[#MAIN + 24]] <_target.thunk.0>
# CHECK-NEXT: ret
# CHECK: [[#MAIN + 24]] <_target.thunk.0>:
# CHECK-NEXT: adrp x16
# CHECK-NEXT: add x16, x16
# CHECK-NEXT: br x16
# CHECK-LABEL: <_coarse>:
# CHECK-LABEL: <_dense>:
# CHECK-LABEL: <_end>:

.subsections_via_symbols
.text

.globl _target
_target:
  ret

.globl _padding
_padding:
  .space 0x9000000

.globl _main
_main:
  nop
  nop
  nop
  nop
  bl _target
  ret

.globl _coarse
_coarse:
  .space 0x7000000

.globl _dense
_dense:
  .space 0x40000

.globl _gap
_gap:
  .space 0x9000000

.globl _end
_end:
  ret

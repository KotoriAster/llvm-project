# REQUIRES: aarch64

## Prefer a coarse boundary over a denser boundary even when the latter is
## farther inward in the thunk's legal placement window. The coarse boundary
## follows _coarse; the boundary following _dense is only 256 KiB later and is
## therefore omitted from the 1 MiB-spaced coarse catalog.

## layout: 
##
##   0             ~112 MiB  ~112.25 MiB    +128 MiB - 4 B    ~256.25 MiB
##   |                  |          |                |                 |
## _main       coarse boundary  dense boundary  BL limit          _target
##   |----------------->|          |                |
##                chosen thunk     `-- also legal --'
##

# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t.o
# RUN: %lld -arch arm64 -dylib -o %t %t.o \
# RUN:   --branch-range-extension-max-hops=0
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t \
# RUN:   | FileCheck %s

# CHECK-LABEL: <_main>:
# CHECK-NEXT: bl 0x[[#%x, THUNK:]] <_target.thunk.0>
# CHECK-LABEL: <_coarse>:
# CHECK: [[#%x, THUNK]] <_target.thunk.0>:
# CHECK-LABEL: <_dense>:
# CHECK-LABEL: <_target>:

.subsections_via_symbols
.text

.globl _main
_main:
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

.globl _target
_target:
  ret

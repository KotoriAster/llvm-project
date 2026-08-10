# REQUIRES: aarch64

## Regression test for hybrid mode inheriting the reserved-region budget and
## failing with "chain branch-extender region overflow". The slop-free planner
## must ignore --branch-island-region-size, even when it is only four bytes.

# RUN: rm -rf %t; mkdir %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t/input.o
# RUN: %lld -arch arm64 -dylib -undefined dynamic_lookup -o %t/out %t/input.o \
# RUN:   --branch-range-extension=hybrid --branch-island-region-size=4 \
# RUN:   --verbose 2> %t/link.log
# RUN: FileCheck %s --check-prefix=LOG --input-file=%t/link.log
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t/out | FileCheck %s

# LOG: hybrid branch extender for __TEXT,__text:
# LOG-SAME: total extenders = 1
# LOG-NOT: region overflow

# CHECK: <_near>:
# CHECK-NEXT: bl 0x{{[0-9a-f]+}} <_far.island.0>
# CHECK: <_far.island.0>:
# CHECK-NEXT: b 0x{{[0-9a-f]+}} <_far>
# CHECK-NOT: adrp{{.*}}x16

.subsections_via_symbols

.text
.globl _near
.p2align 2
_near:
  bl _far
  ret

## _mid gives the planner a real input-section boundary reachable from both
## the callsite and _far.
.space 0x6000000
.globl _mid
.p2align 2
_mid:
  ret

.space 0x6000000
.globl _far
.p2align 2
_far:
  ret

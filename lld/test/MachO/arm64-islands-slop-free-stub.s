# REQUIRES: aarch64

## Regression test for estimating __stubs 128 MiB too early because the large
## unfinalized __TEXT,__const section reported output size zero. The terminal
## island must be planned against the simulated post-__const stub address.

# RUN: rm -rf %t; mkdir %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t/input.o
# RUN: %lld -arch arm64 -dylib -undefined dynamic_lookup -o %t/out %t/input.o \
# RUN:   --branch-range-extension-max-hops=4294967295 --verbose 2> %t/link.log
# RUN: FileCheck %s --check-prefix=LOG --input-file=%t/link.log
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t/out | FileCheck %s

# LOG: maxHops=4294967295 branch extender for __TEXT,__text:
# LOG-SAME: total extenders = 2

# CHECK: <_main>:
# CHECK-NEXT: bl 0x{{[0-9a-f]+}} <_external.island.1>
# CHECK: <_external.island.1>:
# CHECK-NEXT: b 0x{{[0-9a-f]+}} <_external.island.0>
# CHECK: <_external.island.0>:
# CHECK-NEXT: b 0x[[#%x, STUB:]] <{{.*}}>
# CHECK: [[#STUB]] <__stubs>:
# CHECK-NEXT: adrp x16
# CHECK-NEXT: ldr x16
# CHECK-NEXT: br x16
# CHECK-NOT: <_external.thunk.

.subsections_via_symbols
.text
.globl _main
.p2align 2
_main:
  bl _external
  ret

.space 0x6000000
.globl _f0
_f0:
  ret
.space 0x6000000
.globl _f1
_f1:
  ret
.space 0x6000000
.globl _f2
_f2:
  ret

## Keep a large unfinalized text-segment section between __text and __stubs.
## Stub estimation must account for its input sizes before final addresses are
## assigned.
.section __TEXT,__const
.space 0x8000000

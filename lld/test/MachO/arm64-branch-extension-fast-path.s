# REQUIRES: aarch64

# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t.o
# RUN: %lld -arch arm64 -o %t %t.o --verbose 2> %t.log
# RUN: FileCheck %s --check-prefix=LOG --input-file=%t.log
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t \
# RUN:   | FileCheck %s

# LOG-NOT: branch extender

# CHECK-LABEL: <_main>:
# CHECK-NEXT: bl 0x{{[0-9a-f]+}} <_target>
# CHECK-NEXT: ret

.globl _main
.p2align 2
_main:
  bl _target
  ret

.globl _target
.p2align 2
_target:
  ret

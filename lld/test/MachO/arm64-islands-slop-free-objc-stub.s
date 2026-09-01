# REQUIRES: aarch64

## Regression test for treating generated _objc_msgSend$*.island.* symbols as
## __objc_stubs entries solely because of their name prefix. Relay islands are
## ordinary defined symbols; only the terminal target lives in __objc_stubs.

# RUN: rm -rf %t; mkdir %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t/input.o
# RUN: %lld -arch arm64 -dylib -undefined dynamic_lookup -o %t/out %t/input.o \
# RUN:   --branch-range-extension-max-hops=4294967295 --verbose 2> %t/link.log
# RUN: FileCheck %s --check-prefix=LOG --input-file=%t/link.log
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t/out | FileCheck %s
# RUN: %lld -arch arm64 -dylib -undefined dynamic_lookup -o %t/front %t/input.o \
# RUN:   --branch-range-extension-max-hops=4294967295 \
# RUN:   -rename_section __TEXT __objc_stubs __TEXT __mach_header \
# RUN:   --verbose 2> %t/front.log
# RUN: FileCheck %s --check-prefix=FRONT-LOG --input-file=%t/front.log
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t/front | \
# RUN:   FileCheck %s --check-prefix=FRONT

# LOG: branch extender for __TEXT,__text:
# LOG-SAME: total extenders = 2

# CHECK: <_main>:
# CHECK-NEXT: bl 0x{{[0-9a-f]+}} <_objc_msgSend$foo.island.0>
# CHECK: <_objc_msgSend$foo.island.0>:
# CHECK-NEXT: b 0x{{[0-9a-f]+}} <_objc_msgSend$foo.island.1>
# CHECK: <_objc_msgSend$foo.island.1>:
# CHECK-NEXT: b 0x{{[0-9a-f]+}} <_objc_msgSend$foo>
# CHECK: Disassembly of section __TEXT,__objc_stubs:
# CHECK: <_objc_msgSend$foo>:
# CHECK-NOT: <_objc_msgSend$foo.thunk.

## Giving __objc_stubs the header priority moves it before __text. The planner
## must use its already assigned address instead of assuming a suffix target.
# FRONT-LOG: branch extender for __TEXT,__text:
# FRONT-LOG-SAME: targets = 0, total extenders = 0

# FRONT: Disassembly of section __TEXT,__mach_header:
# FRONT: <_objc_msgSend$foo>:
# FRONT: Disassembly of section __TEXT,__text:
# FRONT: <_main>:
# FRONT-NEXT: bl 0x{{[0-9a-f]+}} <_objc_msgSend$foo>

.subsections_via_symbols
.text
.globl _main
.p2align 2
_main:
  bl _objc_msgSend$foo
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

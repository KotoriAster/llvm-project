# REQUIRES: aarch64

## Check the default, finite, maximum, and invalid values.

# RUN: rm -rf %t; mkdir %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t/input.o
# RUN: %lld -arch arm64 -dylib -o %t/out %t/input.o --verbose 2> %t/default.log
# RUN: FileCheck %s --check-prefix=DEFAULT --input-file=%t/default.log
# RUN: %lld -arch arm64 -dylib -o %t/out %t/input.o \
# RUN:   --branch-range-extension-max-hops=0 --verbose 2> %t/zero.log
# RUN: FileCheck %s --check-prefix=ZERO --input-file=%t/zero.log
# RUN: %lld -arch arm64 -dylib -o %t/out %t/input.o \
# RUN:   --branch-range-extension-max-hops=17 --verbose 2> %t/finite.log
# RUN: FileCheck %s --check-prefix=FINITE --input-file=%t/finite.log
# RUN: %lld -arch arm64 -dylib -o %t/out %t/input.o \
# RUN:   --branch-range-extension-max-hops=4294967295 --verbose 2> %t/max.log
# RUN: FileCheck %s --check-prefix=MAX --input-file=%t/max.log
# RUN: not %lld -arch arm64 -dylib -o %t/out %t/input.o \
# RUN:   --branch-range-extension-max-hops=-1 2>&1 \
# RUN:   | FileCheck %s --check-prefix=NEGATIVE
# RUN: not %lld -arch arm64 -dylib -o %t/out %t/input.o \
# RUN:   --branch-range-extension-max-hops=inf 2>&1 \
# RUN:   | FileCheck %s --check-prefix=NONINTEGER
# RUN: not %lld -arch arm64 -dylib -o %t/out %t/input.o \
# RUN:   --branch-range-extension-max-hops=4294967296 2>&1 \
# RUN:   | FileCheck %s --check-prefix=OVERFLOW
# DEFAULT: finalization mode for __TEXT,__text: maxHops=2
# ZERO: finalization mode for __TEXT,__text: maxHops=0
# FINITE: finalization mode for __TEXT,__text: maxHops=17
# MAX: finalization mode for __TEXT,__text: maxHops=4294967295
# NEGATIVE: --branch-range-extension-max-hops=: expected a non-negative integer, but got '-1'
# NONINTEGER: --branch-range-extension-max-hops=: expected a non-negative integer, but got 'inf'
# OVERFLOW: --branch-range-extension-max-hops=: expected a non-negative integer, but got '4294967296'

.subsections_via_symbols
.text

.globl _main
_main:
  ret

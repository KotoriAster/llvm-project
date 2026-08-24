# REQUIRES: aarch64

## Check the default, finite, maximum, and invalid values.

# RUN: rm -rf %t; mkdir %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t/input.o
# RUN: %lld -arch arm64 -dylib -o %t/out %t/input.o
# RUN: %lld -arch arm64 -dylib -o %t/out %t/input.o \
# RUN:   --branch-range-extension-max-hops=0
# RUN: %lld -arch arm64 -dylib -o %t/out %t/input.o \
# RUN:   --branch-range-extension-max-hops=17
# RUN: %lld -arch arm64 -dylib -o %t/out %t/input.o \
# RUN:   --branch-range-extension-max-hops=4294967295
# RUN: not %lld -arch arm64 -dylib -o %t/out %t/input.o \
# RUN:   --branch-range-extension-max-hops=-1 2>&1 \
# RUN:   | FileCheck %s --check-prefix=NEGATIVE
# RUN: not %lld -arch arm64 -dylib -o %t/out %t/input.o \
# RUN:   --branch-range-extension-max-hops=inf 2>&1 \
# RUN:   | FileCheck %s --check-prefix=NONINTEGER
# RUN: not %lld -arch arm64 -dylib -o %t/out %t/input.o \
# RUN:   --branch-range-extension-max-hops=4294967296 2>&1 \
# RUN:   | FileCheck %s --check-prefix=OVERFLOW
# NEGATIVE: --branch-range-extension-max-hops=: expected a non-negative integer, but got '-1'
# NONINTEGER: --branch-range-extension-max-hops=: expected a non-negative integer, but got 'inf'
# OVERFLOW: --branch-range-extension-max-hops=: expected a non-negative integer, but got '4294967296'

.subsections_via_symbols
.text

.globl _main
_main:
  ret

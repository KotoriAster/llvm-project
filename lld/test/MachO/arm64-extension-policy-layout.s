# REQUIRES: aarch64

## The three-hop cases exercise targets above and below their callers.
##
## Final address columns relative to _main 
##
##   0                 ~+96 MiB       ~+192 MiB      ~+288 MiB      ~+384 MiB
##   |                     |               |              |              |
## _three_hop_down/   extenders/      extenders/    extenders/    _three_hop/
## _main              _boundary0      _one_hop      _two_hop      _downward_main
##
## Representative branch paths (`=>` is a thunk's indirect jump):
## `one`, `two`, `three`, and `down` abbreviate the corresponding symbols;
## `.iN` abbreviates `.island.N`.
##
##   maxHops=0:   _main --> {one,two,three}.thunk.0 => each target
##                 down target <= down.thunk.0 <-- _downward_main
##
##   maxHops=2:   _main --> one.i0 --------------------> _one_hop
##                      --> two.i0 --> two.i1 ----------> _two_hop
##                      --> three.thunk.0 =============> _three_hop
##                 down target <= down.thunk.0 <-------- _downward_main
##
##   maxHops=inf: _main --> three.i0 --> three.i1 --> three.i2 --> _three_hop
##                 down target <-- down.i0 <-- down.i1 <-- down.i2 <-- down main

# RUN: rm -rf %t; mkdir %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t/input.o

# RUN: %lld -arch arm64 -dylib -o %t/thunks %t/input.o \
# RUN:   --branch-range-extension-max-hops=0
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t/thunks \
# RUN:   | FileCheck %s --check-prefix=THUNKS
# RUN: rm %t/thunks

# RUN: %lld -arch arm64 -dylib -o %t/hybrid %t/input.o \
# RUN:   --branch-range-extension-max-hops=2
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t/hybrid \
# RUN:   | FileCheck %s --check-prefix=HYBRID
# RUN: rm %t/hybrid

# RUN: %lld -arch arm64 -dylib -o %t/islands %t/input.o \
# RUN:   --branch-range-extension-max-hops=4294967295
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t/islands \
# RUN:   | FileCheck %s --check-prefix=ISLANDS
# RUN: rm %t/islands

# THUNKS-LABEL: <_main>:
# THUNKS-NEXT: bl 0x{{[0-9a-f]+}} <_one_hop.thunk.0>
# THUNKS-NEXT: bl 0x{{[0-9a-f]+}} <_two_hop.thunk.0>
# THUNKS-NEXT: bl 0x{{[0-9a-f]+}} <_three_hop.thunk.0>
# THUNKS: <_one_hop.thunk.0>:
# THUNKS-NEXT: adrp x16
# THUNKS-NEXT: add x16, x16
# THUNKS-NEXT: br x16
# THUNKS: <_two_hop.thunk.0>:
# THUNKS-NEXT: adrp x16
# THUNKS-NEXT: add x16, x16
# THUNKS-NEXT: br x16
# THUNKS: <_three_hop.thunk.0>:
# THUNKS-NEXT: adrp x16
# THUNKS-NEXT: add x16, x16
# THUNKS-NEXT: br x16
# THUNKS: <_three_hop_down.thunk.0>:
# THUNKS-NEXT: adrp x16
# THUNKS-NEXT: add x16, x16
# THUNKS-NEXT: br x16
# THUNKS: <_downward_main>:
# THUNKS-NEXT: bl 0x{{[0-9a-f]+}} <_three_hop_down.thunk.0>

# HYBRID-LABEL: <_main>:
# HYBRID-NEXT: bl 0x{{[0-9a-f]+}} <_one_hop.island.0>
# HYBRID-NEXT: bl 0x{{[0-9a-f]+}} <_two_hop.island.0>
# HYBRID-NEXT: bl 0x{{[0-9a-f]+}} <_three_hop.thunk.0>
# HYBRID: <_one_hop.island.0>:
# HYBRID-NEXT: b 0x{{[0-9a-f]+}} <_one_hop>
# HYBRID: <_two_hop.island.0>:
# HYBRID-NEXT: b 0x{{[0-9a-f]+}} <_two_hop.island.1>
# HYBRID: <_three_hop.thunk.0>:
# HYBRID-NEXT: adrp x16
# HYBRID-NEXT: add x16, x16
# HYBRID-NEXT: br x16
# HYBRID: <_two_hop.island.1>:
# HYBRID-NEXT: b 0x{{[0-9a-f]+}} <_two_hop>
# HYBRID: <_three_hop_down.thunk.0>:
# HYBRID-NEXT: adrp x16
# HYBRID-NEXT: add x16, x16
# HYBRID-NEXT: br x16
# HYBRID: <_downward_main>:
# HYBRID-NEXT: bl 0x{{[0-9a-f]+}} <_three_hop_down.thunk.0>

# ISLANDS-LABEL: <_main>:
# ISLANDS-NEXT: bl 0x{{[0-9a-f]+}} <_one_hop.island.0>
# ISLANDS-NEXT: bl 0x{{[0-9a-f]+}} <_two_hop.island.0>
# ISLANDS-NEXT: bl 0x{{[0-9a-f]+}} <_three_hop.island.0>
# ISLANDS: <_one_hop.island.0>:
# ISLANDS-NEXT: b 0x{{[0-9a-f]+}} <_one_hop>
# ISLANDS: <_two_hop.island.0>:
# ISLANDS-NEXT: b 0x{{[0-9a-f]+}} <_two_hop.island.1>
# ISLANDS: <_three_hop.island.0>:
# ISLANDS-NEXT: b 0x{{[0-9a-f]+}} <_three_hop.island.1>
# ISLANDS: <_three_hop_down.island.0>:
# ISLANDS-NEXT: b 0x{{[0-9a-f]+}} <_three_hop_down>
# ISLANDS: <_two_hop.island.1>:
# ISLANDS-NEXT: b 0x{{[0-9a-f]+}} <_two_hop>
# ISLANDS: <_three_hop.island.1>:
# ISLANDS-NEXT: b 0x{{[0-9a-f]+}} <_three_hop.island.2>
# ISLANDS: <_three_hop_down.island.1>:
# ISLANDS-NEXT: b 0x{{[0-9a-f]+}} <_three_hop_down.island.0>
# ISLANDS: <_three_hop.island.2>:
# ISLANDS-NEXT: b 0x{{[0-9a-f]+}} <_three_hop>
# ISLANDS: <_three_hop_down.island.2>:
# ISLANDS-NEXT: b 0x{{[0-9a-f]+}} <_three_hop_down.island.1>
# ISLANDS: <_downward_main>:
# ISLANDS-NEXT: bl 0x{{[0-9a-f]+}} <_three_hop_down.island.2>
# ISLANDS-NOT: .thunk.

.subsections_via_symbols
.text

.globl _three_hop_down
.p2align 2
_three_hop_down:
  ret

.globl _main
.p2align 2
_main:
  bl _one_hop
  bl _two_hop
  bl _three_hop
  ret

.space 0x6000000
.globl _boundary0
_boundary0:
  ret

.space 0x6000000
.globl _one_hop
_one_hop:
  ret

.space 0x6000000
.globl _two_hop
_two_hop:
  ret

.space 0x6000000
.globl _three_hop
_three_hop:
  ret

.globl _downward_main
.p2align 2
_downward_main:
  bl _three_hop_down
  ret

# REQUIRES: aarch64

## Verify the emitted artifact, rather than only planner diagnostics, matches
## each branch-range extension policy:
##
##   thunks:            every out-of-range call uses a thunk;
##   hybrid:            up to two islands are allowed, then a thunk is used;
##   islands-slop-free: arbitrarily long island chains are allowed.
##
## The three-hop cases exercise targets above and below their callers.

# RUN: rm -rf %t; mkdir %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %s -o %t/input.o

# RUN: %lld -arch arm64 -dylib -o %t/thunks %t/input.o \
# RUN:   --branch-range-extension=thunks --verbose 2> %t/thunks.log
# RUN: FileCheck %s --check-prefix=THUNKS-LOG --input-file=%t/thunks.log
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t/thunks \
# RUN:   | FileCheck %s --check-prefix=THUNKS
# RUN: rm %t/thunks

# RUN: %lld -arch arm64 -dylib -o %t/hybrid %t/input.o \
# RUN:   --branch-range-extension=hybrid --verbose 2> %t/hybrid.log
# RUN: FileCheck %s --check-prefix=HYBRID-LOG --input-file=%t/hybrid.log
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t/hybrid \
# RUN:   | FileCheck %s --check-prefix=HYBRID
# RUN: rm %t/hybrid

# RUN: %lld -arch arm64 -dylib -o %t/islands %t/input.o \
# RUN:   --branch-range-extension=islands-slop-free --verbose \
# RUN:   2> %t/islands.log
# RUN: FileCheck %s --check-prefix=ISLANDS-LOG --input-file=%t/islands.log
# RUN: llvm-objdump --no-print-imm-hex -d --no-show-raw-insn %t/islands \
# RUN:   | FileCheck %s --check-prefix=ISLANDS
# RUN: rm %t/islands

# THUNKS-LOG: Created 4 {{.*}} thunks and updated 4 branch targets
# HYBRID-LOG: hybrid branch extender for __TEXT,__text:
# HYBRID-LOG-SAME: total extenders = 5
# ISLANDS-LOG: islands-slop-free branch extender for __TEXT,__text:
# ISLANDS-LOG-SAME: total extenders = 9

# THUNKS-LABEL: <_main>:
# THUNKS-NEXT: bl 0x{{[0-9a-f]+}} <_one_hop.thunk.0>
# THUNKS-NEXT: bl 0x{{[0-9a-f]+}} <_two_hop.thunk.0>
# THUNKS-NEXT: bl 0x{{[0-9a-f]+}} <_three_hop.thunk.0>
# THUNKS: <_one_hop.thunk.0>:
# THUNKS-NEXT: adrp x16
# THUNKS-NEXT: add x16, x16
# THUNKS-NEXT: br x16
# THUNKS: <_downward_main>:
# THUNKS-NEXT: bl 0x{{[0-9a-f]+}} <_three_hop_down.thunk.0>
# THUNKS: <_three_hop_down.thunk.0>:
# THUNKS-NEXT: adrp x16
# THUNKS-NEXT: add x16, x16
# THUNKS-NEXT: br x16

# HYBRID: <_three_hop.thunk.0>:
# HYBRID-NEXT: adrp x16
# HYBRID-NEXT: add x16, x16
# HYBRID-NEXT: br x16
# HYBRID-LABEL: <_main>:
# HYBRID-NEXT: bl 0x{{[0-9a-f]+}} <_one_hop.island.0>
# HYBRID-NEXT: bl 0x{{[0-9a-f]+}} <_two_hop.island.1>
# HYBRID-NEXT: bl 0x{{[0-9a-f]+}} <_three_hop.thunk.0>
# HYBRID: <_one_hop.island.0>:
# HYBRID-NEXT: b 0x{{[0-9a-f]+}} <_one_hop>
# HYBRID: <_two_hop.island.1>:
# HYBRID-NEXT: b 0x{{[0-9a-f]+}} <_two_hop.island.0>
# HYBRID: <_two_hop.island.0>:
# HYBRID-NEXT: b 0x{{[0-9a-f]+}} <_two_hop>
# HYBRID: <_three_hop_down.thunk.0>:
# HYBRID-NEXT: adrp x16
# HYBRID-NEXT: add x16, x16
# HYBRID-NEXT: br x16
# HYBRID: <_downward_main>:
# HYBRID-NEXT: bl 0x{{[0-9a-f]+}} <_three_hop_down.thunk.0>

# ISLANDS-LABEL: <_main>:
# ISLANDS-NEXT: bl 0x{{[0-9a-f]+}} <_one_hop.island.0>
# ISLANDS-NEXT: bl 0x{{[0-9a-f]+}} <_two_hop.island.1>
# ISLANDS-NEXT: bl 0x{{[0-9a-f]+}} <_three_hop.island.2>
# ISLANDS: <_one_hop.island.0>:
# ISLANDS-NEXT: b 0x{{[0-9a-f]+}} <_one_hop>
# ISLANDS: <_two_hop.island.1>:
# ISLANDS-NEXT: b 0x{{[0-9a-f]+}} <_two_hop.island.0>
# ISLANDS: <_three_hop.island.2>:
# ISLANDS-NEXT: b 0x{{[0-9a-f]+}} <_three_hop.island.1>
# ISLANDS: <_three_hop_down.island.0>:
# ISLANDS-NEXT: b 0x{{[0-9a-f]+}} <_three_hop_down>
# ISLANDS: <_two_hop.island.0>:
# ISLANDS-NEXT: b 0x{{[0-9a-f]+}} <_two_hop>
# ISLANDS: <_three_hop.island.1>:
# ISLANDS-NEXT: b 0x{{[0-9a-f]+}} <_three_hop.island.0>
# ISLANDS: <_three_hop_down.island.1>:
# ISLANDS-NEXT: b 0x{{[0-9a-f]+}} <_three_hop_down.island.0>
# ISLANDS: <_three_hop.island.0>:
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

# REQUIRES: x86 && thread_support
# RUN: rm -rf %t; split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/main.s -o %t/main.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/foo.s -o %t/foo.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/bar.s -o %t/bar.o
# RUN: llvm-as %t/bitcode.ll -o %t/bitcode.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin \
# RUN:   %t/bad-cstring.s -o %t/bad-cstring.o
# RUN: llvm-ar rcs %t/libfoo.a %t/foo.o %t/bad-cstring.o
# RUN: llvm-ar rcS %t/libfoo-no-index.a %t/foo.o
#
# RUN: %lld -no_uuid %t/main.o %t/libfoo.a %t/bar.o %t/bitcode.o -o %t/serial
# RUN: %lld -no_uuid --input-load-demo=ios --input-load-workers=2 \
# RUN:   %t/main.o %t/libfoo.a %t/bar.o %t/bitcode.o -o %t/ios
# RUN: %lld -no_uuid --input-load-demo=elf --input-load-workers=2 \
# RUN:   %t/main.o %t/libfoo.a %t/bar.o %t/bitcode.o -o %t/elf
# RUN: %lld -no_uuid --input-load-demo=prime --input-load-workers=2 \
# RUN:   %t/main.o %t/libfoo.a %t/bar.o %t/bitcode.o -o %t/prime
# RUN: %lld -no_uuid --input-load-demo=mold --input-load-workers=2 \
# RUN:   %t/main.o %t/libfoo.a %t/bar.o %t/bitcode.o -o %t/mold
# RUN: cmp %t/serial %t/ios
# RUN: cmp %t/serial %t/elf
# RUN: cmp %t/serial %t/prime
# RUN: cmp %t/serial %t/mold
# RUN: %lld -no_uuid %t/main.o %t/libfoo-no-index.a %t/bar.o %t/bitcode.o \
# RUN:   -o %t/no-index-serial
# RUN: %lld -no_uuid --input-load-demo=mold --input-load-workers=2 \
# RUN:   %t/main.o %t/libfoo-no-index.a %t/bar.o %t/bitcode.o \
# RUN:   -o %t/no-index-mold
# RUN: cmp %t/no-index-serial %t/no-index-mold
# RUN: %lld -no_uuid %t/libfoo-no-index.a %t/main.o %t/bar.o %t/bitcode.o \
# RUN:   -o %t/no-index-late-serial
# RUN: %lld -no_uuid --input-load-demo=mold --input-load-workers=2 \
# RUN:   %t/libfoo-no-index.a %t/main.o %t/bar.o %t/bitcode.o \
# RUN:   -o %t/no-index-late-mold
# RUN: cmp %t/no-index-late-serial %t/no-index-late-mold
#
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin \
# RUN:   %t/autolink.s -o %t/autolink.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin \
# RUN:   %t/autoload.s -o %t/autoload.o
# RUN: llvm-ar rcs %t/libautoload.a %t/autoload.o
# RUN: %lld -dylib -L%t --input-load-demo=ios --input-load-workers=2 \
# RUN:   %t/autolink.o -o %t/autolink-ios
# RUN: %lld -dylib -L%t --input-load-demo=elf --input-load-workers=2 \
# RUN:   %t/autolink.o -o %t/autolink-elf
# RUN: %lld -dylib -L%t --input-load-demo=prime --input-load-workers=2 \
# RUN:   %t/autolink.o -o %t/autolink-prime
# RUN: %lld -dylib -L%t --input-load-demo=mold --input-load-workers=2 \
# RUN:   %t/autolink.o -o %t/autolink-mold
# RUN: llvm-nm %t/autolink-ios | FileCheck %s --check-prefix=AUTOLINK
# RUN: llvm-nm %t/autolink-elf | FileCheck %s --check-prefix=AUTOLINK
# RUN: llvm-nm %t/autolink-prime | FileCheck %s --check-prefix=AUTOLINK
# RUN: llvm-nm %t/autolink-mold | FileCheck %s --check-prefix=AUTOLINK
# RUN: %lld -dylib -L%t --input-load-demo=prime --input-load-workers=2 \
# RUN:   --input-load-stats %t/autolink.o -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=LATE-WAVES
#
# RUN: not %lld --input-load-demo=unknown %t/main.o -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=BAD-MODE
# RUN: not %lld --input-load-demo=ios --input-load-workers=many \
# RUN:   %t/main.o -o /dev/null 2>&1 | FileCheck %s --check-prefix=BAD-WORKERS
# RUN: not %lld --input-load-workers=2 %t/main.o -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=NEEDS-MODE
# RUN: not %lld --input-load-stats %t/main.o -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=STATS-NEEDS-MODE
# RUN: not %lld --read-workers=2 --input-load-demo=ios %t/main.o \
# RUN:   -o /dev/null 2>&1 | FileCheck %s --check-prefix=CONFLICT
# RUN: %lld -no_uuid --input-load-demo=ios --input-load-workers=2 \
# RUN:   --input-load-stats %t/main.o %t/libfoo.a %t/bar.o %t/bitcode.o \
# RUN:   -o /dev/null 2>&1 | FileCheck %s --check-prefix=STATS
# RUN: %lld -no_uuid --input-load-demo=ios --input-load-workers=2 \
# RUN:   --time-trace=%t.ios.trace --time-trace-granularity=0 \
# RUN:   %t/main.o %t/libfoo.a %t/bar.o %t/bitcode.o -o /dev/null
# RUN: FileCheck %s --check-prefix=TRACE-IOS < %t.ios.trace
# RUN: %lld -no_uuid --input-load-demo=elf --input-load-workers=2 \
# RUN:   --time-trace=%t.elf.trace --time-trace-granularity=0 \
# RUN:   %t/main.o %t/libfoo.a %t/bar.o %t/bitcode.o -o /dev/null
# RUN: FileCheck %s --check-prefix=TRACE-ELF < %t.elf.trace
# RUN: %lld -no_uuid --input-load-demo=prime --input-load-workers=2 \
# RUN:   --time-trace=%t.prime.trace --time-trace-granularity=0 \
# RUN:   %t/main.o %t/libfoo.a %t/bar.o %t/bitcode.o -o /dev/null
# RUN: FileCheck %s --check-prefix=TRACE-PRIME < %t.prime.trace
# RUN: %lld -no_uuid --input-load-demo=mold --input-load-workers=2 \
# RUN:   --time-trace=%t.mold.trace --time-trace-granularity=0 \
# RUN:   %t/main.o %t/libfoo.a %t/bar.o %t/bitcode.o -o /dev/null
# RUN: FileCheck %s --check-prefix=TRACE-MOLD < %t.mold.trace
# RUN: not %lld %t/bad-cstring.o -o /dev/null 2> %t.bad-serial
# RUN: not %lld --input-load-demo=elf --input-load-workers=2 \
# RUN:   %t/bad-cstring.o -o /dev/null 2> %t.bad-elf
# RUN: not %lld --input-load-demo=prime --input-load-workers=2 \
# RUN:   %t/bad-cstring.o -o /dev/null 2> %t.bad-prime
# RUN: not %lld --input-load-demo=mold --input-load-workers=2 \
# RUN:   %t/bad-cstring.o -o /dev/null 2> %t.bad-mold
# RUN: diff %t.bad-serial %t.bad-elf
# RUN: diff %t.bad-serial %t.bad-prime
# RUN: diff %t.bad-serial %t.bad-mold
#
# BAD-MODE: error: --input-load-demo=: expected 'ios', 'elf', 'prime', or 'mold', but got 'unknown'
# BAD-WORKERS: error: --input-load-workers=: expected a non-negative integer, but got 'many'
# NEEDS-MODE: error: --input-load-workers requires --input-load-demo
# STATS-NEEDS-MODE: error: --input-load-stats requires --input-load-demo
# CONFLICT: error: --read-workers cannot be combined with --input-load-demo
# STATS: input-load-stats: mode=ios workers=2 tasks=5 completed=5 peak-active=2 mapped-files=5 mapped-bytes={{[1-9][0-9]*}}
# AUTOLINK: T _autoload
# LATE-WAVES-COUNT-2: input-load-stats: mode=prime
# TRACE-IOS: "name":"iOS input-load worker"
# TRACE-ELF-DAG: "name":"ELF input-load worker"
# TRACE-ELF-DAG: "name":"Mach-O object section parse"
# TRACE-PRIME-DAG: "name":"Prime input-load worker"
# TRACE-PRIME-DAG: "name":"Prime archive batch"
# TRACE-PRIME-DAG: "name":"Mach-O object section parse"
# TRACE-MOLD-DAG: "name":"Mold input-load worker"
# TRACE-MOLD-DAG: "name":"Mach-O object section parse"
# TRACE-MOLD-DAG: "name":"Mold archive member parse"

#--- main.s
.globl _main
_main:
  callq _foo
  callq _bar
  callq _bitcode
  ret

#--- foo.s
.globl _foo
_foo:
  ret

#--- bar.s
.globl _bar
_bar:
  ret

#--- bitcode.ll
target triple = "x86_64-apple-darwin"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"

define void @bitcode() {
  ret void
}

#--- autolink.s
.linker_option "-lautoload"
.globl _autolink
_autolink:
  callq _autoload
  ret

#--- autoload.s
.globl _autoload
_autoload:
  ret

#--- bad-cstring.s
.section __TEXT,__cstring,cstring_literals
.ascii "unterminated"

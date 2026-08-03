---
myst:
  heading_anchors: 3
---

# Appendix A: evidence ledger

This standalone document is Appendix A for
[InputLoadingComparison](InputLoadingComparison.md).

This appendix is the only evidence namespace for the report. Every bracketed
identifier in the body resolves here. A `*-SRC-*` record names a file and
line range at the pinned revision. A `*-BIN-*` or `*-RUN-*` record contains a
normalized raw dump and the command that produces the unnormalized record.
Addresses, temporary paths, timestamps, thread IDs, and output filenames are
descriptive rather than normative.

## Appendix A1: Reproduction entry point

Run the following from the LLVM checkout root on macOS with Xcode 26.3
installed. The commands keep generated corpora, traces, and extracted
disassembly outside the repository. `STUDY_TMP` is printed so the raw
directory can be retained by the reviewer.

```sh
set -eu

STUDY_LLVM_ROOT=$PWD
STUDY_MOLD_ROOT=/Users/bytedance/aster/mold
STUDY_BUILD=$STUDY_LLVM_ROOT/build-lld-parallelization
STUDY_BIN=$STUDY_BUILD/bin
STUDY_XCODE=/Applications/Xcode-26.3.0.app/Contents/Developer
STUDY_LD_PRIME=$STUDY_XCODE/Toolchains/XcodeDefault.xctoolchain/usr/bin/ld
STUDY_SDK=$(xcrun --sdk macosx --show-sdk-path)
STUDY_TMP=$(mktemp -d /private/tmp/input-loading-evidence.XXXXXX)
export STUDY_LLVM_ROOT STUDY_MOLD_ROOT STUDY_BUILD STUDY_BIN
export STUDY_XCODE STUDY_LD_PRIME STUDY_SDK STUDY_TMP
echo "$STUDY_TMP"

cmake -S "$STUDY_MOLD_ROOT" -B "$STUDY_TMP/mold-build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DMOLD_USE_MIMALLOC=OFF
cmake --build "$STUDY_TMP/mold-build" --target mold -j 4

{
  git -C "$STUDY_LLVM_ROOT" rev-parse HEAD
  "$STUDY_BIN/ld64.lld" --version
  "$STUDY_BIN/ld.lld" --version
  git -C "$STUDY_MOLD_ROOT" rev-parse HEAD
  "$STUDY_TMP/mold-build/mold" --version
  "$STUDY_LD_PRIME" -v
} >"$STUDY_TMP/versions.raw" 2>&1
```

### Evidence: VERSIONS

**exact implementations compared**

The captured `versions.raw` record was:

```text
e988985ea898026e79b0ec56ef74b12624aed708
LLD 24.0.0 (... e988985ea898026e79b0ec56ef74b12624aed708)
LLD 24.0.0 (... e988985ea898026e79b0ec56ef74b12624aed708)
f4a62c75039a5d73524e2fc49efa74d2b522256e
mold 2.41.0 (f4a62c75039a5d73524e2fc49efa74d2b522256e; compatible with GNU ld)
@(#)PROGRAM:ld PROJECT:ld-1230.1
BUILD 10:14:01 Jan  9 2026
```

The open-source extracts used by all `*-SRC-*` records can be regenerated in
one pass:

```sh
mkdir -p "$STUDY_TMP/source"

sed -n '426,596p;721,930p;1003,1058p;1099,1110p;1791,1823p;2790,2816p' \
  "$STUDY_LLVM_ROOT/lld/MachO/Driver.cpp" \
  >"$STUDY_TMP/source/macho-driver.raw"
sed -n '229,303p;365,1515p;1772,2100p;2260,2475p' \
  "$STUDY_LLVM_ROOT/lld/MachO/InputFiles.cpp" \
  >"$STUDY_TMP/source/macho-input-files.raw"
sed -n '227,327p' "$STUDY_LLVM_ROOT/lld/MachO/DriverUtils.cpp" \
  >"$STUDY_TMP/source/macho-dylib-cache.raw"
sed -n '31,473p' "$STUDY_LLVM_ROOT/lld/MachO/SymbolTable.cpp" \
  >"$STUDY_TMP/source/macho-symbol-table.raw"
sed -n '50,91p' "$STUDY_LLVM_ROOT/lld/include/lld/Common/Memory.h" \
  >"$STUDY_TMP/source/lld-memory.raw"

sed -n '194,292p;2110,2205p;2775,2825p;3195,3320p;3373,3412p' \
  "$STUDY_LLVM_ROOT/lld/ELF/Driver.cpp" \
  >"$STUDY_TMP/source/elf-driver.raw"
sed -n '214,410p;571,1308p;1552,1605p;1809,1950p' \
  "$STUDY_LLVM_ROOT/lld/ELF/InputFiles.cpp" \
  >"$STUDY_TMP/source/elf-input-files.raw"
sed -n '190,228p;650,720p' "$STUDY_LLVM_ROOT/lld/ELF/Config.h" \
  >"$STUDY_TMP/source/elf-context.raw"

sed -n '1,260p;320,375p' "$STUDY_MOLD_ROOT/src/main.cc" \
  >"$STUDY_TMP/source/mold-main.raw"
sed -n '160,190p' "$STUDY_MOLD_ROOT/src/archive-file.cc" \
  >"$STUDY_TMP/source/mold-archive.raw"
sed -n '960,1145p;1340,1610p' "$STUDY_MOLD_ROOT/src/input-files.cc" \
  >"$STUDY_TMP/source/mold-input-files.raw"
sed -n '210,450p' "$STUDY_MOLD_ROOT/src/passes.cc" \
  >"$STUDY_TMP/source/mold-passes.raw"
sed -n '1215,1250p;1740,1780p;2540,2610p;2835,2870p' \
  "$STUDY_MOLD_ROOT/src/mold.h" \
  >"$STUDY_TMP/source/mold-context.raw"
```

The ld-prime symbol, disassembly, and import dumps are produced by:

```sh
lipo "$STUDY_LD_PRIME" -thin arm64 \
  -output "$STUDY_TMP/ld-prime-arm64"
nm -nm "$STUDY_TMP/ld-prime-arm64" | c++filt \
  >"$STUDY_TMP/ld-prime-symbols.raw"
nm -um "$STUDY_TMP/ld-prime-arm64" \
  >"$STUDY_TMP/ld-prime-imports.raw"
otool -tvV -p __ZN2ld20AtomFileConsolidator10parseFilesEb \
  "$STUDY_TMP/ld-prime-arm64" \
  >"$STUDY_TMP/ld-prime-parse-files.raw"
otool -tvV "$STUDY_TMP/ld-prime-arm64" \
  >"$STUDY_TMP/ld-prime-all-text.raw"
```

Extract the temporary interposer from
[Appendix A2](#appendix-a2-temporary-ld-prime-dispatch-interposer), compile
it, and create equivalent direct-object and selected/unused-archive corpora:

```sh
sed -n '/^\/\* INPUT_LOADING_INTERPOSER_BEGIN \*\/$/,/^\/\* INPUT_LOADING_INTERPOSER_END \*\/$/p' \
  "$STUDY_LLVM_ROOT/lld/docs/MachO/InputLoadingComparisonAppendix.md" \
  | sed '1d;$d' >"$STUDY_TMP/trace-input-loading.c"
xcrun clang -dynamiclib -fblocks -O2 -DLDPRIME_TRACE_EXTENDED=1 \
  "$STUDY_TMP/trace-input-loading.c" \
  -o "$STUDY_TMP/trace-input-loading.dylib"

mkdir -p "$STUDY_TMP/lp/direct/a" "$STUDY_TMP/lp/direct/b"
mkdir -p "$STUDY_TMP/lp/archive/a" "$STUDY_TMP/lp/archive/b"

printf '.globl _main\n_main:\n  ret\n' |
  "$STUDY_BIN/llvm-mc" -filetype=obj -triple=arm64-apple-macos \
    -o "$STUDY_TMP/lp/direct/main.o"
for STUDY_N in $(jot 10); do
  printf '.globl _f%s\n_f%s:\n  ret\n' "$STUDY_N" "$STUDY_N" |
    "$STUDY_BIN/llvm-mc" -filetype=obj -triple=arm64-apple-macos \
      -o "$STUDY_TMP/lp/direct/f$STUDY_N.o"
done

printf '.globl _main\n_main:\n  bl _f1\n  ret\n' |
  "$STUDY_BIN/llvm-mc" -filetype=obj -triple=arm64-apple-macos \
    -o "$STUDY_TMP/lp/archive/main.o"
for STUDY_N in $(jot 10); do
  printf '.globl _f%s\n_f%s:\n  ret\n' "$STUDY_N" "$STUDY_N" |
    "$STUDY_BIN/llvm-mc" -filetype=obj -triple=arm64-apple-macos \
      -o "$STUDY_TMP/lp/archive/f$STUDY_N.o"
done
"$STUDY_BIN/llvm-ar" rcs "$STUDY_TMP/lp/archive/libf.a" \
  "$STUDY_TMP"/lp/archive/f*.o

LDPRIME_TRACE_FILE="$STUDY_TMP/lp-direct.jsonl" \
LDPRIME_TRACE_CALLER_OFFSET=0x107858 \
DYLD_INSERT_LIBRARIES="$STUDY_TMP/trace-input-loading.dylib" \
"$STUDY_LD_PRIME" -arch arm64 -platform_version macos 26 26 \
  -syslibroot "$STUDY_SDK" -lSystem -e _main -no_uuid \
  "$STUDY_TMP/lp/direct/main.o" "$STUDY_TMP"/lp/direct/f*.o \
  -o "$STUDY_TMP/lp/direct/a/app"
"$STUDY_LD_PRIME" -arch arm64 -platform_version macos 26 26 \
  -syslibroot "$STUDY_SDK" -lSystem -e _main -no_uuid \
  "$STUDY_TMP/lp/direct/main.o" "$STUDY_TMP"/lp/direct/f*.o \
  -o "$STUDY_TMP/lp/direct/b/app"

LDPRIME_TRACE_FILE="$STUDY_TMP/lp-archive.jsonl" \
LDPRIME_TRACE_CALLER_OFFSET=0x107858 \
DYLD_INSERT_LIBRARIES="$STUDY_TMP/trace-input-loading.dylib" \
"$STUDY_LD_PRIME" -arch arm64 -platform_version macos 26 26 \
  -syslibroot "$STUDY_SDK" -lSystem -e _main -no_uuid \
  "$STUDY_TMP/lp/archive/main.o" "$STUDY_TMP/lp/archive/libf.a" \
  -o "$STUDY_TMP/lp/archive/a/app"
"$STUDY_LD_PRIME" -arch arm64 -platform_version macos 26 26 \
  -syslibroot "$STUDY_SDK" -lSystem -e _main -no_uuid \
  "$STUDY_TMP/lp/archive/main.o" "$STUDY_TMP/lp/archive/libf.a" \
  -o "$STUDY_TMP/lp/archive/b/app"
```

Normalize each ld-prime trace without erasing distinct worker identities:

```sh
for STUDY_TRACE in "$STUDY_TMP/lp-direct.jsonl" \
                   "$STUDY_TMP/lp-archive.jsonl"; do
  jq -s '
    ([.[] | select(.kind=="dispatch_apply" and .ph=="S")][0]) as $a |
    ([.[] | select(.kind=="dispatch_apply" and .ph=="F"
                   and .id==$a.id)][0]) as $z |
    {initial_count:$a.count,
     initial_worker_tids:
       ([.[] | select(.kind=="dispatch_apply_item" and .ph=="B"
                      and .id==$a.id) | .tid] | unique),
     combiner_submissions_during_initial_apply:
       ([.[] | select(.kind=="dispatch_group_async" and .ph=="S"
                      and .ts_ns >= $a.ts_ns and .ts_ns <= $z.ts_ns)]
        | length),
     later_parse_counts:
       ([.[] | select(.kind=="dispatch_apply" and .ph=="S")][1:]
        | map(.count))}' "$STUDY_TRACE"
done >"$STUDY_TMP/ld-prime-traces.normalized"

shasum -a 256 "$STUDY_TMP"/lp/direct/{a,b}/app \
  "$STUDY_TMP"/lp/archive/{a,b}/app \
  >"$STUDY_TMP/ld-prime-hashes.raw"
```

For Mach-O LLD, the repository test is also the canonical compact corpus. Its
`RUN` lines compile direct object, regular archive, bitcode, and autolink
inputs and exercise serial plus all three experimental modes:

```sh
mkdir -p "$STUDY_TMP/macho"
"$STUDY_BIN/split-file" \
  "$STUDY_LLVM_ROOT/lld/test/MachO/input-load-demo.s" \
  "$STUDY_TMP/macho"
"$STUDY_BIN/llvm-mc" -filetype=obj -triple=x86_64-apple-darwin \
  "$STUDY_TMP/macho/main.s" -o "$STUDY_TMP/macho/main.o"
"$STUDY_BIN/llvm-mc" -filetype=obj -triple=x86_64-apple-darwin \
  "$STUDY_TMP/macho/foo.s" -o "$STUDY_TMP/macho/foo.o"
"$STUDY_BIN/llvm-mc" -filetype=obj -triple=x86_64-apple-darwin \
  "$STUDY_TMP/macho/bar.s" -o "$STUDY_TMP/macho/bar.o"
"$STUDY_BIN/llvm-as" "$STUDY_TMP/macho/bitcode.ll" \
  -o "$STUDY_TMP/macho/bitcode.o"
"$STUDY_BIN/llvm-ar" rcs "$STUDY_TMP/macho/libfoo.a" \
  "$STUDY_TMP/macho/foo.o"

"$STUDY_BIN/ld64.lld" -arch x86_64 -platform_version macos 13 13 \
  -no_uuid "$STUDY_TMP/macho/main.o" "$STUDY_TMP/macho/libfoo.a" \
  "$STUDY_TMP/macho/bar.o" "$STUDY_TMP/macho/bitcode.o" \
  -o "$STUDY_TMP/macho/serial"
"$STUDY_BIN/ld64.lld" -arch x86_64 -platform_version macos 13 13 \
  -no_uuid --input-load-demo=prime --input-load-workers=2 \
  --input-load-stats --time-trace="$STUDY_TMP/macho/prime.trace" \
  --time-trace-granularity=0 \
  "$STUDY_TMP/macho/main.o" "$STUDY_TMP/macho/libfoo.a" \
  "$STUDY_TMP/macho/bar.o" "$STUDY_TMP/macho/bitcode.o" \
  -o "$STUDY_TMP/macho/prime" \
  2>"$STUDY_TMP/macho/prime.stderr"
cmp "$STUDY_TMP/macho/serial" "$STUDY_TMP/macho/prime"
shasum -a 256 "$STUDY_TMP/macho/serial" "$STUDY_TMP/macho/prime" \
  >"$STUDY_TMP/macho/hashes.raw"
jq '[.traceEvents[] |
     select(.name=="Prime input-load worker" or
            .name=="Prime archive batch") |
     {name,tid,ts,dur,args}]' "$STUDY_TMP/macho/prime.trace" \
  >"$STUDY_TMP/macho/prime-events.normalized"
```

Create all three archive encodings from the same member, then substitute each
for `libfoo.a` in the preceding two link commands:

```sh
"$STUDY_BIN/llvm-ar" rcsT "$STUDY_TMP/macho/libfoo-thin.a" \
  "$STUDY_TMP/macho/foo.o"
"$STUDY_BIN/llvm-ar" rcS "$STUDY_TMP/macho/libfoo-noindex.a" \
  "$STUDY_TMP/macho/foo.o"
cp "$STUDY_TMP/macho/libfoo.a" "$STUDY_TMP/macho/libfoo-malformed.a"
truncate -s -8 "$STUDY_TMP/macho/libfoo-malformed.a"
```

The ELF LLD/mold matrix uses the same input bytes and varies only linker and
thread count:

```sh
mkdir -p "$STUDY_TMP/elf"
printf '.globl _start\n_start:\n  call used\n  ret\n' |
  "$STUDY_BIN/llvm-mc" -filetype=obj -triple=x86_64 \
    -o "$STUDY_TMP/elf/main.o"
printf '.globl used\nused:\n  ret\n' |
  "$STUDY_BIN/llvm-mc" -filetype=obj -triple=x86_64 \
    -o "$STUDY_TMP/elf/used.o"
printf '.globl unused\nunused:\n  ret\n' |
  "$STUDY_BIN/llvm-mc" -filetype=obj -triple=x86_64 \
    -o "$STUDY_TMP/elf/unused.o"
"$STUDY_BIN/llvm-ar" rcs "$STUDY_TMP/elf/libcase.a" \
  "$STUDY_TMP/elf/used.o" "$STUDY_TMP/elf/unused.o"
"$STUDY_BIN/llvm-ar" rcsT "$STUDY_TMP/elf/libcase-thin.a" \
  "$STUDY_TMP/elf/used.o" "$STUDY_TMP/elf/unused.o"
"$STUDY_BIN/llvm-ar" rcS "$STUDY_TMP/elf/libcase-noindex.a" \
  "$STUDY_TMP/elf/used.o" "$STUDY_TMP/elf/unused.o"

for STUDY_LINKER in "$STUDY_BIN/ld.lld" \
  "$STUDY_TMP/mold-build/mold"; do
  STUDY_NAME=$(basename "$STUDY_LINKER")
  for STUDY_THREADS in 1 4; do
    for STUDY_KIND in regular thin noindex; do
      case "$STUDY_KIND" in
        regular) STUDY_ARCHIVE="$STUDY_TMP/elf/libcase.a" ;;
        thin) STUDY_ARCHIVE="$STUDY_TMP/elf/libcase-thin.a" ;;
        noindex) STUDY_ARCHIVE="$STUDY_TMP/elf/libcase-noindex.a" ;;
      esac
      "$STUDY_LINKER" -m elf_x86_64 --threads="$STUDY_THREADS" \
        -e _start "$STUDY_TMP/elf/main.o" "$STUDY_ARCHIVE" \
        -o "$STUDY_TMP/elf/$STUDY_NAME-$STUDY_KIND-t$STUDY_THREADS"
    done
    "$STUDY_LINKER" -m elf_x86_64 --threads="$STUDY_THREADS" \
      -e _start "$STUDY_TMP/elf/main.o" --whole-archive \
      "$STUDY_TMP/elf/libcase.a" \
      -o "$STUDY_TMP/elf/$STUDY_NAME-whole-t$STUDY_THREADS"
  done
done
shasum -a 256 "$STUDY_TMP"/elf/ld.lld-* \
  >"$STUDY_TMP/elf/lld-hashes.raw"
shasum -a 256 "$STUDY_TMP"/elf/mold-* \
  >"$STUDY_TMP/elf/mold-hashes.raw"
```

The remaining successful Mach-O cases can be generated and paired exactly as
follows. The fixed install name removes output-path identity from the dylib
comparison.

```sh
STUDY_MACHO_HASHES="$STUDY_TMP/macho/full-matrix-hashes.raw"
: >"$STUDY_MACHO_HASHES"

study_macho_pair() {
  STUDY_CASE=$1
  shift
  "$STUDY_BIN/ld64.lld" -arch x86_64 \
    -platform_version macos 13 13 -no_uuid "$@" \
    -o "$STUDY_TMP/macho/$STUDY_CASE-serial"
  "$STUDY_BIN/ld64.lld" -arch x86_64 \
    -platform_version macos 13 13 -no_uuid \
    --input-load-demo=prime --input-load-workers=2 "$@" \
    -o "$STUDY_TMP/macho/$STUDY_CASE-prime"
  cmp "$STUDY_TMP/macho/$STUDY_CASE-serial" \
    "$STUDY_TMP/macho/$STUDY_CASE-prime"
  shasum -a 256 "$STUDY_TMP/macho/$STUDY_CASE-serial" \
    "$STUDY_TMP/macho/$STUDY_CASE-prime" >>"$STUDY_MACHO_HASHES"
}

"$STUDY_BIN/llvm-ar" rcs "$STUDY_TMP/macho/libbitcode.a" \
  "$STUDY_TMP/macho/bitcode.o"
study_macho_pair thin "$STUDY_TMP/macho/main.o" \
  "$STUDY_TMP/macho/libfoo-thin.a" "$STUDY_TMP/macho/bar.o" \
  "$STUDY_TMP/macho/bitcode.o"
study_macho_pair noindex "$STUDY_TMP/macho/main.o" \
  "$STUDY_TMP/macho/libfoo-noindex.a" "$STUDY_TMP/macho/bar.o" \
  "$STUDY_TMP/macho/bitcode.o"
study_macho_pair lto-archive "$STUDY_TMP/macho/main.o" \
  "$STUDY_TMP/macho/libfoo.a" "$STUDY_TMP/macho/bar.o" \
  "$STUDY_TMP/macho/libbitcode.a"
study_macho_pair whole -all_load "$STUDY_TMP/macho/main.o" \
  "$STUDY_TMP/macho/libfoo.a" "$STUDY_TMP/macho/bar.o" \
  "$STUDY_TMP/macho/bitcode.o"

"$STUDY_BIN/llvm-mc" -filetype=obj -triple=x86_64-apple-darwin \
  "$STUDY_TMP/macho/autolink.s" -o "$STUDY_TMP/macho/autolink.o"
"$STUDY_BIN/llvm-mc" -filetype=obj -triple=x86_64-apple-darwin \
  "$STUDY_TMP/macho/autoload.s" -o "$STUDY_TMP/macho/autoload.o"
"$STUDY_BIN/llvm-ar" rcs "$STUDY_TMP/macho/libautoload.a" \
  "$STUDY_TMP/macho/autoload.o"
study_macho_pair autolink -dylib -L"$STUDY_TMP/macho" \
  -install_name @rpath/libcase.dylib "$STUDY_TMP/macho/autolink.o"

printf '%s\n' \
  '.weak_definition _winner' '.globl _winner' '_winner:' \
  '  movl $1, %eax' '  ret' '.comm _common,8,3' |
  "$STUDY_BIN/llvm-mc" -filetype=obj -triple=x86_64-apple-darwin \
    -o "$STUDY_TMP/macho/weak1.o"
printf '%s\n' \
  '.weak_definition _winner' '.globl _winner' '_winner:' \
  '  movl $2, %eax' '  ret' '.comm _common,16,3' |
  "$STUDY_BIN/llvm-mc" -filetype=obj -triple=x86_64-apple-darwin \
    -o "$STUDY_TMP/macho/weak2.o"
printf '%s\n' \
  '.globl _main' '_main:' '  callq _winner' \
  '  leaq _common(%rip), %rax' '  ret' |
  "$STUDY_BIN/llvm-mc" -filetype=obj -triple=x86_64-apple-darwin \
    -o "$STUDY_TMP/macho/weak-main.o"
study_macho_pair weak-12 "$STUDY_TMP/macho/weak-main.o" \
  "$STUDY_TMP/macho/weak1.o" "$STUDY_TMP/macho/weak2.o"
study_macho_pair weak-21 "$STUDY_TMP/macho/weak-main.o" \
  "$STUDY_TMP/macho/weak2.o" "$STUDY_TMP/macho/weak1.o"

printf '%s\n' \
  '.globl _main' '_main:' '  callq _bar' '  callq __Z3foo' '  ret' |
  "$STUDY_BIN/llvm-mc" -filetype=obj -triple=x86_64-apple-darwin \
    -o "$STUDY_TMP/macho/objc-main.o"
printf '%s\n' '.globl __Z3foo' '__Z3foo:' '  ret' |
  "$STUDY_BIN/llvm-mc" -filetype=obj -triple=x86_64-apple-darwin \
    -o "$STUDY_TMP/macho/objc-foo.o"
printf '%s\n' '.globl _bar' '_bar:' '  callq __Z3foo' '  ret' |
  "$STUDY_BIN/llvm-mc" -filetype=obj -triple=x86_64-apple-darwin \
    -o "$STUDY_TMP/macho/objc-bar.o"
printf '%s\n' '.section __DATA,__objc_catlist' '.quad 0x1234' |
  "$STUDY_BIN/llvm-mc" -filetype=obj -triple=x86_64-apple-darwin \
    -o "$STUDY_TMP/macho/objc-category.o"
"$STUDY_BIN/llvm-ar" rcs "$STUDY_TMP/macho/libobjc-case.a" \
  "$STUDY_TMP/macho/objc-category.o" "$STUDY_TMP/macho/objc-foo.o" \
  "$STUDY_TMP/macho/objc-bar.o"
study_macho_pair objc -ObjC "$STUDY_TMP/macho/objc-main.o" \
  "$STUDY_TMP/macho/libobjc-case.a"
study_macho_pair force "$STUDY_TMP/macho/objc-main.o" \
  -force_load "$STUDY_TMP/macho/libobjc-case.a"
cmp "$STUDY_TMP/macho/objc-serial" "$STUDY_TMP/macho/force-serial"
```

Generate the ELF LTO, DSO/`.deplibs`, and weak/common inputs, then use the same
function for ELF LLD and mold:

```sh
cat >"$STUDY_TMP/elf/used.ll" <<'EOF'
target triple = "x86_64-unknown-linux-gnu"
define void @used() {
  ret void
}
EOF
"$STUDY_BIN/llvm-as" "$STUDY_TMP/elf/used.ll" \
  -o "$STUDY_TMP/elf/used.bc"
"$STUDY_BIN/llvm-ar" rcs "$STUDY_TMP/elf/liblto.a" \
  "$STUDY_TMP/elf/used.bc"
"$STUDY_BIN/ld.lld" -shared -soname libused.so \
  "$STUDY_TMP/elf/used.o" -o "$STUDY_TMP/elf/libused.so"
printf '%s\n' \
  '.globl _start' '_start:' '  call used' '  ret' \
  '.section ".deplibs","MS",@llvm_dependent_libraries,1' \
  '.asciz "used"' |
  "$STUDY_BIN/llvm-mc" -filetype=obj -triple=x86_64 \
    -o "$STUDY_TMP/elf/main-deplib.o"

printf '%s\n' \
  '.weak winner' 'winner:' \
  '  movl $1, %eax' '  ret' '.comm common,8,8' |
  "$STUDY_BIN/llvm-mc" -filetype=obj -triple=x86_64 \
    -o "$STUDY_TMP/elf/weak1.o"
printf '%s\n' \
  '.weak winner' 'winner:' \
  '  movl $2, %eax' '  ret' '.comm common,16,8' |
  "$STUDY_BIN/llvm-mc" -filetype=obj -triple=x86_64 \
    -o "$STUDY_TMP/elf/weak2.o"
printf '%s\n' \
  '.globl _start' '_start:' '  call winner' \
  '  leaq common(%rip), %rax' '  ret' |
  "$STUDY_BIN/llvm-mc" -filetype=obj -triple=x86_64 \
    -o "$STUDY_TMP/elf/weak-main.o"

: >"$STUDY_TMP/elf/extended-hashes.raw"
study_elf_pair() {
  STUDY_LINKER=$1
  STUDY_TAG=$2
  STUDY_CASE=$3
  shift 3
  "$STUDY_LINKER" -m elf_x86_64 --threads=1 -e _start "$@" \
    -o "$STUDY_TMP/elf/$STUDY_TAG-$STUDY_CASE-t1"
  "$STUDY_LINKER" -m elf_x86_64 --threads=4 -e _start "$@" \
    -o "$STUDY_TMP/elf/$STUDY_TAG-$STUDY_CASE-t4"
  cmp "$STUDY_TMP/elf/$STUDY_TAG-$STUDY_CASE-t1" \
    "$STUDY_TMP/elf/$STUDY_TAG-$STUDY_CASE-t4"
  shasum -a 256 "$STUDY_TMP/elf/$STUDY_TAG-$STUDY_CASE-t1" \
    "$STUDY_TMP/elf/$STUDY_TAG-$STUDY_CASE-t4" \
    >>"$STUDY_TMP/elf/extended-hashes.raw"
}

study_elf_pair "$STUDY_BIN/ld.lld" lld lto-direct \
  "$STUDY_TMP/elf/main.o" "$STUDY_TMP/elf/used.bc"
study_elf_pair "$STUDY_BIN/ld.lld" lld lto-archive \
  "$STUDY_TMP/elf/main.o" "$STUDY_TMP/elf/liblto.a"
study_elf_pair "$STUDY_BIN/ld.lld" lld dso \
  "$STUDY_TMP/elf/main.o" "$STUDY_TMP/elf/libused.so"
study_elf_pair "$STUDY_BIN/ld.lld" lld deplibs \
  -L"$STUDY_TMP/elf" "$STUDY_TMP/elf/main-deplib.o"
study_elf_pair "$STUDY_BIN/ld.lld" lld weak-common \
  "$STUDY_TMP/elf/weak-main.o" "$STUDY_TMP/elf/weak1.o" \
  "$STUDY_TMP/elf/weak2.o"

study_elf_pair "$STUDY_TMP/mold-build/mold" mold dso \
  "$STUDY_TMP/elf/main.o" "$STUDY_TMP/elf/libused.so"
study_elf_pair "$STUDY_TMP/mold-build/mold" mold weak-common \
  "$STUDY_TMP/elf/weak-main.o" "$STUDY_TMP/elf/weak1.o" \
  "$STUDY_TMP/elf/weak2.o"
```

Finally, capture failure equivalence rather than hashes. The helper requires
both variants to fail and compares diagnostics after replacing the temporary
prefix:

```sh
study_macho_fail_pair() {
  STUDY_CASE=$1
  shift
  set +e
  "$STUDY_BIN/ld64.lld" -arch x86_64 \
    -platform_version macos 13 13 -no_uuid "$@" -o /dev/null \
    2>"$STUDY_TMP/macho/$STUDY_CASE-serial.stderr"
  STUDY_STATUS_SERIAL=$?
  "$STUDY_BIN/ld64.lld" -arch x86_64 \
    -platform_version macos 13 13 -no_uuid \
    --input-load-demo=prime --input-load-workers=2 "$@" -o /dev/null \
    2>"$STUDY_TMP/macho/$STUDY_CASE-prime.stderr"
  STUDY_STATUS_PRIME=$?
  set -e
  test "$STUDY_STATUS_SERIAL" -ne 0
  test "$STUDY_STATUS_PRIME" -ne 0
  sed "s#$STUDY_TMP#\\$CASE#g" \
    "$STUDY_TMP/macho/$STUDY_CASE-serial.stderr" \
    >"$STUDY_TMP/macho/$STUDY_CASE-serial.normalized"
  sed "s#$STUDY_TMP#\\$CASE#g" \
    "$STUDY_TMP/macho/$STUDY_CASE-prime.stderr" \
    >"$STUDY_TMP/macho/$STUDY_CASE-prime.normalized"
  cmp "$STUDY_TMP/macho/$STUDY_CASE-serial.normalized" \
    "$STUDY_TMP/macho/$STUDY_CASE-prime.normalized"
}

ln -s "$STUDY_TMP/macho/foo.o" "$STUDY_TMP/macho/foo-link.o"
study_macho_fail_pair duplicate -dylib "$STUDY_TMP/macho/foo.o" \
  "$STUDY_TMP/macho/foo-link.o"
study_macho_fail_pair malformed -dylib \
  "$STUDY_TMP/macho/libfoo-malformed.a"

study_elf_fail_pair() {
  STUDY_LINKER=$1
  STUDY_TAG=$2
  STUDY_CASE=$3
  shift 3
  set +e
  "$STUDY_LINKER" -m elf_x86_64 --threads=1 -e _start "$@" -o /dev/null \
    2>"$STUDY_TMP/elf/$STUDY_TAG-$STUDY_CASE-t1.stderr"
  STUDY_STATUS_T1=$?
  "$STUDY_LINKER" -m elf_x86_64 --threads=4 -e _start "$@" -o /dev/null \
    2>"$STUDY_TMP/elf/$STUDY_TAG-$STUDY_CASE-t4.stderr"
  STUDY_STATUS_T4=$?
  set -e
  test "$STUDY_STATUS_T1" -ne 0
  test "$STUDY_STATUS_T4" -ne 0
  sed "s#$STUDY_TMP#\\$CASE#g" \
    "$STUDY_TMP/elf/$STUDY_TAG-$STUDY_CASE-t1.stderr" \
    >"$STUDY_TMP/elf/$STUDY_TAG-$STUDY_CASE-t1.normalized"
  sed "s#$STUDY_TMP#\\$CASE#g" \
    "$STUDY_TMP/elf/$STUDY_TAG-$STUDY_CASE-t4.stderr" \
    >"$STUDY_TMP/elf/$STUDY_TAG-$STUDY_CASE-t4.normalized"
  cmp "$STUDY_TMP/elf/$STUDY_TAG-$STUDY_CASE-t1.normalized" \
    "$STUDY_TMP/elf/$STUDY_TAG-$STUDY_CASE-t4.normalized"
}

cp "$STUDY_TMP/elf/libcase.a" "$STUDY_TMP/elf/libcase-malformed.a"
truncate -s -8 "$STUDY_TMP/elf/libcase-malformed.a"
ln -s "$STUDY_TMP/elf/used.o" "$STUDY_TMP/elf/used-link.o"
for STUDY_LINKER_TAG in \
  "$STUDY_BIN/ld.lld:lld" \
  "$STUDY_TMP/mold-build/mold:mold"; do
  STUDY_LINKER=${STUDY_LINKER_TAG%:*}
  STUDY_TAG=${STUDY_LINKER_TAG##*:}
  study_elf_fail_pair "$STUDY_LINKER" "$STUDY_TAG" duplicate \
    "$STUDY_TMP/elf/main.o" "$STUDY_TMP/elf/used.o" \
    "$STUDY_TMP/elf/used-link.o"
  study_elf_fail_pair "$STUDY_LINKER" "$STUDY_TAG" malformed \
    "$STUDY_TMP/elf/main.o" "$STUDY_TMP/elf/libcase-malformed.a"
done

study_elf_fail_pair "$STUDY_TMP/mold-build/mold" mold lto \
  "$STUDY_TMP/elf/main.o" "$STUDY_TMP/elf/used.bc"
study_elf_fail_pair "$STUDY_TMP/mold-build/mold" mold deplibs \
  -L"$STUDY_TMP/elf" "$STUDY_TMP/elf/main-deplib.o"
```

The maintained tests
[`input-load-demo.s`](../../test/MachO/input-load-demo.s),
[`archive.s`](../../test/MachO/archive.s),
[`objc.s`](../../test/MachO/objc.s),
[`weak-definition-order.s`](../../test/MachO/weak-definition-order.s),
[`deplibs.s`](../../test/ELF/deplibs.s),
[`whole-archive.s`](../../test/ELF/whole-archive.s), and
[`lto/archive.ll`](../../test/ELF/lto/archive.ll) provide independent,
continuously tested forms of the same input syntax.

## Appendix A2: Temporary ld-prime dispatch interposer

This listing is copied to `STUDY_TMP` by A.1; it is not a production linker
component. The caller-offset filter is required because ld-prime uses
`dispatch_apply` in later phases too. `0x107858` is the return address of the
first `dispatch_apply` call in `AtomFileConsolidator::parseFiles(bool)` in the
captured Xcode 26.3 arm64 slice. Recompute it from `ld-prime-parse-files.raw`
for another build.

```c
/* INPUT_LOADING_INTERPOSER_BEGIN */
#define _DARWIN_C_SOURCE

#include <Block.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int trace_fd = STDERR_FILENO;
static mach_timebase_info_data_t timebase;
static _Atomic uint64_t next_event_id = 1;
static pthread_once_t trace_once = PTHREAD_ONCE_INIT;
static uintptr_t trace_caller_offset;

static void initialize_trace_once(void) {
  (void)mach_timebase_info(&timebase);
  const char *path = getenv("LDPRIME_TRACE_FILE");
  if (path && path[0]) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (fd >= 0)
      trace_fd = fd;
  }
  const char *offset = getenv("LDPRIME_TRACE_CALLER_OFFSET");
  if (offset && offset[0])
    trace_caller_offset = (uintptr_t)strtoull(offset, NULL, 0);
}

static uint64_t timestamp_ns(void) {
  (void)pthread_once(&trace_once, initialize_trace_once);
  uint64_t ticks = mach_continuous_time();
  return ticks * timebase.numer / timebase.denom;
}

static uint64_t thread_id(void) {
  uint64_t tid = 0;
  (void)pthread_threadid_np(NULL, &tid);
  return tid;
}

static const char *queue_label(dispatch_queue_t queue) {
  return queue ? dispatch_queue_get_label(queue) : "DISPATCH_APPLY_AUTO";
}

static void describe_address(const void *address, const char **image,
                             uintptr_t *offset) {
  Dl_info info = {0};
  if (address && dladdr(address, &info) && info.dli_fbase) {
    *image = info.dli_fname ? info.dli_fname : "";
    *offset = (uintptr_t)address - (uintptr_t)info.dli_fbase;
    return;
  }
  *image = "";
  *offset = 0;
}

static void emit_event(const char *kind, const char *phase, uint64_t id,
                       size_t index, size_t count, const char *queue,
                       const char *image, uintptr_t caller_offset) {
  char buffer[1536];
  int length = snprintf(
      buffer, sizeof(buffer),
      "{\"ts_ns\":%llu,\"tid\":%llu,\"ph\":\"%s\",\"kind\":\"%s\","
      "\"id\":%llu,\"index\":%zu,\"count\":%zu,\"queue\":\"%s\","
      "\"image\":\"%s\",\"caller_offset\":\"0x%llx\"}\n",
      (unsigned long long)timestamp_ns(), (unsigned long long)thread_id(),
      phase, kind, (unsigned long long)id, index, count, queue ? queue : "",
      image ? image : "", (unsigned long long)caller_offset);
  if (length > 0) {
    size_t bytes = (size_t)length < sizeof(buffer) ? (size_t)length
                                                   : sizeof(buffer) - 1;
    (void)write(trace_fd, buffer, bytes);
  }
}

__attribute__((destructor, used)) static void finish_trace(void) {
  if (trace_fd != STDERR_FILENO)
    (void)close(trace_fd);
}

struct apply_context {
  void (^block)(size_t);
  uint64_t id;
  size_t iterations;
  const char *label;
  const char *image;
  uintptr_t offset;
};

struct passthrough_apply_context {
  void (^block)(size_t);
};

static void invoke_passthrough_apply_item(void *opaque, size_t index) {
  struct passthrough_apply_context *context = opaque;
  context->block(index);
}

static void invoke_apply_item(void *opaque, size_t index) {
  struct apply_context *context = opaque;
  emit_event("dispatch_apply_item", "B", context->id, index,
             context->iterations, context->label, context->image,
             context->offset);
  context->block(index);
  emit_event("dispatch_apply_item", "E", context->id, index,
             context->iterations, context->label, context->image,
             context->offset);
}

static void traced_dispatch_apply(size_t iterations, dispatch_queue_t queue,
                                  void (^block)(size_t)) {
  const void *caller = __builtin_return_address(0);
  const char *image;
  uintptr_t offset;
  describe_address(caller, &image, &offset);
  (void)pthread_once(&trace_once, initialize_trace_once);
  if (trace_caller_offset && trace_caller_offset != offset) {
    struct passthrough_apply_context context = {block};
    dispatch_apply_f(iterations, queue, &context, invoke_passthrough_apply_item);
    return;
  }

  uint64_t id =
      atomic_fetch_add_explicit(&next_event_id, 1, memory_order_relaxed);
  const char *label = queue_label(queue);
  emit_event("dispatch_apply", "S", id, 0, iterations, label, image, offset);
  struct apply_context context = {block, id, iterations, label, image, offset};
  dispatch_apply_f(iterations, queue, &context, invoke_apply_item);
  emit_event("dispatch_apply", "F", id, 0, iterations, label, image, offset);
}

struct group_async_context {
  dispatch_block_t block;
  uint64_t id;
  const char *label;
  const char *image;
  uintptr_t offset;
  int should_trace;
};

static void invoke_group_async(void *opaque) {
  struct group_async_context *context = opaque;
  if (context->should_trace)
    emit_event("dispatch_group_async", "B", context->id, 0, 0, context->label,
               context->image, context->offset);
  context->block();
  if (context->should_trace)
    emit_event("dispatch_group_async", "E", context->id, 0, 0, context->label,
               context->image, context->offset);
  Block_release(context->block);
  free(context);
}

static void traced_dispatch_group_async(dispatch_group_t group,
                                        dispatch_queue_t queue,
                                        dispatch_block_t block) {
  struct group_async_context *context = malloc(sizeof(*context));
  if (!context)
    abort();

  const void *caller = __builtin_return_address(0);
  describe_address(caller, &context->image, &context->offset);
  context->id =
      atomic_fetch_add_explicit(&next_event_id, 1, memory_order_relaxed);
  context->label = queue_label(queue);
  context->should_trace =
      strcmp(context->label, "com.apple.ld.combiner") == 0;
  context->block = Block_copy(block);

  if (context->should_trace)
    emit_event("dispatch_group_async", "S", context->id, 0, 0, context->label,
               context->image, context->offset);
  dispatch_group_async_f(group, queue, context, invoke_group_async);
}

#define DYLD_INTERPOSE(replacement, replacee)                                  \
  __attribute__((used)) static struct {                                        \
    const void *replacement;                                                   \
    const void *replacee;                                                      \
  } _interpose_##replacee __attribute__((section("__DATA,__interpose"))) = {   \
      (const void *)(uintptr_t)&replacement,                                   \
      (const void *)(uintptr_t)&replacee}

DYLD_INTERPOSE(traced_dispatch_apply, dispatch_apply);
#ifdef LDPRIME_TRACE_EXTENDED
DYLD_INTERPOSE(traced_dispatch_group_async, dispatch_group_async);
#endif
/* INPUT_LOADING_INTERPOSER_END */
```

## A.3 Open-source evidence records

All LLVM links in this subsection refer to
`e988985ea898026e79b0ec56ef74b12624aed708`. All mold links include the
`f4a62c75039a5d73524e2fc49efa74d2b522256e` revision in the URL. The A.1
`sed` commands create stable local extracts in addition to these locations.

### Evidence: MO-SRC-DRIVER

**Mach-O driver ownership and `prime` barrier**

Source locations:

- [`processFile`, lines 426–596](../../MachO/Driver.cpp) performs ordered
  classification and constructs archive, object, dylib, and bitcode inputs.
- [`prepareInput`, `prepareInputLoadDemo`, and `deferFile`, lines
  721–930](../../MachO/Driver.cpp) define exactly what workers may prepare,
  use one indexed slot per deferred input, and wait before adoption.
- [ordered adoption and `processFile`, lines
  1791–1823](../../MachO/Driver.cpp) prove that worker completion order does
  not publish files.

The source comment at lines 721–725 explicitly scopes the demos to open/map,
page-in, classification, archive-directory construction, and bitcode
preparation, and explicitly excludes Mach-O `ObjFile` construction.

### Evidence: MO-SRC-IO

**Mach-O mapping, caching, and fat slices**

[`readFile`, lines 229–303](../../MachO/InputFiles.cpp) owns the path-keyed
`cachedReads` lookup, `MemoryBuffer::getFile`, architecture/fat-slice
selection, and buffer retention. [`prepareInputLoadDemo`, lines
817–897](../../MachO/Driver.cpp) shows the `prime` duplicate-map case: after
the barrier the driver adopts through `readFile`, then discards preparation
whose buffer does not match the cache-selected mapping.

### Evidence: MO-SRC-OBJ

**Mach-O native and bitcode parse mutations**

Source locations in [`InputFiles.cpp`](../../MachO/InputFiles.cpp):

- `ObjFile::parseSections`, lines 365–560;
- `ObjFile::parseRelocations`, lines 561–696;
- `ObjFile::parseSymbols`, lines 845–1009;
- `ObjFile` construction and `ObjFile::parse`, lines 1023–1133;
- debug, compact-unwind, and EH-frame work, lines 1137–1515; and
- `BitcodeFile::parse`, lines 2450–2475.

These ranges contain direct calls into `symtab`, the global
`unprocessedLCLinkerOptions` append, global `make<T>` allocation, and global
unwind/debug registration. They are why current Mach-O parsing is not a
file-local worker operation.

### Evidence: MO-SRC-ARCHIVE

**Mach-O archive indexing, selection, and publication**

[`ArchiveFile`, lines 2260–2418](../../MachO/InputFiles.cpp) distinguishes
indexed lazy symbols, the no-index per-child fallback, `fetch`-time member
construction, and publication. [`processFile`, lines
443–544](../../MachO/Driver.cpp) contains whole/force/Objective-C policy.
[`prepareInput`, lines 753–797](../../MachO/Driver.cpp) is a separate
preparation path which enumerates every child only to page-touch batches of
128; it does not call `ArchiveFile::fetch` or construct `ObjFile`.

### Evidence: MO-SRC-DYLIB

**Mach-O dylib/TAPI parse and reexports**

[`DylibFile`, lines 1772–2100](../../MachO/InputFiles.cpp) parses exports,
load commands, TAPI interfaces, and recursive reexports. [`loadDylib`, lines
227–327](../../MachO/DriverUtils.cpp) owns the process-global `DenseMap`
cache, real-path aliasing, cache-before-recursion rule, and recursive load.
There is no worker synchronization around this state.

### Evidence: MO-SRC-SYMTAB

**Mach-O symbol insertion and replacement**

[`SymbolTable.cpp`, lines 31–473](../../MachO/SymbolTable.cpp) implements
insertion and the defined/undefined/common/lazy/dylib replacement rules. The
table and symbol mutation are ordinary serial containers and operations; no
lock or concurrent container is present.

### Evidence: MO-SRC-LTO

**Mach-O LTO and late-input re-entry**

[`compileBitcodeFiles`, lines 1099–1110](../../MachO/Driver.cpp) adds
prevailing IR, compiles it, and appends generated `ObjFile`s. [`parseLCLinkerOption`
and `resolveLCLinkerOptions`, lines 972–1058](../../MachO/Driver.cpp) collect
autolink options globally and load late files to a fixed point.
[Driver order, lines 2790–2816](../../MachO/Driver.cpp) proves LTO precedes
late-input resolution and that both precede later layout work.
[`BitcodeCompiler`, lines 127–260](../../MachO/LTO.cpp) owns the LTO input and
generated-object boundary.

### Evidence: MO-SRC-MEM

**LLD global versus thread-local allocation**

[`Memory.h`, lines 50–91](../../include/lld/Common/Memory.h) defines the
process-global `make`/`makeN` allocator and the explicitly distinct
`makeThreadLocal`/`makeThreadLocalN` APIs. The native Mach-O ranges in
[MO-SRC-OBJ](#evidence-mo-src-obj) use the former.

### Evidence: ELF-SRC-IO

**ELF mapping and buffer lifetime**

[`readFile`, lines 214–294](../../ELF/InputFiles.cpp) maps input during the
ordered driver walk and stores owned buffers in the context.
[`Ctx`, lines 650–720](../../ELF/Config.h) owns those `memoryBuffers`.

### Evidence: ELF-SRC-LOAD

**ELF `LoadJob` construction, expansion, and ordering**

Source locations:

- [`addFile`, lines 194–292](../../ELF/Driver.cpp) maps and classifies inputs
  into ordered `LoadJob`s;
- [`LoadJob`, lines 190–228](../../ELF/Config.h) defines a job-owned output
  vector; and
- [`loadFiles`, lines 2110–2205](../../ELF/Driver.cpp) runs jobs with
  `parallelFor`, expands every archive member, then flattens each job’s
  results in original job/member order after the barrier.

### Evidence: ELF-SRC-PARSE

**ELF serial publication and parallel detailed parse**

[`doParseFile`, `doParseFiles`, and `parseFiles`, lines
298–360](../../ELF/InputFiles.cpp) publish files and global symbols serially.
[`ObjFile::parse` and initialization, lines
571–1308](../../ELF/InputFiles.cpp) separate global symbols from detailed
section/local-symbol/post-parse work. [Driver lines
3195–3320](../../ELF/Driver.cpp) invoke those latter operations through
`parallelForEach`.

### Evidence: ELF-SRC-LATE

**ELF dependent libraries, LTO, and generated objects**

[`addDependentLibrary`, lines 374–410](../../ELF/InputFiles.cpp) resolves
`.deplibs` entries during serial parse and calls `addFile`; the comment at
[Driver lines 226–229](../../ELF/Driver.cpp) states that such a late job is
executed inline rather than added to the completed initial batch.
[`compileBitcodeFiles`, lines 2775–2825](../../ELF/Driver.cpp) and
[generated-object re-entry, lines 3373–3412](../../ELF/Driver.cpp) place the
LTO loop and its later parallel per-file work.

### Evidence: ELF-SRC-MEM

**ELF worker allocation and retention**

[`Ctx`, lines 650–720](../../ELF/Config.h) retains input buffers and file
collections. [`Memory.h`, lines 65–91](../../include/lld/Common/Memory.h)
defines thread-local allocation; examples at
[`InputFiles.cpp`, lines 910–927 and 1102–1185](../../ELF/InputFiles.cpp)
use it in parallel per-object paths.

### Evidence: MOLD-SRC-DISCOVERY

**mold serial discovery and task launch**

Pinned sources:

- [`new_object_file`, `new_shared_file`, `read_file`, and
  `read_input_files`, `main.cc` lines 23–245](https://github.com/rui314/mold/blob/f4a62c75039a5d73524e2fc49efa74d2b522256e/src/main.cc#L23-L245)
  perform serial open/mmap, assign monotonic file priority before launch,
  enumerate archives, and schedule object/DSO parse tasks; and
- [`read_archive_members`, `archive-file.cc` lines
  167–185](https://github.com/rui314/mold/blob/f4a62c75039a5d73524e2fc49efa74d2b522256e/src/archive-file.cc#L167-L185)
  returns all regular or thin members.

Every eligible archive object is passed to `new_object_file`; ordinary archive
mode changes initial reachability, not whether the parser task is created.

### Evidence: MOLD-SRC-PARSE

**mold concurrent per-file parse and interning**

[`ObjectFile::parse`, `input-files.cc` lines
983–1015](https://github.com/rui314/mold/blob/f4a62c75039a5d73524e2fc49efa74d2b522256e/src/input-files.cc#L983-L1015)
parses sections, symbols, relocations, and dependent records on the scheduled
task. [`SharedFile::parse`, lines
1360–1549](https://github.com/rui314/mold/blob/f4a62c75039a5d73524e2fc49efa74d2b522256e/src/input-files.cc#L1360-L1549)
does the analogous DSO work.
[`Context::symbol_map`, `mold.h` lines
2540–2610](https://github.com/rui314/mold/blob/f4a62c75039a5d73524e2fc49efa74d2b522256e/src/mold.h#L2540-L2610)
is a TBB `concurrent_hash_map`.

### Evidence: MOLD-SRC-RESOLVE

**mold deterministic resolution and reachability**

[`ObjectFile::resolve_symbols` and `mark_live_objects`,
`input-files.cc` lines 1017–1145](https://github.com/rui314/mold/blob/f4a62c75039a5d73524e2fc49efa74d2b522256e/src/input-files.cc#L1017-L1145)
rank candidates using definition kind and file priority.
[`resolve_symbols`, `mark_live_objects`, and `do_lto`, `passes.cc` lines
217–437](https://github.com/rui314/mold/blob/f4a62c75039a5d73524e2fc49efa74d2b522256e/src/passes.cc#L217-L437)
show the barrier, parallel resolution, archive/as-needed reachability,
COMDAT rerun, and LTO-generated-object rerun.
[`Symbol::mu`, `mold.h` lines
1215–1250](https://github.com/rui314/mold/blob/f4a62c75039a5d73524e2fc49efa74d2b522256e/src/mold.h#L1215-L1250)
serializes each symbol’s winning-definition update.

### Evidence: MOLD-SRC-MEM

**mold concurrent containers and retained pools**

[`Context`, `mold.h` lines
2540–2610](https://github.com/rui314/mold/blob/f4a62c75039a5d73524e2fc49efa74d2b522256e/src/mold.h#L2540-L2610)
owns concurrent symbol/COMDAT maps and file pools.
[`InputFile::priority`, lines
1740–1780](https://github.com/rui314/mold/blob/f4a62c75039a5d73524e2fc49efa74d2b522256e/src/mold.h#L1740-L1780)
is retained with each file, and the object collections are not pruned merely
because an archive member remains unreachable.

## A.4 ld-prime binary and public evidence records

These records come from the arm64 slice whose version appears in A.1.
Addresses are included so a reviewer can locate the exact instructions; they
must be rediscovered rather than copied when the Xcode build changes.

### Evidence: LP-BIN-SYMS

**parser entry points and classification**

Reproduce:

```sh
rg 'SliceParser::parse(Object|Dylib|Bitcode|Tapi)File|SliceParser::parse\(\)' \
  "$STUDY_TMP/ld-prime-symbols.raw"
otool -tvV -p __ZNK2ld10InputFiles11SliceParser5parseEv \
  "$STUDY_TMP/ld-prime-arm64" \
  >"$STUDY_TMP/ld-prime-slice-parse.raw"
```

Normalized symbol dump:

```text
0x10004c850 ld::InputFiles::SliceParser::parseObjectFile(...)
0x100058360 ld::InputFiles::SliceParser::parseDylibFile(...)
0x100058f14 ld::InputFiles::SliceParser::parseBitcodeFile() const
0x100059688 ld::InputFiles::SliceParser::parse() const
0x10005a0f0 ld::InputFiles::SliceParser::parseTapiFile(...)
0x10005bcf0 ld::InputFiles::SliceParser::parseAtomFile() const
```

`ld-prime-slice-parse.raw` contains direct branches from `SliceParser::parse`
to the object, dylib, TAPI, atom, and bitcode entry points. This establishes
parser classification, but it does not expose the full internal mutation set
of each proprietary parser.

### Evidence: LP-BIN-MAP

**mapping inside parser work**

Reproduce:

```sh
rg 'File::mapReadOnlyAt' "$STUDY_TMP/ld-prime-symbols.raw"
rg -n -C 4 'mapReadOnlyAt' "$STUDY_TMP/ld-prime-all-text.raw" \
  >"$STUDY_TMP/ld-prime-map-sites.raw"
```

Normalized dump:

```text
0x10003c138 ld::File::mapReadOnlyAt(...)
0x10005cb14 call ld::File::mapReadOnlyAt(...)
             [inside SliceParser::parseArchiveFile block]
```

The binary locates mapping in the slice/archive parser path; the independent
worker overlap is in [LP-RUN-DIRECT](#evidence-lp-run-direct).

### Evidence: LP-BIN-PIPE

**parse/combiner split and final barrier**

Reproduce:

```sh
rg -n -C 4 \
  'com.apple.ld.combiner|dispatch_queue_create|dispatch_apply|dispatch_group_wait' \
  "$STUDY_TMP/ld-prime-parse-files.raw"
rg -n -C 4 'dispatch_group_async|addAtomFile' \
  "$STUDY_TMP/ld-prime-all-text.raw" \
  >"$STUDY_TMP/ld-prime-combiner-sites.raw"
```

Normalized instruction dump:

```text
0x1001076d4 literal "com.apple.ld.combiner"
0x1001076d8 x1 = 0
0x1001076dc call dispatch_queue_create
0x100107854 call dispatch_apply          # initial FileInfo array
0x100107910 call dispatch_apply          # pending SliceParser array
0x100107ae0 x1 = -1
0x100107ae4 call dispatch_group_wait

0x100107fa8 call dispatch_group_async     # addParsedAtomFile producer
0x10010a2a8 call AtomFileConsolidator::addAtomFile
                                              # submitted block consumer
```

The zero queue attribute creates a serial dispatch queue. The two
`dispatch_apply` calls are parallel producer waves; `addParsedAtomFile`
submits the parsed result to the combiner; the infinite group wait is the
final parse/combiner barrier.

### Evidence: LP-BIN-ARCHIVE

**archive enumeration and bounded pending work**

Reproduce:

```sh
rg 'Archive::forEachMachO|SliceParser::parseArchiveFile' \
  "$STUDY_TMP/ld-prime-symbols.raw"
rg -n -C 8 \
  'forEachMachO|os_unfair_lock_(lock|unlock)|0x4800|#0x90' \
  "$STUDY_TMP/ld-prime-all-text.raw" \
  >"$STUDY_TMP/ld-prime-archive-sites.raw"
```

Normalized dump:

```text
0x100021044 mach_o::Archive::forEachMachO(...)
0x1000597d8 call mach_o::Archive::forEachMachO(...)
0x10005c058 SliceParser::parseArchiveFile block
imports: os_unfair_lock_lock, os_unfair_lock_trylock, os_unfair_lock_unlock
pending SliceParser stride: 0x90 bytes = 144 bytes
maximum stolen byte span: 0x4800 bytes
0x4800 / 0x90 = 128 SliceParser records
```

The control flow parses one member immediately, appends other member parsers
to the lock-protected pending vector, and permits bounded steals. The binary
does not reveal an API-level promise that the bound or record layout is
stable.

### Evidence: APPLE-ARCHIVE

**public archive-order constraint**

Apple’s [WWDC22 “Link fast: Improve build and launch
times”](https://developer.apple.com/videos/play/wwdc2022/110362/) describes
selective archive loading as fixed-order work for reproducibility and
describes `-all_load` as enabling parallel archive-content parsing. This is
public semantic evidence, not proof of the exact Xcode 26.3 implementation;
the latter comes from LP-BIN-ARCHIVE and LP-BIN-PIPE.

### Evidence: LP-LIMITS

**explicitly unobservable ld-prime details**

Reproduce the supported-option and string-table checks:

```sh
"$STUDY_LD_PRIME" -help >"$STUDY_TMP/ld-prime-help.raw" 2>&1
rg -i 'thread|worker|parallel' "$STUDY_TMP/ld-prime-help.raw" || true
strings "$STUDY_TMP/ld-prime-arm64" |
  rg -i 'thread|worker|parallel|archive|reexport|diagnostic' \
  >"$STUDY_TMP/ld-prime-relevant-strings.raw"
```

No supported input-parser worker-count option was present in the help output.
Symbols and disassembly expose boundaries but not C++ field ownership,
allocator lifetime, cache keys, diagnostic merge policy, selective-member
algorithm, reexport publication point, or LTO scheduling contract. Claims
about those items are therefore deliberately marked unobservable or, where
the producer/consumer control flow permits it, inferred. Absence from help or
strings is not used as positive evidence for an internal algorithm.

### Evidence: PLATFORM-NA

**format-specific non-applicability**

Reproduce:

```sh
file "$STUDY_TMP/macho/main.o" "$STUDY_TMP/elf/main.o"
"$STUDY_BIN/llvm-readobj" --file-headers \
  "$STUDY_TMP/macho/main.o" "$STUDY_TMP/elf/main.o"
```

Normalized result:

```text
macho/main.o: Mach-O 64-bit object x86_64
elf/main.o:   ELF 64-bit LSB relocatable, x86-64
```

Mach-O fat-slice and Objective-C archive semantics are not operations in the
ELF format, so ELF LLD and mold entries are marked not applicable rather than
untested.

## A.5 Runtime evidence records

All hashes below are SHA-256. A repeated value means the files were also
checked with `cmp`; it is not merely a coincidental visual comparison of
hashes. Temporary prefixes in diagnostics are normalized to `$CASE`.

### Evidence: LP-RUN-DIRECT

**ld-prime direct-input overlap**

A.1 produces `lp-direct.jsonl` and the following normalized object. The
numeric thread IDs are raw values from one rerun; only distinctness and
overlap inside the first apply interval are claims.

```json
{
  "initial_count": 12,
  "initial_worker_tids": [
    8752123, 8752151, 8752152, 8752153, 8752154, 8752155,
    8752156, 8752157, 8752158, 8752159, 8752160, 8752161
  ],
  "combiner_submissions_during_initial_apply": 12,
  "later_parse_counts": [39]
}
```

The 12 initial iterations are the 11 direct objects plus the system input in
this command. Begin/end intervals for distinct indices overlap on distinct
thread IDs, while 12 blocks are submitted to
`com.apple.ld.combiner` before the initial `dispatch_apply` finishes. The
later count is retained to show why filtering solely by “any
`dispatch_apply`” would over-attribute later linker work.

### Evidence: LP-RUN-ARCHIVE

**ld-prime selected/unused archive overlap**

Corpus inventory:

```text
llvm-ar t libf.a:
  f1.o f2.o f3.o f4.o f5.o f6.o f7.o f8.o f9.o f10.o
llvm-nm -u main.o:
  _f1
```

Only `f1.o` is selected by the reference. A.1 produces:

```json
{
  "initial_count": 3,
  "initial_worker_tids": [8752171, 8752172, 8752173],
  "combiner_submissions_during_initial_apply": 12,
  "later_parse_counts": [39]
}
```

The three initial tasks are `main.o`, `libf.a`, and the system input. The 12
combiner submissions during that interval demonstrate nested archive parser
work beyond the one selected member, but the interposer alone cannot map each
submission to a member. LP-BIN-ARCHIVE supplies the member-parser mechanism.

### Evidence: LP-RUN-HASH

**ld-prime repeatability**

Raw normalized dump from two directories with the same output basename:

```text
direct/a/app  c8b8a10402369e9cf6144bc65e41d57e505d96e47923f6a5f07b10e95c52fcf9
direct/b/app  c8b8a10402369e9cf6144bc65e41d57e505d96e47923f6a5f07b10e95c52fcf9
archive/a/app 41f12f0ac6e047ce840c00ab38f5b2449d76b572190163e2505227f1277134a8
archive/b/app 41f12f0ac6e047ce840c00ab38f5b2449d76b572190163e2505227f1277134a8
```

The same basename matters because ld-prime’s generated arm64 ad-hoc signature
incorporates output identity. There is no supported forced-single-thread mode;
this record establishes repeatability of normal parallel execution only.

### Evidence: MO-RUN-TRACE

**Mach-O LLD worker overlap and archive-page batch**

Command: the Mach-O command group in A.1 with `--input-load-stats`,
`--time-trace-granularity=0`, and the `jq` normalization.

Raw stderr:

```text
input-load-stats: mode=prime workers=2 tasks=4 completed=4
                  peak-active=2 mapped-files=4 mapped-bytes=2968
```

Normalized time-trace events:

```json
[
  {"name":"Prime input-load worker","tid":7683,"ts":171,"dur":36,
   "args":{"detail":"$CASE/main.o"}},
  {"name":"Prime input-load worker","tid":2819,"ts":173,"dur":257,
   "args":{"detail":"$CASE/libfoo.a"}},
  {"name":"Prime input-load worker","tid":7683,"ts":209,"dur":19,
   "args":{"detail":"$CASE/bar.o"}},
  {"name":"Prime input-load worker","tid":7683,"ts":228,"dur":794,
   "args":{"detail":"$CASE/bitcode.o"}},
  {"name":"Prime archive batch","tid":2819,"ts":431,"dur":0,
   "args":{"detail":"$CASE/libfoo.a batch=0 members=1"}}
]
```

`main.o` and `libfoo.a` overlap (`171..207` and `173..430`) on different
threads. MO-SRC-DRIVER establishes that `Prime archive batch` page-touches
member bytes and does not parse an `ObjFile`.

### Evidence: MO-RUN-RESULTS

**Mach-O LLD equivalence matrix**

Normalized successful-output dump:

```text
native + selected regular archive + direct IR:
  serial = prime
  96f907e6fb672c6689b32048bc9bf20742b6e1d8956cf2a7c9bfdb0d2ec03b11
thin archive = no-index archive = archive-contained IR:
  serial = prime = regular result
  96f907e6fb672c6689b32048bc9bf20742b6e1d8956cf2a7c9bfdb0d2ec03b11
-all_load:
  serial = prime
  9db5ccd4be0840e937b6aa80308dc93f10350ffbb96c571678747e4a2ee639b1
-ObjC = -force_load for the Objective-C archive:
  serial = prime
  d9ad7806a45a1e9e7ce9e2b29a3b318a3738dc5bd63d951dc1e8bce034edecf8
weak order w1,w2:
  serial = prime
  570464d4ba515fe4916ce1fa74c20cd97a3cff53ad2d1476271e4e73d413904f
weak order w2,w1:
  serial = prime
  49c0ec81ce7d358c644e8d6e08d2d8b091cb72bbaec2a3ce2438b312846798d5
LC_LINKER_OPTION archive with fixed install name:
  serial = prime
  53ba95801aa0d36217bbad98d680b647d80d720042dd65c067e66cc872015673
```

The reversed weak-input order intentionally changes the result, while the
serial/`prime` pair remains equal within each order. Normalized diagnostics
were byte-identical between modes:

```text
ld64.lld: error: $CASE/libfoo-malformed.a: failed to parse archive:
  truncated or malformed archive
  (remaining size of archive too small for next archive member header at offset 8)

ld64.lld: error: duplicate symbol: _foo
>>> defined in $CASE/foo.o
>>> defined in $CASE/foo.o
```

### Evidence: ELF-RUN-RESULTS

**ELF LLD thread-count equivalence matrix**

Normalized dump:

```text
regular = thin = no-index, threads 1 = 4:
  f9a924ad7d43dfb28023120e24dd54b03329630831a4b53824884741fbb7bae3
whole archive, threads 1 = 4:
  a9d9badb11c91d1b1e5d44e1bebc8d8488802700b4829384218f2809db20235c
direct IR = archive IR, threads 1 = 4:
  ebfe53bac343ff2c39cfbec693465cdda90873122cdaa0dfc24d5dfcfd90e3e8
explicit DSO = .deplibs DSO, threads 1 = 4:
  63b67080b66c9e2af141f29c3d8f662ee16e5d091b96d8d32779b8347990df62
weak/common definitions, threads 1 = 4:
  f151b5a450ef569eb9236fc520e89c11684b032aaf29b76628ac3d19f7bc38ea
```

Normalized diagnostics at both counts:

```text
ld.lld: error: $CASE/libcase-malformed.a: failed to parse archive:
  truncated or malformed archive
  (remaining size of archive too small for next archive member header for /)

ld.lld: error: duplicate symbol: used
>>> defined at $CASE/used.o:(.text+0x0)
>>> defined at $CASE/used.o:(.text+0x0)
```

### Evidence: MOLD-RUN-RESULTS

**mold thread-count equivalence and local limits**

Normalized successful-output dump:

```text
regular = thin = no-index, threads 1 = 4:
  1ed7af7c91fa365b57d8aec8b135be94de0652d80a848e6ac3e6eedfbf5959fd
whole archive, threads 1 = 4:
  5e467074ac9c0befdaed403cdf1e6609b3d60530cfdd37991f56728f09d12e97
DSO, threads 1 = 4:
  8278ed260210cbe3527a9c4dae6b161fd1050af353676584ccf7ba8461bd34fc
weak/common definitions, threads 1 = 4:
  121f0ae262ab3013aa33f18132ea6b092de7235629771682b09b451594287eef
```

Normalized diagnostics were the same at both counts:

```text
mold: error: duplicate symbol: $CASE/used.o: $CASE/used.o: used

mold: error: undefined symbol: used
>>> referenced by $CASE/main.o:(.text)
>>>               $CASE/main.o
```

For the malformed archive, the second diagnostic is the result: mold did not
emit a malformed-archive error and later diagnosed the unresolved `used`.
The local LTO attempt was unavailable for an environmental reason:

```text
mold: fatal: $CASE/used.bc: unable to handle this LTO object file because
the -plugin option was not provided.
```

The `.deplibs` input likewise reached the unresolved `used` diagnostic at both
counts, which is the runtime basis for marking LLVM dependent-library
autolinking unsupported in this mold build.

### Evidence: RUN-RSS

**resource-procedure sanity check**

Reproduce a measurement, writing the time output separately from the linker
output:

```sh
/usr/bin/time -l -o "$STUDY_TMP/rss-macho-serial.raw" \
  "$STUDY_BIN/ld64.lld" -arch x86_64 -platform_version macos 13 13 \
  -no_uuid "$STUDY_TMP/macho/main.o" "$STUDY_TMP/macho/libfoo.a" \
  "$STUDY_TMP/macho/bar.o" "$STUDY_TMP/macho/bitcode.o" \
  -o "$STUDY_TMP/macho/rss-serial"
/usr/bin/time -l -o "$STUDY_TMP/rss-macho-prime.raw" \
  "$STUDY_BIN/ld64.lld" -arch x86_64 -platform_version macos 13 13 \
  -no_uuid --input-load-demo=prime --input-load-workers=2 \
  "$STUDY_TMP/macho/main.o" "$STUDY_TMP/macho/libfoo.a" \
  "$STUDY_TMP/macho/bar.o" "$STUDY_TMP/macho/bitcode.o" \
  -o "$STUDY_TMP/macho/rss-prime"

/usr/bin/time -l -o "$STUDY_TMP/rss-elf-lld-t4.raw" \
  "$STUDY_BIN/ld.lld" -m elf_x86_64 --threads=4 -e _start \
  "$STUDY_TMP/elf/main.o" "$STUDY_TMP/elf/libcase.a" \
  -o "$STUDY_TMP/elf/rss-lld-t4"
/usr/bin/time -l -o "$STUDY_TMP/rss-mold-t4.raw" \
  "$STUDY_TMP/mold-build/mold" -m elf_x86_64 --threads=4 -e _start \
  "$STUDY_TMP/elf/main.o" "$STUDY_TMP/elf/libcase.a" \
  -o "$STUDY_TMP/elf/rss-mold-t4"

mkdir -p "$STUDY_TMP/lp/rss"
/usr/bin/time -l -o "$STUDY_TMP/rss-ld-prime.raw" \
  "$STUDY_LD_PRIME" -arch arm64 -platform_version macos 26 26 \
  -syslibroot "$STUDY_SDK" -lSystem -e _main -no_uuid \
  "$STUDY_TMP/lp/direct/main.o" "$STUDY_TMP"/lp/direct/f*.o \
  -o "$STUDY_TMP/lp/rss/app"
```

One warm-cache tiny-corpus run reported:

```text
Mach-O LLD serial: approximately 30.3 MB maximum RSS
Mach-O LLD prime:  approximately 30.3 MB maximum RSS
ELF LLD threads=4: approximately 19.4 MB maximum RSS
mold threads=4:    approximately 8.4 MB maximum RSS
ld-prime:          approximately 37.8 MB maximum RSS
```

These values validate only that peak RSS was captured. Different binary
builds, uncontrolled cache order, and startup-dominated inputs make
cross-linker memory or performance conclusions invalid.

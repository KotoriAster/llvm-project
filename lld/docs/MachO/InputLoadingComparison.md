# Full-path input-loading comparison

This document compares the complete input-loading path of four linker
implementations:

1. Apple ld-prime from Xcode 26.3, `ld-1230.1`;
2. Mach-O LLD at `e988985ea898026e79b0ec56ef74b12624aed708`,
   including its serial path and experimental `prime` input-loading mode;
3. ELF LLD at the same LLVM revision; and
4. mold 2.41.0 at `f4a62c75039a5d73524e2fc49efa74d2b522256e`.

The four captured versions are recorded in [VERSIONS](InputLoadingComparisonAppendix.md#evidence-versions).

The comparison stops at the boundary between input loading and the later
linker passes, except where LTO or late inputs re-enter loading. It deliberately
distinguishes four operations which are often all called “loading”:

- **enumeration** discovers an argument or archive member;
- **parsing** turns bytes into per-file sections, symbols, relocations, and
  metadata;
- **selection** decides whether an archive member or as-needed shared library
  participates in the link; and
- **publication** makes a parsed file visible to global symbol resolution and
  order-sensitive linker state.

The existing [InputLoading](InputLoading.rst) page describes the experimental Mach-O LLD
flags. This page supplies the cross-linker design study which must be reviewed
before those experiments are turned into production behavior.

## Evidence and scope

Evidence classes used below are:

- **[S] Source-confirmed**: directly established by the referenced open source.
- **[D] Disassembly-confirmed**: established from symbols and instructions in
  the Xcode 26.3 arm64 linker binary.
- **[R] Runtime-confirmed**: observed using an interposed dispatch trace,
  time-trace, output comparison, or diagnostic comparison.
- **[I] Inferred**: the most likely interpretation of observed control flow,
  but not directly inspectable.
- **[U] Unsupported/unobservable**: unavailable in that linker or not
  observable with the tools used here.

Every evidence-bearing entry below ends in one or more stable evidence links,
such as [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver) or
[LP-RUN-DIRECT](InputLoadingComparisonAppendix.md#evidence-lp-run-direct). The links resolve to
[Appendix A](InputLoadingComparisonAppendix.md#appendix-a-evidence-ledger), which contains the exact source
location or the normalized raw dump and its reproducing command. The
one-letter classes describe the kind of evidence; they are not citations by
themselves.

Apple does not publish the ld-prime source in Xcode 26.3. Consequently, this
report does not label claims about its internal state as source-confirmed.
Function names and control-flow boundaries are from the shipped symbol table
and disassembly; thread overlap is independently runtime-confirmed.
[LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits),
[LP-BIN-PIPE](InputLoadingComparisonAppendix.md#evidence-lp-bin-pipe),
[LP-RUN-DIRECT](InputLoadingComparisonAppendix.md#evidence-lp-run-direct)

The runtime corpus is intentionally small and is a semantic smoke test, not a
performance benchmark. Its timing and RSS numbers mostly measure process
startup and warm/cold cache effects. The source and binary analysis, rather
than those timings, supports the architectural conclusions.
[RUN-RSS](InputLoadingComparisonAppendix.md#evidence-run-rss)

## Conclusions

The implementations do not share one meaning of “parallel input loading.”

| Linker | Earliest parallel stage | Native object parsing | Global publication | Archive policy |
|---|---|---|---|---|
| ld-prime | open/map and slice parsing [LP-BIN-MAP](InputLoadingComparisonAppendix.md#evidence-lp-bin-map), [LP-RUN-DIRECT](InputLoadingComparisonAppendix.md#evidence-lp-run-direct) | parallel `SliceParser` jobs [LP-BIN-SYMS](InputLoadingComparisonAppendix.md#evidence-lp-bin-syms), [LP-RUN-DIRECT](InputLoadingComparisonAppendix.md#evidence-lp-run-direct) | asynchronous, serialized combiner [LP-BIN-PIPE](InputLoadingComparisonAppendix.md#evidence-lp-bin-pipe), [LP-RUN-DIRECT](InputLoadingComparisonAppendix.md#evidence-lp-run-direct) | enumerate members during parsing; immediate plus bounded parser jobs [LP-BIN-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-lp-bin-archive) |
| Mach-O LLD serial | no loading-stage workers [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver) | serial `ObjFile` construction [MO-SRC-OBJ](InputLoadingComparisonAppendix.md#evidence-mo-src-obj) | happens inside parsing [MO-SRC-OBJ](InputLoadingComparisonAppendix.md#evidence-mo-src-obj) | lazy index and serial extraction [MO-SRC-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-mo-src-archive) |
| Mach-O LLD `prime` | open/map, magic, archive directory, page-in, bitcode preparation [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver), [MO-RUN-TRACE](InputLoadingComparisonAppendix.md#evidence-mo-run-trace) | **still serial** after the worker barrier [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver) | serial command-line order [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver) | page-in every member in batches of 128; selection remains serial [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver), [MO-SRC-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-mo-src-archive) |
| ELF LLD | `LoadJob` execution [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load) | split: parallel construction, serial global-symbol parse, then parallel section/local-symbol work [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load), [ELF-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-elf-src-parse) | ordered flattening followed by serial `parseFiles` [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load), [ELF-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-elf-src-parse) | enumerate every member in a worker; represent members lazily; select during serial symbol parsing [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load), [ELF-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-elf-src-parse) |
| mold | each native object and DSO after serial open/enumeration [MOLD-SRC-DISCOVERY](InputLoadingComparisonAppendix.md#evidence-mold-src-discovery) | fully parallel per-file parse [MOLD-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-mold-src-parse) | concurrent interning; parallel locked resolution ordered by priority [MOLD-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-mold-src-parse), [MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve) | parse every member speculatively; reachability selects later [MOLD-SRC-DISCOVERY](InputLoadingComparisonAppendix.md#evidence-mold-src-discovery), [MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve) |

ld-prime can parse native objects concurrently because its parser produces a
file-local result and sends that result to a separate, serial
`AtomFileConsolidator` combiner. mold can do so because concurrent symbol
interning, per-symbol locks, stable file priorities, concurrent pools, and a
later reachability pass are part of its data model.
[LP-BIN-PIPE](InputLoadingComparisonAppendix.md#evidence-lp-bin-pipe),
[LP-RUN-DIRECT](InputLoadingComparisonAppendix.md#evidence-lp-run-direct),
[MOLD-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-mold-src-parse),
[MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve)

Mach-O LLD cannot currently make `ObjFile::parse()` a worker task without
changing its semantics or data ownership. That routine appends autolink
options, allocates through process-global non-thread-safe arenas, inserts
symbols into a non-thread-safe global symbol table, and registers unwind/debug
state as it parses. The experimental `prime` path correctly stops before that
boundary. Merely wrapping `ObjFile` construction in a task would race and would
also make strong/weak/common and archive-order outcomes depend on task
completion order. [MO-SRC-OBJ](InputLoadingComparisonAppendix.md#evidence-mo-src-obj),
[MO-SRC-MEM](InputLoadingComparisonAppendix.md#evidence-mo-src-mem),
[MO-SRC-SYMTAB](InputLoadingComparisonAppendix.md#evidence-mo-src-symtab),
[MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver)

The most reusable design for Mach-O LLD is therefore a combination of ELF
LLD’s indexed job/result slots and ld-prime’s parse/combiner separation. mold’s
concurrent structures are useful reference implementations, but importing
mold’s aggressive parsing alone is not a seamless change: its later
priority-based resolution and reachability algorithm are what make that
parsing safe. [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load),
[LP-BIN-PIPE](InputLoadingComparisonAppendix.md#evidence-lp-bin-pipe),
[MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve)

## Canonical stage matrix

“Worker” means a pool or dispatch worker. “Linker” means the driver thread or
an explicitly serialized combiner.

| Stage | ld-prime `ld-1230.1` | Mach-O LLD serial / `prime` | ELF LLD | mold `f4a62c7` |
|---|---|---|---|---|
| Argument and library discovery | options and `FileInfo` work precede `parseFiles`; exact ownership is not fully observable [LP-BIN-PIPE](InputLoadingComparisonAppendix.md#evidence-lp-bin-pipe), [LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits) | driver walks arguments and resolves `-l`/framework paths; `prime` defers non-autolink inputs [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver) | driver creates ordered `LoadJob`s while interpreting stateful options and scripts [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load) | driver walks arguments serially; `-l` search and linker scripts may add inputs immediately [MOLD-SRC-DISCOVERY](InputLoadingComparisonAppendix.md#evidence-mold-src-discovery) |
| Open and mmap | `File::mapReadOnlyAt` is called by parse workers; overlap was observed [LP-BIN-MAP](InputLoadingComparisonAppendix.md#evidence-lp-bin-map), [LP-RUN-DIRECT](InputLoadingComparisonAppendix.md#evidence-lp-run-direct) | serial path uses `readFile`; `prime` workers call `MemoryBuffer::getFile` into fixed result slots [MO-SRC-IO](InputLoadingComparisonAppendix.md#evidence-mo-src-io), [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver), [MO-RUN-TRACE](InputLoadingComparisonAppendix.md#evidence-mo-run-trace) | `readFile` maps during ordered argument processing; jobs receive `MemoryBufferRef`s [ELF-SRC-IO](InputLoadingComparisonAppendix.md#evidence-elf-src-io), [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load) | `must_open_file` opens, `fstat`s, and `mmap`s serially before scheduling parse [MOLD-SRC-DISCOVERY](InputLoadingComparisonAppendix.md#evidence-mold-src-discovery) |
| Cache and duplicate handling | path/inode policy is unobservable [LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits) | `cachedReads` is keyed by path spelling; dylibs also use a real-path cache; a `prime` duplicate mapping is discarded during ordered adoption [MO-SRC-IO](InputLoadingComparisonAppendix.md#evidence-mo-src-io), [MO-SRC-DYLIB](InputLoadingComparisonAppendix.md#evidence-mo-src-dylib), [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver) | every explicit `readFile` owns a mapping; dependency-file names, but not general input mappings, are deduplicated [ELF-SRC-IO](InputLoadingComparisonAppendix.md#evidence-elf-src-io) | explicit files are mapped each time; repeated `-l` stems are suppressed by a serial `visited` set [MOLD-SRC-DISCOVERY](InputLoadingComparisonAppendix.md#evidence-mold-src-discovery) |
| Fat-slice selection | inside `SliceParser::parse`/file mapping [LP-BIN-SYMS](InputLoadingComparisonAppendix.md#evidence-lp-bin-syms), [LP-BIN-MAP](InputLoadingComparisonAppendix.md#evidence-lp-bin-map) | serial, including validation, in `readFile`; `prime` performs it only after its worker barrier [MO-SRC-IO](InputLoadingComparisonAppendix.md#evidence-mo-src-io), [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver) | not applicable to normal ELF input [PLATFORM-NA](InputLoadingComparisonAppendix.md#evidence-platform-na) | not applicable [PLATFORM-NA](InputLoadingComparisonAppendix.md#evidence-platform-na) |
| Classification | `SliceParser::parse` dispatches object, dylib, bitcode, TAPI, and archive paths [LP-BIN-SYMS](InputLoadingComparisonAppendix.md#evidence-lp-bin-syms) | `identify_magic`; `prime` workers prepare archives/bitcode, then `processFile` repeats ordered classification [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver) | driver classifies to `LoadJob::Obj`, `Bitcode`, `Archive`, `Shared`, or `Binary` [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load) | serial `get_file_type` dispatches in `read_file` [MOLD-SRC-DISCOVERY](InputLoadingComparisonAppendix.md#evidence-mold-src-discovery) |
| Archive enumeration | `Archive::forEachMachO`; pending `SliceParser`s use an unfair lock [LP-BIN-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-lp-bin-archive) | `ArchiveFile` reads the index serially; `prime` separately walks every child and submits page-in batches [MO-SRC-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-mo-src-archive), [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver) | one archive load job calls `getArchiveMembers` and scans every member on its worker [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load) | `read_archive_members` enumerates regular/thin archives serially in the argument walk [MOLD-SRC-DISCOVERY](InputLoadingComparisonAppendix.md#evidence-mold-src-discovery) |
| Archive index use | ordinary selective semantics exist; exact index algorithm is unobservable; `-all_load` removes the dependency [APPLE-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-apple-archive), [LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits) | indexed archives add lazy archive symbols; without an index, every child becomes a lazy object [MO-SRC-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-mo-src-archive), [MO-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mo-run-results) | current code deliberately scans member symbol tables instead of using the archive index [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load) | archive index is not used for selection; all members are instantiated [MOLD-SRC-DISCOVERY](InputLoadingComparisonAppendix.md#evidence-mold-src-discovery) |
| Native sections and symbols | `parseObjectFile` runs in worker `SliceParser` jobs [LP-BIN-SYMS](InputLoadingComparisonAppendix.md#evidence-lp-bin-syms), [LP-RUN-DIRECT](InputLoadingComparisonAppendix.md#evidence-lp-run-direct) | `ObjFile::parse` serially parses load commands, sections, external symbols, and local state after the barrier [MO-SRC-OBJ](InputLoadingComparisonAppendix.md#evidence-mo-src-obj), [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver) | constructor work is parallel; global symbol parsing is serial; section and local-symbol initialization later uses `parallelForEach` [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load), [ELF-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-elf-src-parse) | `ObjectFile::parse` initializes sections and symbols in its task [MOLD-SRC-DISCOVERY](InputLoadingComparisonAppendix.md#evidence-mold-src-discovery), [MOLD-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-mold-src-parse) |
| Relocations | part of object parse before combiner publication [LP-BIN-SYMS](InputLoadingComparisonAppendix.md#evidence-lp-bin-syms), [LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits) | parsed immediately after symbols, serially [MO-SRC-OBJ](InputLoadingComparisonAppendix.md#evidence-mo-src-obj) | detailed relocation/section work is in later per-file parallel stages [ELF-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-elf-src-parse) | sorted during parallel `ObjectFile::parse`; later relocation scanning is also parallel [MOLD-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-mold-src-parse), [MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve) |
| Debug and unwind | exact substage is not separately observable [LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits) | debug info, compact unwind, and `.eh_frame` registration occur inside serial `ObjFile::parse` [MO-SRC-OBJ](InputLoadingComparisonAppendix.md#evidence-mo-src-obj) | per-object section work is parallel; ordered global points remain separate [ELF-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-elf-src-parse) | per-object EH-frame parsing is a later `parallel_for_each` pass [MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve) |
| Global symbol interning | detached parse result implies separation from final combination [LP-BIN-PIPE](InputLoadingComparisonAppendix.md#evidence-lp-bin-pipe), [LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits) | `symtab->addDefined`, `addUndefined`, `addCommon`, and lazy insertion occur during serial parsing [MO-SRC-OBJ](InputLoadingComparisonAppendix.md#evidence-mo-src-obj) | global insertion and lazy-member selection occur in serial `doParseFiles` [ELF-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-elf-src-parse) | `get_symbol` uses a TBB `concurrent_hash_map` during parallel parse [MOLD-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-mold-src-parse), [MOLD-SRC-MEM](InputLoadingComparisonAppendix.md#evidence-mold-src-mem) |
| Symbol resolution | combiner calls `addAtomFile`; exact conflict algorithm is unobservable [LP-BIN-PIPE](InputLoadingComparisonAppendix.md#evidence-lp-bin-pipe), [LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits) | insertion and replacement occur in the non-thread-safe Mach-O `SymbolTable` in publication order [MO-SRC-SYMTAB](InputLoadingComparisonAppendix.md#evidence-mo-src-symtab) | serial parse preserves ordered ELF resolution; later stages can be parallel [ELF-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-elf-src-parse) | `resolve_symbols` is parallel; each symbol has a mutex and file priority breaks ordering ties [MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve) |
| Archive member selection | distinct from parsing; ordinary mode remains order-sensitive [APPLE-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-apple-archive), [LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits) | undefined/lazy symbol resolution calls `ArchiveFile::fetch`; selected member construction and publication are serial [MO-SRC-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-mo-src-archive) | all members have lightweight file objects; lazy symbols make selected members live during serial parsing [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load), [ELF-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-elf-src-parse) | every member is already parsed; `mark_live_objects` computes reachability, then resolution is cleared and repeated [MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve) |
| Whole/force/ObjC loading | `-all_load` permits parallel archive-content parse; force/ObjC internals are unobservable [APPLE-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-apple-archive), [LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits) | flags change which `ArchiveFile` children are constructed; `prime` preparation does not change selection [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver), [MO-SRC-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-mo-src-archive) | `inWholeArchive` changes each member’s lazy flag [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load) | whole archive clears `as_needed`; otherwise archive members begin unreachable [MOLD-SRC-DISCOVERY](InputLoadingComparisonAppendix.md#evidence-mold-src-discovery), [MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve) |
| Shared library/stub parse | `parseDylibFile` and `parseTapiFile` are `SliceParser` targets [LP-BIN-SYMS](InputLoadingComparisonAppendix.md#evidence-lp-bin-syms) | `DylibFile`/TAPI parse and global symbol insertion are serial; `loadedDylibs` is a non-thread-safe `DenseMap` [MO-SRC-DYLIB](InputLoadingComparisonAppendix.md#evidence-mo-src-dylib) | `SharedFile::init` runs in its parallel load job; symbol publication is ordered later [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load), [ELF-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-elf-src-parse) | `SharedFile::parse` is scheduled like object parsing [MOLD-SRC-DISCOVERY](InputLoadingComparisonAppendix.md#evidence-mold-src-discovery), [MOLD-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-mold-src-parse) |
| Reexports/dependencies | parser support is visible; exact recursive publication is not fully observable [LP-BIN-SYMS](InputLoadingComparisonAppendix.md#evidence-lp-bin-syms), [LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits) | dylib reexports recursively call `loadDylib`; the cache entry is installed before recursion [MO-SRC-DYLIB](InputLoadingComparisonAppendix.md#evidence-mo-src-dylib) | `DT_NEEDED`/as-needed decisions follow ordered publication; `.deplibs` can add one inline load job late [ELF-SRC-LATE](InputLoadingComparisonAppendix.md#evidence-elf-src-late) | DSO parse is parallel; reachability decides as-needed DSOs later [MOLD-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-mold-src-parse), [MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve) |
| Bitcode parse | `parseBitcodeFile` is a `SliceParser` target [LP-BIN-SYMS](InputLoadingComparisonAppendix.md#evidence-lp-bin-syms) | `BitcodeFile::parse` mutates the global symbol table serially; `prime` may prepare an `lto::InputFile` on a worker but does not publish it there [MO-SRC-OBJ](InputLoadingComparisonAppendix.md#evidence-mo-src-obj), [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver) | bitcode construction is parallel under a mutex around the string saver; bitcode symbol parse is serial [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load), [ELF-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-elf-src-parse) | LTO input is read via the configured compiler plugin in the serial discovery path [MOLD-SRC-DISCOVERY](InputLoadingComparisonAppendix.md#evidence-mold-src-discovery), [MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve) |
| LTO/generated objects | after a parse barrier; precise scheduling is unobservable [LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits) | `compileBitcodeFiles` runs after initial publication; generated `ObjFile`s re-enter serial parsing [MO-SRC-LTO](InputLoadingComparisonAppendix.md#evidence-mo-src-lto) | LTO runs after initial selection; generated ELF objects re-enter per-object stages [ELF-SRC-LATE](InputLoadingComparisonAppendix.md#evidence-elf-src-late) | `do_lto` adds generated objects, clears symbols/reachability, removes IR objects, and resolves again [MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve) |
| Autolink/late inputs | multiple parse/combiner waves were seen, but assigning a wave to a specific late-input source is inferred [LP-RUN-DIRECT](InputLoadingComparisonAppendix.md#evidence-lp-run-direct), [LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits) | `LC_LINKER_OPTION`s accumulate globally and `resolveLCLinkerOptions` loads them after LTO to a fixed point [MO-SRC-LTO](InputLoadingComparisonAppendix.md#evidence-mo-src-lto) | `.deplibs` is noticed in serial parse and `loadFiles` executes its one late job inline [ELF-SRC-LATE](InputLoadingComparisonAppendix.md#evidence-elf-src-late) | LLVM `.deplibs` autolink is unsupported in the tested build [MOLD-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mold-run-results) |
| Diagnostics and tracing | worker results feed a combiner; error merge details are unobservable [LP-BIN-PIPE](InputLoadingComparisonAppendix.md#evidence-lp-bin-pipe), [LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits) | open errors are stored by result index then reported in order; time-trace has worker lanes; parse diagnostics remain serial [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver), [MO-RUN-TRACE](InputLoadingComparisonAppendix.md#evidence-mo-run-trace), [MO-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mo-run-results) | ordered job flattening and serial parse stabilize important diagnostics [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load), [ELF-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-elf-src-parse), [ELF-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-elf-run-results) | priority-based semantics and tested diagnostics were thread-count invariant [MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve), [MOLD-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mold-run-results) |
| Final barrier | two `dispatch_apply` waves and `dispatch_group_wait(DISPATCH_TIME_FOREVER)` in `parseFiles` [LP-BIN-PIPE](InputLoadingComparisonAppendix.md#evidence-lp-bin-pipe) | `ThreadPoolTaskGroup::wait` completes preparation before ordered adoption and `processFile` [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver) | implicit `parallelFor` barrier before ordered job flattening; later parallel passes also join [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load), [ELF-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-elf-src-parse) | `task_group::wait` completes native/DSO parse tasks before `resolve_symbols` [MOLD-SRC-DISCOVERY](InputLoadingComparisonAppendix.md#evidence-mold-src-discovery), [MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve) |
| Memory lifetime | mapped files and parsed AtomFiles survive through consolidation; exact allocator lifetime is unobservable [LP-BIN-PIPE](InputLoadingComparisonAppendix.md#evidence-lp-bin-pipe), [LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits) | buffers and most parsed nodes are arena-owned until teardown; `prime` temporarily holds mappings and prepared archives/bitcode [MO-SRC-IO](InputLoadingComparisonAppendix.md#evidence-mo-src-io), [MO-SRC-MEM](InputLoadingComparisonAppendix.md#evidence-mo-src-mem), [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver) | `Ctx` owns buffers/files; thread-local allocators are used by parallel per-file stages [ELF-SRC-IO](InputLoadingComparisonAppendix.md#evidence-elf-src-io), [ELF-SRC-MEM](InputLoadingComparisonAppendix.md#evidence-elf-src-mem) | pools retain maps/files; speculative archive members remain allocated even when unreachable [MOLD-SRC-MEM](InputLoadingComparisonAppendix.md#evidence-mold-src-mem), [MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve) |

## Sequence diagrams

### Apple ld-prime

```text
driver          parse worker(s)        pending-member list       serial combiner
  | build FileInfo[]  |                         |                       |
  | dispatch_apply ----------------------------------------------->     |
  |                   | open/map + SliceParser::parse              |     |
  |                   | parse object/dylib/TAPI/bitcode            |     |
  |                   | archive: forEachMachO                      |     |
  |                   | parse one member; enqueue/steal <=128 ---->|     |
  |                   | dispatch_group_async(parsed AtomFile) ---------->|
  |                   |                         |               addAtomFile
  | second dispatch_apply(pending SliceParser[]) |                       |
  | dispatch_group_wait ------------------------------------------------>|
  | continue only after parse jobs and combiner submissions complete     |
```

`AtomFileConsolidator::parseFiles(bool)` creates
`com.apple.ld.combiner`, executes one `dispatch_apply` over the initial
`FileInfo` array, executes another over remaining `SliceParser`s, and waits on
the group. The queue is created without a concurrent attribute, so combiner
work is serial. Runtime interposition observed 12 initial iterations on at
least four worker thread IDs and group submissions to that queue.
[LP-BIN-PIPE](InputLoadingComparisonAppendix.md#evidence-lp-bin-pipe),
[LP-RUN-DIRECT](InputLoadingComparisonAppendix.md#evidence-lp-run-direct)

Archive parsing has a nested work shape rather than one up-front task per
member. The disassembly shows 144-byte `SliceParser` records, an unfair-lock
protected pending vector, and opportunistic steals of at most 128 parsers
(`0x4800 / 144`). [LP-BIN-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-lp-bin-archive)

### Mach-O LLD

```text
driver                    prime workers                    driver/global state
  | argument walk + defer       |                                  |
  | fixed slot per input ------>| open/map, identify magic          |
  |                              | archive directory + page-in batch |
  |                              | optional lto::InputFile prepare   |
  | wait <-----------------------|                                  |
  | ordered buffer adoption + fat-slice selection ----------------->|
  | ordered processFile -------------------------------------------->|
  |   ObjFile::parse: sections, symbols, relocs, debug, unwind       |
  |   ArchiveFile: lazy index; fetch selected members                |
  |   DylibFile/BitcodeFile: publish global symbols                  |
  | compile LTO -> generated ObjFile(s) ---------------------------->|
  | resolve LC_LINKER_OPTION late inputs, repeat to fixed point      |
```

The serial mode performs `readFile` and `processFile` directly during the
argument walk. The `prime` mode changes only the upper left part of this
diagram. Its source comment explicitly says the demos stop before `ObjFile`
construction. [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver)

### ELF LLD

```text
driver                 parallel LoadJob(s)              ordered/serial core
  | read/map + job[i]       |                                  |
  | parallelFor ----------->| construct Obj/Bitcode/Shared      |
  |                         | archive: enumerate all members     |
  | barrier <---------------| store results in job[i].out        |
  | flatten job[i].out in argument/member order ---------------->|
  | doParseFiles: globals, lazy selection, .deplibs (serial)     |
  | parallelForEach object sections/local symbols/post-parse     |
  | LTO -> generated files -> parallel per-file stages           |
```

The important reusable property is that completion order never determines
publication order. Each job owns its output vector, and the driver flattens
those vectors only after the parallel barrier.
[ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load)

### mold

```text
driver                  object/DSO tasks                 concurrent globals
  | serial open/mmap          |                                 |
  | serial archive enumerate  |                                 |
  | assign file priority      |                                 |
  | task_group.run ---------->| sections/symbols/relocs -------->|
  |                           | concurrent symbol interning       |
  | task_group.wait <---------|                                 |
  | parallel resolve_symbols ----------------------------------->| per-symbol lock
  | parallel mark_live_objects (archive/as-needed reachability)   |
  | clear; COMDAT; resolve reachable files again                  |
  | LTO -> generated objects; clear reachability; resolve again   |
```

The serial front end assigns each file a monotonic `priority` before its task
runs. Resolution ranks definitions using both definition kind and that
priority, so a faster worker does not become an earlier input.
[MOLD-SRC-DISCOVERY](InputLoadingComparisonAppendix.md#evidence-mold-src-discovery),
[MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve)

## Per-input flows

### Direct native object

| Linker | Flow |
|---|---|
| ld-prime | worker open/map → `SliceParser::parseObjectFile` → detached AtomFile → group submission → serial `addAtomFile` [LP-BIN-MAP](InputLoadingComparisonAppendix.md#evidence-lp-bin-map), [LP-BIN-SYMS](InputLoadingComparisonAppendix.md#evidence-lp-bin-syms), [LP-BIN-PIPE](InputLoadingComparisonAppendix.md#evidence-lp-bin-pipe), [LP-RUN-DIRECT](InputLoadingComparisonAppendix.md#evidence-lp-run-direct) |
| Mach-O LLD serial | `readFile` → `processFile` → `ObjFile` constructor → parse and global insertion inline [MO-SRC-IO](InputLoadingComparisonAppendix.md#evidence-mo-src-io), [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver), [MO-SRC-OBJ](InputLoadingComparisonAppendix.md#evidence-mo-src-obj) |
| Mach-O LLD `prime` | worker open/map/page-in → barrier → ordered `readFile` adoption → the same serial `ObjFile` path [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver) |
| ELF LLD | driver map → parallel object construction → ordered flatten → serial global-symbol parse → parallel section/local-symbol/post-parse [ELF-SRC-IO](InputLoadingComparisonAppendix.md#evidence-elf-src-io), [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load), [ELF-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-elf-src-parse) |
| mold | driver map and priority assignment → task parses sections/symbols/relocations with concurrent interning → later parallel resolution [MOLD-SRC-DISCOVERY](InputLoadingComparisonAppendix.md#evidence-mold-src-discovery), [MOLD-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-mold-src-parse), [MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve) |

### Archive

Archive parsing, member parsing, member selection, and member publication are
separate stages:

| Linker | Directory/member enumeration | Member parse | Selection | Publication |
|---|---|---|---|---|
| ld-prime | inside archive parser worker [LP-BIN-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-lp-bin-archive) | immediate member plus pending/batched parser jobs [LP-BIN-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-lp-bin-archive) | ordinary mode remains order-sensitive; exact algorithm unobservable [APPLE-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-apple-archive), [LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits) | serial combiner [LP-BIN-PIPE](InputLoadingComparisonAppendix.md#evidence-lp-bin-pipe) |
| Mach-O LLD serial | serial archive construction/index [MO-SRC-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-mo-src-archive) | only when `fetch` selects, unless force/whole behavior applies [MO-SRC-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-mo-src-archive) | lazy symbol lookup in command-line order [MO-SRC-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-mo-src-archive) | `fetch` inserts serially [MO-SRC-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-mo-src-archive) |
| Mach-O LLD `prime` | worker enumerates all children for page-in [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver) | unchanged serial `fetch`; worker page-in is **not parse** [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver), [MO-SRC-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-mo-src-archive) | unchanged [MO-SRC-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-mo-src-archive) | unchanged [MO-SRC-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-mo-src-archive) |
| ELF LLD | a worker scans every member [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load) | lightweight file construction in the job; global symbols later serial [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load), [ELF-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-elf-src-parse) | lazy files selected during serial parse [ELF-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-elf-src-parse) | ordered job/member order [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load) |
| mold | driver enumerates every member [MOLD-SRC-DISCOVERY](InputLoadingComparisonAppendix.md#evidence-mold-src-discovery) | every eligible object is parsed, even when later unused [MOLD-SRC-DISCOVERY](InputLoadingComparisonAppendix.md#evidence-mold-src-discovery), [MOLD-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-mold-src-parse) | reachability after parse [MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve) | concurrent interning plus priority-ordered resolution [MOLD-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-mold-src-parse), [MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve) |

Apple’s public description of ld64 explains the semantic constraint: selective
archive loading uses a fixed serial order for reproducibility, while
`-all_load` permits archive contents to be parsed in parallel.
[APPLE-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-apple-archive)
This public statement describes the semantic constraint; the Xcode 26.3
binary evidence above describes the newer parser/combiner mechanism.

### Shared library, DSO, or TAPI stub

- ld-prime exposes `parseDylibFile` and `parseTapiFile` as `SliceParser`
  targets. They can participate in the parallel parse wave; the exact point at
  which reexports mutate global state is not observable.
  [LP-BIN-SYMS](InputLoadingComparisonAppendix.md#evidence-lp-bin-syms),
  [LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits)
- Mach-O LLD constructs `DylibFile`, inserts exports into the global symbol
  table, and recursively processes reexports using a process-global
  `loadedDylibs` map. This entire path is serial in both current modes.
  [MO-SRC-DYLIB](InputLoadingComparisonAppendix.md#evidence-mo-src-dylib)
- ELF LLD runs `SharedFile::init` in the parallel load job, then preserves
  ordered publication and as-needed semantics in later stages.
  [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load),
  [ELF-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-elf-src-parse)
- mold schedules `SharedFile::parse`; its later reachability walk decides
  which as-needed DSOs are live.
  [MOLD-SRC-DISCOVERY](InputLoadingComparisonAppendix.md#evidence-mold-src-discovery),
  [MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve)

### Bitcode, LTO, and generated objects

| Linker | Initial IR work | Selection/resolution | Generated native objects |
|---|---|---|---|
| ld-prime | `parseBitcodeFile` is a parallel `SliceParser` target [LP-BIN-SYMS](InputLoadingComparisonAppendix.md#evidence-lp-bin-syms) | exact LTO boundary is unobservable [LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits) | must precede later layout; scheduling unobservable [LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits) |
| Mach-O LLD | serial `BitcodeFile::parse`; `prime` may prepare an `lto::InputFile` on a worker [MO-SRC-OBJ](InputLoadingComparisonAppendix.md#evidence-mo-src-obj), [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver) | serial global symbol resolution determines prevailing IR [MO-SRC-OBJ](InputLoadingComparisonAppendix.md#evidence-mo-src-obj), [MO-SRC-SYMTAB](InputLoadingComparisonAppendix.md#evidence-mo-src-symtab) | `compileBitcodeFiles` creates and inserts Mach-O `ObjFile`s serially [MO-SRC-LTO](InputLoadingComparisonAppendix.md#evidence-mo-src-lto) |
| ELF LLD | parallel bitcode construction, serialized only around the string saver; serial bitcode symbol parse [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load), [ELF-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-elf-src-parse) | LTO after lazy selection [ELF-SRC-LATE](InputLoadingComparisonAppendix.md#evidence-elf-src-late) | generated objects re-enter parallel section/local-symbol work [ELF-SRC-LATE](InputLoadingComparisonAppendix.md#evidence-elf-src-late), [ELF-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-elf-src-parse) |
| mold | plugin reads IR during discovery [MOLD-SRC-DISCOVERY](InputLoadingComparisonAppendix.md#evidence-mold-src-discovery) | resolve, run plugin, then clear symbol state [MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve) | append generated objects, remove IR objects, reset archive reachability, and resolve again [MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve) |

LTO is therefore a loop edge in the loading graph, not a terminal parsing
stage. Any Mach-O design must allow generated files to enter the same
parse/publication protocol without exposing half-published symbol state.
[MO-SRC-LTO](InputLoadingComparisonAppendix.md#evidence-mo-src-lto),
[ELF-SRC-LATE](InputLoadingComparisonAppendix.md#evidence-elf-src-late),
[MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve)

## Concurrency boundaries

| Boundary | Owner before | Synchronization | Owner after |
|---|---|---|---|
| ld-prime initial files → parsed AtomFiles | global dispatch workers | `dispatch_apply` plus group submissions [LP-BIN-PIPE](InputLoadingComparisonAppendix.md#evidence-lp-bin-pipe), [LP-RUN-DIRECT](InputLoadingComparisonAppendix.md#evidence-lp-run-direct) | serial combiner |
| ld-prime archive → pending members | archive worker | unfair lock; bounded steals [LP-BIN-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-lp-bin-archive) | parser workers |
| Mach-O `prime` prepared slot | LLD pool worker | one writer per index; group wait [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver) | driver |
| Mach-O file parse → symbol table | driver | none required because serial [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver), [MO-SRC-OBJ](InputLoadingComparisonAppendix.md#evidence-mo-src-obj) | already global |
| ELF LoadJob result | one worker per job | parallel barrier; result vector owned by job [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load) | driver flattens in order |
| ELF per-file allocator use | worker | `makeThreadLocal`/thread-local allocation where required [ELF-SRC-MEM](InputLoadingComparisonAppendix.md#evidence-elf-src-mem), [ELF-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-elf-src-parse) | context-owned objects |
| mold spelling → global symbol | parse worker | TBB concurrent hash map [MOLD-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-mold-src-parse), [MOLD-SRC-MEM](InputLoadingComparisonAppendix.md#evidence-mold-src-mem) | shared symbol object |
| mold candidate definition → winner | resolve worker | mutex in each symbol plus deterministic rank [MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve) | shared resolved symbol |

The final barrier differs in strength. Mach-O LLD’s experimental barrier is
before **all** semantic parsing. ELF LLD’s first barrier is before serial global
symbol parsing. mold’s barrier is after full per-file parse but before
reachability/resolution. ld-prime overlaps full parsing with serialized
combination and waits for both before continuing.
[MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver),
[ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load),
[MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve),
[LP-BIN-PIPE](InputLoadingComparisonAppendix.md#evidence-lp-bin-pipe)

## Determinism

| Source of order sensitivity | ld-prime | Mach-O LLD | ELF LLD | mold |
|---|---|---|---|---|
| Direct strong/weak/common definitions | combiner is serialized; exact ordering key unobservable [LP-BIN-PIPE](InputLoadingComparisonAppendix.md#evidence-lp-bin-pipe), [LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits) | original process order is publication order [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver), [MO-SRC-SYMTAB](InputLoadingComparisonAppendix.md#evidence-mo-src-symtab) | job flattening and serial parse preserve input order [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load), [ELF-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-elf-src-parse) | definition rank includes serial file priority [MOLD-SRC-DISCOVERY](InputLoadingComparisonAppendix.md#evidence-mold-src-discovery), [MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve) |
| Archive before/after DSO | public behavior is order-sensitive [APPLE-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-apple-archive) | serial lazy extraction [MO-SRC-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-mo-src-archive) | serial lazy publication after ordered flatten [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load), [ELF-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-elf-src-parse) | reachability plus priority ranking, not parse completion [MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve) |
| Parallel task completion | does not directly call global `addAtomFile`; combiner mediates [LP-BIN-PIPE](InputLoadingComparisonAppendix.md#evidence-lp-bin-pipe) | cannot affect semantics because workers stop before parse [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver) | cannot affect flatten order [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load) | cannot affect priority [MOLD-SRC-DISCOVERY](InputLoadingComparisonAppendix.md#evidence-mold-src-discovery), [MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve) |
| Diagnostics | exact worker-error merge is unobservable [LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits) | preparation errors are reported by index; parse errors serial [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver), [MO-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mo-run-results) | tested duplicate/malformed errors match across thread counts [ELF-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-elf-run-results) | tested duplicate/undefined errors match across thread counts [MOLD-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mold-run-results) |
| Output identity control | repeated same-basename links matched [LP-RUN-HASH](InputLoadingComparisonAppendix.md#evidence-lp-run-hash) | `-no_uuid`; fixed install name for dylibs [MO-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mo-run-results) | default test outputs matched [ELF-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-elf-run-results) | default test outputs matched [MOLD-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mold-run-results) |

Output filenames are a semantic input for Mach-O dylibs and arm64 ad-hoc code
signatures. The controlled comparison therefore used a fixed
`-install_name` for Mach-O dylibs and the same output basename for ld-prime
executables. Comparing differently named outputs without that normalization
produces different bytes even when loading is deterministic.
[MO-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mo-run-results),
[LP-RUN-HASH](InputLoadingComparisonAppendix.md#evidence-lp-run-hash)

## Speculation and memory cost

| Linker/path | Speculative work | Retained memory consequence | Back-pressure |
|---|---|---|---|
| ld-prime | archive members may become parser jobs before combination [LP-BIN-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-lp-bin-archive) | detached parse results and mappings coexist until combination; this lifetime is inferred from the producer/combiner hand-off [LP-BIN-PIPE](InputLoadingComparisonAppendix.md#evidence-lp-bin-pipe), [LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits) | pending vector plus steals of at most 128 members [LP-BIN-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-lp-bin-archive) |
| Mach-O LLD serial | parses selected members only in ordinary archive mode [MO-SRC-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-mo-src-archive) | low archive-member speculation [MO-SRC-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-mo-src-archive) | naturally serial [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver) |
| Mach-O LLD `prime` | page-touches **all** archive members, even unused ones [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver) | temporary opened buffers, archive directory objects, prepared bitcode, and duplicate mappings [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver), [MO-SRC-IO](InputLoadingComparisonAppendix.md#evidence-mo-src-io) | batches of 128 page-in operations [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver) |
| ELF LLD | constructs a file object for every eligible member but defers most detailed work [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load) | member descriptors and mapped archives retained in `Ctx` [ELF-SRC-MEM](InputLoadingComparisonAppendix.md#evidence-elf-src-mem), [ELF-SRC-IO](InputLoadingComparisonAppendix.md#evidence-elf-src-io) | one job per top-level archive, not per member [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load) |
| mold | fully parses every eligible archive member [MOLD-SRC-DISCOVERY](InputLoadingComparisonAppendix.md#evidence-mold-src-discovery), [MOLD-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-mold-src-parse) | all object/section/symbol structures remain in pools even if unreachable [MOLD-SRC-MEM](InputLoadingComparisonAppendix.md#evidence-mold-src-mem), [MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve) | TBB scheduler; no archive-byte budget in this path [MOLD-SRC-DISCOVERY](InputLoadingComparisonAppendix.md#evidence-mold-src-discovery) |

mold trades peak memory and wasted parse work for maximum parallelism and
simple archive discovery. ELF LLD’s hybrid path is less speculative: it
parallelizes archive expansion but preserves a serial global-symbol boundary.
The current Mach-O `prime` mode pays page-fault and mapping costs without yet
removing the serial native parse cost, so it is an instrumentation prototype,
not evidence that the final architecture is complete.
[MOLD-SRC-DISCOVERY](InputLoadingComparisonAppendix.md#evidence-mold-src-discovery),
[ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load),
[MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver)

## Controlled runtime checks

The generated files and raw traces were kept under `/private/tmp`; no corpus or
interposer output is part of the repository. The following table records the
normalized result, not a performance claim.

| Scenario | ld-prime | Mach-O LLD serial vs `prime` | ELF LLD `--threads=1` vs `4` | mold `--threads=1` vs `4` |
|---|---|---|---|---|
| Direct objects | 11 arm64 objects parsed with overlapping dispatch workers; two repeated same-basename outputs matched SHA-256 [LP-RUN-DIRECT](InputLoadingComparisonAppendix.md#evidence-lp-run-direct), [LP-RUN-HASH](InputLoadingComparisonAppendix.md#evidence-lp-run-hash) | included in mixed corpus; outputs matched [MO-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mo-run-results) | outputs matched [ELF-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-elf-run-results) | outputs matched [MOLD-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mold-run-results) |
| Indexed regular archive, selected and unused members | an isolated archive had one referenced and nine unused members; parser workers and combiner submissions overlapped, and repeated outputs matched [LP-RUN-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-lp-run-archive), [LP-BIN-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-lp-bin-archive), [LP-RUN-HASH](InputLoadingComparisonAppendix.md#evidence-lp-run-hash) | selected `foo.o`, plus direct and IR inputs; outputs matched [MO-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mo-run-results) | outputs matched [ELF-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-elf-run-results) | outputs matched; source confirms unused member was nevertheless parsed [MOLD-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mold-run-results), [MOLD-SRC-DISCOVERY](InputLoadingComparisonAppendix.md#evidence-mold-src-discovery) |
| Thin archive | not tested [LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits) | output matched regular archive and serial/`prime` modes [MO-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mo-run-results) | output matched regular archive and both thread counts [ELF-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-elf-run-results) | output matched regular archive and both thread counts [MOLD-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mold-run-results) |
| No-index archive | not tested [LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits) | accepted by constructing lazy objects per child; output matched indexed archive and both modes [MO-SRC-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-mo-src-archive), [MO-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mo-run-results) | accepted; output matched indexed archive [ELF-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-elf-run-results) | accepted; output matched indexed archive [MOLD-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mold-run-results) |
| Whole archive | not tested [LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits) | `-all_load` serial/`prime` output hashes matched [MO-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mo-run-results) | thread-count outputs matched [ELF-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-elf-run-results) | thread-count outputs matched [MOLD-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mold-run-results) |
| Force-load/Objective-C member | code paths located; runtime not isolated [LP-BIN-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-lp-bin-archive), [LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits) | `-force_load` and `-ObjC` outputs matched between serial and `prime` [MO-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mo-run-results) | Objective-C semantics not applicable [PLATFORM-NA](InputLoadingComparisonAppendix.md#evidence-platform-na) | Objective-C semantics not applicable [PLATFORM-NA](InputLoadingComparisonAppendix.md#evidence-platform-na) |
| Malformed archive | not isolated [LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits) | identical explicit truncated-archive diagnostic in serial and `prime` [MO-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mo-run-results) | identical explicit truncated-archive diagnostic at both thread counts [ELF-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-elf-run-results) | both counts produced the same later undefined-symbol diagnostic rather than a malformed-archive diagnostic [MOLD-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mold-run-results) |
| Duplicate symlinked object | cache policy unobservable [LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits) | identical duplicate-symbol diagnostic in serial and `prime` [MO-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mo-run-results) | identical duplicate-symbol diagnostic at both counts [ELF-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-elf-run-results) | identical duplicate-symbol diagnostic at both counts [MOLD-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mold-run-results) |
| Shared library/DSO | system TAPI/dylib parse participated in runtime waves, but the interposer cannot assign every wave to an input [LP-RUN-DIRECT](InputLoadingComparisonAppendix.md#evidence-lp-run-direct), [LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits) | source path located; not isolated [MO-SRC-DYLIB](InputLoadingComparisonAppendix.md#evidence-mo-src-dylib) | DSO output hashes matched [ELF-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-elf-run-results) | DSO output hashes matched [MOLD-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mold-run-results) |
| Native and archive-contained bitcode | parser entrypoint located [LP-BIN-SYMS](InputLoadingComparisonAppendix.md#evidence-lp-bin-syms) | direct and archive-contained IR produced the same hash in serial and `prime` [MO-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mo-run-results) | direct and archive IR outputs all shared one hash across both counts [ELF-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-elf-run-results) | runtime unavailable because this local mold build had no GNU LTO plugin; source path located [MOLD-SRC-DISCOVERY](InputLoadingComparisonAppendix.md#evidence-mold-src-discovery), [MOLD-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mold-run-results) |
| Autolink/late input | later parse waves observed; assigning them to a specific late-input source is inferred [LP-RUN-DIRECT](InputLoadingComparisonAppendix.md#evidence-lp-run-direct), [LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits) | `LC_LINKER_OPTION` archive was loaded; fixed-install-name output hashes matched [MO-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mo-run-results) | `.deplibs` loaded `libused.so`; output matched explicit-DSO link and both counts [ELF-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-elf-run-results) | `.deplibs` unsupported; both counts reported the same undefined symbol [MOLD-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mold-run-results) |
| Weak/common/order-sensitive definitions | combiner boundary located; isolated corpus not run [LP-BIN-PIPE](InputLoadingComparisonAppendix.md#evidence-lp-bin-pipe), [LP-LIMITS](InputLoadingComparisonAppendix.md#evidence-lp-limits) | weak/common corpus matched in serial and `prime` for both input orders; reversing the weak-definition order changed the output as expected [MO-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mo-run-results) | thread-count outputs matched for the weak/common corpus [ELF-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-elf-run-results) | thread-count outputs matched; priority-based resolver source-confirmed [MOLD-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mold-run-results), [MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve) |

The scenarios marked “not tested” are not silently treated as passing. They
are explicitly unobservable or source-only evidence and remain follow-up items
if the review requires runtime coverage on all four implementations. In
particular, ld-prime exposes no supported worker-count option, so a true
single-thread versus multithread comparison of the same corpus is unavailable.
The repeated-output check establishes repeatability of its normal parallel
mode, not equivalence to a forced serial mode.

### Normalized hashes

The consolidated hash dump and the command that emits it are in
[LP-RUN-HASH](InputLoadingComparisonAppendix.md#evidence-lp-run-hash),
[MO-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mo-run-results),
[ELF-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-elf-run-results), and
[MOLD-RUN-RESULTS](InputLoadingComparisonAppendix.md#evidence-mold-run-results).

### Observed overlap and resource sanity check

Mach-O LLD reported:

```text
input-load-stats: mode=prime workers=2 tasks=4 completed=4
                  peak-active=2 mapped-files=4 mapped-bytes=2968
```

Its time trace placed `main.o` and `libfoo.a` on different worker thread IDs
with overlapping timestamp ranges and recorded one `Prime archive batch`.
This confirms task overlap, but the source shows that the batch only touched
the member’s pages. [MO-RUN-TRACE](InputLoadingComparisonAppendix.md#evidence-mo-run-trace),
[MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver)

The dispatch interposer observed ld-prime create
`com.apple.ld.combiner`, call `dispatch_apply` on the automatic queue, execute
iterations on multiple worker thread IDs, and submit parsed work to the
combiner group. Additional parallel waves occur later in the link, so only the
wave whose caller falls in `AtomFileConsolidator::parseFiles` is attributed to
input parsing. [LP-RUN-DIRECT](InputLoadingComparisonAppendix.md#evidence-lp-run-direct),
[LP-BIN-PIPE](InputLoadingComparisonAppendix.md#evidence-lp-bin-pipe)

One warm-cache, tiny-corpus `/usr/bin/time -l` run measured approximately
30.3 MB maximum RSS for both Mach-O LLD modes, 19.4 MB for ELF LLD with four
threads, 8.4 MB for mold with four threads, and 37.8 MB for ld-prime. These
numbers are recorded only to validate the measurement procedure. The corpus is
far too small, the binaries are built differently, and the cache order is not
controlled, so no cross-linker or speed conclusion is valid.
[RUN-RSS](InputLoadingComparisonAppendix.md#evidence-run-rss)

## Reproduction

The single entry point for reproduction is
[Appendix A1](InputLoadingComparisonAppendix.md#appendix-a1-reproduction-entry-point). It declares all tool
and temporary paths, captures versions, produces the binary/source extracts,
builds the controlled corpora, runs the four linkers, normalizes nondeterminism,
and emits the raw records cited throughout this report. The appendix then maps
every evidence ID to either a version-pinned source location or one of those
raw records.

## Implementation implications for Mach-O LLD

### Reuse from ELF LLD

The following mechanisms transfer without adopting ELF symbol semantics:

1. Assign an immutable ordinal to each top-level input and archive member.
2. Give each task a private result slot or result vector.
3. Flatten results only in the original argument/member order.
4. Use `makeThreadLocal` or task-owned allocators for worker-created nodes.
5. Keep late inputs as explicit new job waves rather than mutating the active
   wave.
6. Preserve a named barrier between detached parsing and global publication.

ELF LLD does not demonstrate fully concurrent global symbol insertion; it
demonstrates how to move substantial work off the serial path while retaining
an ordered publication phase. [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load),
[ELF-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-elf-src-parse),
[ELF-SRC-MEM](InputLoadingComparisonAppendix.md#evidence-elf-src-mem)

### Learn from mold without copying its assumptions

mold shows that aggressive native parsing is possible when the whole linker is
designed around:

- concurrent string/symbol/COMDAT maps;
- stable serial priorities assigned before task launch;
- per-symbol locking during winner selection;
- parse-time structures whose ownership is file-local or concurrent;
- archive selection as a reachability problem after speculative parse; and
- clearing and repeating resolution after COMDAT and LTO change the graph.

Mach-O LLD currently has none of these as a complete set. Adding just the
concurrent hash map would not make its global arena, autolink vector, unwind
registration, dylib cache, archive fetch state, or diagnostics safe.
[MOLD-SRC-PARSE](InputLoadingComparisonAppendix.md#evidence-mold-src-parse),
[MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve),
[MO-SRC-OBJ](InputLoadingComparisonAppendix.md#evidence-mo-src-obj),
[MO-SRC-DYLIB](InputLoadingComparisonAppendix.md#evidence-mo-src-dylib)

### Adopt the ld-prime parse/combiner contract

A production Mach-O plan should define a detached result such as:

```text
ParsedMachOInput
  ordinal and source identity
  validated target/fat-slice metadata
  file-local sections and subsections
  raw symbol records with stable string storage
  relocation records referencing local indices
  debug/unwind descriptors
  LC_LINKER_OPTION records
  diagnostics tagged with input ordinal and byte offset
```

Workers may build this result without accessing `macho::symtab`,
`inputFiles`, `unprocessedLCLinkerOptions`, `loadedDylibs`, or the global
`make<T>` allocator. A serial combiner then consumes results in semantic order,
interns symbols, performs lazy archive extraction, registers global unwind and
debug state, emits ordered diagnostics, and creates later waves for autolink
or LTO-generated files. [LP-BIN-PIPE](InputLoadingComparisonAppendix.md#evidence-lp-bin-pipe),
[MO-SRC-OBJ](InputLoadingComparisonAppendix.md#evidence-mo-src-obj),
[MO-SRC-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-mo-src-archive),
[MO-SRC-LTO](InputLoadingComparisonAppendix.md#evidence-mo-src-lto)

Archive policy needs an explicit choice:

- **select then parse** retains current memory behavior but limits member
  concurrency;
- **preparse summaries, parse selected bodies** resembles ELF LLD’s hybrid;
- **parse all then reachability-select** resembles mold and requires a memory
  budget plus priority-based resolution; or
- **nested bounded parse jobs with serial combination** resembles ld-prime.

The evidence favors detached parsing with ordered combination, initially using
ELF-style fixed result slots and bounded archive work. It preserves Mach-O
LLD’s current semantics while creating a path to broader concurrency. A
mold-style resolve-after-speculation design remains possible, but it is a
larger symbol-table and archive-semantics project rather than a seamless
`ObjFile::parse` scheduling change. [ELF-SRC-LOAD](InputLoadingComparisonAppendix.md#evidence-elf-src-load),
[LP-BIN-ARCHIVE](InputLoadingComparisonAppendix.md#evidence-lp-bin-archive),
[MOLD-SRC-RESOLVE](InputLoadingComparisonAppendix.md#evidence-mold-src-resolve),
[MO-SRC-OBJ](InputLoadingComparisonAppendix.md#evidence-mo-src-obj)

## Review gate

Production implementation should not resume until review has agreed on:

- which of the runtime gaps in the controlled-scenario table must be closed;
- the detached parse-result ownership contract;
- archive speculation and memory limits;
- the exact ordered-publication key for direct, archive, autolink, dylib
  reexport, and generated inputs;
- deterministic diagnostic merging; and
- the barrier/re-entry protocol for LTO and late inputs.

Until then, the current experimental `prime` mode should continue to be
described as parallel preparation and page-in, not parallel Mach-O object
parsing. [MO-SRC-DRIVER](InputLoadingComparisonAppendix.md#evidence-mo-src-driver),
[MO-RUN-TRACE](InputLoadingComparisonAppendix.md#evidence-mo-run-trace)

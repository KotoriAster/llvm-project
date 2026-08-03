Mach-O input-loading parallelism
================================

This document describes the intuition and implementation boundaries of the
Mach-O input-loading modes in this branch.  The modes optimize different parts
of the path from a file name to an ``InputFile``:

.. code-block:: text

  command-line order
        |
        v
  open/map file -> make pages resident -> classify/prepare -> publish InputFile
                                                            and symbols

The central constraint is deterministic publication.  Mach-O symbol parsing
updates the global symbol table, lazy archive state, and other order-sensitive
linker state.  Consequently, the modes described here parallelize only work
that can be owned by one input.  They publish inputs and symbols in command-line
order on the linker thread.

This is input-loading parallelism, not parallel linking in general.  Later
stages controlled by ``--threads=`` are outside the scope of these modes.

Summary
-------

.. list-table::
   :header-rows: 1
   :widths: 15 24 23 23 15

   * - Mode
     - Open/map
     - Page-in
     - Early preparation
     - Publication
   * - ``serial``
     - Linker thread
     - On demand
     - Linker thread
     - Linker thread
   * - ``--read-workers=N``
     - Linker thread
     - Background worker pool, files no larger than 10 MiB
     - Linker thread
     - Linker thread
   * - ``ios``
     - Worker pool
     - Worker pool, complete top-level input
     - None
     - Linker thread, ordered
   * - ``elf``
     - Linker thread
     - Worker pool, complete top-level input
     - Archive directory, bitcode, and native section preparation
     - Linker thread, ordered symbols and remaining parse
   * - ``prime``
     - Worker pool
     - Worker pool, archive members in batches
     - Archive directory, bitcode, and native section preparation
     - Linker thread, ordered symbols and remaining parse
   * - ``mold``
     - Linker thread
     - Per-native-object worker task
     - Every direct native object and native archive member
     - Linker thread, ordered archive selection, symbols, and remaining parse

``serial``: demand-driven baseline
----------------------------------

In serial mode, the linker walks the arguments in order.  For each input it
opens or maps the file and immediately calls ``processFile``.  Parsing naturally
faults in pages as their contents are inspected.

The intuition is to do exactly the work that is needed, exactly when it is
needed.  It has no task-dispatch overhead, does not increase concurrent I/O,
and generally has the lowest transient memory pressure.  It is therefore a
strong baseline for small links, hot page caches, fast storage, and workloads
whose time is dominated by symbol processing or later linker passes.

Here and in benchmark names, *serial* refers only to the input-loading path.
It does not imply that every other linker pass is single-threaded.  For
example, a serial-input benchmark may still pass ``--threads=18`` so that
unrelated parallel linker stages use the same thread setting as other modes.

``--read-workers=N``: overlap page faults with parsing
------------------------------------------------------

``--read-workers`` retains the ordered, linker-thread open/map step.  After the
initial inputs have been collected as mapped buffers, it starts background
workers that touch one byte per virtual-memory page.  The linker thread then
processes the inputs in order while this page-in work proceeds.

The intended pipeline is:

.. code-block:: text

  linker thread:  map inputs | parse A | parse B | parse C | ...
  workers:                   | page in B/C/...          |

This mode attacks stalls caused by the linker touching a mapped page for the
first time.  Its benefit comes from overlap: while the main thread performs
CPU work on an early input, workers ask the operating system to make later
input pages resident.

The implementation skips buffers larger than 10 MiB.  This limits aggressive
read-ahead and memory pressure, but it also means that a workload dominated by
large static archives may expose little useful work to the workers.  The
background job also competes with parsing for memory bandwidth and CPU time.
On a hot cache, or when parsing is not page-fault bound, task and bandwidth
overhead can make it neutral or slower.

``--read-workers`` cannot be combined with ``--input-load-demo``.  A value of
zero disables it.

``ios``: parallel top-level open and page-in
--------------------------------------------

The iOS-style demo defers top-level command-line inputs as paths instead of
opening them during the argument walk.  It creates one task per input.  Each
task opens/maps its file and touches the complete mapping.  Results are written
to fixed slots indexed by command-line position.

The fixed-slot design separates *completion order* from *publication order*:

.. code-block:: text

  workers complete:     C -------- A ---- B
  result slots:        [A]        [B]    [C]
  linker publishes:    A -> B -> C

The intuition is that file opens, mappings, and page faults for unrelated
inputs do not need to wait for one another.  Slow inputs can overlap with fast
ones, while the ordered join preserves deterministic diagnostics and symbol
resolution.

Unlike ``--read-workers``, this mode includes opening/mapping in the parallel
region and does not apply the 10 MiB skip.  It can therefore expose more I/O
parallelism for archive-heavy applications.  The tradeoffs are a full barrier
before publication, potentially higher memory pressure, and no overlap between
ordered object parsing and the initial worker phase.  It performs no archive
or bitcode preparation on workers.

``elf``: map first, then parallel preparation
---------------------------------------------

The ELF-style demo follows the scheduling shape used by ELF lld: the ordered
argument walk opens/maps inputs first, then creates a parallel load job for
each mapped file.

Each worker:

* touches all pages in the top-level buffer;
* identifies the file kind;
* constructs the LLVM archive directory representation for an archive; or
* constructs the LTO input representation for bitcode.

After that first barrier, native objects enter a second worker wave which
creates their file-local ``Section`` and ``InputSection`` graphs, splits
literal and record sections, and prepares call-graph data.  Those nodes are
owned by the corresponding ``ObjFile`` rather than the linker-global arena.
The worker records diagnostics in its fixed result slot; the linker thread
replays them in input order.

Prepared objects are stored by input index and later consumed by
``processFile`` in command-line order.

The intuition is to keep path resolution, diagnostics, and mapping simple and
ordered, but move independent, CPU-visible preparation off the linker thread.
This is attractive when archive-directory or bitcode preparation is
significant.  It cannot hide the cost of the initial ordered opens, and a task
per top-level input can be inefficient when there are many tiny files.

The name describes the scheduler being modeled.  As in ELF lld, file-local
construction is parallel while global symbol publication remains serial.  It
does not mean that the Mach-O linker runs the ELF linker's complete parser.

``prime``: hierarchical archive work
-------------------------------------

The ld-prime-style demo begins like ``ios``: workers open/map top-level inputs
into ordered result slots.  A worker also classifies the input and prepares
archive or bitcode metadata.  Direct native objects then use the same
file-local section-parse wave as ``elf`` before ordered combination.

For an archive, the top-level task enumerates members and submits page-in work
back to the same pool.  Members are grouped into batches of 128 rather than
creating one scheduler task per member:

.. code-block:: text

  libA.a task -> batch members   0..127
             -> batch members 128..255
             -> ...

The intuition is that a large archive is itself a collection of independent
inputs.  Top-level-only scheduling has poor load balance when a link contains a
few very large archives: one worker can become the long pole while other
workers go idle.  Nested member batches reveal additional parallel work and
give the pool opportunities to balance it.

Batching is essential because compiler and linker archives can contain
thousands of members.  One task per member would add excessive queue,
allocation, and tracing overhead.  The tradeoff is more total work and
potentially much more page-in traffic: prime may touch members that lazy
archive resolution never extracts.  It is most plausible for large,
archive-heavy, cold or partially cold workloads, and least plausible when
only a small fraction of archive members is needed.

``mold``: speculative object parsing
-------------------------------------

The mold-style demo keeps top-level open/map and archive enumeration on the
linker thread.  It then creates one parse task for every direct native object
and every native object member of an archive.  Unlike the other modes, archive
members are prepared whether or not lazy archive resolution will eventually
extract them:

.. code-block:: text

  linker thread: map libA.a -> enumerate [A.o, B.o, C.o] -> ordered combine
  workers:                         parse A.o | parse B.o | parse C.o

Each worker constructs only its object's file-local section graph and records
diagnostics in the object.  An indexed archive keeps the prepared object
detached until a lazy symbol selects that member.  For an archive without an
index, ordered publication first registers the prepared object as lazy and a
later extraction reuses the already-built sections.  Diagnostics from an
unselected speculative member are never emitted.

This captures mold's speculative, one-task-per-object parsing policy and its
ability to expose parallelism inside large archives.  It also exposes the
policy's central tradeoff: links that use only a small fraction of an archive
pay to parse all of its native members and retain their section graphs until
archive selection finishes.

The Mach-O implementation deliberately stops short of mold's concurrent symbol
interning and priority-based resolver.  Symbols, archive selection, and all
other global state remain ordered to preserve Mach-O LLD behavior.  Bitcode
archive members also continue through the existing on-demand LTO path.  Thus
the mode is a safe mold-shaped parsing experiment, not a port of mold's entire
resolver.

What deliberately remains serial
---------------------------------

The ``elf``, ``prime``, and ``mold`` modes construct unpublished ``ObjFile``
shells and their file-local section graphs before the ordered combine.  They
deliberately do not publish global linker state from a worker.  The following
operations remain ordered on the linker thread:

* adopting buffers into the linker's read cache;
* duplicate-path handling and fat-slice selection;
* reproducer and diagnostic updates;
* assignment of deterministic ``InputFile`` IDs and publication of inputs;
* Mach-O symbol and relocation parsing;
* updates to the global symbol table; and
* debug/unwind registration, archive extraction decisions, and their
  observable ordering.

Deferring the file ID is important: publishing an object may extract a member
from an earlier archive, and that member must receive its ID before the next
top-level object just as it does in serial mode.  This boundary preserves that
behavior without locking the global symbol table.  It also caps potential
speedup.  If symbol, relocation, or unwind processing dominates the link, the
remaining serial work will dominate wall time.

``LC_LINKER_OPTION`` inputs discovered after the initial combine enter an
explicit new loading wave and use the selected scheduler again.  They never
mutate a worker wave that is already active.  Native objects generated by LTO
still use the serial object parser and remain a future re-entry point for this
protocol.

Selecting and observing a mode
------------------------------

The experimental modes are selected with:

.. code-block:: console

  $ ld64.lld --input-load-demo=ios --input-load-workers=18 ...
  $ ld64.lld --input-load-demo=elf --input-load-workers=18 ...
  $ ld64.lld --input-load-demo=prime --input-load-workers=18 ...
  $ ld64.lld --input-load-demo=mold --input-load-workers=18 ...

If ``--input-load-workers`` is zero or omitted, the mode uses the linker's
configured thread count.  ``--input-load-stats`` prints scheduled and completed
tasks, peak active tasks, mapped files, and mapped bytes.  A peak-active value
greater than one demonstrates concurrent task execution, but does not by
itself prove a wall-time improvement.

Time traces provide stronger structural evidence:

.. code-block:: console

  $ ld64.lld --time-trace --time-trace-granularity=0 \
      --input-load-demo=prime --input-load-workers=18 ...

The trace contains worker lanes named ``input load worker`` and events named
``iOS input-load worker``, ``ELF input-load worker``,
``Prime input-load worker``, or ``Prime archive batch``.  Overlapping events on
different lanes demonstrate that the intended work actually ran concurrently.
The ``elf``, ``prime``, and ``mold`` modes additionally contain ``Mach-O object
section parse`` events for direct native inputs.  Mold archive-member tasks are
named ``Mold archive member parse``.

Benchmarking guidance
---------------------

Time tracing changes the measured work and should be disabled for performance
measurements.  Since expected improvements can be only a few percent, compare
modes in one randomized, interleaved run using:

* the exact same target link command and immutable linker binary;
* the same ``--threads`` value for every mode;
* the same output directory and delete-before-link policy;
* identical warm-up and page-cache conditions;
* enough repetitions to expose variance and outliers; and
* output hashes to verify semantic equivalence.

``lld/utils/benchmark_macho_input_loading.py`` automates the five-way
``serial``/``ios``/``elf``/``prime``/``mold`` comparison and verifies that
their outputs are byte-identical.  Results from ``--read-workers`` must not be
used as a proxy for an experimental mode: the modes move different work and
intentionally make different I/O and scheduling tradeoffs.

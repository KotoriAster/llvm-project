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

The central constraint is deterministic publication.  Mach-O object parsing
allocates from linker-global arenas and updates the global symbol table.
Consequently, the modes described here parallelize only work that can be
performed independently.  They publish inputs in command-line order on the
linker thread.

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
     - Archive directory and bitcode preparation
     - Linker thread, ordered
   * - ``prime``
     - Worker pool
     - Worker pool, archive members in batches
     - Archive directory and bitcode preparation
     - Linker thread, ordered

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

Prepared objects are stored by input index and later consumed by
``processFile`` in command-line order.

The intuition is to keep path resolution, diagnostics, and mapping simple and
ordered, but move independent, CPU-visible preparation off the linker thread.
This is attractive when archive-directory or bitcode preparation is
significant.  It cannot hide the cost of the initial ordered opens, and a task
per top-level input can be inefficient when there are many tiny files.

The name describes the scheduler being modeled; it does not mean that the
Mach-O linker runs the ELF linker's complete input-file parser.

``prime``: hierarchical archive work
-------------------------------------

The ld-prime-style demo begins like ``ios``: workers open/map top-level inputs
into ordered result slots.  A worker also classifies the input and prepares
archive or bitcode metadata.

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

What deliberately remains serial
---------------------------------

All experimental modes stop before constructing ``ObjFile`` instances.
The following operations remain ordered on the linker thread:

* adopting buffers into the linker's read cache;
* duplicate-path handling and fat-slice selection;
* reproducer and diagnostic updates;
* construction and publication of ``InputFile`` objects;
* Mach-O section, relocation, and symbol parsing;
* updates to the global symbol table; and
* archive extraction decisions and their observable ordering.

This boundary is the reason the modes can preserve output semantics without
locking the global symbol table.  It also caps their potential speedup.  If
ordered parsing and publication dominate the link, input-loading parallelism
cannot substantially change total wall time.

Selecting and observing a mode
------------------------------

The experimental modes are selected with:

.. code-block:: console

  $ ld64.lld --input-load-demo=ios --input-load-workers=18 ...
  $ ld64.lld --input-load-demo=elf --input-load-workers=18 ...
  $ ld64.lld --input-load-demo=prime --input-load-workers=18 ...

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

``lld/utils/benchmark_macho_input_loading.py`` automates the four-way
``serial``/``ios``/``elf``/``prime`` comparison and verifies that their outputs
are byte-identical.  Results from ``--read-workers`` must not be used as a
proxy for an experimental mode: the modes move different work and intentionally
make different I/O and scheduling tradeoffs.

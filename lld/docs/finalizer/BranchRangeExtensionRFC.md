# [LLD, Mach-O] RFC: A Flexible Multipass Algorithm for Branch-Range Extension

## Summary

This RFC proposes a flexible, multipass algorithm for resolving out-of-range
ARM64 branches in LLD's Mach-O port. The algorithm eliminates fixed slop
reservations. It uses compact 4-byte islands for most out-of-range branches and
12-byte thunks for branches that require greater reach. The `maxHops` parameter
controls the trade-off between the two. It is exposed as
`--branch-range-extension-max-hops=<N>` and defaults to `2`. Thunk fallback
remains available when the hop budget is exhausted or an island route cannot
be formed.

## Motivation

LLD's Mach-O port currently uses a single-pass finalizer that resolves every
out-of-range branch with a 12-byte thunk. These thunks can reach any target in
a `__TEXT` segment smaller than 4 GiB, which is sufficient in most cases. The
finalizer pessimistically reserves slop to keep every call-to-thunk branch in
range as later insertions shift the layout. This reservation reduces the usable
branch range. If it is insufficient, users must work around thunk-range
overruns by tuning `--slop_scale`. This need for manual tuning results in a
poor user experience.

Although thunks generally provide sufficient reach, islands offer two key
advantages: they preserve the `x16` register and use less space when the
`__TEXT` segment is smaller than roughly 384 MiB. Two island hops are generally
enough to cover branches in binaries of this size. We therefore propose a
slop-free hybrid that favors islands and falls back to thunks after a
configurable number of hops.

## Results

We evaluated two production binaries: App1, with a 456.05 MiB `__TEXT` segment,
and App2, with a 449.16 MiB `__TEXT` segment. We linked each binary using four
configurations: the current Mach-O thunk strategy, ld64-style islands, and the
proposed algorithm with `maxHops=inf` and `maxHops=2`. *Contribution* is the
total size of the emitted extender code: 4 bytes per island and 12 bytes per
thunk.

| Configuration | `x16` clobber | Slop-free | App1 contribution | App2 contribution |
|---|:---:|:---:|---:|---:|
| `current Mach-O thunks` | yes | no | 3.68 MiB | 6.82 MiB |
| `ld64-style islands` | no | no | 1.83 MiB | 4.16 MiB |
| `maxHops=inf` | no | yes | 1.76 MiB | 3.32 MiB |
| `maxHops=2` | on fallback | yes | 1.76 MiB | 3.67 MiB |

The corresponding extender counts are:

| Configuration | App1 thunks | App1 islands | App2 thunks | App2 islands |
|---|---:|---:|---:|---:|
| `current Mach-O thunks` | 321,574 | 0 | 596,310 | 0 |
| `ld64-style islands` | 0 | 478,439 | 0 | 1,089,665 |
| `maxHops=inf` | 0 | 461,523 | 0 | 869,543 |
| `maxHops=2` | 0 | 461,523 | 59,371 | 783,688 |

Two island hops cover all call distances in App1, so `maxHops=2` and
`maxHops=inf` produce the same layout. App2 contains calls that require more
than two island hops; with `maxHops=2`, those calls use thunks instead.

### Liking performance

![alt text](image.png)

### Runtime performance

App1 launch time:

![App1 launch time](app1-launch-time.png)

App1 page-in events:

![App1 page-in events](app1-page-in-events.png)

App2 launch time:

![App2 launch time](app2-launch-time.png)

App2 page-in events:

![App2 page-in events](app2-page-in-events.png)

App2 task faults:

![App2 task faults](app2-task-faults.png)

App2 text page-fault events:

![App2 text page-fault events](app2-text-page-faults.png)

App2 copy-on-write faults:

![App2 copy-on-write faults](app2-cow-faults.png)

## Proposed algorithm

The key to eliminating fixed slop is to replace its global estimate with a
per-boundary `reservedExtra` value learned during finalization. The hybrid uses
an ELF-style multipass algorithm because inserting an island or thunk shifts
later addresses and may push other branches out of range. Unlike ELF, which
retains the concrete thunks and `ThunkSection`s created by earlier passes, this
algorithm replans the islands and thunks on every pass against the layout
defined by `reservedExtra`. If a proposal fails validation, only the required
reservation at each boundary is retained for the next pass. Once a proposal
validates, only its live extenders are emitted.

A stable reservation layout gives every callsite and callee a stable virtual
address within a pass. Because proposed extenders do not disturb these
addresses, each callee can be planned against the same fixed layout without
recomputing the effects of other callee groups.

As a result, only the farthest callsite on each side of a callee needs to drive
island placement. An island chain built to reach that callsite also covers the
nearer callsites, which can reuse reachable islands instead of planning chains
of their own. The planner places each island as far outward from the callee as
its inward hop permits, maximizing the additional callsite range covered by
that hop. When thunk fallback is required, it places the thunk at the inward
edge of the furthest callsite's branch window. Nearer callsites can then reuse
the thunk without coupling universal-thunk placement to the island spine.
Together, these choices improve sharing and reduce the total extender
contribution.

By contrast, ELF and the previous Mach-O thunk finalizer use callsite-guided
planning. They walk callsites and find or create a reachable thunk as each
out-of-range branch is encountered. The proposed finalizer instead walks
callees and plans one shared route for all callsites that have the same
effective target.

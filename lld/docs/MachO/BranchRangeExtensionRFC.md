# RFC: Exact-Layout Branch Range Extension

## Summary

ARM64 direct branches have limited reach. When a destination is out of range in
the final linked image, lld must redirect the *long branch* through a
[range extender](#term-extender) — either a 4-byte [island](#term-island) or a
12-byte [thunk](#term-thunk). For background, see @MaskRay's blog
[Long branches in compilers, assemblers, and linkers](https://maskray.me/blog/2026-01-25-long-branches-in-compilers-assemblers-and-linkers).

This RFC adds opt-in extension modes that achieve two things; the default
`--branch-range-extension=thunks` path is unchanged.

**Slop-free.** The default `thunks` mode finalizes text in address order, so it
must decide where to place thunks before the resulting layout is known. To do
that it keeps a fixed *slop* allowance for future thunks — a pessimistic,
workload-dependent guess. Too much slop commits thunks early and creates extras;
too little produces a late `thunk range overrun` failure. Rather than tune a
better constant, these modes drop slop entirely (see the core idea below).

**Hybrid.** Extenders combine cheap 4-byte islands with 12-byte thunk fallback,
and each mode picks the balance between them: `islands-slop-free` uses islands
only, and `hybrid` allows up to
two hops plus thunks.

## 1. Algorithm

The core idea replaces the fixed slop guess with reservations driven by actual
demand: the planner reserves exactly the space the extenders it placed need
(`reservedExtra`) and lays the program out against it, so a branch is in range
iff the proposal's own exact addresses put it there. A proposal is never
patched: the failing proposal update the reservedSpace and contribute to next
pass.

The modes differ only in how far an island chain may grow before falling back to
a thunk (the per-group planning that grows those chains is covered in §4.3):

| Mode | Maximum island depth | Thunk fallback |
|---|---:|---|
| `hybrid` | 2 | Yes |
| `islands-slop-free` | Unlimited | No |

### 1.1 The exact-layout fixed point

Because every new extender moves later code, a single pass cannot be trusted:
extenders placed early can push other edges out of range, and reservations sized
for one proposal may be wrong for the next. The algorithm therefore runs as a
fixed point over repeated passes rather than a one-shot plan. Each pass does the
same three things against the current reservations:

1. Build a proposal from the current addresses.
2. Lay out the program plus *only* the proposed extenders — the exact layout.
3. Validate every `BRANCH26` edge against that exact layout.

If validation is clean the fixed point has converged and the proposal is
accepted. Otherwise the pass records what it learned and the next pass starts
over from a fresh proposal. Two pieces of monotonic memory carry across passes,
and only they:

- Each boundary's `reservedExtra` grows to the largest extender demand ever seen
  there. More room usually lets the next proposal place extenders that hold up
  under the exact layout.
- If no reservation can grow yet an edge is still invalid, a direct branch that
  the exact layout proved out of range is *forced* onto an extender path, so the
  next pass will not repeat the same mistake.

Because both facts only ever move in one direction, successive passes strictly
tighten toward a valid layout: the process makes progress or stops. The linker
fails if a pass makes no progress or the fixed point does not converge within 30
passes.

To make placement choices in each pass without thrashing, the build step uses a
second, looser layout: a *reservation envelope* that pads each input boundary
with its current `reservedExtra`. The envelope is never emitted; it only guides
where extenders can go. The exact layout in step 2 discards the unused
reservation and places the real extenders alone.

### 1.2 Driver

```cpp
collectFinalizerContext();

for (pass = 0; pass < 30; ++pass) {
    ExtensionLayout reservedLayout(ctx); // loose, padded
    proposal = planExtensionProposal(reservedLayout);
    ExtensionLayout proposalLayout(ctx, proposal); // exact
    if (validateExtensionProposal(proposal, proposalLayout).empty()) {
      materializeExtenders(proposal); // create synthetic sections/symbols
      rewriteBranches(proposal, proposalLayout);
      emitInBoundaryOrder(proposal); // assert exact-layout addresses match
      return;
    }

    bool updated = ctx.updateReservation(proposal.desiredExtra);
    if (!updated)
      updated = forceInvalidDirectBranches(proposal, proposalLayout);
    if (!updated)
      fail();
}
fail();
```

## 2. Benchmark results

The following measures each mode on two production links, AwemeLGCore and
TikTokCore. `thunks` clobbers `x16` and reserves slop; the register-safe
`islands` mode reserves a fixed island region instead. Only `islands-slop-free`
and `hybrid` are slop-free, and `hybrid` falls back to `x16` thunks only where
two island hops cannot reach — zero thunks on AwemeLGCore, a small tail on
TikTokCore in exchange for fewer islands than `islands-slop-free`. The
*contribution* column is the total code these extenders add, at 4 bytes per
island and 12 bytes per thunk.

| Mode | `x16` clobber | Slop-free | AwemeLGCore thunks | AwemeLGCore islands | AwemeLGCore contribution | TikTokCore thunks | TikTokCore islands | TikTokCore contribution |
|---|:---:|:---:|---:|---:|---:|---:|---:|---:|
| `thunks` | yes | no | 321,574 | 0 | 3.68 MiB | 596,310 | 0 | 6.82 MiB |
| `islands` | no | no | 0 | 478,439 | 1.83 MiB | 0 | 1,089,665 | 4.16 MiB |
| `islands-slop-free` | no | yes | 0 | 461,523 | 1.76 MiB | 0 | 869,543 | 3.32 MiB |
| `hybrid` | on fallback | yes | 0 | 461,523 | 1.76 MiB | 59,371 | 783,688 | 3.67 MiB |

## 3. Data model

Three structures carry the algorithm, separated strictly by lifetime. Keeping
decisions, facts, and addresses apart is what makes a proposal disposable.

| Structure | Lifetime | Holds | Mutable across passes? |
|---|---|---|---|
| `FinalizerContext` | whole finalization | topology + fixed-point memory | only the two monotonic facts |
| `ExtensionProposal` | one pass | the extender graph + routing | rebuilt every pass |
| `ExtensionLayout` | one pass | derived addresses | recomputed every pass |

### 3.1 `FinalizerContext` — facts and cross-pass memory

The immutable topology, gathered once:

- `inputs`: every code input section, flattened into one address-ordered array
  that spans the contiguous run of `__text`-family output sections.
- `ownerRuns`: for each output section, the slice of `inputs` it owns, so the
  flat index space can be mapped back to real sections at emit time.
- `callees`: branch destinations, each grouped by *effective* target
  (identity + addend). Every callee owns its `callsites`, kept sorted by
  address.

Plus the only two things that persist between passes, both monotonic:

- `reservedExtra`: per-boundary reservation, non-decreasing.
- per-callsite `forceExtender`: once set, stays set.

Everything else in the context is frozen after collection. These two fields are
the entire memory of the fixed point.

### 3.2 `ExtensionProposal` — the decisions

A disposable graph produced by one planning pass:

- `extenders`: the islands and thunks to create,
  each recording its kind, the callee it serves, its boundary, and (for a relay
  island) the next island in its chain.
- `routing`: for each callsite, which extender it uses, in callee-major
  callsite order. A sentinel means "stays direct."
- `desiredExtra`: how many extender bytes each boundary actually wants, the
  signal used to grow the reservation.

The proposal holds *no addresses*. It is a pure statement of intent that can be
thrown away and rebuilt.

### 3.3 `ExtensionLayout` — the consequences

Addresses derived from the context topology, a set of reservations, and
(optionally) a proposal:

- `inputVA`, `extenderVA`: where each input and extender lands.
- `calleeTargetVA`: each destination's resolved address, or "unresolved."
- `textEndVA`: the end of the extended text, used to estimate later sections.

The layout holds *no decisions*. It is a pure function of its inputs, so the
planner can produce a loose *planning layout* (padded by `reservedExtra`) and an
exact *proposal layout* (only the accepted extenders) from the same machinery.

### 3.4 How they interplay

> Context is read during a pass and written only at its end; proposal and layout
> live and die within the pass; validation closes the loop between them.

```text
                 reservedExtra, forceExtender   (monotonic memory)
                          |
        FinalizerContext  |  (frozen topology)
             |            |
             v            v
   layoutReservationEnvelope --> planning ExtensionLayout (loose)
             |                          |
             |          planExtensionProposal  (reads topology + addresses)
             |                          |
             v                          v
        ExtensionProposal  --> layoutExtensionProposal --> proposal
        (decisions)                              ExtensionLayout (exact)
                     \                          /
                      validateExtensionProposal
                                |
                 valid? accept  |  invalid? grow reservation
                                |            or force a branch,
                                |            then rebuild proposal+layout
```

Within a pass the context is read-only. The planner reads topology from the
context and addresses from the planning layout, and emits decisions into the
proposal. The proposal layout turns those decisions back into addresses.
Validation is exactly the question "do the proposal's decisions hold under the
proposal's own addresses?" Only when a pass ends unresolved does anything write
back to the context, and only to the two monotonic fields. That write-back is
the sole channel between iterations, which is why proposals and layouts can be
discarded freely.

## 4. Phase semantics

Each phase is described by what it does with the data model, not how it is
coded.

### 4.1 Collection — build the context

Walk the contiguous run of code output sections from the first, appending their
input sections into the single address-ordered `inputs` array and recording each
section's slice as an owner run. Then visit every `BRANCH` relocation in
ascending address order and bucket it by effective destination:

- A non-binding local `Defined` is keyed by its address so aliasing symbols
  collapse into one callee.
- Anything else is keyed by its symbol.
- The addend is always part of the key.

Each callsite (relocation + owning input) is appended to its callee. Because
relocations are visited in address order, every callee's `callsites` end up
sorted, which later phases rely on. Reservations start at zero.

### 4.2 Reservation-envelope layout — a loose planning layout

Lay out inputs owner by owner, and after each input insert its current
`reservedExtra` bytes of headroom. This produces `inputVA`, `textEndVA`, and
resolved `calleeTargetVA`. The padding gives the planner room to place extenders
whose addresses will not immediately shift everything downstream out from under
it. Early passes, with small reservations, are tight; later passes are roomier.

### 4.3 Proposal planning — grow the graph

For each callee, take its target address from the planning layout:

- If the target is unresolved (for example a stub whose address cannot yet be
  estimated) and the mode allows thunks, route every callsite through a thunk;
  an islands-only mode fails here.
- Otherwise split the callee's sorted callsites at the target and process each
  side from the farthest caller inward, so island chains grown from the
  destination are shared by nearer callers.

For a single callsite: if it is in range and not forced, leave it direct.
Otherwise try to build an island chain; if that fails and the
mode allows thunks, place or reuse a thunk; if everything fails,
error.

Building an island chain starts at the destination and repeatedly asks for a
real input boundary that is within range and closer to the caller, up to the
mode's hop limit. The boundary search first samples coarse boundaries about
1 MiB apart, then falls back to every boundary, then to a dense local window.
Each accepted boundary becomes an island; the callsite routes to the head of the
chain.

Thunk placement reuses an existing in-range thunk for the same callee when one
exists — reuse is callee-scoped because each thunk carries that callee's
addend — otherwise it finds an in-range boundary and creates one.

Throughout, the planner accumulates `desiredExtra` per boundary: the total
extender space this proposal wants there.

### 4.4 Proposal layout — lay out exactly what was proposed

Order the accepted extenders by boundary, then lay out the inputs and *only*
those extenders, with no unused reservation. This yields the exact `inputVA`,
`extenderVA`, `calleeTargetVA`, and `textEndVA` the program would have if this
proposal were emitted.

### 4.5 Validation — check decisions against consequences

Check every edge against the exact proposal layout, in two parallel sweeps:

- Extenders: each terminal island must reach its callee's resolved target;
  each relay island must reach a lower-indexed island that is in range.
- Callsites: a routed callsite must be in range of its extender; an unrouted
  callsite must be in range of its direct target.

The result aggregates counts of each invalid category. Empty means the proposal
is self-consistent under its own layout and can be accepted.

### 4.6 Learning from failure

If validation is non-empty, update the context's monotonic memory:

- Grow each boundary's `reservedExtra` up to its `desiredExtra`. If anything
  grew, the next pass replans with more room.
- If nothing grew — reservations already cover demand yet edges remain
  invalid — set `forceExtender` on the direct callsites the exact layout proved
  out of range, so the next proposal will not leave them direct.

If neither makes progress, the fixed point cannot converge and the link fails.

### 4.7 Materialize, rewrite, emit — commit the accepted proposal

Only the first self-valid proposal reaches these phases:

- **Materialize**: create one synthetic input section and symbol per extender at
  its boundary. Islands branch to their relay or target; thunks emit
  `adrp; add; br x16` to the target (carrying the addend, and adding a stub
  entry when the target needs binding).
- **Rewrite**: for each callsite, keep it direct if the final layout puts its
  target in range; otherwise repoint its relocation at the extender symbol.
- **Emit in boundary order**: finalize inputs and their boundary-attached
  extenders in the stable order the proposal layout used, so real addresses
  match the addresses that were validated.

### 4.8 Failure modes

The link fails deterministically rather than emitting an out-of-range branch:

- Topology exceeds the planner's index space (too many inputs, callees, or
  extenders at one boundary).
- Computing an effective target address overflows or underflows `uint64_t`.
- A callsite cannot be given a valid route: no in-range thunk placement, no
  legal island chain, or an unresolvable islands-only target.
- The fixed point does not converge within 30 passes, or a pass makes no
  progress.

## 5. Comparison to prior algorithms

> Difference from the proposed design: prior algorithms either guess the layout
> once or iterate against the real output; this design iterates against a
> disposable exact layout of each proposal.

- **Thunks (Mach-O lld), one pass.** Finalizes text in address order and uses a
  fixed slop allowance to decide whether a branch can be deferred. The layout is
  never revisited, so a bad slop guess costs extra thunks or a late failure.
  This design replaces the guess with a fixed point over exact layouts.
- **ELF lld, iterative.** Repeatedly creates thunks and reassigns real output
  addresses until nothing changes, retargeting edges that a later pass pushed
  out of range. This design shares the iterate-to-a-fixed-point shape but
  iterates over a *proposed* exact layout that is discarded on rejection, rather
  than mutating the committed output in place.

## Appendix A: Terminology

<a id="term-extender"></a>

### Extender

An *extender* is linker-generated code that gives an out-of-range branch a
reachable intermediate destination. In this RFC, an extender is either an
island or a thunk.

<a id="term-island"></a>

### Island

An *island* is a 4-byte ARM64 direct branch inserted between a callsite and its
destination. Its outgoing edge is also range-limited, so islands may be
chained: a terminal island branches to the original destination, while a relay
island branches to another island closer to that destination.

<a id="term-thunk"></a>

### Thunk

A *thunk* is a 12-byte `adrp; add; br x16` sequence inserted within
direct-branch range of a callsite. Its branch to the original destination does
not use a range-limited `BRANCH26` relocation, so a thunk does not need an
island chain. It clobbers `x16`.

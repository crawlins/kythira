# Design Document

## Overview

Two opt-in changes to the object-store engine's on-bucket format, plus one
change to the order of calls `node` makes:

| Part | Knob | What it costs today | What it costs after |
|---|---|---|---|
| One hard-state write per RequestVote (`node`) | none — all engines | follower: term, `"none"`, vote = **3 PUTs** | **2 PUTs** on the legacy layout |
| Combined hard state | `hard_state_layout::combined` | candidate 2 + follower 2 sequential PUTs | candidate **1** + follower **1** |
| Segmented log | `log_layout::segmented` | one PUT per entry per follower | one PUT per AppendEntries run (≤ `max_segment_entries` / `max_segment_bytes`) |

No concept changes. `kythira::persistence_engine`,
`bulk_append_persistence_engine` and `hard_state_persistence_engine`
(`include/raft/persistence.hpp:138,159`) keep their signatures and contracts;
everything new is a field on `object_persistence_options`
(`include/raft/object_store_persistence.hpp:441`). Defaults are today's bucket,
byte for byte and request for request (cloud-object-persistence Property 8).

The design rests on one decision that the rest follows from: **every new
format lives in a key an older binary already reads, in a body that older
binary already refuses to parse.** The combined hard state is written to
`<prefix>/term`; segments are written to `<prefix>/log/<first index>`. An older
engine (and the older backup tool) fails construction on either, naming the
key, so a rollback onto an upgraded bucket is *refused* rather than silently
served stale state. A new key would have been invisible to the old load path
(`object_store_persistence.hpp:1649` ignores unrecognised keys), and invisible
is exactly the failure that turns a rollback into a double vote.

## Why this is two layouts and one node change

### The election path, read from `raft.hpp`

Every hard-state write in `node` already routes through two helpers, which is
what batched-durable-writes introduced:

| Site | Line | Call |
|---|---|---|
| `become_candidate` — term + 1, self-vote | `raft.hpp:6592` | `persist_term_and_vote_locked()` |
| `handle_request_vote` — grant | `raft.hpp:5594` | `persist_term_and_vote_locked()` |
| `become_follower` — newer term, vote cleared | `raft.hpp:6558-6562` | `persist_term_locked()` |
| `handle_install_snapshot` — after the snapshot | `raft.hpp:6285` | `persist_term_locked()` |
| `persist_term_and_vote_locked` / `persist_term_locked` | `raft.hpp:7404`, `:7415` | `save_hard_state` when the engine has it |

AppendEntries makes no hard-state write of its own any more — "the term only
changes in become_follower(), which stores it before this node answers"
(`raft.hpp:6060-6061`) — so the append path pays none. The only direct
`save_current_term` outside the helpers is the multi-raft split, which
seeds a fresh child store (`multi_raft_impl.hpp:2167`); under the combined
layout that is one PUT of `{term, null}`, and it needs nothing else.

So every path *can* be one write, and each one already is one call — except
that `handle_request_vote` at a newer term makes **two**: `become_follower()`
(`raft.hpp:5534`) persists `(T, no vote)`, and the grant persists
`(T, candidate)`. On the legacy object layout, against a follower that voted in
the previous term, `save_hard_state` turns those into three PUTs: `term`, then
`voted_for = "none"`, then `voted_for = candidate`. The documented
`4 × p99(PUT)` floor counts two writes per side; the common case pays five sequential PUTs per
election round, not four. The first write is pure waste: it is superseded
before the node replies, and nothing reads it in between.

That is a node-level fix (Requirement 1) and it is the first slice because it
helps every engine and needs no format change. The combined hard state then
halves what remains.

### The append path, read from `raft.hpp`

A leader appends through `submit_command` (`raft.hpp:2742`), which takes
`_mutex` and calls `persist_append` (`:2808`) → `append_log_entry` → one PUT
while holding it. Client commands therefore serialise on the leader at one PUT
each, and **no change to the follower's write pattern can raise the cluster's
commit rate above about `1 / p50(PUT)`**. A follower's AppendEntries carries
whatever the leader appended while the previous one was in flight —
`multi-raft-performance` task 8 measured 1.03 entries per AppendEntries at one
proposal in flight, and the leader here cannot have more than one in flight.

The segmented log is still worth having, for three reasons that do not depend
on steady-state batching: a lagging or restarted follower catches up in
`N / max_segment_entries` requests instead of `N`; recovery GETs fall by the
same factor; and it is the prerequisite for leader group commit, which is the
change that would lift the ceiling and which this spec leaves to its own spec.
The operating envelope below says this in numbers rather than implying more.

## Data Models

### Object layout, all settings

```
<prefix>/term                       legacy:    "42"
                                    combined:  {"term":42,"voted_for":7}
                                               {"term":42,"voted_for":null}
<prefix>/voted_for                  legacy:    "7" | "none"
                                    combined:  absent on a fresh prefix,
                                               "retired" on a migrated one
<prefix>/log/<20-digit first index> singleton: {"term":3,"index":42,"command":"…","type":0}
                                    segment:   {"first_index":42,"last_index":57,
                                                "entries":[{…42…},{…43…},…,{…57…}]}
<prefix>/snapshot                   unchanged
<prefix>/snapshots/<20-digit index> unchanged
<prefix>/owner                      unchanged (fencing only)
```

What does **not** change, and why each matters:

- **Key names.** `is_engine_owned` in the backup tool
  (`object_store_backup.hpp:623`) already covers every key either layout
  writes, so `restore_clone --force` still deletes exactly the engine's keys.
- **20-digit first-index keys.** Listing order stays index order (Property 2),
  and the key of a log object is unique per first index — which is what makes
  truncation an in-place overwrite and keeps create-only collisions meaningful
  (below).
- **The entry codec.** A singleton body and each element of a segment's
  `entries` array are byte-identical to today's `entry_to_json` output
  (`object_store_persistence.hpp:1836`), so a segmented bucket still diffs
  entry-for-entry against a file engine's data directory.

### The combined record

```
{"term":<uint64>,"voted_for":<NodeId as JSON number or string> | null}
```

- Exactly those two members, in that order; any other member, a missing one, or
  a wrong JSON type fails construction naming `<prefix>/term`. Strictness is
  deliberate: it makes this binary refuse a *future* format the same way older
  binaries refuse this one.
- Detection is by the first byte: `{` is combined, anything else goes to the
  existing `parse_unsigned` (`:1794`), unchanged. The two cannot be confused —
  `parse_unsigned` requires the whole body to be decimal digits (with one
  tolerated trailing newline), and the conformance case
  `a_partially_numeric_term_is_corruption_not_a_prefix` already pins that.
- `term` is read with `to_number<std::uint64_t>()`, not the `as_int64()` the
  entry codec uses, so the full `TermId` range round-trips.

### The segment record

- `first_index` equals the key's index; `last_index > first_index`;
  `entries.size() == last_index - first_index + 1`; `entries[k].index ==
  first_index + k`. Any disagreement fails construction naming the key — the
  same rule as today's "a record that disagrees with its key"
  (`:1636-1645`).
- A one-entry segment is invalid: one entry is always a singleton. One
  representation per state keeps backups, diffs and tests unambiguous.
- No top-level `term`, `index` or `command` member, so an older
  `json_to_entry` (`:1845`) — which does `obj.at("term")` — throws on it.

### Engine options

```cpp
enum class hard_state_layout : std::uint8_t { separate, combined };
enum class log_layout : std::uint8_t { per_entry, segmented };

struct object_persistence_options {
    // … existing fields unchanged …
    hard_state_layout hard_state{hard_state_layout::separate};
    log_layout log{log_layout::per_entry};
    /// Default 100 = types.hpp's default max_entries_per_append, so a full
    /// AppendEntries is one segment. Must be ≥ 2.
    std::size_t max_segment_entries{100};
    /// Default 1 MiB. Bounds the in-place rewrite truncation pays and the
    /// GET recovery pays per object. Must be ≥ 1 and ≤ max_object_bytes.
    std::size_t max_segment_bytes{1024U * 1024U};
};
```

Runtime fields, not template arguments, unlike `fencing_mode`. The rule that
made fencing a template argument — a capability the store may lack must be a
compile error — does not apply: both layouts use only operations every shipped
client has (Requirement 13). A non-default `max_segment_*` under `per_entry` is
rejected at construction, following the existing "no field that is accepted
and ignored" rule (`owner_id` on an unfenced engine, `:762-770`).

Mirror additions, `_mu`-protected like the rest:

```cpp
/// Under log_layout::segmented only: first index → last index, plus the
/// version under compare_and_swap for segments (never for singletons, which
/// are never conditionally rewritten). Empty under per_entry, so the default
/// engine's memory is unchanged.
std::map<LogIndex, log_object_meta> _objects;
/// A pending retirement of <prefix>/voted_for (combined layout, migrated
/// prefix), with the version it was loaded at.
std::optional<object_version> _vote_slot_to_retire;
```

`_strays` (`:2014`) stays a `std::set<LogIndex>`: a log object's key is its
first index, so the set of stray first indices *is* the set of stray keys.

## Part 1 — One hard-state write per RequestVote

`become_follower(new_term)` (`raft.hpp:6519`) is split, behaviour-preserving
for its other ten callers:

```cpp
/// Everything become_follower() does except persisting. Returns whether the
/// term moved, i.e. whether the caller now owes a persist.
auto step_down_locked(term_id_type new_term) -> bool;
auto become_follower(term_id_type new_term) -> void {   // unchanged contract
    if (step_down_locked(new_term)) { persist_term_locked(); }
}
```

`handle_request_vote` (`raft.hpp:5464`) calls `step_down_locked` at Rule 2
(`:5534`) and remembers `owes_persist`. After that point it has exactly three
returns — Rule 3 deny (unreachable after a bump, since the vote was just
cleared, but written for the same-term case), Rule 4 deny, and the grant — and
each persists once before returning: the denies with `persist_term_locked()` if
`owes_persist`, the grant with `persist_term_and_vote_locked()`
unconditionally, which already carries the new term. The response is built
after the persist on every path, as Figure 2 requires.

**Why deferring is safe.** The handler holds `_mutex` throughout
(`:5466`); nothing between the step-down and the persist sends a message,
starts a timer callback that reads durable state, or returns to the caller.
A crash before the persist loses a term bump the node never acted on.

**What it changes on a legacy engine with `save_hard_state`.** The grant now
writes `term` then `voted_for = candidate`: two PUTs, down from three. The
crash state between those two is `(T, V_prev)` — new term, the vote from the
previous term — where it used to be `(T, none)`. The concept allows that state
(`persistence.hpp:152-156`), and it cannot produce a second vote in `T`: the
node never replied to the candidate, so the only vote it can cast in `T` after
restart is to `V_prev`, which it believes it already gave. It is a liveness
regression confined to a crash in a one-PUT window, on the layout this spec
exists to replace, and it is stated rather than engineered away. Under the
combined layout there is no intermediate state at all.

On an engine without `hard_state_persistence_engine`, the grant becomes
`save_current_term` then `save_voted_for` (term first, Requirement 1.5) instead
of `save_current_term`, `save_current_term`, `save_voted_for`.

## Part 2 — Combined hard state

### Write path

```
save_hard_state(term, vote)       → skip if (term, vote) == mirror
save_current_term(term)           → record {term, mirror vote}
save_voted_for(node)              → record {mirror term, node}
   └─ retire_vote_slot_if_pending()          (Part 2, migration)
   └─ put_single_slot(term_key(), record)    (:1423 — CAS chokepoint under fencing)
   └─ mirror ← (term, vote)                  (after the acknowledgement, as today)
```

One PUT, one object. A follower granting at a newer term (after Part 1) and a
candidate starting an election each pay exactly one sequential round trip.

Atomicity is the store's, not the engine's: every one of the five services
writes a whole object atomically per key (`key_object_store.hpp`, "per-key
atomic whole-object writes"), so after a crash at any point a reader sees the
old record or the new one. The `hard_state_persistence_engine` contract's third
state (new term, old vote) cannot arise.

### Load precedence

| `<prefix>/term` | `<prefix>/voted_for` | Read as | `separate` engine | `combined` engine |
|---|---|---|---|---|
| absent | absent | `(0, none)` | as today | as today; first write is create-only |
| decimal | any legacy value / absent | legacy pair | as today | legacy pair; **first write migrates** |
| `{…}` | absent | combined record | **refused** | combined record |
| `{…}` | `retired` | combined record | **refused** | combined record |
| `{…}` | a legacy value | combined record, vote slot ignored | **refused** | combined record; retirement pending |
| decimal | `retired` | — | corrupt `voted_for` | corrupt `voted_for` |

The last row is unreachable through the engine (`retired` is written only after
the combined `term`) and through the backup tool (restore refuses a non-empty
target without `--force`, and `--force` deletes `voted_for` too); it is
corruption, so it fails construction naming the key. Resolution happens after
the listing loop, so it does not depend on the order the store lists `term`
and `voted_for` in.

### Migration, in place, lazily

Construction writes nothing for a migration (the existing "construction writes
nothing until the first mutation" property, `:719-723`), with one exception
under fencing, below. The first hard-state write on a legacy prefix is:

1. **m1** — PUT `<prefix>/term` ← combined record. Under `compare_and_swap`,
   `If-Match` the legacy `term` object's version, which `load_all` already
   records (`:1572`). **Commit point.** Mirror updated.
2. **m2** — if `<prefix>/voted_for` exists: PUT `<prefix>/voted_for` ←
   `retired`. Under `compare_and_swap`, `If-Match` its loaded version.

m2 runs after the commit point, so — like snapshot pruning (cloud design
Property 4) — a transient failure does not fail the call. It is recorded in
`_vote_slot_to_retire` and retried at the start of the next hard-state write,
before that write's PUT; a refused precondition on it latches.

**Crash at every boundary:**

| Crash | Store | New code reads | Older binary |
|---|---|---|---|
| before m1 lands | legacy pair | old pair (call unacknowledged) | old pair |
| m1 landed, response lost | combined + legacy vote slot | new pair | **refuses** (`term` not decimal) |
| between m1 and m2 | combined + legacy vote slot | new pair; retirement pending | **refuses** |
| m2 landed | combined + `retired` | new pair | **refuses** |

**The rollback argument (Requirement 3.6).** A node can acknowledge `(T', V')`
only after m1 is acknowledged. From then on `<prefix>/term` is never again a
decimal — no engine code path writes one under `combined`, a `separate` engine
refuses to construct, and `restore_seed` writes one only into an empty prefix.
Every older engine in this repository's history parses `term` with the strict
`parse_unsigned` and throws `corrupt object <prefix>/term`; every older backup
tool parses it with the equally strict `parse_u64`
(`object_store_backup.hpp:255`). So no binary reads a pair older than one the
node acknowledged — older binaries read nothing at all. The runbook must say
that an older binary's `corrupt object <prefix>/term` on an upgraded bucket is
the rollback guard, not damage.

**Why m1 must come before m2, and not the other way round.** Retiring the vote
slot first would leave, after a crash between the two, a legacy decimal `term`
beside a vote slot that no longer holds the vote: a legacy reader would load
`(T, none)` for a term in which the node had voted, and could vote again. The
order above is the only one with no such state.

**Why retire rather than delete.** The vote slot has to change *version* so
that a pre-migration writer still running against the prefix is refused at its
next conditional write to it (Part 2, fencing). A DELETE would do that only if
every provider answers `If-Match` against a missing key with a precondition
failure. That is an OPEN cell — S3, Azure and OCI plausibly answer 404, which
the clients map to an ordinary error, not `object_precondition_failed` — and
`If-Match` against an existing object's stale version is the cell
cloud-object-persistence live-verified on all four fencing providers. Retiring
costs one PUT once per bucket lifetime and leaves a 7-byte object.

### Downgrade

`separate` over a combined `term` throws at construction:

```
object_store_persistence_engine: <prefix>/term holds a combined hard-state
record, but this engine is configured hard_state_layout::separate. An in-place
downgrade is not supported: run `raft_object_backup relayout --hard-state
separate` against the stopped node, or set hard_state_layout::combined.
```

The relayout verb is task 11 and optional. It is a two-PUT operation that is
itself crash-safe in this order — PUT `voted_for` ← legacy value (ignored while
`term` is combined), then PUT `term` ← decimal (commit point) — but it is
offline-only so that it never races a running engine.

### Fencing

The chokepoint argument (cloud design, "Fencing state machine") becomes simpler:
**a second writer cannot change this node's term or vote without writing
`<prefix>/term`**, a single object written `If-Match` on every mutation. The
vote slot is no longer state on a combined prefix; it matters only as a place a
*pre-migration* writer can still write, and m2's `If-Match` is what detects
that writer:

- the stale writer writes `voted_for` before m2 → m2's `If-Match` is refused →
  **the migrating engine latches** (detection; it does not arbitrate, exactly as
  today);
- the stale writer writes after m2 → its own `If-Match` is refused → it latches;
- the stale writer writes `term` at any time after m1 → refused, as today.

That is the same coverage the legacy layout has today, where a stale
`voted_for` write is detected at the rightful engine's next `voted_for` write.
**The residual is the window between m1 and m2**: if m2 fails transiently, a
stale writer's same-term vote goes undetected until the retry. Under
`compare_and_swap` a retirement still pending at load is therefore completed
during construction, after `claim_ownership()` (`:1704`) — fenced construction
already writes the owner object, so this adds no new kind of startup write —
which bounds the window to one process lifetime at most.

**Lost-response residual, unchanged in kind.** A combined PUT whose response is
lost after it applied leaves a stale tracked version and latches the next write
— the residual cloud design Property 3 already states for `term`. It now covers
the vote as well, which is one object's worth of exposure instead of two.

### GCS

GCS throttles mutations of one object to about one per second
(cloud-object-persistence spike-notes Finding 19). The combined record is one
object mutated once per election per node and once per term change — no more
often than `term` is mutated today — so the limit binds no harder. It does bind
the floor below: on GCS an `election_timeout_min` under ~1 s can drive a
repeatedly campaigning node into `429`, as it can today.

## Part 3 — Segmented log

### Keys, and why first-index-only

A log object is keyed by its **first** index alone. Two shapes that were
considered and rejected (see Alternatives) keyed by last index, or by
`<first>-<last>`; first-only is the shape under which:

- listing order is index order with no tie-break;
- truncation of a straddling segment is an **atomic overwrite of the same key**,
  so it has no torn state at all;
- create-only still collides whenever two writers begin an object at the same
  index, whatever its length — a singleton and a segment at index `k` share the
  key `log/k`.

### Append

```
append_log_entries(run)                      segmented, run extends the log
  ├─ refuse before sending if run.front() != last + 1 (log non-empty)
  ├─ chunk greedily: ≤ max_segment_entries, ≤ max_segment_bytes;
  │    a chunk of one entry is a singleton; an entry larger than
  │    max_segment_bytes is its own singleton (max_object_bytes still applies)
  ├─ check_object_size on every chunk before any request   (:992 today)
  ├─ clear_strays(keep = { chunk keys })                    (Requirement 6.5)
  ├─ put_log_objects(chunk keys, chunk bodies, workers)     (:1245, unchanged)
  └─ mirror: _log ← entries, _objects ← chunks              (after all acks)
```

`append_log_entry`, and a run that does not extend the log, send singletons —
exactly today's requests. Under `segmented` a non-extending append is refused
with `std::invalid_argument` before any request, because a singleton written at
an index inside an existing segment would be a stray the mirror believed was
the log. `node` never sends one: cloud-object-persistence task 5 verified that
the follower appends only above its last index and truncates first, and the
leader appends at `get_last_log_index() + 1` (`raft.hpp:2804`).

**A trap in the existing code.** `append_log_entries` builds the stray
keep-set from the run's entry indices (`std::set<LogIndex> covered`,
`object_store_persistence.hpp:995`). Under `fencing_mode::none`, `clear_strays`
then skips deleting any stray in that set on the grounds that the run's PUT will
overwrite it (`:1286-1302`). With segments that is wrong: a stray singleton at
index 18 is inside a run 16..25 that writes one object at key 16, so it is not
overwritten, it is kept, and it is forgotten. The keep-set must be the set of
**keys** the run writes (Requirement 6.5). Under `per_entry` the two sets are
equal, so the fix is behaviour-preserving for the default.

### Truncation — `truncate_log(i)`

```
clear_strays({})
DELETE every object with first ≥ i, highest first     (each ack → mirror)
if an object S = [f..l] has f < i ≤ l:
    PUT log/f ← S's entries f..i-1                      (singleton if f == i-1)
        If-Match S's version under compare_and_swap; unconditional otherwise
    mirror: _log erase [i..l]; _objects[f].last = i-1, new version
```

Crash at every boundary — the pre-call chain is `C`, the post-call log ends at
`i-1`:

| Crash | Chain recovery reads |
|---|---|
| during the DELETEs | `C` minus its top k objects: a prefix of `C` that still ends at or above `l ≥ i-1` |
| overwrite not landed | as above, ending at `l` |
| overwrite landed | ends at `i-1`: the post-call log |

Every intermediate is a prefix of the pre-call log no shorter than the
post-call log, which is what `truncate_log`'s caller can tolerate: the
truncated entries conflicted with the leader and were uncommitted
(`raft.hpp:5979` refuses a conflict with a committed entry), so reading
them back after a crash is reading entries the node was in the middle of
removing — the same tolerance the legacy layout's highest-first DELETEs rely on
today. Highest-first is load-bearing: overwriting before the DELETEs would
leave the objects above as strays past a gap, which recovery also handles, but
the monotone-prefix argument above is simpler to test exhaustively.

Cost: one DELETE per object wholly above `i`, plus at most one PUT of at most
`max_segment_bytes`.

### Compaction — `delete_log_entries_before(j)`

```
clear_strays({})                                    (new — today's compaction does not)
DELETE every object with last < j, lowest first    (each ack → mirror)
if an object S = [f..l] has f < j ≤ l:
    PUT log/j ← S's entries j..l   create-only under compare_and_swap
        → recorded as a stray until the next step is acknowledged
    DELETE log/f                                    commit point
    mirror: _log erase [f..j-1]; _objects: drop f, add j
```

Crash at every boundary — the post-call log starts at `j`:

| Crash | Store | Chain recovery reads |
|---|---|---|
| during the DELETEs | `C` minus its lowest k objects | a suffix of `C` starting at or below `f` |
| Q landed, S not deleted | `S = [f..l]` and `Q = [j..l]`, `f < j` | starts at `f`, takes `S`, continues at `l+1`: `Q` begins inside `S`, so it is a **stray** — the pre-compaction suffix |
| S deleted | `Q` | starts at `j`: the post-call log |

Lowest-first is load-bearing here, in the opposite direction from truncation:
deleting below-objects highest-first would leave the lowest one standing alone
past a gap, recovery would start the chain there and stop at the gap, and the
entire log above it would be read as strays. The legacy compaction is already
lowest-first (`:1099-1107`), so this is not a new constraint, but it becomes
the property a mistake would violate silently, so the crash suite pins it.

**Why split rather than keep the straddler.** Leaving `S` whole would make
compaction free of PUTs, and `node` would not notice — `initialize_from_storage`
reads only from `last_included_index + 1` (`raft.hpp:5326-5338`). But the
engine's own contract ("every entry with index < `j`" deleted) would then be
false after a restart, and the conformance case
`delete_before_survives_reload` pins that contract. One PUT per snapshot is not
worth weakening it.

### Recovery — the chain

```
objects ← every log key, GET each, parse singleton or segment (validated)
pos ← lowest first index
while objects has an object starting at pos:
    chain ← that object; pos ← its last + 1
everything not on the chain → _strays
```

Under the legacy layout every object is a singleton, no two share a first
index, and this is exactly `keep_contiguous_log()` (`:1669`): it keeps the
contiguous run from the lowest index and records the rest. Task 6 replaces one
with the other and must leave the default engine's conformance results and
request logs untouched.

### The stray invariant

The crash tables above each assume the pre-call store holds exactly the chain.
That is an invariant, not an assumption:

> **I1.** No stray exists on the store when a log mutation sends its first
> write.

It holds because (a) load records every non-chain object as a stray; (b) every
log mutation — append, truncation, and compaction, which today skips it — calls
`clear_strays` first and throws without writing if a DELETE fails
(Requirement 9.2); (c) a mutation that fails part-way records its own leftovers
as strays before throwing. So the store's non-chain objects at any moment are
the leftovers of at most one operation, and the per-operation tables are the
whole crash analysis.

One more fact closes it: **deleting a stray never changes the chain.** A stray
cannot start at the chain's next position (it would be on the chain), and
cannot lie below the chain's start (the chain starts at the lowest first
index). So a crash during `clear_strays` reads the same chain as before it.

Why I1 matters, by counter-example: without (b) on compaction, a stray `S'`
left by a failed truncation that overlaps a later straddler could become the
lowest object once the compaction deletes everything below it, and the chain
would start at stale entries.

### Fencing

| Write | Precondition |
|---|---|
| new-key log PUT (append, compaction's `Q`) | create-only, as today |
| truncation's in-place overwrite | `If-Match` the segment's tracked version |
| every DELETE | none — the stated residual (cloud Requirement 9.3) |

Versions are tracked for segments only (Requirement 10.2): a singleton never
straddles anything, so it is never conditionally rewritten, and today's choice
not to track per-entry versions (`:2018-2022`) stands for it.

**What a stale appender now does**, compared with the legacy layout:

| Stale writer's object starts at… | Legacy layout | Segmented layout |
|---|---|---|
| the first index of one of the owner's objects | create-only collision — **detected** | the same key — **detected** |
| strictly inside one of the owner's segments | collision at that index — **detected** | different key, no collision; at the next load it begins inside a chain object and is a stray — **discarded, not detected** |
| the owner's `last + 1`, before the owner writes there | loaded at restart until the owner's write collides | the same |

So the segmented layout loads nothing the legacy layout would not, and in the
middle row it is safer at load (the legacy layout may load whichever writer won
the collision) but **weaker at detection**. That is a residual, stated: under
`compare_and_swap` a stale appender whose writes all fall inside the owner's
segments is not detected by log writes; it is still detected the moment it
touches `term`.

The lost-response residual gains one surface: truncation's `If-Match` overwrite
can now latch a sole writer whose previous segment PUT landed with its response
lost.

### Bounds

- `max_segment_entries = 100`: one default AppendEntries
  (`types.hpp:775`) is one segment.
- `max_segment_bytes = 1 MiB`: the largest rewrite truncation can cost and the
  largest GET recovery pays per object. At the measured ~30-150 ms in-region
  p50 for small objects a 1 MiB PUT is plausibly still well inside an election
  timeout, but **no segment-sized PUT has been measured** on any provider;
  Requirement 16.3 measures it before the default is quoted as safe.
- Both are validated at construction (Requirement 5.6). `max_object_bytes`
  (64 MiB) still bounds every single PUT, including an oversized singleton.

## Layout transitions, summarised

| From → to | In place? | How |
|---|---|---|
| separate → combined | yes | lazily, at the first hard-state write (m1, m2) |
| combined → separate | **refused** at construction | offline `relayout` (task 11, optional) |
| per_entry → segmented | yes | nothing to do: singletons are valid objects |
| segmented → per_entry | **refused** while any segment exists | offline `relayout`; or naturally, once compaction has removed every segment |
| any upgraded bucket → older binary | **refused** by the older binary | its strict parse of `term` / `log/<index>` |

The two knobs are independent, and each node's layout is independent of every
other node's — nothing on the wire changes — so a rolling upgrade can enable
them one node at a time.

## Part 4 — Backup and restore

`object_store_backup` reads the format directly and says so
(`object_store_backup.hpp:60-66`), so it changes with it:

| Function | Change |
|---|---|
| `interpret_metadata` (`:730`) | `term` beginning `{` → `current_term` and `voted_for` from the record (`null` → `"none"`, matching the legacy sentinel's manifest value); `voted_for == "retired"` contributes nothing; resolved after the listing loop |
| `interpret_log_key` (`:803`) | unchanged for the key; for a segment body, `last_log_index` comes from `last_index`, and an unparseable body throws so no manifest is written — the existing "present but unparseable" rule |
| `check_objects` (`:831`) | per-entry `check_entry_term` over every entry of a segment; restorable objects collected as `(first, last)` |
| contiguity | the engine's chain rule over the restorable objects; non-chain objects reported in a new `verification_report::notes` as `"stray_object"`, **not** as problems — a quiesced torn source legitimately has one, and the restored engine deletes it before its first write |
| manifest | `format_version = 2` iff the copy holds a combined `term` or a segment, else 1 |
| `is_engine_owned` (`:623`) | unchanged |
| `restore_clone` (`:453`) | unchanged — byte-for-byte copy preserves the layout, and the owner epoch bump is untouched |
| `restore_seed` (`:509`) | unchanged — writes a decimal `term`, which either setting accepts (a `combined` engine migrates it on its first write) |

**What an older backup tool does with an upgraded bucket.** A combined `term`:
`create` throws (`parse_u64`), no manifest. A segment with a legacy `term`:
`create` succeeds with a wrong `last_log_index` (it reads only keys), `verify`
then reports `unreadable_entry` for the segment (its `check_entry_term` needs a
top-level `term`), and restore runs `verify` first and aborts. So it is never
silent, but the second case is an older tool producing a backup it then
rejects; the runbook says to back up an upgraded bucket with the upgraded tool.

`cmd/raft_object_backup` needs no new flag for any of this
(Requirement 12.5). The optional `relayout` verb is the one addition.

## Part 5 — Providers

Nothing new is asked of a client:

| Needs | S3 | Azure Blob | GCS | OCI | Alibaba OSS |
|---|---|---|---|---|---|
| whole-object atomic overwrite of `term` | live | live | live | live | live |
| `If-Match` on overwrite (combined CAS, m1, m2, truncation overwrite) | live | live | live | live | n/a — `fencing_mode::none` only |
| create-only on new log keys | live | live | live | live | live (`x-oss-forbid-overwrite`) |
| bodies up to `max_segment_bytes` | documented, **unmeasured** | documented, **unmeasured** | documented, **unmeasured** | documented, **unmeasured** | documented, **unmeasured** |
| `Content-MD5` / local digest on segment bodies | client sends, engine verifies | client sends | client sends | client sends | client sends, engine verifies |
| concurrency over chunks | declared | declared | declared | declared | declared |

"Live" means cloud-object-persistence task 0 / task 16 verified it on the real
service for single-entry objects. Every one of those rows is a property of the
request, not of the body, so it carries over; the size row does not, and
Requirement 16 closes it. No client is expected to change; if a real run says
otherwise, that client changes in its own commit.

## The operating envelope, reworked

### Election latency floor

Sequential PUTs per election round (candidate, then a follower that voted in
the previous term):

| Code | Candidate | Follower | Rule |
|---|---|---|---|
| today | 2 | 3 (term, `"none"`, vote) | `5 × p99(PUT) + rpc_rtt` |
| after Part 1 | 2 | 2 | `4 × p99(PUT) + rpc_rtt` |
| Part 1 + combined | 1 | 1 | `2 × p99(PUT) + rpc_rtt` |

A follower that had no vote pays one fewer today (`4 ×`), which is what the
current documentation's table actually computes. Worked from the CI-runner
p99s in `doc/cloud_object_persistence.md` (worst-of-run at 8 samples, so a floor
rather than a tail):

| p99(PUT) | today (5×) | after Part 1 (4×) | combined (2×) |
|---|---|---|---|
| 30.4 ms Azure Blob, in-provider | 152 ms | 122 ms | **61 ms** |
| 81.3 ms S3 `us-east-1` | 407 ms | 325 ms | **163 ms** |
| 153.2 ms GCS `us-central1` | 766 ms | 613 ms | **306 ms**, but see GCS above: keep ≥ ~1 s |
| 1140.5 ms OSS, cross-ocean | 5.70 s | 4.56 s | **2.28 s** |

all plus `rpc_rtt` and margin, and the randomized range width must be at least
the same quantity. Pre-vote adds one RPC round trip and no PUT.

### Throughput ceiling

**Unchanged by either layout**: the leader's `submit_command` holds `_mutex`
across one PUT per command, so a cluster commits at most about `1 / p50(PUT)`
entries/s — ~35 (Azure in-provider), ~15 (S3), ~8 (GCS), ~0.9 (OSS
cross-ocean). What changes is the follower and recovery side:

| | per_entry | segmented |
|---|---|---|
| Follower requests per AppendEntries run of N | N | ⌈N / 100⌉ (by default) |
| Follower catch-up, N = 10,000 on S3 (p50 ≈ 67 ms) | 10,000 PUTs; ≈ 1,250 rounds at concurrency 8 ≈ 84 s | 100 PUTs; ≈ 13 rounds ≈ 1 s **plus the unmeasured cost of a 100-entry body** |
| Recovery GETs for a log of N | N | ≈ N / B |

### Cost

Per **three-node cluster**, ~$0.005 per 1,000 PUTs (S3 / Azure / GCS / OCI),
`B` = entries per AppendEntries that carries entries:

| Sustained rate | per_entry (3 PUTs/entry) | segmented, B = 1 | B = 2 | B = 10 | B → ∞ |
|---|---|---|---|---|---|
| 1 entry/s | ~$39/month | ~$39 | ~$26 | ~$16 | ~$13 |
| 10 entries/s | ~$389/month | ~$389 | ~$259 | ~$156 | ~$130 |

The honest reading: **at steady state with one client command in flight, B is
about 1 and the segmented layout saves almost nothing.** It saves up to a third
of the bill when AppendEntries batch (a burst, a slow follower, many concurrent
clients queuing on the leader's mutex), and its large savings are on catch-up
and recovery, which are not steady-state costs. The leader's PUT per entry
remains the floor — about $13/month per entry/s per cluster — until leader group
commit exists. The 100 entries/s row the current document carries stays
unreachable on every measured provider.

The combined hard state's cost effect is negligible (one fewer PUT per election
per node).

## Correctness Properties

### Property 1: Defaults are today's bucket, to the request
**Validates: Requirements 11.1, 2.1, 5.1**

At default options no code path writes a combined record, a segment, or a
retired vote slot, and the load path reduces to today's: the chain walk equals
`keep_contiguous_log()` on singletons, and `term` without `{` goes to the
unchanged `parse_unsigned`. **How it could fail silently:** a refactor that
routes the default through the new write path and "happens" to produce the same
bytes for one entry but not the same requests (an extra stray DELETE, a GET).
The unmodified conformance suite — including
`a_default_engine_writes_exactly_the_documented_key_set` and the request-count
cases — is the check, run before and after each task.

### Property 2: A combined write is one request and atomic
**Validates: Requirements 2.2, 2.3**

Atomicity is the service's per-key whole-object write; the engine sends one PUT
and updates the mirror after it. **How it could fail silently:** the combined
layout quietly still writing `voted_for` "for compatibility", which doubles the
election cost while every read still looks right. The request-log case asserts
one PUT to `term` and zero to `voted_for`.

### Property 3: No binary reads hard state older than the node acknowledged
**Validates: Requirements 3.4, 3.6**

Argued in Part 2: m1 is the commit point and leaves a body every other binary
refuses. **How it could fail silently:** a future "lenient" `parse_unsigned` (one
that reads the leading digits, or skips a `{`) would turn the refusal into a
silent stale read for every older-format reader written after it. The
legacy-reader case freezes the parse rules (Requirement 15.5), and this
property is cited beside `parse_unsigned` so the next person to touch it sees
why it must stay strict.

### Property 4: Every request boundary reads as a legal log
**Validates: Requirements 7.2, 8.2, 9.3**

The truncation and compaction tables, plus I1 and "deleting a stray never
changes the chain". **How it could fail silently:** an ordering slip — DELETE
the straddler before writing `Q`, or delete below-objects highest-first — loses
or hides committed entries only after a crash at exactly the wrong request, and
never in an uninterrupted run. That is why Requirement 15.3 enumerates every N
rather than sampling.

### Property 5: Strays never meet a write
**Validates: Requirements 6.4, 6.5, 9.2**

I1. **How it could fail silently:** a stray kept by the index-based keep-set,
or a compaction that skips `clear_strays`, sits harmlessly until a later
operation makes it the chain's start or a create-only key's occupant. A
targeted case seeds each of those two shapes.

### Property 6: Fencing still has a single chokepoint
**Validates: Requirements 4.1-4.5, 10.1-10.3**

Under `combined`, `term` is the only object whose content is term or vote, and
it is `If-Match` on every write; the vote slot's retirement is `If-Match` too.
**How it could fail silently:** the migration's m1 sent create-only (because the
engine "has never seen a combined record") instead of `If-Match` the legacy
version — it would be refused against an existing object and latch a healthy
node, or, worse, under a client that mapped that refusal wrongly, overwrite a
second writer's term. The fenced conformance harness runs the migration.

## Error Handling

| Condition | Behaviour |
|---|---|
| combined `term` that does not parse, or has extra / missing / mistyped members | construction throws naming `<prefix>/term` |
| segment whose key, range, count or indices disagree, or with < 2 entries | construction throws naming the key |
| `separate` engine over a combined `term` | construction throws: downgrade not supported in place, names the remedy |
| `per_entry` engine over any segment | construction throws: same |
| decimal `term` beside `voted_for == "retired"` | construction throws naming `voted_for` |
| `max_segment_entries < 2`, `max_segment_bytes` 0 or above `max_object_bytes`, or non-default `max_segment_*` under `per_entry` | `std::invalid_argument` at construction |
| non-extending append under `segmented` | `std::invalid_argument`, no request sent |
| retirement PUT fails transiently | recorded, retried before the next hard-state write (under fencing: at construction too); the call that committed m1 succeeds |
| retirement PUT or truncation overwrite refused | `persistence_fenced_error`, latched |
| compaction's straddler DELETE fails | throws; `Q` recorded as a stray; mirror unchanged for `S` |

## Testing Strategy

### Existing suites that must stay green unmodified

- `tests/object_store_conformance.hpp` via
  `tests/object_store_persistence_unit_test.cpp:208,215,222` (mock, fenced
  mock, MD5-versioned mock) — Property 1.
- `tests/batched_durable_writes_unit_test.cpp` (25 engine cases) and
  `tests/raft_batched_durable_writes_test.cpp` (cluster cases, including
  `a_refused_vote_from_a_newer_term_stores_the_term` and
  `a_follower_that_moves_terms_does_not_keep_its_previous_vote`, which are
  exactly the deny path Part 1 touches).
- `tests/object_store_backup_unit_test.cpp`,
  `tests/object_store_backup_cli_unit_test.cpp`,
  `tests/alibaba_oss_persistence_mock_test.cpp`,
  `tests/oci_object_storage_mock_conformance_test.cpp`, and each provider's
  client unit test.

### Additions

- **Conformance, per layout.** The harness gains `default_options()`; three new
  instantiations (combined, segmented, both) over the mock and the fenced mock,
  plus one over the Alibaba mock server and one over the OCI mock server, which
  verify signatures from the bytes that arrived and so exercise real request
  shapes at segment size. Layout-dependent cases (the exact key set, request
  counts, key shapes) read their expectation from the harness rather than being
  forked.
- **Crash injection** (`mock_object_store` gains `crash_after_requests(n)`:
  every request after the n-th throws and nothing further reaches the shared
  state). For each operation — combined write, migration, segmented append of a
  multi-chunk run, truncation across a segment, compaction across a segment,
  `clear_strays` — and for **every** n from 0 to the operation's request count:
  a fresh engine loads; its log and hard state are in the oracle set for that
  operation; its next mutation succeeds; and a further fresh engine equals the
  second engine's mirror. Run at `append_concurrency = 1` for determinism, and
  separately at 8 with randomised completion order for the append case.
- **Model-based property case.** Random sequences of append / truncate /
  compact / hard-state writes / restart / crash-at-random-request against both
  layouts and a reference model (a `std::map` and a pair), for a fixed seed set
  in CI and a seed taken from the environment for soak runs.
- **Frozen legacy reader** (`tests/legacy_layout_reader.hpp`): today's
  `parse_unsigned`, `json_to_entry` and the backup tool's `parse_u64` and
  `check_entry_term` predicates, copied verbatim with the commit they came from.
  A case asserts each rejects every body the new layouts write. It exists
  because "an older binary refuses" cannot be tested by running an older binary
  in CI, and the predicates are what the claim rests on.
- **Targeted cases**: the index-based keep-set trap; a stray made the chain's
  start by a compaction that skipped `clear_strays`; a non-extending segmented
  append refused; the fenced migration with a second writer touching
  `voted_for` before and after m2; each downgrade refusal message.
- **Raft-level** (extending `raft_batched_durable_writes_test.cpp`'s
  `recording_engine`): a follower at term T-1 that voted, granting a RequestVote
  at T, records **one** hard-state call — three-before against the current
  `raft.hpp` — and the deny-after-bump path records one storing no vote.

### Real-cloud (opt-in, `KYTHIRA_<PROVIDER>_REAL_TESTS`, needs Clark's go)

New cases in `tests/object_persistence_real_cases.hpp`, run on all five
providers (OSS unfenced), never CTest-registered:

1. **Migration read-back**: a default engine writes term, vote and a log; a
   `combined` + `segmented` engine loads it, writes a vote and a 100-entry run;
   a fresh engine reads everything back; a `separate` engine is refused.
2. **Truncation and compaction across a real segment**, then fresh read-back.
3. **Live negative control** (four fencing providers): an engine holding the
   pre-migration `voted_for` version writes it after m2 and is refused by the
   service.
4. **Latency**: p50/p99 of a combined hard-state write and of a full-size
   (100-entry, and `max_segment_bytes`) segment PUT, with measurement position.
5. **Backup → verify → clone-restore → read-back** of an upgraded prefix.

GCS's case 1 spaces its hard-state writes at 1100 ms, as the existing latency
case does (Finding 19).

## Alternatives considered

1. **A new key, `<prefix>/hard_state`.** The obvious shape, and rejected for
   finding 3: an older engine ignores it, reads the stale decimal `term` and
   `voted_for`, and can vote twice. Making it safe would need the legacy slots
   poisoned anyway — at which point the record might as well be the poison.
2. **A vote record carrying its term** (`voted_for = "<term>:<node>"`), PUT
   concurrently with `term`, the load path adopting the higher term — the option
   batched-durable-writes' future-work section named. It halves latency the same
   way but costs two requests, keeps two chokepoints, and is not a refusal for a
   string-`NodeId` older binary, which would read `"5:7"` as a node id.
3. **Segment keys `<first>-<last>`.** Truncation becomes write-new, delete-old,
   with two objects sharing a first index and a tie-break rule at recovery
   (shortest wins). Worse, a singleton `log/k` and a segment `log/k-m` no longer
   collide under create-only, so a stale appender's singleton would *win* the
   tie-break. First-only keys remove the tie-break and keep the collision.
4. **Keyed by last index.** Compaction's suffix object would share the
   straddler's key.
5. **Segments under `<prefix>/segments/`.** Invisible to an older engine, which
   would load a short log — the failure finding 3 exists to prevent.
6. **A `<prefix>/layout` marker object.** Same objection: an older binary ignores
   it, so it cannot refuse anything. Layout is inferred from bodies that older
   binaries parse.
7. **Delete the vote slot instead of retiring it.** Depends on an unverified
   provider cell (`If-Match` on an absent key), Part 2.
8. **Lazy compaction.** Part 3.
9. **Make the combined hard state the default.** It is the better layout, but
   Property 8 is a promise to existing deployments; and the downgrade refusal
   would then fire on any rollback of the *code*, not only of a setting.

## Residual gaps, stated

1. **The cluster's append ceiling is unchanged** (leader, one PUT per command
   under `_mutex`). Leader group commit is out of scope.
2. **Segmented-layout fencing detects less**: a stale appender writing only
   inside the owner's segments is discarded at load, not detected.
3. **DELETEs and snapshot writes stay unconditional** (unchanged).
4. **The lost-response latch** now also covers truncation's `If-Match`
   overwrite.
5. **A lagging listing that hides `<prefix>/term`** reads term 0 on any binary,
   as today; list-after-write is empirically closed on all five providers but
   not documented by Azure or OCI.
6. **Window between m1 and m2** under fencing, bounded by construction-time
   completion.
7. **No in-place downgrade**; the remedy is the optional offline `relayout`.
8. **An older backup tool** produces a backup it then rejects from a bucket with
   segments and a legacy `term`.
9. **On the legacy layout after Part 1**, a crash inside the grant's two PUTs
   stores the previous term's vote beside the new term — liveness only.
10. **Segment-sized PUT latency is unmeasured** on every provider, so the 1 MiB
    default is reasoned, not measured, until Requirement 16.3.

## Open questions

1. **Leader group commit.** Without it the segmented log's steady-state saving
   is near zero (the cost table, `B ≈ 1`). Ship Parts 1-2 now and the segmented
   log after (as tasks are ordered), or hold tasks 6-9 until a group-commit spec
   exists so the two are measured together?
2. **Downgrade.** Is the optional offline `relayout` verb wanted, or is
   "forward-only, restore from a pre-upgrade backup" an acceptable policy?
3. **Real-cloud runs** (Requirement 16) need approval; they reuse the existing
   per-provider CI bundles and buckets and add a few hundred requests per run.

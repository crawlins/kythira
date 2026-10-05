# Design Document

## Overview

Two optional extensions on the persistence seam, in `include/raft/persistence.hpp`
next to `barriered_persistence_engine` and detected the same way:

```cpp
template<typename P>
concept bulk_append_persistence_engine =
    requires(P& p, std::span<const typename P::log_entry_t> entries) {
        { p.append_log_entries(entries) } -> std::same_as<void>;
    };

template<typename P>
concept hard_state_persistence_engine =
    requires(P& p, decltype(p.load_current_term()) term, decltype(p.load_voted_for()) vote) {
        { p.save_hard_state(term, vote) } -> std::same_as<void>;
    };
```

Optional rather than added to `persistence_engine` because a required method is
a breaking change for every third-party engine, and because `node` has a correct
fallback for each.

## `node`

- **Follower appends.** `append_entries_with_consistency_check` used to call
  `append_log_entry` (in memory) then `persist_append` for each new entry. The
  new entries are always a suffix of the request, so the loop now finds the
  first one and hands the suffix to `persist_and_append_run`. On an engine with
  bulk append and no barrier, that is one `append_log_entries` call made
  **before** the entries join `_log`; the engine's all-or-nothing contract means
  a throw leaves `_log` and the engine agreeing, and the leader's retry sends the
  same run. Every other engine keeps the existing per-entry path, unchanged.
  Barriered engines (file, memory) stay on it deliberately: they already pay one
  barrier per AppendEntries.
- **Hard state.** Two helpers replace the four direct call sites.
  `persist_term_and_vote_locked()` (vote grant, election start) and
  `persist_term_locked()` (after a log change, after InstallSnapshot). With the
  extension both are `save_hard_state(_current_term, _voted_for)`, which is what
  stores "no vote" after `become_follower()`. Without it, the first writes term
  then vote and the second writes only the term, so a legacy engine sees the
  same calls as before except that the grant path is now term-first.

## Memory and file engines

Both implement both extensions. The memory engine's `save_hard_state` keeps the
existing fault points reachable: the term's on every call (every site it replaces
called `save_current_term`), the vote's when the vote changes. The file engine
writes `term` then `voted_for`, each only when changed, and writes "none" for no
vote, which `load_all` has always read back as no vote. Its `append_log_entries`
writes all lines with one `write` and updates the mirror afterwards; inside an
open batch it uses the batch's own per-entry path to keep the undo record.

## Object-store engine

### `save_hard_state`

`term` then `voted_for`, each only when changed, through the same
`put_single_slot` (so under `compare_and_swap` both remain the CAS chokepoint).
The saving on the election path is small: a follower granting a vote in a term
it has just learned changes both slots, so it still pays two PUTs. The saving
that matters is on the append path, where the per-AppendEntries term write is
now skipped, halving the cost of a single-entry AppendEntries.

### `append_log_entries`

One PUT per entry, as before, because the layout stays one object per entry.
For a run that extends the log, up to `append_concurrency` are in flight at once
on worker `std::jthread`s that pull indices from an atomic counter. Workers read
only immutable members (`_store`, `_bucket`, `_opts`) and report an outcome per
entry; the calling thread holds `_mu` throughout and alone mutates state. Once a
PUT fails no further PUT is started.

Concurrency needs the store's consent. `concurrent_key_object_store` is a
declaration by the client, like `content_md5_versioned_store`: the engine cannot
tell a client that opens a connection per call from one sharing a socket. All
five shipped clients declare it (S3 and GCS SDK clients are documented
thread-safe; the Azure, OCI and OSS clients build an `httplib::Client` per call
and keep shared token caches behind mutexes), as does the test mock.

### Strays

A failed run leaves an unknown subset of its objects on the store while the
mirror has none of them. Two things go wrong if they stay: under
`compare_and_swap` the create-only retry of the same index is refused and latches
the engine as if a second writer existed; and if the log is later rewritten
shorter, an old object can end up adjacent to the new tail and be read back as
part of it after a restart, possibly with a lower term than the entry below it.

So every index the run sent is recorded in `_strays`, and `clear_strays` deletes
them (highest first) before the next log write or truncation. Under
`fencing_mode::none` a stray the next write is about to overwrite is not
deleted first, which makes the ordinary retry free. A DELETE that fails fails the
write that asked for it: writing on top of an undeleted stray is the thing being
prevented.

### Holes at load

A crash mid-run can leave a hole: entry 13 on the store, 12 not. Before this
change `load_all` loaded whatever it listed, and `node`, which keeps its log in a
vector indexed by position, would have shifted every entry above the hole. Now
`load_all` keeps the contiguous run from the lowest index and records the rest as
strays, deleted before the next log write (construction still writes nothing).
Dropping them is safe: an object past a hole is from an unacknowledged run, or
from a truncation that was removing it. Truncation now deletes highest-first so
it cannot create a hole itself.

A lagging listing that hides an entry in the middle of the log now also drops
everything above it. That listing had already lost an acknowledged entry, which
is the residual the engine header already states; the change is that the loaded
log is at least a valid one.

## Future work

- **Concurrent term and vote.** The two PUTs on the election path must be
  ordered because the vote object does not say which term it belongs to. A vote
  record carrying its own term (`"<term>:<node>"`) would let the load path adopt
  the higher of the two and the writes go out together, halving the election
  path's durable-write latency. It changes the on-bucket format, so it is left
  for its own change.
- **Batching a leader's own appends**, which arrive one client command at a
  time.

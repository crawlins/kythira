# Requirements Document

## Introduction

`kythira::persistence_engine` has one way to write a log entry,
`append_log_entry`, and two separate calls for Raft's hard state,
`save_current_term` and `save_voted_for`. `doc/TODO.md` and
`doc/cloud_object_persistence.md` both name the cost: on an engine where every
durable write is a network round trip, a follower handed N entries pays N round
trips back to back, so append throughput is capped at about one entry per
round trip.

Reading the call sites turned up three more problems with the same root:

1. **The vote-granting path wrote the vote before the term.**
   `handle_request_vote` called `save_voted_for` and then
   `save_current_term`. `become_follower()` persists nothing, so when a node
   grants a vote in a term it has just learned, the stored term is still the
   previous one when the vote is written. A crash between the two writes leaves
   the new vote beside the previous term. After a restart the node believes it
   voted for the candidate in a term in which it may already have voted for
   someone else, and grants that candidate a second vote in that term. That is
   a double vote, the one thing Raft's election safety rests on not happening.
2. **"No vote" could not be stored.** `save_voted_for` takes a node id. A node
   that moves to a newer term without voting clears `_voted_for` in memory and
   writes only the term, so its previous term's vote stays on storage beside
   the new term. After a restart it believes it voted in a term it never voted
   in. That only ever makes it refuse a vote, so it is a liveness defect, but it
   is wrong state on disk.
3. **Every log-modifying AppendEntries rewrote the term.** The follower path
   calls `save_current_term` after any log change whether or not the term
   changed. On the object-store engine that is one extra sequential PUT per
   AppendEntries, doubling the cost of a single-entry append.

**Scope**: two optional extensions to the persistence seam, their
implementations in the three shipped engines (memory, file, object store), and
`node`'s use of them. The base concept does not change, so a third-party engine
keeps compiling and keeps its behaviour, apart from the corrected write order.

**Out of scope**: batching a leader's own appends (one per client command,
which would need a group-commit queue in front of `submit_command`); shared
multi-entry objects on the object store (a layout change); writing the term and
the vote concurrently (needs a vote record that carries its own term, see the
design's future-work section).

## Requirements

### Requirement 1: Bulk append extension

**User Story:** As an operator running Raft on an object store, I want a
follower to write the entries of one AppendEntries together, so that append
throughput is not capped at one entry per store round trip.

#### Acceptance Criteria

1. THE persistence seam SHALL define an optional `bulk_append_persistence_engine`
   concept with `append_log_entries(std::span<const log_entry_t>)`, detected with
   `if constexpr` and never required.
2. WHEN `append_log_entries` returns THEN every entry SHALL be as durable as
   `append_log_entry` would have made it.
3. WHEN `append_log_entries` throws THEN none of the run SHALL be visible
   through the engine's reads.
4. WHEN a follower appends entries on an engine that has the extension and no
   durability barrier THEN `node` SHALL make one `append_log_entries` call per
   AppendEntries, before the entries join its in-memory log.
5. WHEN the engine has a durability barrier THEN `node` SHALL keep the existing
   per-entry path, which already costs one barrier per AppendEntries.
6. THE memory, file and object-store engines, and `group_scoped_persistence`
   over any of them, SHALL satisfy the concept.

### Requirement 2: Concurrent log PUTs on the object store

1. `object_store_persistence_engine::append_log_entries` SHALL keep up to
   `object_persistence_options::append_concurrency` (default 8, `0` rejected)
   PUTs in flight for a run that extends the log.
2. Concurrency SHALL be used only when the store declares
   `supports_concurrent_requests` (`concurrent_key_object_store`); any other
   store gets one PUT at a time.
3. Each PUT SHALL be the one `append_log_entry` sends: same key, same body, the
   same single retry when unconditional, create-only under `compare_and_swap`.
4. WHEN a run fails THEN every entry it sent SHALL be recorded as a stray and
   deleted before the engine next writes or truncates the log.
5. WHEN a PUT's precondition is refused THEN the engine SHALL latch, exactly as
   `append_log_entry` does.
6. WHEN the engine loads a log with a hole THEN it SHALL keep the contiguous run
   from the lowest index and treat the objects past the hole as strays.
7. `truncate_log` SHALL delete from the highest index down, so a crash part-way
   through leaves a contiguous log.
8. An oversized entry SHALL fail the run before any PUT is sent.

### Requirement 3: Hard-state extension

**User Story:** As a Raft operator, I want the term and the vote persisted
together and in a crash-safe order, so a restart can never produce a double
vote or a vote the node never cast.

1. THE persistence seam SHALL define an optional `hard_state_persistence_engine`
   concept with `save_hard_state(term, std::optional<vote>)`.
2. AFTER a crash at any point during the call, the stored pair SHALL be the old
   pair, the new pair, or the new term with the old vote, never the old term
   with the new vote.
3. `std::nullopt` SHALL be stored as "no vote" and read back as no vote.
4. An engine MAY skip writing a slot whose value has not changed; the file and
   object-store engines SHALL.
5. `node` SHALL persist term and vote through `save_hard_state` where the engine
   has it, at every site that persisted either.
6. WHERE the engine lacks it, `node` SHALL write the term before the vote at
   every site that writes both.

### Requirement 4: Tests

1. Engine-level cases SHALL cover both extensions on all three engines, the
   concurrency bound, failure and retry, strays, holes at load, truncation
   order, and the fenced engine.
2. Raft-level cases SHALL run a real three-node cluster and SHALL fail on the
   previous `raft.hpp`: the follower's run, the term-before-vote order, and a
   follower that moved terms without voting storing no vote.

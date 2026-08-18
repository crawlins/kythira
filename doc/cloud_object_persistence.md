# Cloud object persistence

Raft state — term, vote, log, snapshot — stored as individual objects in a
cloud key-object store, with one engine generic over five providers (AWS S3,
Azure Blob, Google Cloud Storage, OCI Object Storage, Alibaba OSS).

> **Status.** This document currently covers **backup and restore**
> (Requirements 10 and 11). The durability contract, the per-provider evidence
> table and the operator configuration examples are task 18 of
> `.kiro/specs/cloud-object-persistence/` and are not here yet. What is written
> below is complete and current; what is missing is missing, not summarised.

## Backup and restore

`include/raft/object_store_backup.hpp` provides `object_store_backup<Store>`
with `create`, `list`, `verify`, `restore_clone` and `restore_seed`. It takes a
store and a prefix, never instantiates the persistence engine, and cannot be
reached from the Raft hot path.

A CLI — `cmd/raft_object_backup` — is **task 14 and does not exist yet**. The
runbooks below therefore give the operations and their exact arguments rather
than a shell transcript; the command sequences will be filled in against the
real binary rather than guessed at now.

### Where backups go, and why not beside the source

Write backups to a **different bucket** from the source, and to a different
account or project where the provider supports it. Nothing enforces this,
because one bucket is better than no backup — but a backup sharing its source's
bucket shares its blast radius: one bad lifecycle rule, one mistaken prefix
delete, one compromised credential takes both copies.

### Layout

```
<backup_prefix>/<backup_id>/objects/<key relative to the source prefix>
<backup_prefix>/<backup_id>/manifest.json      ← written LAST
```

The manifest being written last is the entire commit protocol. None of the five
services offers cross-key atomicity, so "the manifest exists" is the only thing
"the backup finished" can mean. A run that dies partway leaves object copies and
no manifest, and `list` ignores any directory without one — so a torn copy is
invisible rather than restorable.

`backup_id` defaults to `YYYYMMDDTHHMMSSZ`: fixed width, zero padded, no
punctuation, so lexicographic listing order *is* chronological order, which is
the only ordering these stores give.

### `verify`, and why a quiesced claim is not believed

A backup taken from a running node is a **smear** across the copy window.
Objects are read one at a time and the node keeps writing, so the copy can hold
state read early and state written late. `backup_options::source_quiesced`
records the operator's claim about the source and the manifest carries it — but
a claim is not evidence, so `verify` checks the copy against itself:

| Check | Catches |
|---|---|
| index contiguity from the snapshot's `last_included_index + 1` | an entry truncated out from under the copy |
| every object's size and MD5 | destination corruption, or a copy rewritten after its checksum was taken |
| `current_term` ≥ every copied entry's term | term read early, node advanced and appended |

Contiguity is judged over the entries that actually **verified**, not over what
the manifest lists: an object named in the manifest but absent or corrupt is not
present, and it is a hole a restore would reproduce. So a deleted log object
reports twice — once as the object that broke, once as the gap it costs. That is
deliberate. The first says what happened; the second says what it means.

Every problem carries a machine-readable `kind` (`missing_object`,
`checksum_mismatch`, `size_mismatch`, `log_gap`, `term_regression`,
`unreadable_entry`, `manifest_version`) and a sentence naming the actual values,
because "this backup is bad" is not actionable and "log index 42 is missing
between the copied entries at 41 and 43" is.

`verify` reports **every** problem it finds. Restore aborts on the **first**.
That difference is intentional: `verify` is a diagnostic run by a human who
wants the whole picture, and restore is a gate.

---

## Runbook — clone restore

**Use it for:** replacing the storage or the instance underneath an otherwise
unchanged node identity. The node keeps its identity; only where its bytes live
changes.

**Do not use it for:** starting a new cluster from an old one's state. That is
seed restore, and the two are separate operations precisely so they cannot be
confused.

### The safety check that is not ours to make

**Starting the restored node while the original is still running is a
split-brain, and no restore tool can prevent it.** Two copies of one node
identity, both writing, is an operational fact about what you have started — not
something this code can detect. Restore only when the original is
**definitively gone**.

What the tool *can* do, and does, is make the failure loud where the engine is
fenced (`fencing_mode::compare_and_swap`). If the backup carried an `owner`
object, it is restored with a strictly **higher epoch**. A returning original
then finds its own epoch stale and fences itself out on its **next write**
rather than writing alongside the restored node. The returned report names the
epoch the restored prefix now claims.

Under `fencing_mode::none` there is no owner object, none is invented, and this
protection does not exist. That is a reason to run fenced, not a gap in restore.

### Sequence

1. `list(destination)` — find the backup id. Ids sort chronologically.
2. `verify(destination, backup_id)` — read the whole problem list yourself
   before committing to anything. Restore runs this again and aborts on the
   first problem, but it aborts; it does not explain.
3. Confirm the original node is stopped and will not return. If you cannot
   confirm it, stop here.
4. `restore_clone(destination, backup_id, target, {.force = …})`.
5. Read the returned `restore_report`: `objects_written`, `owner_epoch` if the
   backup was fenced, and `keys_deleted` if you passed `force`.
6. Start the node against the target prefix.

### Failure modes

| What you see | What it means | What to do |
|---|---|---|
| `refusing to restore backup … — <kind> on <key>` | `verify` found a problem; nothing was written | Run `verify` for the full list. A `log_gap` or `term_regression` means the backup is a smear — prefer an earlier one |
| `target prefix … already holds N object(s) — refusing to restore into it without force` | The target is not empty | Confirm you have the right target. If you do, re-run with `force`, which **deletes** the target's engine-owned keys first |
| `corrupt object …/owner: owner record has no epoch` | The backed-up owner object is malformed | The restored node could not fence a returning original. Investigate before starting anything |
| `… vanished from the backup during the restore — the target prefix … is now partially written` | The backup was mutated mid-restore | **Do not start the target.** Clear it and restore again from an intact backup |

`force` deletes the target's **engine-owned** keys — `term`, `voted_for`,
`snapshot`, `owner`, `log/*`, `snapshots/*` — and nothing else. Objects under
the prefix that the engine does not own are left untouched, because they are
yours and the engine neither reads nor writes them either.

`force` never merges. There is no code path that writes into a prefix still
holding another node's Raft state: the target is either emptied of engine-owned
keys or the restore throws. A merged log is two nodes' histories interleaved,
which is not a recoverable state and which no later repair untangles, so the
tool cannot produce one.

---

## Runbook — seed restore

**Use it for:** starting a **new** cluster from an old cluster's snapshot —
disaster recovery into fresh infrastructure, or forking an environment.

**Do not use it for:** replacing one node. That is clone restore.

### What is kept and what is discarded

| Kept | Discarded |
|---|---|
| the snapshot's state-machine bytes | the old cluster's configuration — **replaced** by your node set |
| `last_included_index` / `last_included_term` | the log, which is left empty |
| | the vote, which is cleared |
| | any joint-consensus marker |

The term is reset to the snapshot's `last_included_term`. The vote is cleared by
**omission** rather than by writing a `"none"` sentinel: an absent object is how
this engine spells "never voted", and writing the sentinel would make a fenced
engine's first vote an `If-Match` against an object it never wrote.

The joint-consensus marker is dropped deliberately. The old cluster may have
been mid-reconfiguration; a new one never is, and carrying the marker across
would seed every node with a membership change nobody proposed.

**The new cluster's node identities are unrelated to the old one's.** Nothing is
inherited but the state machine.

### Sequence

1. `list` and `verify`, exactly as for clone restore.
2. Decide the new node ids. They must match the representation this cluster's
   engine uses — see the failure table below.
3. `restore_seed(destination, backup_id, target, new_nodes, {.force = …})`.
   One prefix is written per node, at `<target.prefix>/<node_id>`.
4. Read the returned report's `nodes` list: it gives the node id and prefix for
   every node written. **This is what you must configure**; the tool writes the
   state, not the deployment.
5. Start the new cluster with exactly that node set and those prefixes.

Every target is checked before any is written, so a refusal on the third node
does not leave the first two seeded into a cluster that will never reach quorum.

### Failure modes

| What you see | What it means | What to do |
|---|---|---|
| `backup … has no snapshot, so there is nothing to seed a new cluster from` | The backup holds a log but no snapshot | Use a backup that has one. A log without a snapshot is the history of a cluster whose configuration is exactly what seed restore discards |
| `this cluster's node ids are numbers, but "…" is not one` | The engine's `NodeId` is an integer type and you supplied a name | Supply numeric ids. This is checked because the engine parses the configuration with `as_int64()` and **throws** on a string — the failure would otherwise surface when you start the new cluster, not here |
| `the snapshot in … records no cluster configuration, so the node-id representation this cluster uses cannot be determined` | The snapshot's node list is empty | The representation cannot be inferred and is not guessed. Seed from a backup whose snapshot has a configuration |
| `seed restore needs at least one node id` | Empty node set | Supply the new cluster's membership |
| `target prefix … already holds N object(s)` | One of the per-node prefixes is occupied | As for clone restore. Note nothing has been written yet — every target is checked first |

---

## What is not here yet

- `cmd/raft_object_backup` and its exact command lines (task 14).
- The durability contract and the per-provider evidence table (task 18).
- Operator configuration examples per provider (task 18).

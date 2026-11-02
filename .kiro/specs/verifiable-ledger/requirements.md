# Requirements Document

## Introduction

Kythira replicates state through Raft, but nothing it stores is
*tamper-evident*: an operator with disk access, or a bug, can change a value
and no client can tell. Amazon QLDB sold exactly that property (an append-only
journal chained with SHA-256, digests, and Merkle proofs that a revision is in
the journal) until AWS ended its support on 2025-07-31. AWS's migration path,
Aurora PostgreSQL, keeps no verifiable change record.

This spec adds a **native verifiable ledger** to Kythira: ledgers of Ion
documents whose every committed change is a block in a hash-chained journal,
with digests, proofs, full document history, serializable optimistic
transactions, and journal export. It follows the QLDB scoping doc
(2026-11-02), where Clark decided to build the native ledger first; the
QLDB-compatible wire API (PartiQL, `QLDBSession`, SigV4) is **out of scope**
and may never be built. Where it costs little, block and revision hashing
follow QLDB's published conventions so that a compatibility layer or an
importer stays possible.

The ledger is the second consumer of `.kiro/specs/persistent-state-machine/`
and `.kiro/specs/file-install-snapshot/` (PR #670): a ledger never deletes
anything, so it cannot be a RAM-resident, whole-buffer state machine.

**Scope**: the ledger state machine, journal and its storage, the Merkle
accumulator, digests and proofs, documents, history and equality indexes,
optimistic transactions, a native HTTP API, journal export to object storage,
a verifier tool, and the tests. **Out of scope**: PartiQL, the QLDB wire and
control-plane APIs, SigV4/IAM authorization, streaming to Kinesis, a ledger
spread over several Raft groups, and transactions across ledgers.

## Glossary

- **Ledger**: a named, independent database of tables and documents with its
  own journal. One ledger is served by exactly one Raft group.
- **Journal**: the ledger's append-only, totally ordered sequence of blocks.
  It is applied state, not the Raft log (which is compacted).
- **Block**: one committed transaction in the journal: its sequence number,
  transaction id, timestamp, statements (the native operations), the document
  revisions it wrote, `entriesHash`, `previousBlockHash` and `blockHash`.
- **Revision**: one version of one document: `data` (an Ion value, absent for
  a delete) and `metadata {id, version, txTime, txId}`.
- **Ion hash**: the Amazon Ion hash algorithm (amazon-ion-hash spec) with
  SHA-256, the canonical hash of an Ion value regardless of encoding.
- **dot(a, b)**: QLDB's order-free hash combiner: compare the two 32-byte
  hashes from the last byte to the first as signed bytes, concatenate the
  smaller first, and SHA-256 the result.
- **Digest**: the root of the Merkle accumulator over all block hashes up to
  a tip; 32 bytes, with its **tip address** (the last sequence number covered).
- **Proof**: the ordered list of hashes that folds with `dot` from a block or
  revision hash to a digest.
- **Read set**: what a transaction observed: document ids with versions,
  index keys looked up, and tables scanned, each tagged with the journal
  sequence it was read at.

## Requirements

### Requirement 1: Opt-in module, zero cost when off

**User Story:** As a maintainer, I want the ledger built only when asked for,
and kept separable from the core so it could move to its own repo later.

#### Acceptance Criteria

1. The ledger SHALL be built only with a new Kconfig option
   `KYTHIRA_LEDGER` (default off), which requires `KYTHIRA_ENGINE_ROCKSDB`
   or the in-memory engine from persistent-state-machine.
2. Its sources SHALL live under `services/ledger/` (headers, sources, tests)
   and `cmd/ledger_node/`, with their own CMake targets.
3. Ledger code SHALL include only Kythira's public concept and host headers,
   never a `*_impl.hpp`; a CI check SHALL enforce this.
4. With the option off, no ledger target SHALL be configured and no existing
   target, test or binary SHALL change.

### Requirement 2: One ledger, one group, one strand

**User Story:** As a client, I want one total order of transactions per
ledger.

#### Acceptance Criteria

1. Each ledger SHALL be served by its own Raft group under keys
   `l/<ledger id>/`, created through the multi-raft host and placed by the
   normal placement driver.
2. A ledger group SHALL veto every split and merge through the split/merge
   policy hook (multi-raft Requirement 11), and the veto SHALL be visible in
   the policy metrics.
3. A ledger catalog (names, ids, state, deletion protection) SHALL be kept
   in a ledger-catalog group, so ledger creation and deletion are themselves
   linearizable.
4. Deleting a ledger SHALL require deletion protection to be off and SHALL
   be recorded in the catalog; the ledger's journal SHALL be kept until an
   explicit purge (a separate, logged admin operation).

### Requirement 3: Journal and hash chain

**User Story:** As an auditor, I want every committed change to be in a
journal where changing any byte breaks the chain.

#### Acceptance Criteria

1. Each committed transaction SHALL append exactly one block with sequence
   number one above the previous block's; a transaction rejected by OCC
   (Requirement 7) SHALL append nothing.
2. `blockHash` SHALL be computed from the block's fields (sequence number,
   transaction id, timestamp, `entriesHash`, `previousBlockHash`) with Ion
   hash; `entriesHash` SHALL be the Merkle root (with `dot`) of the hashes of
   the block's statements and revisions. The exact field list SHALL be fixed
   in the design and versioned by a `hashVersion` field in the block.
3. The leader SHALL put the transaction id, the block timestamp and its
   computed `blockHash` in the Raft entry. Every replica SHALL recompute the
   hash during apply; a mismatch SHALL stop the group and report it, never
   apply.
4. The block timestamp SHALL be the leader's wall clock clamped so that it
   never runs backwards within a ledger.
5. A block SHALL be written in the same engine batch as its revisions and
   the applied index, so a crash can never separate them. Blocks older than
   a size bound (default 64 MiB) SHALL be sealed into immutable journal
   segment files, by a Raft entry, and only then removed from the engine.
6. Sealed segments MAY be offloaded to object storage (Requirement 10); a
   block SHALL stay readable whether its segment is local or offloaded.

### Requirement 4: Digests and proofs

**User Story:** As a client, I want to save a digest and later prove that a
block or revision is in the journal it covers.

#### Acceptance Criteria

1. The ledger SHALL maintain a Merkle accumulator over block hashes, combined
   with `dot`, whose frontier is O(log n) and whose interior nodes are stored
   in the ledger's engine range.
2. `GetDigest` SHALL return the digest and tip address for the latest
   committed block, read through the linearizable read path.
3. `GetBlock(seq, tip)` and `GetRevision(table, doc id, version, tip)` SHALL
   return the block or revision with a proof that folds with `dot` to the
   digest at `tip`; a `tip` below `seq`, or beyond the latest block, SHALL be
   rejected.
4. Proof generation SHALL cost O(log n) engine reads.
5. Optionally (config), each digest SHALL be signed by a key issued by the
   Kythira CA, so a third party can check that a digest came from this
   cluster.
6. Proofs produced by Kythira SHALL verify with the QLDB verification
   procedure as published by AWS (fold with `dot`, compare to the digest).

### Requirement 5: Documents and tables

**User Story:** As an application developer, I want tables of Ion documents
with stable ids and versions.

#### Acceptance Criteria

1. A ledger SHALL hold named tables (create, drop, undrop); dropping a table
   SHALL inactivate it and keep its history.
2. Each document SHALL have a system-assigned id (22-character base62,
   random) and a version starting at 0 and incremented by each revision.
3. Documents SHALL be Amazon Ion values, accepted as Ion binary, Ion text or
   JSON (read as Ion text), up to a size limit (default 128 KiB).
4. Writes SHALL be insert, replace, patch (set or remove paths) and delete,
   each optionally conditional on the current version.
5. A read SHALL return the current revision with its metadata, or the
   committed view (data, metadata and revision hash).

### Requirement 6: History and indexes

**User Story:** As an auditor, I want every revision of a document; as a
developer, I want lookups that are not full scans.

#### Acceptance Criteria

1. `history(table, [doc id], [start time, end time])` SHALL return every
   revision in commit order, deletes included, paginated.
2. A table SHALL support equality indexes on top-level or nested paths
   (default limit 20 per table). An index on a non-empty table SHALL build by
   background backfill and become active through a Raft entry; queries SHALL
   not use it before then.
3. Queries SHALL support equality filters on indexed paths and full scans
   with a filter; a full scan SHALL be bounded by a page size and a byte
   budget per call (persistent-state-machine Requirement 5.3).

### Requirement 7: Transactions

**User Story:** As an application developer, I want serializable multi-
document transactions without locks.

#### Acceptance Criteria

1. A transaction SHALL run on the ledger group's leader: begin records the
   applied journal sequence S, reads see the newest revision at or below S,
   and writes are buffered.
2. The transaction SHALL record a read set covering documents read (id and
   version), index keys looked up, and tables scanned.
3. Commit SHALL propose one Raft entry with the read set, write set and
   statements. Apply SHALL reject it with `occ_conflict` if any read document
   has a newer version, any looked-up index key or scanned table has a change
   after S, or a version condition fails; otherwise it SHALL write the
   revisions and append the block in one engine batch.
4. The result SHALL be serializable, phantoms included, for the queries
   Requirement 6.3 allows.
5. Transactions SHALL be bounded (defaults: 30 s, 40 changed documents,
   4 MiB of statements), and the bound SHALL be below the Raft entry limit.
6. A leader change SHALL fail open transactions with a retryable error; the
   client library SHALL retry `occ_conflict` and that error up to a limit.
7. Single-statement writes SHALL be one-call transactions.

### Requirement 8: Native API

**User Story:** As a client, I want a simple HTTP API with no SDK.

#### Acceptance Criteria

1. `ledger_node` SHALL serve an HTTP/JSON API (Ion accepted with
   `application/ion`) for ledger, table and index management, document reads
   and writes, transactions (begin, statements, commit, abort, by token),
   history, digest, block and revision proofs, and export.
2. Clients SHALL authenticate with Kythira CA client certificates (mTLS);
   per-ledger read/write/admin roles SHALL be mapped from the certificate
   subject in config.
3. Request limits SHALL follow `http-server-request-limits`.
4. A C++ client library SHALL wrap the API, including OCC retry.

### Requirement 9: Verification tool

**User Story:** As an auditor, I want to check a journal end to end without
trusting the cluster.

#### Acceptance Criteria

1. `ledger_ctl verify` SHALL re-hash every block of a ledger (from the API,
   a local data directory or an export) and check the chain, each
   `entriesHash`, and the digest against a saved digest.
2. `ledger_ctl prove` SHALL verify a block or revision proof against a saved
   digest offline.
3. A verification failure SHALL name the first bad block and exit non-zero.

### Requirement 10: Storage, snapshots and export

**User Story:** As an operator, I want a journal that grows for years
without filling a disk, and followers that can catch up.

#### Acceptance Criteria

1. Documents, revisions, indexes and Merkle nodes SHALL live in the
   persistent state machine's engine; a ledger's snapshot SHALL be an engine
   checkpoint plus the journal segment list, sent with file-install-snapshot.
2. Sealed segments SHALL be uploadable to any of Kythira's object stores,
   with their SHA-256 recorded in the ledger state, and fetched back on read.
3. A disk guard SHALL stop accepting transactions (with a clear error)
   before the data directory's free space falls below a floor.
4. `ExportJournal(start time, end time, format)` SHALL write the blocks of
   that range to an object-store prefix as Ion text, Ion binary or JSON, with
   a manifest, as a background job whose state is in the ledger.

### Requirement 11: Observability

#### Acceptance Criteria

1. Metrics SHALL report per ledger: blocks, commit rate, OCC conflict rate,
   journal bytes local and offloaded, digest latency, index backfill progress
   and hash-check failures.
2. A hash mismatch (Requirement 3.3) SHALL log the block, both hashes and the
   replica, and raise a metric that alerting can watch.

### Requirement 12: Testing

#### Acceptance Criteria

1. Ion hash SHALL pass the amazon-ion-hash test vectors.
2. `dot` and proof folding SHALL be tested against the example in the AWS
   QLDB verification tutorial.
3. Property tests SHALL commit random transactions, then check that every
   proof verifies, that tampering with any stored byte is detected by
   `ledger_ctl verify`, and that committed histories are serializable.
4. Crash tests SHALL kill replicas between engine batch and segment append,
   and during segment sealing and offload, and SHALL assert identical
   journals on all replicas after recovery.
5. A docker-chaos scenario SHALL run a ledger workload under kills and
   partitions and then verify the journal, under Docker and rootless Podman,
   with service-name addressing.

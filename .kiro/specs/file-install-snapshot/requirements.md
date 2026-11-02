# Requirements Document

## Introduction

`InstallSnapshot` is chunked on the wire but not in memory. The leader builds
one `std::vector<std::byte>` from `get_state()` and slices it
(`raft.hpp` `send_install_snapshot_chunk`); the follower appends every chunk
to `_snapshot_buffer` and hands the whole buffer to
`restore_from_snapshot` when `done` arrives (`handle_install_snapshot`). A
1 GB state therefore costs at least 1 GB of extra memory on each end for the
whole transfer, and a follower that restarts mid-transfer starts again from
byte 0.

`.kiro/specs/persistent-state-machine/` adds state machines whose state is an
engine checkpoint: a directory of immutable files described by a
`snapshot_manifest`. This spec carries such a checkpoint from leader to
follower **as files**, streamed from disk on the leader and to disk on the
follower, verified, and ingested. Until it lands, persistent groups keep their
whole log and refuse to send snapshots (that spec's Requirement 10.2).

**Scope**: the wire fields, every serializer and transport that carries
`InstallSnapshot`, the follower's staging and ingest, the leader's streaming
sender and checkpoint lifetime, resume, pacing, and the tests. **Out of
scope**: changing the byte-buffer path for in-memory state machines (it keeps
working unchanged), object-store upload of checkpoints, and migrating a group
from an in-memory to a persistent state machine.

## Glossary

- **Format 0**: today's transfer, one opaque byte buffer.
- **Format 1**: a file-set transfer described by a manifest.
- **Manifest**: the ordered list of files in a checkpoint (relative name,
  size, SHA-256), the engine format tag, the group range, and a digest of the
  list itself.
- **Stream offset**: a byte position in the concatenation of the manifest's
  files in manifest order. Format 1 reuses the existing `offset` field with
  this meaning, so the chunking rules stay the same.
- **Staging directory**: where a follower writes an incoming format-1
  snapshot before it is complete and verified.

## Requirements

### Requirement 1: Wire format

**User Story:** As a transport author, I want one small, additive change to
the message, not a new RPC.

#### Acceptance Criteria

1. `install_snapshot_request` SHALL gain `format` (default 0) and `manifest`
   (bytes, present only on the first chunk of a format-1 transfer).
2. For format 1, `offset` SHALL be the stream offset and `data` a chunk of
   the stream; a chunk MAY span a file boundary.
3. `install_snapshot_response` SHALL gain an optional `bytes_received` (the
   follower's contiguous verified-or-written prefix for this transfer) and an
   optional `reject_reason` code.
4. A message with the new fields absent SHALL decode exactly as today, in
   every serializer (JSON, CBOR, protobuf, Ion) and in `proto/raft.proto` and
   `proto/raft_messages.proto` (new field numbers only).
5. Existing serializer bounds checks (PR #500 style) SHALL cover the new
   fields, including a manifest size cap.

### Requirement 2: Every transport carries it

**User Story:** As an operator, I want file snapshots on whichever transport
my cluster uses.

#### Acceptance Criteria

1. tcp_rpc (plain and TLS), HTTP (httplib, Beast, Proxygen), CoAP (libcoap,
   cantcoap, libnyoci), gRPC, the group transport and the network simulator
   SHALL carry the new fields.
2. Each transport's existing InstallSnapshot test SHALL gain a multi-chunk
   format-1 case with a chunk spanning a file boundary.
3. Message size limits (`http-server-request-limits`, CoAP block limits) SHALL
   admit a full chunk plus the manifest on the first chunk.

### Requirement 3: Follower staging and verification

**User Story:** As a follower, I want to receive a snapshot larger than my RAM
and never install a corrupt one.

#### Acceptance Criteria

1. On a format-1 first chunk, the follower SHALL validate the manifest
   (names relative, no `..`, size cap, digest), check free disk space against
   the total size plus a margin, and create a staging directory under the
   group's data directory. A failed check SHALL reject with a reason code and
   a metric.
2. Each chunk SHALL be written to the right file(s) at the right offsets; the
   follower SHALL NOT hold more than one chunk in memory per transfer.
3. Each file SHALL be fsynced when complete and its SHA-256 checked against
   the manifest; the staging directory SHALL be fsynced before ingest.
4. A new transfer for a higher (index, term), or a higher term from a new
   leader, SHALL abandon and delete an older staging directory.
5. Staging directories left by a crash SHALL be deleted at group start.
6. All existing checks (stale term, stale index at or below commit index,
   offset mismatch, missing first chunk) SHALL apply unchanged.

### Requirement 4: Install

**User Story:** As a follower, I want installing a verified snapshot to be one
atomic step.

#### Acceptance Criteria

1. On `done`, after verification, the node SHALL call the state machine's
   `ingest(dir, manifest, last_included_index)` under the node mutex, then
   save metadata-only snapshot state, then apply today's log-retention rule
   (keep entries after the snapshot if the term matches, else discard).
2. A crash between ingest and the metadata save SHALL recover correctly,
   because the state machine's applied index (persistent-state-machine
   Requirement 2) is the source of truth at restart.
3. A format-1 request to a group whose state machine is not persistent, or a
   format-0 request to one that is, SHALL be rejected with a reason code.

### Requirement 5: Leader streaming and checkpoint lifetime

**User Story:** As a leader, I want to send a 100 GB snapshot without loading
it, and without filling my disk with stale checkpoints.

#### Acceptance Criteria

1. The leader SHALL take a checkpoint at `applied_index()` and read chunks
   from its files with positioned reads; it SHALL NOT hold more than one chunk
   per transfer in memory.
2. Concurrent transfers to several followers at the same index SHALL share
   one checkpoint, reference-counted.
3. A checkpoint SHALL be deleted when its last transfer ends or when it is
   older than a configured age and unused. A metric SHALL report checkpoint
   count and bytes per node.
4. Losing leadership SHALL stop its transfers and release their checkpoints.

### Requirement 6: Resume and pacing

**User Story:** As an operator, I want a transfer interrupted by a timeout to
continue, and snapshots not to starve replication.

#### Acceptance Criteria

1. After a failed or timed-out chunk, the leader SHALL retry the same
   transfer from the follower's `bytes_received` when it reports one for the
   same (index, term, manifest digest), instead of from 0.
2. A follower SHALL report `bytes_received` only for a staging directory
   matching that triple.
3. Snapshot bytes SHALL be paced by a per-node token bucket (configurable
   bytes/s, default unlimited) shared by all groups, so AppendEntries and
   heartbeats keep their latency during a large transfer.

### Requirement 7: Compaction for persistent groups

**User Story:** As an operator, I want persistent groups to compact their logs
once followers can catch up.

#### Acceptance Criteria

1. With this spec in, persistent-state-machine Requirement 10.2 SHALL be
   lifted: persistent groups compact under that spec's Requirement 3 and send
   format-1 snapshots to followers that fall behind the log.
2. An integration test SHALL partition a follower, write past compaction on a
   group of at least 1 GB, heal, and assert the follower catches up through a
   format-1 snapshot with peak RSS growth under a bound (default 64 MB) on
   both leader and follower.

### Requirement 8: Faults

**User Story:** As a reviewer, I want the transfer proved under the faults
that will happen.

#### Acceptance Criteria

1. Failpoints SHALL corrupt a chunk, fail a file fsync, and kill the follower
   after ingest but before the metadata save; tests SHALL assert rejection or
   correct recovery in each case.
2. Tests SHALL cover a leader change mid-transfer and a follower restart
   mid-transfer (resume when the same leader continues; restart from 0
   otherwise).
3. A docker-chaos scenario SHALL kill and partition a lagging follower during
   a large transfer, under Docker and rootless Podman.

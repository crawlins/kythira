# Design Document

## Overview

Format 1 keeps InstallSnapshot's existing contract (one RPC per chunk,
`offset`, `done`, the same term and index checks) and changes what the bytes
are and where they live:

```
leader                                                  follower
  checkpoint(dir) at applied_index ─► manifest           
  chunk k = pread(stream offset k*C, C)                  
  request{format=1, manifest (k==0), offset, data, done}──►
                                                          k==0: validate manifest, check disk,
                                                                mkdir staging/<index>-<term>-<digest>
                                                          write data at stream offset → file(s)
                                                          file complete → fsync, SHA-256 check
                                                       ◄── response{term, bytes_received}
  ...                                                    done → fsync dir → sm.ingest(dir, manifest, index)
                                                                → save metadata snapshot → log rule
```

## Key design decisions

### Reuse `offset` as a stream offset

A per-file `(file_index, file_offset)` address would be more explicit, but it
changes the meaning of the existing chunk checks and adds two fields to every
serializer and transport. A single stream offset over the files in manifest
order keeps `buffer.size() != request.offset()` as the receiver's contiguity
check and keeps the sender's loop unchanged. The manifest maps a stream offset
to `(file, offset)`; that mapping is a few lines on each end.

### Manifest on the first chunk only

The manifest is small (a few hundred files at most for a RocksDB checkpoint)
and is needed before the first byte is written, so it rides on offset 0. A
resume (Requirement 6) does not resend it; the follower already has it in the
staging directory as `MANIFEST.kythira`.

### Rejecting the format mismatch instead of negotiating

All replicas of a group run the same `state_machine_type` (that is already
required: they apply the same log). A persistent state machine type does not
exist in older binaries, so a mixed-version group of persistent replicas
cannot form. The only mismatch possible is a bug or a misconfiguration, which
Requirement 4.3 turns into a clear rejection instead of a negotiation
protocol nobody would exercise.

### Pacing at the node, not the group

Snapshot bytes compete with every group's AppendEntries on the same links. A
per-group limit multiplies by the group count; one node-wide token bucket is
the knob an operator can reason about. The existing chunk fetcher's token
bucket (`cache_chunk_fetcher.hpp`) is the model.

## Components

| Component | File | Notes |
|---|---|---|
| Wire fields | `include/raft/types.hpp` | `_format`, `_manifest`, response `_bytes_received`, `_reject_reason`; accessors with defaults |
| Serializers | `json_serializer.hpp`, `cbor_serializer.hpp`, `protobuf_serializer.hpp`, `ion_serializer.hpp`, `grpc_message_conversion.hpp`, `proto/*.proto` | Optional fields, absent means format 0 |
| `snapshot_stream_reader` | `include/raft/snapshot_stream.hpp` | Leader: stream offset → pread across files |
| `snapshot_stream_writer` | same | Follower: staging dir, write at stream offset, per-file fsync and SHA-256 |
| `checkpoint_registry` | `include/raft/checkpoint_registry.hpp` | Refcounted checkpoints per (group, index), age-based GC, metrics |
| `snapshot_pacer` | `include/raft/snapshot_pacer.hpp` | Node-wide token bucket |
| Node integration | `include/raft/raft.hpp` | Format-1 branches in `send_install_snapshot_to`, `send_install_snapshot_chunk`, `handle_install_snapshot`, `install_snapshot`; staging cleanup at start |

## Reject reasons

`format_mismatch`, `manifest_invalid`, `disk_space`, `checksum`,
`io_error`, plus today's `stale_term`, `stale_snapshot`, `offset_mismatch`,
`missing_first_chunk` (which today exist only as metric dimensions and become
response codes too). The leader logs the reason and backs off; `disk_space`
and `checksum` back off longer.

## Testing strategy

- Serializer round-trip and absent-field tests for each serializer.
- Per-transport multi-chunk format-1 test.
- Stream reader/writer unit tests, including chunks that span files and
  zero-length files.
- Node tests on `memory_kv_engine` (fast) and RocksDB (CI leg).
- The RSS-bounded catch-up integration test (Requirement 7.2).
- Failpoint and docker-chaos tests (Requirement 8).

## Open questions

1. Should format 0 also spill to a staging file on the follower, removing the
   in-memory buffer for large in-memory state machines too? Cheap once the
   writer exists; not required here.
2. Default pacing: unlimited, or a fraction of a measured link rate?

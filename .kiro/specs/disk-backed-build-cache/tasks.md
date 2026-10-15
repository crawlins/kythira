# Implementation Plan — Disk-Backed Build Cache

## Status: 0/12 tasks complete

**Last Updated**: October 15, 2026 (spec written, then amended the same day
for inode limits: a free-inode floor, small-value pack files, voter
free-space reports and an inode chaos case; nothing implemented).

## Overview

Move the Redis gateway's values from RAM to a per-node, content-addressed
chunk store. Make Raft state durable, make snapshots carry only the index, and
add an HTTP front end that sccache (WebDAV) and vcpkg (`http` binary source)
can both use. The goal is a self-hosted cache cluster of minimal containers,
each with local disk, beside self-hosted CI runners.

Reference material to read before starting:
- `.kiro/specs/disk-backed-build-cache/design.md`
- `include/raft/redis_kv_state_machine.hpp` and `redis_kv_commands.hpp`, the
  store and codec this extends
- `include/raft/redis_gateway.hpp` / `_impl.hpp`: the read path that already
  streams a value outside the node lock, the eviction and sweep policy, and
  the forwarding link
- `include/raft/persistence.hpp`: the persistence concept the binary engine
  satisfies
- `doc/redis-gateway.md`: the ACL format, roles and existing variables
- `.kiro/specs/oci-build-cache/tasks.md` task 11 and
  `doc/sccache_dogfood_cost_estimate.md`: the sizing this is built for

## Task Dependency Graph

```json
{
  "waves": [
    { "wave": 1, "tasks": [1], "description": "Measure both clients and the real object sizes; gates the WebDAV subset" },
    { "wave": 2, "tasks": [2, 4], "description": "Chunk store and the binary persistence engine, independent of each other" },
    { "wave": 3, "tasks": [3], "description": "cache_kv_state_machine and the v2 codec on top of the chunk store" },
    { "wave": 4, "tasks": [5, 6], "description": "Chunk fetcher; HTTP front end and auth cache" },
    { "wave": 5, "tasks": [7], "description": "Daemon wiring, configuration, eviction against the disk budget" },
    { "wave": 6, "tasks": [8, 9, 10], "description": "Containers, end-to-end client test, chaos scenarios" },
    { "wave": 7, "tasks": [11, 12], "description": "Docs, then close-out" }
  ]
}
```

## Tasks

- [ ] 1. Measure the clients, the object sizes and the working set
  - Per design.md Component 0. A recording stub stands in for the server.
    Capture sccache 0.17.0's WebDAV requests for `--start-server`, a miss and
    a hit, and vcpkg's `http` source requests for a restore, a miss, a 500 and
    a refused connection, at CI's pinned vcpkg commit.
  - From the OCI bucket listing (read-only key, maintainer machine or a
    workflow, since the cloud sandbox has no OCI credentials), record the
    largest object, p99, and the share of sccache objects over 8 MiB per
    prefix.
  - Record one `main` run's working set (bytes read, both caches).
  - Update design.md's WebDAV rows if the capture differs (Requirement 1.5).
  - _Requirements: 1.1–1.6_

- [ ] 2. `cache_chunk_store` and `chunk_refcounts`
  - `include/raft/cache_chunk_store.hpp` per design.md Components 1 and 1a:
    the two-level layout, temp-write-fsync-rename, idempotent `put`, streaming
    `verify`, a `tmp/` wipe on open, and pack files for chunks under the pack
    threshold (append, `.dead` sidecar, start-up scan, compaction below half
    live).
  - A `statvfs` probe that returns free bytes and free inodes together.
  - Unit tests: concurrent `put` of the same SHA, crash simulation
    (libfiu between write and rename leaves no named chunk), a corrupted file
    failing `verify`, a torn pack tail truncated on open, a crash
    mid-compaction leaving no lost or resurrected chunk, and 100,000 small
    `put`s creating a bounded number of files.
  - _Requirements: 2.1, 2.5, 3.3, 6.7, 6.8, 10.1_

- [ ] 3. `cache_kv_state_machine` and codec version 2
  - Add `put_chunk`, `commit` and `touch` to `redis_kv_commands.hpp` under
    format `0x02`, keeping `0x01` decodable.
  - `include/raft/cache_kv_state_machine.hpp` per Component 3: index,
    pending table with the orphan grace counted in log entries, refcounts,
    manifest-only `get_state` / `split_state` / `absorb` /
    `restore_from_snapshot`, and the `has_all` bit.
  - Make `redis_gateway` a template on its state machine and instantiate the
    existing tests over both machines (Requirement 2.7).
  - Property tests for design.md properties 1–4, and a memory test with
    100,000 keys for Requirement 2.2.
  - _Requirements: 2.1–2.4, 2.6, 2.7, 3.1–3.7, 4.1, 4.2, 4.4_

- [ ] 4. `binary_file_persistence_engine`
  - `include/raft/binary_file_persistence.hpp` per Component 4: an atomically
    rewritten `meta`, CRC'd log segments, prefix truncation by segment, torn
    tail recovery, and the snapshot file.
  - It must satisfy the persistence concept. Port the cases in
    `tests/file_persistence_unit_test.cpp` to it, and add a kill-mid-append
    test.
  - _Requirements: 5.1, 5.3_

- [ ] 5. Chunk fetcher
  - `include/raft/cache_chunk_fetcher.hpp` per Component 5: a queue, a
    leader-first peer order, verify then `put`, the token bucket, and
    `has_all` refresh.
  - A test with two in-process nodes: restore a manifest on an empty store,
    fetch completes, reads move from forwarded to local.
  - _Requirements: 4.3, 4.5, 4.6, 10.1_

- [ ] 6. HTTP front end and auth cache
  - `include/raft/cache_http_front_end.hpp` per Components 6 and 7: the method
    table, streaming `PUT` with two chunks in flight, `Range` `GET`, the
    abort on mid-body hash mismatch, forwarding of writes, `/_chunk/` and
    `/_metrics`.
  - Request limits from `http-server-request-limits`. Plaintext refused
    without the opt-in.
  - The auth cache: HMAC-keyed, bounded, flushed on reload.
  - Tests: a 1 GiB `PUT` with peak RSS under 64 MiB above baseline; a
    disconnect mid-`PUT` leaves the old value; a `read_only` `PUT` gives 403
    and no log entry; a 413 above the maximum.
  - _Requirements: 3.4, 7.1–7.8, 8.1–8.5, 10.2_

- [ ] 7. Daemon wiring, configuration and the disk budget
  - `cmd/redis_gateway_node`: `KYTHIRA_CACHE_*` parsing in `config.cpp`,
    selection of `memory` versus `file` in `run_host.hpp`, start-up orphan
    collection, eviction against `KYTHIRA_CACHE_MAX_DISK_BYTES`, the
    free-space and free-inode floors, follower `touch` batches carrying each
    node's free bytes and inodes, the leader refusing writes while any voter
    is below a floor, and the `INFO` fields.
  - Existing `redis_gateway_node` tests must pass with
    `KYTHIRA_CACHE_PERSISTENCE=memory`.
  - _Requirements: 2.6, 5.2, 5.4, 6.1–6.6_

- [ ] 8. Container image and compose file
  - `docker/cache-cluster/` per design.md "Containers": non-root, a `/data`
    volume, three services addressed by name, no static IPs.
  - It must come up under Docker and under rootless Podman
    (`kythira-rootless-podman-sandbox-recipe` in project memory has the
    sandbox steps).
  - _Requirements: 11.1–11.3_

- [ ] 9. End-to-end test with the real clients
  - A container test: build a small CMake project twice through sccache
    WebDAV (misses, then hits), and install one small vcpkg port twice
    through the `http` source (build, then restore). Repeat with every node
    stopped and show the builds still pass.
  - Record the throughput of a 500 MB `PUT` and `GET` (design.md Risks).
  - _Requirements: 9.1, 9.2, 12.1, 12.2_

- [ ] 10. Chaos scenarios
  - Kill the leader during a 100 MB `PUT`. Stop and restart all three nodes.
    Replace a node with an empty volume. Flip a byte in a stored chunk
    (libfiu). Fill a small-inode volume with small RESP values: a
    loop-mounted filesystem needs privileges the container rules forbid, so
    use a tmpfs mount with `nr_inodes` (`--tmpfs /data:size=…,nr_inodes=…`).
    Confirm that Docker and rootless Podman both honour the option. If one
    does not, drive the floor with a libfiu hook on the `statvfs` probe for
    that runtime instead. Each scenario asserts what Requirements 10.3 and
    12.3–12.6 state.
  - _Requirements: 10.3, 12.3–12.6_

- [ ] 11. Documentation
  - `doc/redis-gateway.md`: the variables, HTTP API, WebDAV subset, disk
    layout, and client settings for sccache and vcpkg with read-only and
    read-write users.
  - `doc/sccache_dogfood_cost_estimate.md`: the home-hosting sizing with disk
    instead of RAM.
  - _Requirements: 13.1–13.3_

- [ ] 12. Close-out
  - Update `doc/TODO.md`'s row and `doc/CHANGELOG.md`. Record what task 1 and
    task 9 measured against what this spec assumed.
  - _Requirements: all_

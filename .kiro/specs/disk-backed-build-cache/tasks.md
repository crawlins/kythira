# Implementation Plan — Disk-Backed Build Cache

## Status: 12/13 tasks complete

**Last Updated**: November 2, 2026 (task 2: the chunk store, pack files and
node-wide reference counts; task 3: the cache state machine, codec version 2
and the gateway over either machine; task 4: the binary persistence engine;
task 5: the chunk fetcher; task 6: the HTTP front end and auth cache;
task 7: the daemon's disk mode, the disk budget and the voter floors;
task 8: the container image and compose file. November 2: task 9, the
real-client end-to-end test, and the three faults it found; task 10, the
chaos scenarios, and the corrupt-chunk repair they found broken.
Task 1 was measured on October 27. The spec was
written on October 15 and amended the same day for inode limits and the
full-tree WebDAV measurement).

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
    { "wave": 7, "tasks": [11], "description": "Full-tree sccache WebDAV measurement against the OCI baseline" },
    { "wave": 8, "tasks": [12, 13], "description": "Docs, then close-out" }
  ]
}
```

## Tasks

- [x] 1. Measure the clients, the object sizes and the working set
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

### Task 1 results (measured 2026-10-27 on Clark's workstation)

**The stub.** A Python `ThreadingHTTPServer` (HTTP/1.1) that logs method,
path, selected headers, request-body size and status, and answers from a dict:

- GET/HEAD: 200 or 404.
- PUT: 201.
- DELETE: 204 or 404.
- MKCOL: 201, and records the directory.
- PROPFIND: 207 for a stored key or a known directory, 404 otherwise.
- Any request: a forced 404 or 500 when switched to that mode with
  `GET /_mode/<normal|404|500>`.

It is not committed; the behaviour the gateway must copy is below.

#### sccache 0.17.0 WebDAV (Requirement 1.1)

The binary was the release tarball CI installs:
`sccache-v0.17.0-x86_64-unknown-linux-musl.tar.gz`, SHA-256 `67c4a96d…0006`,
the same checksum `.github/actions/oci-build-cache/action.yml` pins.

```bash
SCCACHE_WEBDAV_ENDPOINT=http://127.0.0.1:18080/cache SCCACHE_SERVER_PORT=14226 \
SCCACHE_DIR=$W/local sccache --start-server
sccache cc -c a.c -o a.o            # miss
rm a.o; sccache cc -c a.c -o a.o    # hit
```

What it sent:

| Phase | Requests (in order) |
|---|---|
| `--start-server` | `GET /cache/.sccache_check` → 404; `PROPFIND /cache/` `Depth: 0` → 207; `PUT /cache/.sccache_check` (13 bytes) → 201 |
| miss | `GET /cache/8/5/6/8561f8…ce90` → 404; `PROPFIND /cache/8/5/6/` `Depth: 0` → 207; `PUT /cache/8/5/6/8561f8…ce90` (539 bytes, `Content-Length`) → 201 |
| miss, parent directories absent | `GET` → 404; `PROPFIND /cache/8/5/6/` → 404; `PROPFIND /cache/8/5/` → 404; `PROPFIND /cache/8/` → 404; `MKCOL /cache/8/`, `MKCOL /cache/8/5/`, `MKCOL /cache/8/5/6/` → 201 each; `PUT` → 201 |
| hit | `GET /cache/8/5/6/8561f8…ce90` → 200. Nothing else. |

Facts the gateway must honour:

- **Methods used:** GET, PUT, PROPFIND and MKCOL only. Never HEAD, DELETE,
  MOVE, COPY or `Range`.
- **Keys:** `<prefix>/<k0>/<k1>/<k2>/<64-hex>`.
- **PUTs** always carry `Content-Length`; there is no chunked transfer
  encoding. No `Expect`, no conditional headers. The User-Agent is
  `sccache/0.17.0`.
- **PROPFIND targets the parent collection, never the key.** It is
  `Depth: 0`, and its 91-byte body is
  `<D:propfind xmlns:D="DAV:"><D:allprop/></D:propfind>`.
  - A 404 makes OpenDAL walk upward with further PROPFINDs, then MKCOL each
    missing level top-down.
  - A 207 for every collection path means no MKCOL is ever sent.
- **OpenDAL parses the PROPFIND reply strictly.** A minimal multistatus
  (`href`, `resourcetype`, `status` only) failed with
  `write failed Unexpected (permanent) at write => deserialize xml`, and every
  write failed with it. A mod_dav-shaped reply worked: per `D:response`,
  `displayname`, `getlastmodified` (RFC 1123), `resourcetype`, and for a file
  `getcontentlength`, `getcontenttype` and `getetag`.
- **Read-only mode exists:** `SCCACHE_WEBDAV_RW_MODE=READ_ONLY`.
  - `--start-server` sends only `GET /cache/.sccache_check`; a 404 there is
    accepted.
  - Misses send no PROPFIND, MKCOL or PUT, and are counted as "Cache write
    errors". So a read-only client never needs `read_write`.
- **Failures:**

  | When | Server response | sccache behaviour |
  |---|---|---|
  | startup check (`.sccache_check` GET) | 500 | `--start-server` exits 2 ("Server startup failed: cache storage failed to read"), and every compile exits 2 |
  | startup check | connection refused | the same, "send http request" |
  | mid-build, after a healthy start | 500 on GET/PROPFIND | compile succeeds locally (rc 0), counted as a miss plus a write error; one request each, no retry |
  | mid-build | connection refused | the same, about 0.16 s per compile |

  The startup check is the gateway's real availability contract:
  `start-sccache.sh`'s probe exists for exactly this case.

#### vcpkg `http` binary source (Requirement 1.2)

Measured with a scratch clone of `microsoft/vcpkg` at `9a7f7340` (vcpkg.json's
builtin-baseline, which CI pins), bootstrapped to tool
`2025-10-16-71538f26`. Port: `zlib` (plus host `vcpkg-cmake`), classic mode,
`x64-linux`.

```bash
VCPKG_BINARY_SOURCES='clear;http,http://127.0.0.1:18081/{name}/{version}/{sha},readwrite' \
  ./vcpkg install zlib
```

| Case | Requests | Outcome |
|---|---|---|
| fresh | `GET /vcpkg-cmake/2024-04-23/4f9a5f…1b0c` → 404, `GET /zlib/1.3.1/d0fef2…525e` → 404, then after building `PUT` both (10,267 and 226,219 bytes, `Content-Length`) → 201 | built, "Completed submission … to 1 binary cache(s)" |
| restore | `GET /zlib/1.3.1/d0fef2…525e` → 200 | "Restored 1 package(s) from HTTP servers in 94.5 ms" |
| 404 everywhere | `GET` → 404, `PUT` → 404 | built from source, warning "curl failed to put file … http code 404", rc 0 |
| 500 everywhere | `GET` ×4 → 500 (retried, 7.1 s total), `PUT` ×1 → 500 | built from source, warning, rc 0 |
| refused | no request reaches the server | warning "curl operation failed with error code 7", built, PUT warning, rc 0 |

- `{name}`/`{version}`/`{sha}` expand to the port name, the port version
  (`1.3.1`, `2024-04-23`) and the 64-hex ABI hash. There is no triplet and no
  `.zip`.
- **No `HEAD` is ever sent.** A `--dry-run` install sends no request at all.
- PUTs are not retried. No failure ever reddens the install.
- User-Agent is `vcpkg/2025-10-16-71538f2694db93da4668782d094768ba74c45991 (curl)`.

CI today uses `x-aws` with keys `vcpkg/<triplet>/<abi>.zip`, not this
template. The http source's keys therefore will not match the existing objects
without a re-seed.

#### OCI bucket sizes (Requirement 1.3)

`kythira-build-cache`, us-phoenix-1, listed 2026-10-27T14:41Z with
`oci os object list --all --fields name,size,timeCreated`. This was list-only,
through the local `DEFAULT` CLI profile; the `KYTHIRA_CI` profile cannot see
the bucket. Percentiles are nearest-rank.

| Prefix | Objects | Total | Largest | p50 | p99 | Over 8 MiB (count / bytes) |
|---|---|---|---|---|---|---|
| `sccache/` | 109,166 | 108.39 GiB | 16.6 MiB (17,417,848 B) | 487 KiB | 10.14 MiB (10,636,940 B) | **2.46 %** / 25.65 % |
| `vcpkg/` | 1,128 | 21.93 GiB | 955.3 MiB (1,001,753,768 B) | 297 KiB | 480.6 MiB (503,902,591 B) | 14.1 % / 96.4 % |
| `sccache-measure-3/` (leftover from oci-build-cache task measurements, 2026-09-07) | 4,421 | 2.04 GiB | 19.3 MiB | 155 KiB | 4.09 MiB | 0.66 % / 16.4 % |

- **sccache:** mean 1.02 MiB, p99.9 13.5 MiB, nothing over 64 MiB.
- **vcpkg:** 81 objects over 64 MiB. The five largest are all ~955 MiB
  `x64-linux` zips.
- **Implications.** The value-on-disk design must stream vcpkg objects of
  nearly 1 GiB. The 8 MiB chunk boundary splits about one sccache object in
  forty, but those objects hold a quarter of sccache's bytes.

#### Working set of one `main` run (Requirement 1.4)

CI run 40423882255 (`ci.yml`, push to `main` at `e9f8b8e2`, 28 jobs, all
green). Each job's log was fetched with
`gh api repos/crawlins/kythira/actions/jobs/<id>/logs`, because
`gh run view --log` does not work on this machine.

- **sccache:** the jobs' final `--show-stats` sum to **6,200 hits** and
  17 misses. The heavy jobs are the five Build & Test legs, Full suite
  (boost), Coverage, Config Variants and No Folly, each with 496-713 hits.
  The stats carry no keys, so bytes are approximated as hits × the prefix's
  mean object size: **≈ 6.2 GiB** (≈ 2.9 GiB at the median size). That
  counts hits per job, so an object read by several jobs counts once per
  job, which is what the server serves.
- **vcpkg:** the `vcpkg_installed/` actions/cache tree hit in 18 jobs. In
  those jobs the binary cache is never read.
  - Four jobs missed it and restored from OCI:

    | Job | Packages restored | Restore time |
    |---|---|---|
    | Build & Test (g++-14, x64) | 131 | 5.7 min |
    | Build & Test (clang++-18, arm64) | 131 | 5.2 min |
    | Build & Test (g++-13, arm64) | 131 | 4.3 min |
    | GCP SDK Build (clang++-18, x64) | 133 | 5.7 min |

  - Every restored package's ABI hash appears in the job log. All 526 were
    matched exactly to `vcpkg/<triplet>/<abi>.zip` objects in the listing:
    **4.44 GiB** in total, at 0.68-1.34 GiB per job, the largest object
    read being 483.9 MiB.
- **One run's read working set ≈ 10.6 GiB** (6.2 sccache + 4.4 vcpkg). That
  is ≈ 8 % of the bucket's 132 GiB. The vcpkg share depends entirely on how
  many tree-cache misses a run has. It would be ≈ 0 on a fully warm run, and
  about 1.1 GiB more for each additional leg that misses.

- [x] 2. `cache_chunk_store` and `chunk_refcounts`
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

### Task 2 results (2026-11-01)

`include/raft/cache_chunk_store.hpp` (with `include/raft/crc32.hpp`) and
`tests/cache_chunk_store_unit_test.cpp`, 20 cases, all passing locally
(16 without libfiu). Choices the design left open:

- **`link` instead of `rename`** for a loose chunk. `link` fails with
  `EEXIST` when the name exists, so exactly one of several concurrent writers
  of a chunk is told it created it, and the file and byte counters stay
  exact. The temporary name is unlinked either way.
- **`put` checks the bytes against the name.** One SHA-256 next to an fsync
  is cheap, and it keeps "a name is its content's hash" unconditional.
- **The `.dead` sidecar records offsets, not hashes.** A chunk removed and
  stored again can have a dead and a live record in the same pack.
- **Duplicates found by the open scan are written to the sidecar.** A crash
  mid-compaction leaves each live chunk in two packs. The lower-numbered copy
  wins, and the other is marked dead on disk, so a later `remove` of that
  chunk cannot be undone by the copy surviving a restart.
- **A bad CRC ends the pack wherever it is**, not only at the tail. Chunks
  after it become absent, which the state machine already treats as "fetch
  from a peer" (Requirement 2.6). A fatal error would turn one bad sector
  into a node that cannot start.
- **`chunk_refcounts` serialises create and delete** under one mutex
  (`put_and_acquire`, `release`), so one shard cannot remove a chunk between
  another shard's write of the same bytes and its reference. That holds the
  mutex across a large chunk's fsync; chunk writes on one disk contend
  anyway.
- **Compaction is a call, `compact()`**, for task 7's policy tick to make. It
  holds the store mutex while it copies one pack (64 MiB at most).
- **Fault points** (`raft/cache/chunk_store/write`, `.../before_rename`,
  `.../compact_before_unlink`, `raft/cache/statvfs_low_bytes`,
  `raft/cache/statvfs_low_inodes`) are there for task 10's chaos scenarios as
  well as the unit tests.
- 100,000 64-byte `put`s make one pack file (four files in the data
  directory in all), each fsynced, in about 20 s on the sandbox's disk.

- [x] 3. `cache_kv_state_machine` and codec version 2
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

### Task 3 results (2026-11-01)

`include/raft/cache_kv_state_machine.hpp`, the format 0x02 records in
`redis_kv_commands.hpp`, and `tests/cache_kv_state_machine_unit_test.cpp`.
That test has 14 cases, including properties 1-4 as seeded random trials
(20, 10, 20 and 30 trials). `redis_kv_commands_unit_test` gains 5 cases for
the new records. Departures from the design, and choices it left open:

- **`put_chunk` carries the uploading key.** design.md left it empty, but
  `multi_raft` routes every proposal through `redis_kv_partitioner`, and
  cross-shard admission refuses an empty routing key. The key is routing
  only; `apply` ignores it.
- **Each opcode has exactly one version.** 0x01-0x04 stay under 0x01, so the
  in-memory gateway's entries are byte-identical to today's. 0x05-0x07 are
  valid only under 0x02. `redis_kv_state_machine` treats the new opcodes as
  no-ops.
- **`commit` requires every chunk to be pending**, not merely referenced.
  Pending-ness is replicated state, so the check cannot diverge. It does
  require the front end (task 6) to propose `put_chunk` for every chunk of
  every upload, including chunks already stored, which also restarts their
  grace.
- **A pending chunk stays pending for the whole grace period**, even after a
  `commit` binds it. Another upload of the same bytes may be in flight, and
  the manifest's own reference keeps the chunk alive afterwards.
- **RESP `SET` cuts values into fixed 4 MiB pieces**, not the configured
  chunk size. The chunk list is replicated state, and a node configured
  differently would otherwise build a different manifest from the same entry.
- **No per-replica chunk counts are kept.** Each occurrence in a manifest,
  and each pending chunk, holds one reference in the node-wide
  `chunk_refcounts` directly. A restore computes the before/after difference
  from the two indexes and adjusts by it (acquires first, then releases).
  A replica that is destroyed (shutdown, or the source of a merge) `forget`s
  its references without deleting files. Start-up orphan collection (task 7)
  removes whatever that leaves unreferenced.
- **Requirement 2.2, measured:** 100,000 sccache-shaped keys (78-byte keys,
  one chunk each) cost **318 bytes per key**, against a budget of 334
  (256 + key), node-wide reference counts included. The first draft cost
  508. What got it under:
  - an intrusive handle (`cache_manifest_ptr`) instead of `shared_ptr`;
  - a one-chunk value stores no chunk list, because its chunk is the value;
  - longer chunk lists live in the manifest's own allocation;
  - the `has_all` bit is packed into the count word, so a manifest is
    56 bytes;
  - the per-replica count map is dropped (see above).
- `chunk_refcounts` gained bulk `acquire`/`release`, `forget` and
  `put_referenced`, and `cache_chunk_store` gained `put_verified`, so a chunk
  `apply` has already hashed is not hashed twice.

**The gateway over either machine (Requirement 2.7).** `redis_gateway` takes
its state machine from `Host::types::state_machine_type` rather than a new
template parameter, so no call site changes. It reads entries through
`redis_gateway_value_access<StateMachine>`
(`include/raft/redis_gateway_value_access.hpp`): length, servability, bytes
and equality, plus a context taken under the node lock (the node-wide
`chunk_refcounts` for the cache) so the bytes are read after the lock is
released. `tests/redis_gateway_integration_test.cpp` builds a second time as
`redis_gateway_disk_store_integration_test`, with each host's groups sharing
one chunk store as a real node's do. All 25 cases pass over both machines
locally (boost future backend), and the disk build adds a 26th:

- **An unservable entry is not served here.** `read_local` checks the
  `has_all` bit for every read, so `EXISTS`, `STRLEN` and `TTL` agree with
  what `GET` would return. A follower forwards to the leader; a leader
  answers a miss rather than wait (design Component 3).
- **A chunk that fails after `lookup` is a miss**, and its manifest's
  `has_all` bit is cleared so the fetcher (task 5) picks it up.
- **The immutable-values check compares length and sha256** on the disk
  store instead of reading the old value back. The value sha is already in
  the manifest, and keys there are content-addressed.
- **`cache_kv_state_machine` is default-constructible**, as an unbound
  placeholder that throws on `apply` and restore. `raft`'s node
  configuration default-constructs its state machine before `multi_raft`
  moves the factory's over it.
- The disk variant is its own source file that defines
  `KYTHIRA_GATEWAY_TEST_DISK_STORE` and includes the original, so it can
  keep reusing the shared precompiled header. clang rejects a PCH when the
  target adds its own `-D`.

- [x] 4. `binary_file_persistence_engine`
  - `include/raft/binary_file_persistence.hpp` per Component 4: an atomically
    rewritten `meta`, CRC'd log segments, prefix truncation by segment, torn
    tail recovery, and the snapshot file.
  - It must satisfy the persistence concept. Port the cases in
    `tests/file_persistence_unit_test.cpp` to it, and add a kill-mid-append
    test.
  - _Requirements: 5.1, 5.3_

### Task 4 results (2026-11-01)

`include/raft/binary_file_persistence.hpp` and
`tests/binary_file_persistence_unit_test.cpp`: the 21 cases of
`file_persistence_unit_test.cpp` ported unchanged apart from the engine type,
plus 14 of its own (35 with libfiu), all passing locally, under ASan/UBSan,
and the barrier cases under TSan. Choices the design left open:

- **Records carry the entry type** (`u8` after the term), which design.md's
  record layout left out. Configuration and no-op entries must reload as
  what they were.
- **RAM holds each entry's location, not its command.** About 40 bytes per
  entry; commands are read back with `pread`. Holding chunk-carrying commands
  until compaction would make memory follow bytes written (Requirement 2.1).
- **The log is one contiguous run.** An append at a held index with the same
  term is a no-op, with a different term it truncates first (Raft's conflict
  rule), and a gap throws. The memory engine's map would accept a gap; Raft
  never asks for one, so the engine refuses it instead of persisting a log
  that cannot be reloaded in order.
- **`meta` also records the first live index** (`log_start`).
  `delete_log_entries_before` writes it before deleting segments, so a crash
  in between leaves dead records the load ignores, and dead whole segments it
  deletes. An append to an empty log first deletes whatever dead segments
  remain, so lowering `log_start` cannot bring old records back.
- **`truncate_log` syncs before returning**, deleting later segments newest
  first, so a crash leaves a prefix. A truncated entry from an old term that
  came back after a crash would contradict what this node had already
  answered.
- **Only the last segment may have a torn tail.** A bad record in an earlier
  segment, or a bad `meta` or `snapshot` CRC, throws on open: those files
  were complete and synced before anything later was written.
- **The barrier follows `file_persistence_engine`'s rule**: sample, unlock,
  fsync the segments written since the last barrier (and the directory if
  one was created), relock, publish. 200 appends from 8 threads took 48
  fsyncs in the coalescing test.
- The kill test forks a writer that appends and barriers in a loop and
  reports each durable index on a pipe; after `SIGKILL` every reported index
  reloads and the log is one valid run (5 rounds).
- `batched_persistence_engine` (`begin_batch` / `commit_batch`) is not
  implemented. Nothing on the node's append path calls it, and the barrier
  already coalesces.

- [x] 5. Chunk fetcher
  - `include/raft/cache_chunk_fetcher.hpp` per Component 5: a queue, a
    leader-first peer order, verify then `put`, the token bucket, and
    `has_all` refresh.
  - A test with two in-process nodes: restore a manifest on an empty store,
    fetch completes, reads move from forwarded to local.
  - _Requirements: 4.3, 4.5, 4.6, 10.1_

### Task 5 results (2026-11-01)

`include/raft/cache_chunk_fetcher.hpp` and
`tests/cache_chunk_fetcher_unit_test.cpp` (8 cases, clean under ASan/UBSan
and TSan), plus a gateway case in the disk-store build of
`redis_gateway_integration_test.cpp`: a follower that lost a chunk forwards
`GET` to the leader, the fetcher brings the chunk back from the leader's
store, and the next `GET` is served locally with no forward. Choices the
design left open:

- **The transport is a callback.** `GET /_chunk/<sha>` belongs to the HTTP
  front end (task 6); task 7 wires the fetcher to it. The tests read another
  node's store directly.
- **The state machine reports what it lacks.**
  `cache_kv_options::_on_missing_chunks` is called after a restore or an
  absorb, and after a `commit` that binds pending chunks a snapshot brought
  without their bytes. It runs under the node lock, so the fetcher's `want`
  only queues.
- **One queue per node, keyed by chunk.** A chunk several shards want is
  fetched once, and each of those shards is refreshed when its last
  outstanding chunk is done (and at most once a second while a long fetch
  runs, so keys become servable as their chunks land).
- **`chunk_refcounts::put_if_referenced`** stores a fetched chunk only if
  something on the node still references it, under the same mutex as
  `release`. A key deleted while its chunk was in flight leaves no orphan.
- **A chunk no peer supplies is retried** after 250 ms, doubling to a 30 s
  cap, and dropped once nothing references it. A peer that sends bytes
  whose hash is wrong counts as a miss, and the next peer is asked.
- **The token bucket charges after each fetch**, because a chunk's size is
  known only when it arrives; the next fetch waits out the debt. With
  several workers, up to one chunk per worker can overshoot the rate.
- ASan caught a worker waiting on a reference to a queue entry's deadline
  that another worker then erased; the deadline is copied before the wait.

- [x] 6. HTTP front end and auth cache
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

### Task 6 results (2026-11-01)

`include/raft/cache_http_front_end.hpp`, `include/raft/cache_http_auth.hpp`
and `tests/cache_http_front_end_test.cpp` (13 cases over three in-process
hosts, each with its own chunk store). The four the task names:

- **A 1 GiB `PUT`** on a one-node cluster (one Raft log in the process,
  snapshotting every 8 MB) peaked at **47 MiB** of RSS above the baseline,
  against the 64 MiB bound, and took about 20 s over loopback.
- **A disconnect mid-`PUT`** leaves the old value and counts one aborted
  put.
- **A `read_only` `PUT`, `DELETE`, `MKCOL` or `PROPFIND`** gets 403, and the
  shard's last log index does not move.
- **413** on a declared length over the maximum, and on a chunked body that
  grows past it.

The others cover `GET`, `HEAD`, `Range` across a chunk boundary, 416, empty
values, `DELETE`, chunked uploads with `Expect: 100-continue`, follower
relays, the WebDAV replies, `/_chunk/` and `/_metrics` access, 507 below the
free-space floor, the plaintext opt-in, and HTTPS with TLS between front ends.
Choices the design left open:

- **One thread per connection**, each with a private `io_context`. Every
  socket operation is asynchronous with a Beast deadline, driven to
  completion before the handler moves on. A Raft proposal therefore blocks
  only its own connection, and the connection gate caps the threads.
- **Relays carry the client's own `Authorization`**, not the internal secret,
  so the leader authorizes the same identity and the ACL is checked where
  the write lands. A relayed request is marked `X-Kythira-Forwarded` and is
  never relayed twice. The internal secret opens `/_chunk/` and nothing else.
- **`PROPFIND` is `read_write`**, per the design, so a `read_only` client is
  refused it. sccache in read-only mode never sends one.
- **The free-space floor is local only.** Voters' free space arrives with the
  advisory batches of task 7; until then a leader checks its own disk.
- **The auth cache holds the ACL table it was filled from.** A reload
  installs a new table, and the next lookup sees a different one and
  empties the cache. Holding the old table stops its address being reused,
  which would otherwise hide a reload.
- **Refusals drain small bodies.** A 401, 403 or 507 for a request whose
  declared body is at most 64 KiB reads and drops that body, so the client's
  connection survives. Larger bodies, and every 413, close the connection.
  Closes linger for up to a second, reading and discarding, so the kernel
  does not reset the connection before the client has read its 413.
- Beast can report a chunked body's limit while still reading the header,
  when the first chunks arrive in the same read. That path answers 413 too.
- TSan caught `stop()` closing an acceptor while its thread was still in
  `accept()`. `stop()` now shuts the listening socket down to wake the
  thread, joins it, and only then closes the acceptor. ASan is clean.

- [x] 7. Daemon wiring, configuration and the disk budget
  - `cmd/redis_gateway_node`: `KYTHIRA_CACHE_*` parsing in `config.cpp`,
    selection of `memory` versus `file` in `run_host.hpp`, start-up orphan
    collection, eviction against `KYTHIRA_CACHE_MAX_DISK_BYTES`, the
    free-space and free-inode floors, follower `touch` batches carrying each
    node's free bytes and inodes, the leader refusing writes while any voter
    is below a floor, and the `INFO` fields.
  - Existing `redis_gateway_node` tests must pass with
    `KYTHIRA_CACHE_PERSISTENCE=memory`.
  - _Requirements: 2.6, 5.2, 5.4, 6.1–6.6_

### Task 7 results (2026-11-01)

`KYTHIRA_CACHE_PERSISTENCE=file` (the default) runs the CBOR stack over
`cache_kv_state_machine`, `binary_file_persistence_engine` under
`DATA_DIR/raft/<group>`, the chunk fetcher and the HTTP front end, in its
own translation unit (`run_cbor_file.cpp`). `memory` keeps today's daemon
and refuses every disk-only variable. `include/raft/cache_disk_guard.hpp`
holds the budget and the floors; the gateway gains `KYTHIRA.ADVISE` and a
set of cache hooks the daemon fills.

Checked on a local three-node daemon cluster:

- **Restart (Requirement 5.2).** A RESP `SET` and a 3 MB HTTP `PUT` survive
  all three nodes being killed and restarted, byte for byte.
- **A voter's floor (6.6).** One node restarted with a free-space floor
  above its disk's free space: `SET` on the leader is refused with
  `-ERR node 2 has … bytes free, below the floor of …, retry later`, `PUT`
  gets 507, and `GET` and `DEL` still work.
- **The budget (6.1).** One follower with a 2.5 MB budget and a 3 MB
  upload: nothing is evicted while the upload's chunks are inside the
  orphan grace, then exactly that key is evicted once the grace passes,
  every store drops to about 1 KB, and the keys read through the follower
  survive.
- **`INFO`** has a `# Cache` section with the fields of Requirement 6.5,
  and the gateway's stats count refusals and advisories.

Unit tests: `cache_disk_guard_unit_test` (12 cases), new cases in the chunk
store, state machine, config and front-end tests, and three gateway
integration cases (follower reads reach the leader's order, a refusal stops
`SET` but not `DEL`, only peer gateways may advise) that run over both
state machines. Choices the design left open, and what the cluster run
turned up:

- **Reports carry the sender's floors and budget.** Requirement 6.6 holds a
  voter to *its* floor, so each report is `free,inodes,store,pinned,
  min_free,min_inodes,max_disk`, and the leader judges the voter by the
  voter's settings. A report older than 30 s is ignored: a voter that has
  gone quiet is not applying, and holding writes for it would turn one
  failure into an outage.
- **Chunks inside the orphan grace are pinned.** A committed chunk stays
  pending for the whole grace (task 3), so evicting a fresh key frees
  nothing. The first cluster run emptied the shard chasing an excess it
  could not free. Now each node reports its pinned bytes; a node whose
  pinned bytes alone fill its budget asks for no eviction, and the
  eviction walk skips keys whose eviction would free nothing on disk
  (`cache_kv_state_machine::reclaimable_bytes`). The walk is capped at 16
  times the sweep batch per tick.
- **No double eviction on stale reports.** After a shard evicts for the
  budget, reports taken before that eviction are ignored; they still count
  the bytes it just freed.
- **HTTP traffic reaches the eviction order.** The first run never evicted
  an HTTP-only key: only RESP accesses entered the gateway's order. The
  front end now reports hits, writes and deletes to the gateway
  (`note_access`, `note_removed`), and a follower's are queued for the
  leader like RESP reads.
- **Start-up orphan collection runs after `host->start()`,** once every
  group has restored its snapshot. Without a snapshot yet, that collects
  every chunk and the log replay writes them again, since `put_chunk`
  carries its bytes; the snapshot threshold in file mode is 256 MiB, so the
  rewrite is bounded by it.
- **Advisories are only for leaders' LRUs and the guard.** They travel on
  the forwarding link under the internal secret, once per maintenance
  tick, with at most 4096 keys per shard; overflow is counted, not queued.

- [x] 8. Container image and compose file
  - `docker/cache-cluster/` per design.md "Containers": non-root, a `/data`
    volume, three services addressed by name, no static IPs.
  - It must come up under Docker and under rootless Podman
    (`kythira-rootless-podman-sandbox-recipe` in project memory has the
    sandbox steps).
  - _Requirements: 11.1–11.3_

### Task 8 results (2026-11-01)

`docker/cache-cluster/Dockerfile`, `compose.yml` and `acl.txt`, with
`tests/docker_chaos/cache_cluster_test.cpp` and the CMake targets
`docker-cache-cluster-image` and `docker-cache-cluster-tests`, run by the
container-tests workflow under both runtimes.

- **The image** is the gateway image's recipe: the host-built daemon on
  Ubuntu 24.04 with `ca-certificates` only, `USER 10001:10001`, and
  `VOLUME /data`, created and owned by that user before the `VOLUME` line
  so a fresh named volume starts out writable by it. File mode and
  `DATA_DIR=/data` are the image's defaults.
- **The compose file** starts `cache-1` to `cache-3`, each with its own
  named volume, addressed by service name for Raft, forwarding and the
  chunk fetcher; no static addresses, capabilities, or host networking.
  RESP and HTTP are published on the loopback (1639n and 1809n). The
  budget and floor are compose variables so a scenario can shrink them.
- **The scenario test** checks that every node runs as uid 10001 in file
  mode, that a RESP `SET` and a 3 MB HTTP `PUT` on one node read back on
  all three, that a `read_only` user gets 403 on `PUT`, and that data
  survives restarting every node.

All three cases pass under Docker in the sandbox, with the daemon built
against Folly as CI builds it. The sandbox cannot pull from Docker Hub, so
the local run used a stand-in base made from the host's runtime libraries;
the Dockerfile itself is exercised by the container-tests workflow. The
Podman leg runs there as well.

A daemon built on the Boost future backend crashes (SIGSEGV in a pre-vote
retry posting to a freed strand) as soon as peers are given by host name
and are not yet up. It happens in memory mode too, so it predates this
spec; Folly builds, which every image uses, are unaffected.

- [x] 9. End-to-end test with the real clients
  - A container test: build a small CMake project twice through sccache
    WebDAV (misses, then hits), and install one small vcpkg port twice
    through the `http` source (build, then restore). Repeat with every node
    stopped and show the builds still pass.
  - Record the throughput of a 500 MB `PUT` and `GET` (design.md Risks).
  - _Requirements: 9.1, 9.2, 12.1, 12.2_

### Task 9 results (2026-11-02)

`docker/cache-cluster/client/` is the CI job: sccache 0.17.0 (the release
tarball and checksums `oci-build-cache` pins), vcpkg at vcpkg.json's
baseline `9a7f7340`, CI's own `start-sccache.sh`, a five-file CMake project
and `run.sh`. Everything that needs the internet happens when the image is
built, including zlib's source (`--only-downloads`), so a stopped cluster
leaves the client nothing to reach. The compose file runs it as the
`client` service behind a profile; four new cases in `cache_cluster_test.cpp`
drive it.

| Case | Result (sandbox, 3 host processes, 4 cores) |
|---|---|
| sccache WebDAV, two clean builds | 5 misses and 5 writes, then 5 hits and 0 misses; 7.5 s then 0.5 s |
| vcpkg `http`, two fresh trees | zlib built and both packages submitted (3.6 s), then restored, not built (0.6 s) |
| every node stopped | `start-sccache.sh` turns sccache off and both builds pass; vcpkg's requests are refused and it builds zlib, rc 0 |
| 500 MB `PUT` then `GET` from another node | `PUT` 201 in 27-33 s (15-19 MiB/s); `GET` 200 in 2.0-2.4 s (210-250 MiB/s), hash-equal, four runs, no election |

The `PUT` rate is the sandbox's disk: every node writes each chunk to its
Raft log and to its chunk store, each with an `fsync`. Task 11's hardware
gives the figure that matters. The `GET` can need a moment for the other
node to apply the commit (a 201 means committed, not applied everywhere);
`run.sh` waits and reports it separately, 0.01-0.02 s here.

**Three faults, found only by large uploads and fixed here.**

1. **A multi-chunk `PUT` never committed.** The leader packed up to 100
   entries into one AppendEntries with no byte limit, so two 4 MiB chunk
   entries exceeded the Raft listener's 10 MB request limit. The follower
   closed the connection, every retry carried the same batch, and the
   upload timed out with 503 after ~16 MB. Raft now has
   `raft_configuration::_max_append_bytes` (default 4 MiB, at least one
   entry always sent), and the daemon sets it to its largest entry (chunk
   size or RESP value limit) and sizes the Raft listener's body limit to fit.
2. **File mode evicted at 1 GiB per shard.** `KYTHIRA_REDIS_MAX_SHARD_BYTES`
   is the memory mode's RAM bound, and its 1 GiB default still applied with
   values on disk: two 500 MB archives in one shard and the leader evicted
   keys while the 4 GiB disk budget was half empty. In file mode it now
   applies only when set; the disk budget governs (Requirement 6.1).
3. **A lagging follower was flooded.** Every proposal triggers a
   replication round, and each round sent a behind follower the same range
   again: 125 chunk proposals per 500 MB, each copy up to the batch limit
   and each retried. The follower's Raft listener hit its 100-connection
   cap and reset connections, and it caught up at ~1 MB/s. The leader now
   keeps one entry-carrying AppendEntries in flight per follower, for at
   most one election timeout, sends heartbeats meanwhile, and sends the next
   batch as soon as a success comes back. The first version held the slot
   for the whole retry budget, and one Raft property test
   (`property_replicate_tracks_follower_lag`) failed 4 runs in 8, against 0
   in 8 on `main`; with the election-timeout hold it passes 10 of 10.

The compose file's election timeouts went from 150-300 ms to 1-2 s. A
follower persists each 4 MiB entry before answering, which on a slow disk
alone passes 300 ms, and an election mid-upload is a 503.

The sandbox cannot pull the Ubuntu base image, so the clients ran as host
processes against three host daemons with the compose file's settings; the
container-tests workflow runs the real images under Docker and Podman.

- [x] 10. Chaos scenarios
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

### Task 10 results (2026-11-02)

Five cases in `tests/docker_chaos/cache_cluster_test.cpp`, run by the
container-tests workflow under both runtimes with the task 8 and 9 cases.

| Case | Requirement | What it shows (sandbox run, Docker) |
|---|---|---|
| Leader of the vcpkg shard killed (`SIGKILL`) once 8 MB of a 100 MB `PUT` through a follower is stored | 12.3 | The client's connection drops; after the node is started again all three nodes answer 404, so the key is absent, not partial. A 201 would have required it whole on every node. |
| `compose stop` of all three nodes, then `start` | 12.4 | 20 RESP keys and two 9 MB objects read back whole from every node. |
| One byte flipped in two stored chunks on cache-2: the only chunk of a 100 KB object and the last of an 8.5 MB one | 10.1-10.3 | Bad first chunk: a miss (404, or the leader's copy where the node is a follower and forwards). Bad last chunk: 200, then the connection aborted after 8,388,608 of 8,488,615 bytes. Both chunks are then repaired from a peer and served whole. |
| cache-3's container and volume removed and the service brought back up empty, after 73 MB of objects | 12.5 | The new node gets a snapshot and fetches 45 chunks; meanwhile every GET on every node is a whole object or, on the new node before it has the key, a 404 miss; no 5xx, no wrong body. |
| cache-3 on a tmpfs of 4,000 inodes, floor 2,500, 70 KiB RESP values | 12.6 | 703 values written, then `-ERR node 3 has 2353 inodes free, below the floor of 2500, retry later`, and refused again a moment later; sampled values read back on every node; all three containers running. |

Choices and deviations:

- **The byte is flipped in the file, not with libfiu.** The image's daemon is
  a release build without libfiu and has no fault-control socket; changing
  the stored file through `exec` corrupts exactly what a disk would and
  exercises the read path's hash check end to end. The libfiu coverage of the
  store's own fault points stays in `cache_chunk_store_unit_test`.
- **The inode floor uses a tmpfs with `nr_inodes`**, a named volume with the
  local driver's tmpfs options mounted on cache-3 at `/data-small` by
  `compose.yml` and selected with `KYTHIRA_CACHE_3_DATA_DIR`. Not the
  service's `tmpfs:` key: Podman 4.9 rejects `nr_inodes` there ("unknown
  mount option"), which failed the first Podman run, while both runtimes
  accept it as a volume option. The case checks the runtime honoured the
  option (`stat -f` reports 4,000 inodes) and fails if it did not. Values are 70 KiB, just over the 64 KiB pack threshold, so each is
  one file: smaller values share pack files (Requirement 6.7) and barely use
  inodes at all. Mode 1777 instead of `uid=`, which the runtimes treat
  differently.
- **Forwarding and chunk fetches dial through the bounded resolver.** The
  first rootless Podman run failed the kill, stop-all and byte-flip cases:
  each time a follower forwarded to a leader that had just restarted, the
  request hung past the client's 10 s and the follower answered nothing or
  503. The HTTP front end resolved peers with a plain getaddrinfo(), and
  once a node has looked up a killed peer, aardvark-dns answers it nothing
  for 40 s or more (`bounded_resolver.hpp`). It now dials as the RESP
  forward link does: a capped lookup, the address the peer's Raft traffic
  last came from when DNS has nothing, a 3 s connect, and one retry after a
  failed dial retires the address. Docker never showed it, because its
  embedded DNS answers at once.
- **The test cluster snapshots every 64 MiB** (`compose.yml`, was the 256 MiB
  default). With the default the replaced node caught up from the log, which
  carries the chunk bytes, and fetched nothing; the case would not have
  tested the fetch at all.
- **The leader is found by forwarding:** a one-byte `PUT` to a follower is
  forwarded (`/_metrics` `forwards` goes up) and to the leader it is not.

**A fault, found by the byte-flip case and fixed here.** A chunk that failed
its hash on a read was never repaired. The front end queued a repair fetch,
but the bad file was still stored, so the fetcher counted the chunk as
present and skipped it, and a `put` of the good bytes would have kept the
bad file anyway (a chunk's name is its hash, and `link` refuses a taken
name). Every later GET of that object failed the same way. The store now has
`drop_if_corrupt`, which removes a stored copy only if it still fails its
hash, and the daemon calls it before queueing the repair, as design.md's
error table always said ("drop the chunk and queue a repair").

- [x] 11. Full-tree sccache measurement in WebDAV mode
  - Per Requirement 13. Point sccache 0.17.0's WebDAV backend at the
    three-node compose cluster. Build this repository's tree on two
    configurations at once (`g++-13` and `clang++-18`, x64), cold and then
    warm at one commit. Record hit rate, the Build step time, sccache's full
    `--show-stats`, the cluster's peak RSS, chunk-store bytes and file count.
  - Compare against `oci-build-cache` task 10's numbers. If the hardware is
    not a GitHub-hosted runner, also time the same commit against the OCI
    cache on that hardware, and compare against that.
  - Repeat the warm run with a `read_only` user. Use sccache's WebDAV
    read-only mode if task 1 found one; otherwise count the refused `PUT`s.
  - _Requirements: 13.1–13.6_

### Task 11 results (2026-11-04 and 2026-11-05, both legs)

Run [41777755237](https://github.com/crawlins/kythira/actions/runs/41777755237)
of `.github/workflows/cache-cluster-measure.yml`, dispatched on `main` at
28d1bc29 after main's own CI run at that commit had finished green, so the
OCI bucket held that commit's objects. Hardware: a GitHub-hosted
`ubuntu-24.04` runner (image 20261025.353.1), the class the OCI baseline was
measured on. The three cache nodes ran on the same runner as the build,
with `KYTHIRA_CACHE_CLUSTER_MAX_DISK_BYTES` at 16 GiB. Commands:
`scripts/cache-cluster-measure/measure.sh cluster-up`, then
`measure.sh build <label> <webdav|webdav-ro|oci>` per row and
`measure.sh cluster-stats webdav-warm`. Each build is a clean configure
identical to ci.yml's Release configure and a timed `cmake --build`.

| g++-13, x64 | Build | Requests | Hits | Hit rate | Read / write errors, timeouts | Writes | Avg read hit |
|---|---|---|---|---|---|---|---|
| WebDAV cold (empty cluster) | 7,862.4 s (131.0 min) | 752 | 0 | 0% | 0 / 0, 0 | 752 | n/a |
| WebDAV warm | 317.8 s (5.30 min) | 752 | 752 | 100% | 0 / 0, 0 | 0 | 3.8 ms |
| WebDAV warm, `read_only` + `READ_ONLY` mode | 317.1 s (5.29 min) | 752 | 752 | 100% | 0 / 0, 0 | 0 | 3.5 ms |
| OCI warm, same runner, same commit | 334.6 s (5.58 min) | 752 | 752 | 100% | 0 / 0, 0 | 0 | 168.2 ms |

Cluster after the warm run (every node holds every chunk):

| Node | Peak RSS | RSS after | Store bytes | Loose files | Packed chunks | Files under /data | Hash failures |
|---|---|---|---|---|---|---|---|
| cache-1 | 240 MiB | 98 MiB | 288,182,670 | 604 | 154 | 611 | 0 |
| cache-2 | 207 MiB | 146 MiB | 288,182,670 | 604 | 154 | 611 | 0 |
| cache-3 | 150 MiB | 51 MiB | 288,182,670 | 604 | 154 | 611 | 0 |

Against Requirement 13, for this leg:

- **13.1** Full tree, cold then warm at one commit: met.
- **13.2** Warm hit rate 100% (≥ 99%): met. The warm WebDAV Build took
  0.95× the OCI warm Build on the same runner and commit (≤ 1.2×): met.
  The `oci-build-cache` task 10 figure of 2.23–4.05 min is from a smaller
  tree; the tree has grown since, and OCI itself now takes 5.58 min, so the
  same-run OCI time is the comparison that holds the work constant.
- **13.3** Zero read errors, write errors and timeouts on the cold and warm
  runs: met. The cluster counted 752 PUTs on the cold run, one per write,
  and no 403s.
- **13.4** The `read_only` run with `SCCACHE_WEBDAV_RW_MODE=READ_ONLY`
  served 752 hits with zero writes, zero write attempts and zero PUTs at
  the cluster: met.
- **13.5** Per-hit read latency was 3.5–3.8 ms against OCI's 168 ms. A
  node's peak RSS stayed at or under 240 MiB, and 288 MB of chunks took 611
  files per node.
- The cold run compiled all 752 translation units on the runner's cores
  in 131 min, the PUTs included. A cold OCI build was not part of the run,
  so this says nothing about WebDAV's cost on a miss.

In run 41777755237 the clang++-18 leg did not start its cluster: a clang
build that finds the Azure SDK links libatomic into the daemon, and the
image lacked `libatomic1`, so every node exited at load. The image now
installs it (#689), and both legs were run again.

#### Second run, both legs (2026-11-05)

Run [41924204158](https://github.com/crawlins/kythira/actions/runs/41924204158),
the same workflow and commands at ab4265da, after main's own CI run at that
commit had finished its Build & Test jobs. Main had moved on by dispatch
time, so the run was dispatched from a working branch pinned at ab4265da;
the dispatch uses the read-only OCI key on any ref, so the OCI row reads the
same objects a run on `main` would. Same runner class and image
(`ubuntu-24.04` 20261025.353.1), same 16 GiB disk cap. The tree had one
more translation unit (753) than at 28d1bc29.

| clang++-18, x64 | Build | Requests | Hits | Hit rate | Read / write errors, timeouts | Writes | Avg read hit |
|---|---|---|---|---|---|---|---|
| WebDAV cold (empty cluster) | 7,771.1 s (129.5 min) | 753 | 0 | 0% | 0 / 0, 0 | 753 | n/a |
| WebDAV warm | 246.9 s (4.12 min) | 753 | 752 | 99.87% | 0 / 0, 0 | 1 | 3.0 ms |
| WebDAV warm, `read_only` + `READ_ONLY` mode | 245.1 s (4.09 min) | 753 | 752 | 99.87% | 0 / 1, 0 | 0 | 2.9 ms |
| OCI warm, same runner, same commit | 256.8 s (4.28 min) | 753 | 751 | 99.73% | 0 / 2, 0 | 0 | 135.0 ms |

| g++-13, x64 (repeat) | Build | Requests | Hits | Hit rate | Read / write errors, timeouts | Writes | Avg read hit |
|---|---|---|---|---|---|---|---|
| WebDAV cold (empty cluster) | 7,109.0 s (118.5 min) | 753 | 0 | 0% | 0 / 0, 0 | 753 | n/a |
| WebDAV warm | 295.9 s (4.93 min) | 753 | 753 | 100% | 0 / 0, 0 | 0 | 5.5 ms |
| WebDAV warm, `read_only` + `READ_ONLY` mode | 292.0 s (4.87 min) | 753 | 753 | 100% | 0 / 0, 0 | 0 | 4.9 ms |
| OCI warm, same runner, same commit | 313.8 s (5.23 min) | 753 | 753 | 100% | 0 / 0, 0 | 0 | 191.0 ms |

Cluster after each leg's warm run (every node holds every chunk):

| Leg | Node | Peak RSS | Store bytes | Files under /data | Hash failures |
|---|---|---|---|---|---|
| clang++-18 | cache-1 / 2 / 3 | 291 / 164 / 158 MiB | 264,288,053 | 561 | 0 |
| g++-13 | cache-1 / 2 / 3 | 163 / 263 / 235 MiB | 288,500,128 | 612 | 0 |

Against Requirement 13, for the clang++-18 leg:

- **13.1** Full tree, cold then warm at one commit: met.
- **13.2** Warm hit rate 99.87% (≥ 99%): met. The warm WebDAV Build took
  0.96× the OCI warm Build (≤ 1.2×): met. The g++-13 repeat came in at
  0.94×, in line with the first run's 0.95×.
- **13.3** Zero read errors, write errors and timeouts on the cold and warm
  runs: met. The cluster counted 753 PUTs on the cold run and 1 on the warm
  run, with no 403s.
- **13.4** The `read_only` run wrote nothing: zero writes and zero PUTs or
  403s at the cluster: met. Its one write error is sccache declining to
  store the run's one miss because of `READ_ONLY` mode. That is the mode
  doing its job, not a failed request. OCI's read-only run shows the same
  thing for its two misses.
- **13.5** Per-hit read latency was 2.9–3.0 ms against OCI's 135 ms. A
  node's peak RSS stayed at or under 291 MiB, and 264 MB of chunks took
  561 files per node.
- The clang warm runs missed one translation unit whose cache key changed
  between the cold and warm builds at the same commit. OCI missed two at
  that commit, and the g++-13 leg missed none, so the cause is in the clang
  build's inputs, not in the cache. The log does not name the unit. Finding
  it is follow-up work if the clang hit rate ever drops toward 99%.

Both legs meet every Requirement 13 threshold, so task 11 is complete.

- [x] 12. Documentation
  - `doc/redis-gateway.md`: the variables, HTTP API, WebDAV subset, disk
    layout, and client settings for sccache and vcpkg with read-only and
    read-write users.
  - `doc/sccache_dogfood_cost_estimate.md`: the home-hosting sizing with disk
    instead of RAM.
  - _Requirements: 14.1–14.3_

- [ ] 13. Close-out
  - Update `doc/TODO.md`'s row and `doc/CHANGELOG.md`. Record what task 1 and
    task 9 measured against what this spec assumed.
  - _Requirements: all_

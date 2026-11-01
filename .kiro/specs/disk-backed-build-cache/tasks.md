# Implementation Plan — Disk-Backed Build Cache

## Status: 2/13 tasks complete

**Last Updated**: November 1, 2026 (task 2: the chunk store, pack files and
node-wide reference counts. Task 1 was measured on October 27. The spec was
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

- [ ] 11. Full-tree sccache measurement in WebDAV mode
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

- [ ] 12. Documentation
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

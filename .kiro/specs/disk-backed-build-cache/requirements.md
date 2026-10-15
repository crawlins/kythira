# Requirements Document — Disk-Backed Build Cache

## Introduction

This spec turns the Redis-compatible gateway (`.kiro/specs/redis-compatible-kv/`,
`cmd/redis_gateway_node/`) into a build cache that can hold CI's whole cache
on disk. It also adds an HTTP front end, so that both of the build's caches
can live in one Kythira cluster:

- **sccache** (compiler outputs), through sccache's WebDAV backend, or through
  RESP as today.
- **vcpkg** (per-port binary archives), through vcpkg's `http` binary source.
  vcpkg has no Redis backend, so without an HTTP front end vcpkg cannot use
  this cluster at all.

The intended deployment is a self-hosted CI (a GitLab server with its own
runners) beside a small fleet of minimal-container gateway nodes, each with
some local disk. The runners and the cache share a network, so the egress and
per-request charges that make up the whole bill for today's OCI bucket
(`.kiro/specs/oci-build-cache/tasks.md` task 11) go away.

### Why the gateway cannot do this today

Every limit below was read from `main` on October 15, 2026.

- **Values live in RAM.** `redis_kv_state_machine` holds every value as a
  `std::vector<std::byte>` in a `std::map`
  (`include/raft/redis_kv_state_machine.hpp`), and the daemon uses
  `memory_persistence_engine` (`cmd/redis_gateway_node/run_host.hpp`). A node
  needs RAM equal to everything it stores, times three replicas across the
  fleet.
- **Snapshots copy everything.** The `state_machine` concept's `get_state()`
  returns the whole store as one `std::vector<std::byte>`
  (`include/raft/types.hpp`). A snapshot of a 30 GiB shard would be a 30 GiB
  allocation and a 30 GiB InstallSnapshot.
- **Values are capped at 8 MiB** (`KYTHIRA_REDIS_MAX_VALUE_BYTES`). One Raft
  entry carries one whole value. vcpkg archives reach hundreds of MB, and
  some coverage and ThreadSanitizer objects are over 8 MiB already.
- **A restart loses the cache.** With memory persistence, a replica that
  restarts catches up from its peers, and a cluster that loses every replica
  at once (a power cut) comes back empty.
- **RESP only.** Nothing speaks HTTP.

### Sizing this is built for

Measured on the OCI bucket on October 9, 2026 (`oci-build-cache` task 11), and
projected in the "OCI build cache size" project thread on October 15, 2026:

| Cache | Under age-based expiry | Working set (inferred) |
|---|---:|---:|
| sccache objects (avg 0.85 MiB) | 64 GiB, 77,064 objects | 2–3 GiB per full population; 4–8 GiB with LRU headroom |
| vcpkg archives | 20 GiB, 854 archives | 5–7 GiB per compiler and architecture; 30–40 GiB with headroom |

The working-set column is inferred, not measured. Requirement 1 measures what
the design depends on before anything is built.

### Out of scope

- Wiring a CI system (GitLab or GitHub Actions) to the cluster. That becomes
  a follow-up spec once Requirement 12's end-to-end test passes.
- An S3-compatible API. GitLab's runner cache speaks S3, and SigV4 signing
  makes that the largest of the possible front ends. It can be specified
  separately.
- Replacing `oci-build-cache`. The OCI bucket stays the cache for
  GitHub-hosted runners.

## Glossary

- **gateway**: `redis_gateway_node` and the `redis_gateway` class it hosts.
- **chunk**: An immutable run of value bytes no larger than the chunk size
  (Requirement 3), stored once per node as a file named by its SHA-256.
- **manifest**: The ordered list of chunk hashes, total length and value hash
  for one key. A key maps to a manifest. A small value is a manifest of one
  chunk.
- **chunk store**: The per-node directory of chunk files, shared by every
  shard replica on that node.
- **index**: The in-memory map from key to manifest metadata (size, expiry,
  chunk list). It is the only per-key state in RAM.
- **disk budget**: The configured number of bytes of chunk files a node may
  hold before the leader starts evicting.
- **HTTP front end**: The Boost.Beast listener added by Requirement 7.
- **WebDAV subset**: The HTTP methods sccache 0.17.0's WebDAV backend actually
  sends, as recorded by Requirement 1.

## Requirements

### Requirement 1: Measure the clients and the objects first

**User Story:** As the maintainer, I want the exact HTTP behaviour of both
clients and the real object sizes written down before the design is coded,
so that the front end implements what the clients send, not what their
documentation implies.

#### Acceptance Criteria

1. THE measurement SHALL record every request that sccache 0.17.0 (the version
   CI pins) sends to a WebDAV endpoint during `--start-server`, one cache miss
   and one hit: method, path, the headers that matter (`Depth`,
   `Content-Length`, `Content-Type`, `Authorization` scheme) and the status
   codes it accepts. It SHALL include whether it sends `PROPFIND` or `MKCOL`,
   and what it does when a request fails.
2. THE measurement SHALL record the same for vcpkg's `http` binary source at
   the vcpkg commit CI pins: the URL each `{name}`, `{version}` and `{sha}`
   template expands to, whether it sends `HEAD` before `GET`, whether a `PUT`
   is streamed or carries a `Content-Length`, which status codes it treats as
   a miss, and what it does when the server is unreachable.
3. THE measurement SHALL record, from the OCI bucket's object listing, the
   size distribution of both prefixes: largest object, 99th percentile, and
   the share of sccache objects over 8 MiB.
4. THE measurement SHALL record the working set: the bytes of sccache and
   vcpkg objects actually read by one full CI run on `main` at a single
   commit, taken from the bucket's access pattern or from client statistics.
5. IF Requirement 1.1 shows sccache needs a WebDAV method this spec does not
   list, THEN the method SHALL be added to the WebDAV subset in design.md
   before task 6 starts.
6. THE results SHALL be written into tasks.md task 1, with the commands used.

### Requirement 2: Values on disk, index in RAM

**User Story:** As an operator, I want a node's memory use to depend on the
number of keys, not the bytes stored, so that a container with 1 GB of RAM can
serve 100 GiB of cache.

#### Acceptance Criteria

1. THE state machine SHALL keep value bytes only in the chunk store. RAM SHALL
   hold the index and nothing proportional to value size, apart from buffers
   in flight.
2. THE index SHALL cost no more than 256 bytes per key on average plus the key
   itself, measured with 100,000 sccache-shaped keys.
3. `apply()` SHALL remain a pure function of the log, as
   `redis_kv_state_machine` requires today: no clock, environment or random
   source. Whether a chunk file already exists SHALL NOT change the index
   that results.
4. WHEN `apply()` cannot write a chunk (I/O error, disk full), THEN the node
   SHALL stop applying and shut down with a fatal log line. It SHALL NOT skip
   the entry, which would make its replica diverge.
5. A chunk file SHALL be written to a temporary name, flushed with `fsync`,
   and renamed into place, so that a crash never leaves a chunk with a valid
   name and partial contents.
6. ON start, the node SHALL check the chunk store against its last snapshot
   manifest. Chunks no manifest references SHALL be deleted, and a referenced
   chunk that is missing or fails its hash check SHALL be treated as absent
   (Requirement 4.4).
7. THE RESP commands the gateway supports today SHALL behave the same on the
   disk-backed store, and the existing gateway tests SHALL pass against it.

### Requirement 3: Chunked values

**User Story:** As a vcpkg client, I want to store a 500 MB archive without a
500 MB Raft entry, so that a large upload cannot stall replication for every
other key in its shard.

#### Acceptance Criteria

1. THE chunk size SHALL be configurable (`KYTHIRA_CACHE_CHUNK_BYTES`), with a
   4 MiB default and a 16 MiB maximum.
2. A value SHALL be proposed as one `put_chunk` entry per chunk, followed by
   one `commit` entry that binds the key to its manifest. A key SHALL become
   visible only when its `commit` is applied.
3. Chunks SHALL be addressed by SHA-256, so identical chunks from different
   keys are stored once per node.
4. THE largest value SHALL be configurable (`KYTHIRA_CACHE_MAX_VALUE_BYTES`),
   with a 2 GiB default. A request over it SHALL be refused before any chunk
   is proposed.
5. Chunk reference counts SHALL be kept by `apply()`. A chunk SHALL be deleted
   when the last committed manifest referencing it is deleted, swept or
   evicted, and when an orphan grace period passes with no `commit`
   referencing it (an abandoned upload).
6. THE orphan grace period SHALL be measured in log entries, not wall time, so
   that every replica deletes the same orphans (Requirement 2.3).
7. `commit` SHALL carry the SHA-256 of the whole value. A reader SHALL be able
   to check the reassembled bytes against it, and the HTTP front end SHALL
   send it as the `ETag`.

### Requirement 4: Snapshots that scale with keys, not bytes

**User Story:** As an operator, I want snapshotting and replica catch-up to
cost about the size of the index, so that a node holding 100 GiB can take a
snapshot and a replacement node can join.

#### Acceptance Criteria

1. `get_state()` SHALL return the index and the chunk reference counts only.
   Its size SHALL be proportional to the number of keys.
2. `split_state()` and `absorb()` SHALL work on the same manifest-only format,
   and the existing split/absorb law (`splittable_state_machine.hpp`) SHALL
   hold.
3. WHEN a replica restores a snapshot that names chunks it does not have, THEN
   it SHALL fetch them from a peer out of band, verifying each against its
   hash. The fetch SHALL NOT block `restore_from_snapshot` from returning.
4. WHILE a key's chunks are not all present and verified on a replica, THE
   replica SHALL treat the key as a miss for reads it answers itself, or
   forward the read to the shard leader. It SHALL NOT return partial or
   unverified bytes.
5. A node that joins with an empty disk SHALL become serving once it has
   applied the snapshot, before its chunk fetch finishes.
6. THE chunk fetch SHALL be rate-limited (`KYTHIRA_CACHE_FETCH_BYTES_PER_SEC`)
   so a rejoining node cannot starve the leader's client traffic.

### Requirement 5: Durable Raft state

**User Story:** As an operator, I want a cache that survives every node
restarting at once, so that a power cut is not a cold rebuild of everything.

#### Acceptance Criteria

1. THE daemon SHALL use a file-backed persistence engine for term, vote, log
   and snapshot. The log format SHALL be binary: no base64 or JSON around
   chunk bytes, unlike `file_persistence.hpp` today.
2. WHEN every node restarts with its data directory intact, THEN the cluster
   SHALL elect leaders and serve every key that was committed before the
   restart, without a client re-uploading anything.
3. Log compaction SHALL keep working at `snapshot_threshold_bytes`, and the
   log file SHALL be truncated after a snapshot so the log's disk use stays
   bounded.
4. `memory_persistence_engine` SHALL remain selectable
   (`KYTHIRA_CACHE_PERSISTENCE=memory|file`, default `file`), so that the
   existing memory-only deployment still works.

### Requirement 6: Bounded disk

**User Story:** As an operator, I want each node to stay inside the disk I
give it, so that the cache never fills a host's disk.

#### Acceptance Criteria

1. Each node SHALL have a disk budget (`KYTHIRA_CACHE_MAX_DISK_BYTES`). Over
   it, the leader SHALL propose `evict` entries for least-recently-read keys,
   as the gateway does today against `KYTHIRA_REDIS_MAX_SHARD_BYTES`.
2. Reads served by followers SHALL count toward the leader's least-recently-
   used order, so eviction is not blind to traffic that never reached the
   leader.
3. WHEN free space on the data directory's filesystem falls below
   `KYTHIRA_CACHE_MIN_FREE_BYTES`, THEN the node SHALL refuse new writes with
   a retryable error and keep serving reads.
4. Expiry (`SETEX`, TTL) SHALL keep working, with the leader-proposed `sweep`
   it uses today.
5. `INFO` and a metrics endpoint SHALL report chunk-store bytes, chunk count,
   key count, evictions, orphan deletions and the bytes of chunks still being
   fetched.

### Requirement 7: HTTP front end

**User Story:** As a CI runner, I want to read and write cache objects over
HTTP, so that vcpkg and sccache can both use the cluster.

#### Acceptance Criteria

1. THE gateway SHALL serve HTTP/1.1 on `KYTHIRA_CACHE_HTTP_LISTEN` and HTTPS
   on `KYTHIRA_CACHE_HTTPS_LISTEN`, using Boost.Beast. Both SHALL be off when
   unset.
2. THE object API SHALL map a path `/<key>` to the key of the same name, with
   `GET` (200 with the body, or 404), `HEAD` (200 with `Content-Length` and
   `ETag`, or 404) and `PUT` (201 on commit).
3. `GET` SHALL support a single `Range` request (206), and SHALL stream from
   the chunk store without holding the node mutex while bytes are written to
   the socket.
4. `PUT` SHALL stream its body into chunks as it arrives and SHALL NOT buffer
   the whole value in memory. It SHALL accept both a `Content-Length` body and
   a chunked transfer encoding. A body over `KYTHIRA_CACHE_MAX_VALUE_BYTES`
   SHALL be refused with 413.
5. WHEN a client disconnects mid-`PUT`, THEN no `commit` SHALL be proposed and
   the key's previous value, if any, SHALL stay visible.
6. THE WebDAV subset recorded by Requirement 1.1 SHALL be served, with
   `PROPFIND` (`Depth: 0`) answered from the index and `MKCOL` accepted as a
   no-op, since keys have no directories.
7. Request and connection limits SHALL follow `http-server-request-limits`:
   header size, header-read deadline, idle timeout and connection cap.
8. A request for a key whose shard this node does not lead SHALL be forwarded
   one hop over the existing inter-gateway forwarding link for writes, and
   SHALL be answered locally for reads when the chunks are present
   (Requirement 4.4).

### Requirement 8: Authentication and authorization

**User Story:** As the maintainer, I want merge-request runners to read the
cache but not write it, so that an untrusted branch cannot poison it.

#### Acceptance Criteria

1. HTTP requests SHALL authenticate with a bearer token or HTTP Basic, checked
   against the same `redis_acl` file the RESP listener uses: the `read_only`
   and `read_write` roles and the key-prefix scoping it already has.
2. A `read_only` user's `PUT`, `MKCOL` or `DELETE` SHALL be refused with 403
   and SHALL never reach the log. `DELETE` SHALL need the same role as RESP
   `DEL` (`read_write`).
3. Failed authentications SHALL be rate-limited per source address with the
   existing `redis_auth_limiter`.
4. ACL secrets are PBKDF2 records with 600,000 iterations, which RESP pays
   once per connection and HTTP would pay on every request. A successful
   check SHALL therefore be cached in memory for at most
   `KYTHIRA_CACHE_AUTH_CACHE_SECONDS` (default 300), keyed by a keyed hash of
   the presented credential and never by the credential itself, and the
   cache SHALL be cleared when the ACL file is reloaded.
5. IF the plaintext HTTP listener is enabled without
   `KYTHIRA_CACHE_ALLOW_PLAINTEXT_HTTP=true`, THEN the node SHALL refuse to
   start, matching `KYTHIRA_REDIS_ALLOW_PLAINTEXT_FORWARDING`.

### Requirement 9: Never a build dependency

**User Story:** As a developer, I want a build to pass when the cache is down,
only slower.

#### Acceptance Criteria

1. THE end-to-end test (Requirement 12) SHALL show a build that passes with
   every cache node stopped, for both sccache and vcpkg.
2. Any error the HTTP front end returns for an internal fault (no leader,
   disk full, fetch pending) SHALL be one Requirement 1 shows each client
   treats as a miss, not as a build failure.

### Requirement 10: Integrity

**User Story:** As a developer, I want a corrupted object to be a cache miss,
never a wrong binary.

#### Acceptance Criteria

1. Every chunk SHALL be hash-checked when read from disk, and a mismatch SHALL
   be a miss plus a local repair fetch from a peer.
2. WHEN a `GET` finds a bad chunk after the response headers have been sent,
   THEN the node SHALL abort the connection rather than finish the body, so
   the client sees a truncated transfer and not a complete wrong object.
3. A fault-injection test (libfiu, `.kiro/specs/libfiu-integration/`) SHALL
   flip a byte in a stored chunk and show both behaviours.

### Requirement 11: Containers

**User Story:** As the maintainer, I want the cluster to run as minimal
containers under Docker and rootless Podman, as every other container test in
this repository does.

#### Acceptance Criteria

1. THE image SHALL contain the daemon and its runtime libraries only, run as a
   non-root user, and take its data directory as a volume.
2. THE compose file SHALL wire nodes by service name with no static addresses
   and SHALL work under rootless Podman (CLAUDE.md "Container runtime
   compatibility").
3. No container SHALL need `--privileged`, host networking or extra kernel
   capabilities.

### Requirement 12: End-to-end proof

**User Story:** As the maintainer, I want proof against the real clients, not
only unit tests.

#### Acceptance Criteria

1. A container test SHALL start a three-node cluster and build a small CMake
   project twice through sccache's WebDAV backend, showing misses then hits.
2. THE same test SHALL install a small vcpkg port twice through the `http`
   binary source, showing a build then a restore.
3. A chaos scenario SHALL kill the leader during a 100 MB `PUT` and show that
   the key is either absent or complete and hash-correct afterwards, never
   partial.
4. A chaos scenario SHALL stop all three nodes, start them again and show that
   every committed key is served.
5. A chaos scenario SHALL replace one node with an empty data directory and
   show it serving reads once its fetch completes, with no client error
   during the fetch.

### Requirement 13: Documentation

1. `doc/redis-gateway.md` SHALL document the new variables, the HTTP API, the
   WebDAV subset and the disk layout.
2. A client section SHALL give the sccache and vcpkg settings that point at
   the cluster, with read-only and read-write examples.
3. `doc/sccache_dogfood_cost_estimate.md`'s "Home hosting" section SHALL be
   updated with the disk-backed sizing.

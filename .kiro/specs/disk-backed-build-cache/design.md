# Design Document — Disk-Backed Build Cache

## Overview

Four changes, in the order they are built:

1. **A disk-backed state machine** (`cache_kv_state_machine`, Components 1–3)
   that keeps the index in RAM and value bytes in a per-node chunk store. It
   satisfies the same `state_machine` and `splittable_state_machine` concepts
   as `redis_kv_state_machine`, so `multi_raft`, splitting and the RESP
   gateway run on it unchanged.
2. **A binary file persistence engine** (Component 4), so Raft state survives
   a restart.
3. **Out-of-band chunk fetch** (Component 5), so a snapshot carries only the
   index and replicas pull the bytes they lack.
4. **An HTTP front end** (Components 6–7) on the same `redis_gateway`
   instance, sharing its ACL, forwarding link and shard routing.

```
   sccache (WebDAV)    vcpkg (http source)    sccache / redis-cli (RESP)
          │                    │                         │
          └──── HTTPS ─────────┤                         │ rediss://
                               ▼                         ▼
                ┌──────────────────────────────────────────────┐
                │ redis_gateway_node                            │
                │   http_front_end (Beast)    resp listener     │
                │            │  ACL + auth cache  │             │
                │            └────────┬───────────┘             │
                │          route key → shard; lead? propose     │
                │          : forward one hop (writes)           │
                │                     │                          │
                │   multi_raft ── shard replicas ──┐             │
                │   cache_kv_state_machine: index (RAM)          │
                │                     │                          │
                │   chunk_store  DATA_DIR/chunks/ab/cd/<sha256>  │
                │   binary_file_persistence DATA_DIR/raft/<group>│
                └──────────────────────────────────────────────┘
                         ▲ chunk fetch (GET /_chunk/<sha>) ▲
                         └──────── peers, internal secret ─┘
```

## Component 0: Measurement (task 1)

There are no code changes. Results go into tasks.md task 1.

- **sccache:** run sccache 0.17.0 with `SCCACHE_WEBDAV_ENDPOINT` pointed at a
  recording stub, a few lines of Python `http.server` that logs every request
  and answers from a dict. Exercise `--start-server`, one miss and one hit.
  sccache's WebDAV backend is built on OpenDAL, which is expected to stat with
  `PROPFIND`, create parents with `MKCOL`, and write with `PUT`. That
  expectation is what the stub confirms or corrects; the front end implements
  what the log shows.
- **vcpkg:** at the pinned vcpkg commit, install one small port with
  `VCPKG_BINARY_SOURCES=clear;http,http://stub/{name}/{version}/{sha},readwrite`,
  and again with the stub returning 404, 500 and a refused connection.
- **Sizes and working set:** list both OCI prefixes with the read-only key
  (`doc/ci_build_cache.md` "Seeding" has the AWS CLI recipe). The working set
  comes from one `main` run's sccache stats (hits times object sizes) plus
  vcpkg's restore log.

## Component 1: Chunk store

`include/raft/cache_chunk_store.hpp`. One per node, shared by every shard
replica on that node.

```
DATA_DIR/
  chunks/ab/cd/abcd…(64 hex)   one large chunk's bytes, nothing else
  chunks/tmp/                  in-progress writes, wiped on start
  packs/pack.<n>               small chunks, appended; Component 1a
  raft/<group>/                Component 4
```

- `put(sha, bytes)`: if the file exists, return. Otherwise write
  `tmp/<random>`, `fsync` it, `rename` it into place and `fsync` the
  directory. Writing an existing chunk is a no-op, which makes replay
  idempotent.
- `open(sha)` returns a read handle. `verify(sha)` streams the file through
  SHA-256 (OpenSSL EVP, already a dependency).
- `remove(sha)` unlinks. It is only ever called from `apply()`, or from start-up
  orphan collection (Requirement 2.6).
- Two-level fan-out (`ab/cd/`) keeps directories under about 1,000 entries at
  a few million chunks.

Reference counts live in the state machine, not on disk. The store has no
idea who uses a chunk, and that keeps it testable on its own.

**Inodes.** One file per chunk makes the file count follow the key count, so
a workload of small values would run out of inodes long before bytes. Two
rules bound it. First, chunks under the pack threshold go to pack files
(Component 1a), so loose files number at most the disk budget divided by the
threshold: 1.6 million at a 100 GiB budget and 64 KiB, and far fewer in
practice, because sccache objects average 0.85 MiB. Second, the free-space
check that gates writes (Requirement 6.3) reads `f_favail` as well as
`f_bavail` from the same `statvfs` call.

**Shared across shards.** Two replicas on one node may reference the same
chunk, which is likely after a split. Counts are therefore kept per node in a
`chunk_refcounts` object that every replica's state machine updates under the
store's own mutex. A chunk is removed when the node-wide count reaches zero.
Each replica's snapshot carries only its own references (Component 3), and
the node-wide count is rebuilt on start by summing the restored replicas.

## Component 1a: Pack files

Part of `cache_chunk_store`, invisible to its callers. `put`, `open` and
`remove` take a SHA and decide the location by size.

- A pack is an append-only file of records
  `{ 32-byte sha, u32 BE len, bytes, u32 crc32 }`. One pack is open for
  appends at a time and is closed at 64 MiB.
- Appends are batched and fsynced before `put` returns, as loose chunks are.
  A torn tail is truncated on open.
- RAM holds `sha → (pack, offset, len)` for packed chunks, about 48 bytes
  each. It is rebuilt on start by scanning the packs. That is a sequential
  read of the packed bytes only, at most the disk budget divided by the
  threshold times the threshold, and in practice a small share of it.
- `remove` marks the record dead in RAM and adds its length to the pack's dead
  count. Dead records are recorded in a per-pack `.dead` sidecar, appended and
  fsynced, so a restart does not resurrect them.
- When a closed pack's live bytes fall below half its size, a background task
  copies its live records into the open pack, updates the locations, and
  unlinks the old pack and its sidecar. Copying is idempotent by SHA, so a
  crash mid-compaction leaves duplicates that the next scan ignores (the
  first live location wins).

None of this touches replicated state. `get_state()` names chunks by hash and
never by location, so two replicas may pack the same chunks differently and
remain identical (correctness property 1).

## Component 2: Log entries

The `redis_kv_commands.hpp` codec gains format version `0x02` and three
opcodes. Version `0x01` entries still decode, so a log written by today's
gateway can be replayed.

```
put_chunk (0x05)  key empty; 32-byte sha256; u32 BE len; bytes
commit    (0x06)  key; u64 BE expire_at_ms; u64 BE total_len;
                  32-byte value sha256; u32 BE n; n x 32-byte chunk sha256
touch     (0x07)  key empty; u32 BE n; n x { u32 BE keylen, key }
```

- `set` (0x01) on the disk store becomes one implicit `put_chunk` plus a
  one-chunk `commit`. This keeps RESP `SET` a single proposal.
- `touch` carries follower-observed reads to the leader's LRU (Requirement
  6.2). It is not proposed through the log. It travels on the forwarding link
  as an advisory message, batched once per policy tick. The opcode is
  reserved here so the format stays one namespace.
- The same advisory batch carries the sending node's free bytes and free
  inodes. A leader refuses writes while any voter of the shard reports
  either below its floor (Requirement 6.6), because a follower that runs out
  mid-apply stops rather than diverge.
- `put_chunk` for a chunk the replica already has costs a hash check and no
  write.

**Pending chunks and the orphan grace.** `apply(put_chunk)` records
`sha → applied_index` in a pending table if no committed manifest references
it yet. On every apply, pending chunks older than
`KYTHIRA_CACHE_ORPHAN_GRACE_ENTRIES` (default 100,000 entries) are dropped
and removed from disk if unreferenced. Because this counts log indices and
not seconds, every replica drops the same set (Requirement 3.6).

**Upload ordering.** The leader proposes a value's chunks in order and the
`commit` last. A `commit` naming a chunk that is neither pending nor
committed on that replica is applied as a no-op, and the key is not created.
That can only happen if the grace period expired mid-upload. The front end
sees the commit fail to bind and returns 503, so the client retries or treats
the result as a miss.

## Component 3: `cache_kv_state_machine`

`include/raft/cache_kv_state_machine.hpp`. It has the same public surface as
`redis_kv_state_machine` (`lookup`, `collect_expired`, `for_each_key`,
`approximate_size_bytes` and the rest), so `redis_gateway` can be
instantiated over either. The gateway becomes a template on the state
machine. Today it names `redis_kv_state_machine` directly.

```cpp
struct cache_manifest {
    std::uint64_t _expire_at_ms;
    std::uint64_t _total_len;
    std::array<std::byte, 32> _value_sha;
    std::vector<std::array<std::byte, 32>> _chunks;   // in order
};
using index_type = std::map<std::string, std::shared_ptr<const cache_manifest>, std::less<>>;
```

At 4 MiB chunks, an sccache object is one chunk, and its manifest is about 120
bytes plus the key, inside Requirement 2.2's 256-byte bound. A 500 MB archive
is 120 chunks and about 4 KB.

- `lookup(key)` returns the manifest handle, as today's `lookup` returns a
  value handle. The gateway streams chunks after the node lock is released,
  as it already does for values.
- `approximate_size_bytes()` returns the sum of `_total_len` over the
  replica's keys. It drives splitting and the disk budget.
- `get_state()` serializes the index plus the pending table, in key order:

  ```
  u32 BE version (2); u32 BE nkeys;
    nkeys x { u32 keylen, key, u64 expire, u64 total, 32 sha, u32 n, n x 32 }
  u32 BE npending; npending x { 32 sha, u64 applied_index }
  ```

  For 77,000 sccache-shaped keys that comes to about 10 MB, against about
  64 GiB of values.
- `split_state()` cuts the index by key. Pending chunks belong to no key yet,
  so every child receives the whole pending table: whichever child the
  eventual `commit` lands in still finds its chunks, and the other children
  drop theirs when the grace period passes. That costs only the bytes of
  in-flight uploads, held a little longer.
- `restore_from_snapshot()` swaps in the index, recomputes this replica's
  references, adjusts the node-wide counts by the difference, and hands every
  chunk it names but cannot find to the fetcher (Component 5). It returns
  without waiting.

**Missing chunks on read.** A manifest whose chunks are not all present
locally is *unservable here*. `lookup` callers check `chunk_store::has_all`
(a cached bit per manifest, cleared and set by the fetcher). The RESP `GET`
and HTTP `GET` paths then forward to the leader. If this node is the leader
and still fetching (a fresh leader after a failover), they answer as a miss.

## Component 4: `binary_file_persistence_engine`

`include/raft/binary_file_persistence.hpp`. It satisfies the existing
persistence concept (`include/raft/persistence.hpp`).

```
DATA_DIR/raft/<group>/
  meta        term (u64), voted_for (u64 or ~0), crc32; rewritten atomically
  log.<n>     segment files: { u64 index, u64 term, u32 len, bytes, u32 crc }*
  snapshot    last_included_index/term, configuration, get_state() bytes, crc
```

- Appends go to the open segment, and a batch is fsynced before Raft
  acknowledges it. The `durable-append-barrier` spec's barrier is where that
  hooks in.
- `truncate_prefix(index)` after a snapshot deletes whole segments below the
  index, so compaction frees disk (Requirement 5.3).
- A torn tail (CRC mismatch on the last record) is truncated on open, which
  is the standard write-ahead-log rule. A bad CRC anywhere else is fatal.
- `file_persistence.hpp` (JSON lines, base64) is left alone. It serves
  `chaos_node`, and chunk bytes through base64 would cost a third more disk
  and a lot of CPU.

**Double write, accepted.** A chunk is written once into the log segment and
once into the chunk store. The log copy disappears at the next compaction
(10 MB of applied entries by default), so the steady-state overhead is the
threshold, not a second copy of the cache. Linking chunks out of log
segments would avoid the write, but would tie the store's lifetime to the
log's. That is not worth it at a CI cache's write rate (about 50 GiB a month,
per the sizing in requirements.md).

## Component 5: Chunk fetcher

`include/raft/cache_chunk_fetcher.hpp`. One per node.

- A queue of wanted SHAs, each with the shard it came from. A worker asks the
  shard's current leader first, then the other members, for
  `GET /_chunk/<sha>` on the HTTP front end. It authenticates with the
  gateway's existing internal secret, over TLS whenever forwarding TLS is
  configured, under the same rule as `KYTHIRA_REDIS_ALLOW_PLAINTEXT_FORWARDING`.
- Each fetched chunk is verified, then `put` into the store. Its manifests'
  `has_all` bits are recomputed.
- A token bucket enforces `KYTHIRA_CACHE_FETCH_BYTES_PER_SEC`
  (default 50 MB/s).
- `/_chunk/` is served only to the internal secret. It reads chunks by hash
  and has no key or ACL semantics.

## Component 6: HTTP front end

`include/raft/cache_http_front_end.hpp`, built on Boost.Beast. The Beast
transport's server already enforces `http-server-request-limits` (header
size, header-read deadline, idle timeout, connection cap) and the handshake
deadline from PR #505. The front end reuses those pieces, not the Raft RPC
transport itself.

| Request | Role | Behaviour |
|---|---|---|
| `GET /<key>` | read_only | 200 and stream, 206 for `Range`, 404 on miss or unservable-and-leader |
| `HEAD /<key>` | read_only | 200 with `Content-Length` and `ETag: "<value sha>"`, or 404 |
| `PUT /<key>` | read_write | stream, chunk, propose, `commit`; 201. 413 over max, 507 under the free-space floor |
| `DELETE /<key>` | read_write | propose `del`; 204 |
| `PROPFIND /<dir>/` `Depth: 0` | read_write | 207 multistatus for any collection path (keys have no real directories), so sccache never needs `MKCOL`; 207 with `getcontentlength` for a stored key, 404 otherwise |
| `MKCOL /<dir>/` | read_write | 201, no-op |
| `GET /_chunk/<sha>` | internal secret | raw chunk bytes |
| `GET /_metrics` | admin | Requirement 6.5's counters |

Task 1 measured these rows against sccache 0.17.0; the capture is in tasks.md
task 1. sccache uses exactly GET, PUT, PROPFIND and MKCOL, so no method is
added (Requirement 1.5). HEAD and DELETE stay for other clients and for
operators.

The capture corrected two details of the provisional rows:

- **PROPFIND targets the parent collection, not the key.** OpenDAL sends it
  before every PUT, as `Depth: 0` with an `allprop` body, and only on the
  write path. A read-only client never sends it, so the role is `read_write`.
- **The 207 body must be a complete multistatus.** OpenDAL rejected a minimal
  one with "deserialize xml", and that failed every write. Each
  `D:response` needs `displayname`, `getlastmodified` (RFC 1123) and
  `resourcetype`, plus `getcontentlength`, `getcontenttype` and `getetag` for
  a file.

Answering 207 for every collection path avoids the upward PROPFIND walk and
the per-level MKCOLs a 404 triggers. A PUT therefore costs one PROPFIND plus
the PUT. sccache fails to start, and every compile fails, if the
`.sccache_check` GET at startup gets a 5xx or no connection. That one request
is the gateway's real availability contract. Later failures only cost cache
hits.

**PUT path.** The body parser reads into a chunk-sized buffer. Each full
buffer is hashed and proposed as `put_chunk`, and the next buffer fills while
that proposal is in flight. At most two chunks per request are in memory.
After the last buffer, the front end proposes `commit` and answers once it is
applied. On a non-leader, the request is forwarded whole over the existing
forwarding link: the link streams the body, so the forwarding node never
holds more than one chunk either.

**GET path.** Under the node lock, take the manifest handle. Outside it,
open and stream each chunk, verifying as it goes. A mismatch after headers
are sent aborts the connection (Requirement 10.2), drops the chunk, and
queues a repair fetch.

**Keys.** The path after the leading `/` is the key, percent-decoded, and
must be under 1 KiB. vcpkg's `{name}/{version}/{sha}` and sccache's
`<prefix>/a/b/c/<hash>` both fit. Shard routing is unchanged:
`KYTHIRA_SHARD_CUTS` and automatic splits treat these keys as the byte
strings they are.

## Component 7: Authentication cache

`Authorization: Bearer <secret>` or `Basic user:secret` is checked against
`redis_acl`. A bearer token is matched by trying each user's record, so with
more than a handful of users the client should use Basic.

A success is cached as `HMAC-SHA256(process_key, user ‖ secret) → (user,
expiry)`, where `process_key` is random per process. The cache never stores
the secret. It is flushed on ACL reload and bounded at 1,024 entries.
Failures are not cached; `redis_auth_limiter` rate-limits them as on RESP.

## Configuration

| Variable | Default | Meaning |
|---|---|---|
| `KYTHIRA_CACHE_DATA_DIR` | none, required for `file` | Chunk store and Raft state |
| `KYTHIRA_CACHE_PERSISTENCE` | `file` | `file` or `memory` (today's behaviour, values in RAM) |
| `KYTHIRA_CACHE_CHUNK_BYTES` | 4 MiB | Chunk size, at most 16 MiB |
| `KYTHIRA_CACHE_MAX_VALUE_BYTES` | 2 GiB | Largest value over HTTP and RESP |
| `KYTHIRA_CACHE_MAX_DISK_BYTES` | none, required | Disk budget per node |
| `KYTHIRA_CACHE_MIN_FREE_BYTES` | 1 GiB | Free-space floor below which writes are refused |
| `KYTHIRA_CACHE_MIN_FREE_INODES` | 10000 | Free-inode floor below which writes are refused |
| `KYTHIRA_CACHE_PACK_THRESHOLD_BYTES` | 64 KiB | Chunks smaller than this go to pack files |
| `KYTHIRA_CACHE_ORPHAN_GRACE_ENTRIES` | 100000 | Log entries an uncommitted chunk survives |
| `KYTHIRA_CACHE_FETCH_BYTES_PER_SEC` | 50000000 | Peer fetch rate limit |
| `KYTHIRA_CACHE_HTTP_LISTEN` / `_HTTPS_LISTEN` | empty | Front-end listeners |
| `KYTHIRA_CACHE_ALLOW_PLAINTEXT_HTTP` | `false` | Required to enable the plaintext listener |
| `KYTHIRA_CACHE_AUTH_CACHE_SECONDS` | 300 | Successful-auth cache lifetime |

With `KYTHIRA_CACHE_PERSISTENCE=memory`, the daemon keeps today's state
machine and engine, and the variables above that need a data directory are
refused. That keeps the existing deployment and its tests unchanged.

## Containers

`docker/cache-cluster/Dockerfile` follows the gateway's existing image: a
host-built binary on `ubuntu:24.04` with runtime libraries only, a
`USER 10001`, and a `VOLUME /data`.
`docker/cache-cluster/compose.yml` starts three nodes named `cache-1` to
`cache-3`, which find each other by service name, with one named volume each
and no static IPs. The chaos scenarios run it through `container_runtime()` and
`compose_prefix()` (`tests/docker_chaos/os_faults.hpp`).

## Correctness properties

1. **Determinism.** For any log, two replicas that apply it end with equal
   `get_state()`, whatever chunks each had on disk beforehand.
2. **Atomic visibility.** A key read from any replica is either its previous
   value or the complete new value, hash-correct, and never a mix.
3. **No unreferenced growth.** After the orphan grace period, every chunk
   file and every live pack record on disk is referenced by a committed
   manifest or by a pending entry.
4. **Split/absorb law.** Unchanged from `splittable_state_machine.hpp`, over
   the manifest format.
5. **Bounded RAM.** Resident memory grows with key count and connections, not
   with stored bytes.

Each property gets a property test in the style of the existing
`*_property_test.cpp` suites.

## Error handling

| Condition | Behaviour |
|---|---|
| Chunk write fails in `apply` | Fatal log, process exits, replica rejoins on restart |
| Free space or inodes below floor, here or on any voter of the shard | 507 on `PUT`, `-ERR` on `SET`, reads continue |
| Chunk missing on a follower | Forward the read to the leader |
| Chunk missing on the leader | Miss (404 or nil), queue a fetch |
| Hash mismatch on read | Miss before headers, abort after; drop the chunk and queue a repair |
| No shard leader | 503 with `Retry-After`; clients treat it as a miss (verified in task 1) |
| Client disconnect mid-`PUT` | No `commit`; chunks age out as orphans |
| Torn log tail | Truncate on open; anything else corrupt is fatal |

## Risks

- **Large uploads and the commit latency.** A 500 MB `PUT` is about 120 Raft
  rounds. Pipelining two chunks keeps the link busy, but a slow follower
  still sets the pace. Task 9 measures it. If it is too slow, the leader could
  commit after a majority has each chunk, which is already Raft's rule, and
  not wait on a slow follower.
- **sccache's WebDAV behaviour may differ from OpenDAL's documentation.**
  That is why task 1 records it before task 6.
- **Snapshot manifest growth.** 10 MB per 77,000 keys is fine. At millions of
  keys per shard, splitting keeps each shard's snapshot bounded, which is
  already how `multi_raft` scales.

# The Redis-compatible KV gateway

`redis_gateway_node` is a replicated, sharded key/value store that speaks
enough of the Redis protocol to be sccache's `redis` cache backend. It is the
daemon built by `CONFIG_REDIS_GATEWAY` (`services/redis/cmd/redis_gateway_node`), a RESP2/RESP3
front end (`services/redis/include/kythira/redis/redis_gateway.hpp`) over the `multi_raft` sharding
layer, with a state machine that splits with its shard
(`services/redis/include/kythira/redis/redis_kv_state_machine.hpp`) and an ACL with PBKDF2-hashed
secrets (`services/redis/include/kythira/redis/redis_acl.hpp`).

It is deliberately not a Redis. The command set is exactly the closure sccache
puts on the wire through OpenDAL and redis-rs — `GET SET SETEX DEL EXISTS
STRLEN GETRANGE PING AUTH HELLO SELECT CLIENT SETINFO QUIT RESET` — plus the
diagnostics an operator reaches for (`COMMAND`, `INFO`, `DBSIZE`, `TTL`,
`ECHO`). Everything else is refused with Redis's own `unknown command` error.
A build with the transaction adapter adds the multi-key commands of
*Transactions* below, off unless asked for.
The design, its requirements and its acceptance tests are in
`.kiro/specs/redis-compatible-kv/`.

## Running a cluster

Every voter runs one daemon. A daemon is configured entirely through the
environment; the only arguments are `--help` and the `--hash-secret`
subcommand.

```sh
export KYTHIRA_NODE_ID=1
export KYTHIRA_PEERS="1=http://kv1:7000,2=http://kv2:7000,3=http://kv3:7000"
export KYTHIRA_SHARD_CUTS="sccache/8"
export KYTHIRA_REDIS_ACL_FILE=/etc/kythira/acl.txt
export KYTHIRA_REDIS_INTERNAL_SECRET="$(cat /run/secrets/kythira-internal)"
export KYTHIRA_REDIS_TLS_LISTEN=0.0.0.0:6380
export KYTHIRA_REDIS_TLS_CERT=/etc/kythira/kv.crt   # names this node's host
export KYTHIRA_REDIS_TLS_KEY=/etc/kythira/kv.key
export KYTHIRA_REDIS_TLS_CA=/etc/kythira/ca.crt     # the CA the peers chain to
redis_gateway_node
```

With a TLS listener, forwarding goes to each peer's TLS listener (`kv2:6380`
here, derived from `KYTHIRA_PEERS` and the TLS listen port; set
`KYTHIRA_REDIS_PEER_GATEWAYS` when the peers listen elsewhere). See
*Forwarding security* below.

`services/redis/docker/sccache-e2e-compose.yml` is a complete three-node example, and
`services/redis/docker/redis_gateway_node/Dockerfile` packages the host-built binary the same
way the other daemons in `docker/` are packaged (`make docker-redis-gateway-image`).

### Environment variables

Raft and sharding:

| Variable | Default | Meaning |
|---|---|---|
| `KYTHIRA_NODE_ID` | required | This node's id: a positive integer, or in a `KYTHIRA_NODE_ID_KIND=text` build also canonical composite text or `metadata:<provider>` (see `README.md`, "Node ids in the host binaries") |
| `KYTHIRA_PEERS` | required | `id=http://host:port,...` for every voter, this node included; each id in the form `KYTHIRA_NODE_ID` takes, never `metadata:` |
| `KYTHIRA_RAFT_BIND` | `0.0.0.0` | Bind address for Raft RPCs |
| `KYTHIRA_ALLOW_PLAINTEXT_RAFT` | `false` | Raft RPC has no TLS. The daemon refuses to start on a non-loopback `KYTHIRA_RAFT_BIND` unless this is `true`, and warns at every start when it is |
| `KYTHIRA_RAFT_PORT` | `7000` | Raft RPC port |
| `KYTHIRA_WIRE_SERIALIZER` | `cbor` | `cbor` or `json` for Raft RPCs; see *Serializer* below |
| `KYTHIRA_SHARD_CUTS` | none | Comma-separated initial shard boundaries; empty means one shard |
| `KYTHIRA_TICK_INTERVAL_MS` | `2` | Raft tick |
| `KYTHIRA_ELECTION_TIMEOUT_MIN_MS` / `_MAX_MS` | `150` / `300` | Election timeout range |
| `KYTHIRA_HEARTBEAT_INTERVAL_MS` | `50` | Leader heartbeat |
| `KYTHIRA_RPC_TIMEOUT_MS` | `1000` | Per-RPC timeout |
| `KYTHIRA_POLICY_INTERVAL_MS` | `10000` | How often the host's placement policy (splits, eviction, expiry sweeps) runs |
| `KYTHIRA_EXECUTOR_STRIPES` | `4` | multi_raft executor stripes |
| `KYTHIRA_LOG_LEVEL` | `info` | `trace`, `debug`, `info`, `warning`, `error` or `critical`, applied to the daemon and every per-shard logger |

The Redis front end:

| Variable | Default | Meaning |
|---|---|---|
| `KYTHIRA_REDIS_LISTEN` | `0.0.0.0:6379` | Plaintext RESP listener. The daemon warns at every start that secrets and values cross this unencrypted; set it empty to run TLS-only |
| `KYTHIRA_REDIS_TLS_LISTEN` | none | TLS listener; needs `_TLS_CERT` and `_TLS_KEY` |
| `KYTHIRA_REDIS_TLS_CERT` / `_TLS_KEY` / `_TLS_CA` | none | PEM server certificate, key and (for client certificates) the CA to verify them against |
| `KYTHIRA_REDIS_REQUIRE_CLIENT_CERT` | `false` | Refuse TLS clients without a certificate |
| `KYTHIRA_REDIS_ACL_FILE` | required unless anonymous | The ACL file; format below. A file that fails to parse stops the daemon before it binds a port |
| `KYTHIRA_REDIS_ALLOW_ANONYMOUS` | `false` | Run with no ACL at all. Every connection is an admin. The daemon warns at every start |
| `KYTHIRA_REDIS_READ_CONSISTENCY` | `leader` | `leader` (reads go to the shard leader), `any_replica` (any local replica answers, may lag) or `linearizable` (a read goes through the log) |
| `KYTHIRA_REDIS_MAX_VALUE_BYTES` | `8388608` | Largest value a `SET` may carry; larger is refused |
| `KYTHIRA_REDIS_MAX_CLIENTS` | `1024` | Connection cap; beyond it new connections are refused |
| `KYTHIRA_REDIS_IDLE_TIMEOUT_MS` | `300000` | Idle connections are closed after this |
| `KYTHIRA_REDIS_COMMAND_TIMEOUT_MS` | `5000` | A command that cannot be committed in this time answers with an error |
| `KYTHIRA_REDIS_MAX_SHARD_BYTES` | `1073741824` | Per-shard budget; over it the leader evicts least-recently-used keys (the LRU is the leader's own view and starts over on a leadership change) |
| `KYTHIRA_REDIS_SWEEP_BATCH` | `1024` | Expired keys removed per sweep command |
| `KYTHIRA_REDIS_IMMUTABLE_VALUES` | `true` | A `SET` to an existing key with different bytes is refused. sccache keys are content hashes, so a conflicting write is a corrupted client, not an update |
| `KYTHIRA_REDIS_FORWARDING` | `true` | Forward a command whose shard this node does not lead to the leader's gateway, one hop. Off, the client gets a retryable error instead |
| `KYTHIRA_REDIS_INTERNAL_USER` | `kythira-internal` | The ACL user forwarded commands authenticate as on the peer |
| `KYTHIRA_REDIS_INTERNAL_SECRET` | none | That user's secret. Empty disables forwarding. The daemon appends the user to the loaded ACL itself, so the file never carries it |
| `KYTHIRA_REDIS_FORWARD_TLS` | `true` when `TLS_LISTEN` is set | Forward to the peers' TLS listeners |
| `KYTHIRA_REDIS_FORWARD_TLS_CA` | `_TLS_CA` | CA every peer's certificate must chain to. Forwarding over TLS without one refuses to start |
| `KYTHIRA_REDIS_FORWARD_TLS_CERT` / `_KEY` | `_TLS_CERT` / `_TLS_KEY` | Client certificate presented to peers that require one; it needs the `clientAuth` extended key usage, or none |
| `KYTHIRA_REDIS_ALLOW_PLAINTEXT_FORWARDING` | `false` | Forward over plaintext TCP, sending the internal secret unencrypted. Without this or TLS forwarding, a command for a shard led elsewhere gets the retry error |
| `KYTHIRA_REDIS_PEER_GATEWAYS` | peer host + `LISTEN` port (`TLS_LISTEN` port when forwarding over TLS) | `id=host:port,...` of every peer's RESP listener, for forwarding |
| `KYTHIRA_REDIS_IO_THREADS` | `2` | Listener/IO threads |
| `KYTHIRA_REDIS_WORKER_THREADS` | `8` | Command workers |
| `KYTHIRA_REDIS_LOG_COMMANDS` | `false` | Log every command (keys, never values or secrets) at debug |
| `KYTHIRA_REDIS_TRANSACTIONS` | `off` | Run `MSET`, multi-key `DEL`, `MGET` and `MULTI`/`EXEC`/`DISCARD` as cross-shard transactions; see *Transactions*. Refused at startup by a build without `CONFIG_REDIS_GATEWAY_TRANSACTIONS=y`, and with `KYTHIRA_CACHE_PERSISTENCE=file` |
| `KYTHIRA_REDIS_TXN_ISOLATION` | `read_committed` | Each connection's isolation until it sends `KYTHIRA.ISOLATION`. A level the build cannot give refuses to start |
| `KYTHIRA_REDIS_TXN_MAX_RETRIES` | `2` | Automatic retries of a transaction that met another one's lock, each under a fresh transaction id |
| `KYTHIRA_REDIS_TXN_LOCK_TTL_MS` | `3000` | How long a prepared write stays locked without its coordinator's heartbeat before any party may abort the transaction |

`KYTHIRA_RAFT_BIND` and the host part of the two listen addresses accept:

- An IPv4 or IPv6 literal. `0.0.0.0` is IPv4 only and `::` is IPv6 only.
  In a listen address, write IPv6 in brackets, for example `[::1]:6379`.
- `*`, which listens on both `0.0.0.0` and `::` with separate sockets.
- A host name listed in `/etc/hosts`. DNS is never consulted. The daemon
  listens on every address the name is listed under, so `localhost` covers
  `127.0.0.1` and `::1`. Each address must belong to this host, or the
  daemon refuses to start.

The `KYTHIRA_CACHE_*` variables of the disk-backed mode, which is the
default, are under *The disk-backed cache* below.

### Shard cuts

Keys are plain byte strings ordered lexicographically, and a shard is a
half-open range of them. `KYTHIRA_SHARD_CUTS` gives the initial boundaries;
the host splits shards further as they grow. sccache keys have the shape
`<prefix><h>/<h>/<h>/<64 hex>` where `h` is a hex digit of the hash, so with
`SCCACHE_REDIS_KEY_PREFIX=sccache/` the cut `sccache/8` puts half the keys in
each shard. Cuts that sit outside the keys actually written are harmless but
useless.

## The ACL file

One user per line; `#` starts a comment and blank lines are ignored:

```
user <name> <secret-record|nopass> <role> [prefix ...] [cert=<subject>]
```

- `<secret-record>` is `pbkdf2-sha256$<iterations>$<base64 salt>$<base64 key>`
  as printed by `redis_gateway_node --hash-secret [iterations]`, which reads
  the secret from stdin so it never appears on a command line or in shell
  history. `nopass` means the user has no password: it can only connect
  with a client certificate, so the line must carry a `cert=` (the ACL is
  rejected otherwise) and `AUTH` as that user always fails. `disabled` in
  this position keeps the line but lets nobody authenticate as the user.
- `<role>` is `read_only` (`GET`, `EXISTS`, `STRLEN`, `GETRANGE`, `TTL`,
  `DBSIZE`, `COMMAND` and the connection commands), `read_write` (adds `SET`,
  `SETEX`, `DEL`) or `admin` (adds `INFO`). `ro` and `rw` are accepted
  abbreviations.
- Each `prefix` is a key prefix the user may touch; `*` means every key. A
  user with no prefix can run `PING` and nothing that names a key: scope is
  granted, never assumed.
- `cert=<subject>` maps an mTLS client certificate's subject to this user,
  so it is authenticated on connect without an `AUTH`.

Example, the shape of a build farm:

```
# the main-branch runners populate the cache
user ci-main   pbkdf2-sha256$600000$...$...  read_write sccache/
# PR runners read it and cannot poison it
user ci-pr     pbkdf2-sha256$600000$...$...  read_only  sccache/
# operators
user ops       pbkdf2-sha256$600000$...$...  admin      *  cert=CN=ops.example
```

A refused command answers `-NOPERM ...` and the daemon logs one audit line
per refusal (`redis audit`, tagged `stream=audit`) naming the user, the source
address, the command and the reason, and never the secret or the value.
Failed `AUTH`s are rate-limited per source (the IPv4 address, or the IPv6
/64; not per connection, so reconnecting does not reset the count), because
each one costs a PBKDF2 derivation. An attempt is counted when it starts and
refunded if it succeeds, so concurrent guesses cannot overrun the limit.
Clients behind one NAT share a budget; size `_auth_failure_limit`
accordingly. At most `_max_concurrent_auth` derivations run at once across
the gateway (default: half the worker pool); past that `AUTH` answers
`-ERR too many concurrent authentication attempts, retry later`, which
keeps the other workers free for authenticated traffic.

Until a connection authenticates it is held to Redis's limits for
unauthenticated clients: at most 10 arguments of at most 16 KiB each, parsed
one command at a time. A client that pipelines `AUTH` and a large `SET` in one
write is fine; the `SET` is parsed after `AUTH` has run. A client that stops
reading its replies stops having its commands executed once
`_max_output_buffer_bytes` (32 MiB; 64 KiB before `AUTH`) of replies are
waiting for it, and the idle timeout eventually closes it.

## Configuring sccache

sccache's `redis` backend needs nothing beyond its standard variables:

```sh
export SCCACHE_REDIS_ENDPOINT=tcp://kv1:6379      # rediss:// for the TLS listener
export SCCACHE_REDIS_USERNAME=ci-main
export SCCACHE_REDIS_PASSWORD=...
export SCCACHE_REDIS_KEY_PREFIX=sccache/          # must match the user's ACL prefix
export SCCACHE_REDIS_EXPIRATION=1209600           # optional; makes every write a SETEX
```

Any node's listener will do as the endpoint: a node that does not lead the
key's shard forwards the command one hop. Point different runners at
different nodes to spread the connection load.

### Forwarding security

A forwarded command reaches the peer on a connection authenticated as
`KYTHIRA_REDIS_INTERNAL_USER`, which can read and write every key, so that
connection's `AUTH` must not cross the network in the clear. The gateway
forwards in one of two ways:

- **TLS** (the default once `KYTHIRA_REDIS_TLS_LISTEN` is set). The peer's
  certificate must chain to `KYTHIRA_REDIS_FORWARD_TLS_CA` and name the host
  in the peer's endpoint: a DNS SAN for a name, an IP SAN for an address.
  The node presents its own certificate, so peers may require client
  certificates on their TLS listener.
- **Plaintext**, only with `KYTHIRA_REDIS_ALLOW_PLAINTEXT_FORWARDING=true`,
  for a network private to the cluster. The daemon warns at every start.

With neither, forwarding is off in effect: the gateway logs why at startup,
and a command for a shard led elsewhere gets
`-ERR shard has no reachable leader, retry`. Every forward, connect and TLS
handshake included, must finish within `KYTHIRA_REDIS_COMMAND_TIMEOUT_MS`;
a peer that stops answering costs one command timeout and the retry error.

**`SCCACHE_REDIS_RW_MODE=READ_ONLY` is a client-side convenience, not a
control.** It stops a runner from *trying* to write; the gateway's `read_only`
role is what stops it from *succeeding*. Give a PR runner both — the role for
safety and the mode so its sccache does not spend a round trip per compile
being told no — but never rely on the mode alone, because the password it
holds is the same one every other runner holds.

### What sccache does when the cache is unreachable

sccache 0.10 probes its storage when its server starts: it writes
`<prefix>.sccache_check` and reads it back. A refused write only demotes the
server to read-only mode (hits are still served; every attempted store counts
as a `cache_write_error`). But a read that fails — the gateway down, a user
whose prefix does not cover `.sccache_check` — aborts the server, and every
subsequent `sccache rustc` fails with `Server startup failed`. Left as it is,
that makes an unreachable cache a build dependency.

The fix is in the CI job, not the gateway: start the server explicitly and
only export the wrapper if that worked.

```sh
if sccache --start-server; then
    export RUSTC_WRAPPER=sccache
fi
```

`services/redis/docker/sccache_runner/run.sh` does exactly this, and the acceptance test
`docker_sccache_e2e_test` (`make docker-sccache-e2e-tests`) asserts that a
build with every gateway stopped still succeeds. It also asserts the read-only
runner's builds succeed with a non-zero `cache_write_errors`, and that a user
scoped to another prefix gets an uncached build rather than a failed one.

## The disk-backed cache

By default (`KYTHIRA_CACHE_PERSISTENCE=file`) a node keeps values on disk
and only the key index in RAM, so a node with 1 GB of memory can serve a
cache much larger than that. It also serves an HTTP object API next to
RESP, which is what sccache's WebDAV backend and vcpkg's `http` binary
source speak. `KYTHIRA_CACHE_PERSISTENCE=memory` is the older mode with
every value in RAM. It refuses the variables below that need a data
directory, and the rest of this section does not apply to it.

`services/redis/docker/cache-cluster/compose.yml` runs three nodes this way as uid 10001,
with no capabilities, under Docker or rootless Podman, and
`services/redis/docker/cache-cluster/client/` holds a real sccache and vcpkg pointed at it.
The design is `.kiro/specs/disk-backed-build-cache/`.

### Variables

| Variable | Default | Meaning |
|---|---|---|
| `KYTHIRA_CACHE_PERSISTENCE` | `file` | `file`, or `memory` for values in RAM. `file` needs `KYTHIRA_WIRE_SERIALIZER=cbor` |
| `KYTHIRA_CACHE_DATA_DIR` | required | Chunk store and Raft state; see *Disk layout* |
| `KYTHIRA_CACHE_MAX_DISK_BYTES` | required | This node's disk budget. Over it, shard leaders evict least-recently-read keys, counting reads served by followers |
| `KYTHIRA_CACHE_MIN_FREE_BYTES` | `1073741824` | Below this much free space on the data filesystem, writes are refused with a retryable error and reads continue |
| `KYTHIRA_CACHE_MIN_FREE_INODES` | `10000` | The same for free inodes (`statvfs` `f_favail`). A leader also refuses while any voter of the shard reports itself under either floor |
| `KYTHIRA_CACHE_CHUNK_BYTES` | `4194304` | Values are split into chunks of this size, at most 16 MiB. One chunk is one Raft entry |
| `KYTHIRA_CACHE_PACK_THRESHOLD_BYTES` | `65536` | Chunks smaller than this go into shared pack files instead of a file each, so small values cannot exhaust inodes |
| `KYTHIRA_CACHE_MAX_VALUE_BYTES` | `2147483648` | Largest value an HTTP `PUT` may carry; larger answers 413 |
| `KYTHIRA_CACHE_ORPHAN_GRACE_ENTRIES` | `100000` | Log entries a chunk with no committed key survives before it is collected (the chunks of a `PUT` whose client went away) |
| `KYTHIRA_CACHE_FETCH_BYTES_PER_SEC` | `50000000` | Rate limit for fetching missing chunks from peers |
| `KYTHIRA_CACHE_SNAPSHOT_THRESHOLD_BYTES` | `268435456` | Applied bytes after which a shard snapshots and compacts its log. A snapshot names chunks by hash, so a node that installs one fetches the chunks from peers |
| `KYTHIRA_CACHE_HTTP_LISTEN` | empty | Plaintext HTTP listener, `host:port`. Refused unless `KYTHIRA_CACHE_ALLOW_PLAINTEXT_HTTP=true` |
| `KYTHIRA_CACHE_ALLOW_PLAINTEXT_HTTP` | `false` | Allow the plaintext listener, which carries credentials and values unencrypted |
| `KYTHIRA_CACHE_HTTPS_LISTEN` | empty | TLS listener |
| `KYTHIRA_CACHE_HTTPS_CERT` / `_HTTPS_KEY` | `KYTHIRA_REDIS_TLS_CERT` / `_TLS_KEY` | PEM certificate and key for the TLS listener |
| `KYTHIRA_CACHE_PEER_HTTP` | each peer's host with this node's listener port | `id=host:port,...` of every peer's HTTP front end, the kind this node forwards over (TLS when `KYTHIRA_REDIS_FORWARD_TLS` is on) |
| `KYTHIRA_CACHE_AUTH_CACHE_SECONDS` | `300` | How long a successful authentication is remembered |

Two gateway variables change meaning in file mode:

- `KYTHIRA_REDIS_MAX_VALUE_BYTES` may be at most 16 MiB, because a RESP
  `SET` is stored as one chunk. Larger values go over HTTP.
- `KYTHIRA_REDIS_MAX_SHARD_BYTES` applies only when set. Left unset, the
  disk budget is what bounds a shard; the 1 GiB default meant for RAM
  evicted keys long before the disk was full.

Forwarding, TLS for forwarding, `KYTHIRA_REDIS_INTERNAL_SECRET` and the ACL
are shared with RESP: an HTTP request for a shard another node leads is
relayed to the leader's front end with the client's own credentials, one
hop, under the same plaintext and TLS rules as a forwarded command.

### HTTP API

The path after the leading `/` is the key, percent-decoded, under 1 KiB.
Authentication is `Authorization: Basic` with an ACL user and its secret,
or `Bearer <secret>`. Basic is cheaper with more than a few users, because
a bearer token is tried against each user's record. The role and prefixes
are the user's ACL line, exactly as on RESP.

| Request | Role | Answer |
|---|---|---|
| `GET /<key>` | `read_only` | 200 with the value streamed, 206 for one `Range`, 404 on a miss |
| `HEAD /<key>` | `read_only` | 200 with `Content-Length` and `ETag` (the value's SHA-256), or 404 |
| `PUT /<key>` | `read_write` | 201 once committed; 413 over `KYTHIRA_CACHE_MAX_VALUE_BYTES`; 507 under a free-space or inode floor |
| `DELETE /<key>` | `read_write` | 204, or 404 |
| `PROPFIND /<dir>/` | `read_write` | 207 for any collection; 207 or 404 for a key |
| `MKCOL /<dir>/` | `read_write` | 201, and does nothing |
| `GET /_metrics` | `admin` | Counters as text, one `name value` per line |
| `GET /_chunk/<sha>` | internal | One chunk, for peers only (`KYTHIRA_REDIS_INTERNAL_SECRET`) |

401 means no or wrong credentials, and 403 a user whose role or prefixes do
not cover the request.

A `PUT` holds at most two chunks of its body in memory at a time, and a
client that disconnects mid-body leaves the old value in place. Every chunk
is hash-checked as it is read. A bad first chunk is a miss, or the leader's
copy where the node forwards. A bad later chunk is found after the headers
have gone out, so the node closes the connection and the client sees a
short body, never a wrong one. Either way the node drops the bad chunk and
fetches a good copy from a peer.

**The WebDAV subset** is just what sccache 0.17.0's WebDAV backend sends:
`GET`, `PUT`, `PROPFIND` and `MKCOL`. sccache sends a `PROPFIND` on the
parent collection before every `PUT`, and only on the write path. The node
answers 207 for any collection path because keys have no real directories,
so a `PUT` costs one `PROPFIND` and the `PUT`, with no `MKCOL` walk. There
are no locks, no `COPY` or `MOVE`, and no listings.

### Disk layout

```
DATA_DIR/
  chunks/ab/cd/abcd…      one chunk at or over the pack threshold, by SHA-256
  chunks/tmp/             writes in progress, cleared at start
  packs/pack.<n>          smaller chunks, appended; a .dead sidecar per pack
  raft/<group>/           each shard replica's log, snapshots and state
```

Chunks are content-addressed and shared by every key and every shard
replica on the node that uses them. They are deleted when the last key that
references them is gone. A pack is closed at 64 MiB and compacted once its
live bytes fall under half. So the file count follows the disk budget, not
the key count: the task 11 measurement stored a full g++ build of this
repository, 752 objects and 288 MB, in 611 files per node.

`INFO` has a `# Cache` section (`cache_keys`, `cache_chunks`,
`cache_store_bytes`, `cache_store_files`, `cache_loose_chunks`,
`cache_packed_chunks`, `cache_hash_failures`, `cache_fetch_pending_chunks`,
`cache_fetched_chunks`, `cache_fs_free_bytes`, `cache_fs_free_inodes`,
`cache_space_refusals` and more), and `GET /_metrics` has the HTTP
counters (`kythira_cache_http_requests`, `_hits`, `_misses`, `_puts`,
`_forwards`, `_forward_failures`, `_auth_failures`, `_forbidden`, ...)
plus the store's.

### Clients

The same ACL users serve RESP and HTTP. A build farm usually has a
`read_write` user for main-branch runners and a `read_only` user for pull
requests:

```
user ci-main  pbkdf2-sha256$600000$...$...  read_write  sccache/ vcpkg/
user ci-pr    pbkdf2-sha256$600000$...$...  read_only   sccache/ vcpkg/
```

**sccache (0.17 or later) over WebDAV.** Any node will do as the endpoint:

```sh
# read-write: main-branch runners
export SCCACHE_WEBDAV_ENDPOINT=https://cache1.example:8443/sccache
export SCCACHE_WEBDAV_USERNAME=ci-main
export SCCACHE_WEBDAV_PASSWORD=...

# read-only: pull-request runners
export SCCACHE_WEBDAV_ENDPOINT=https://cache1.example:8443/sccache
export SCCACHE_WEBDAV_USERNAME=ci-pr
export SCCACHE_WEBDAV_PASSWORD=...
export SCCACHE_WEBDAV_RW_MODE=READ_ONLY
```

As with `SCCACHE_REDIS_RW_MODE`, `READ_ONLY` is a client-side courtesy:
the `read_only` role is what stops a write, and the mode stops sccache from
spending a request per compile being told no. Measured with both set, a
full warm build served every object and attempted no write. Unset any
`SCCACHE_BUCKET` / `SCCACHE_S3_*` variables on the same runner: sccache
picks S3 over WebDAV when both are configured. The `.sccache_check` probe
at server start is the cache's availability contract, as on RESP; start
the server explicitly and only export the wrapper if it started.

**vcpkg's `http` binary source.** The template is `{name}/{version}/{sha}`
under a prefix the user may touch, and the fourth field is a header vcpkg
sends with every request:

```sh
# read-write
auth="Authorization: Basic $(printf '%s:%s' ci-main "$SECRET" | base64 -w0)"
export VCPKG_BINARY_SOURCES="clear;http,https://cache1.example:8443/vcpkg/{name}/{version}/{sha},readwrite,${auth}"

# read-only
auth="Authorization: Basic $(printf '%s:%s' ci-pr "$SECRET" | base64 -w0)"
export VCPKG_BINARY_SOURCES="clear;http,https://cache1.example:8443/vcpkg/{name}/{version}/{sha},read,${auth}"
```

The header carries the secret, so keep `VCPKG_BINARY_SOURCES` out of logs.

### Measured

Task 11 built this repository through the cluster on a GitHub-hosted
runner, with the three nodes on that same runner. A warm g++-13 build took
5.30 minutes against 5.58 through the OCI bucket on the same runner and
commit. All 752 objects were hits, at 3.8 ms each against OCI's 168 ms,
with no read or write errors. Each node's memory peaked at 240 MiB or
less. The full numbers are in `.kiro/specs/disk-backed-build-cache/tasks.md`.

## Transactions

A daemon built with `CONFIG_REDIS_GATEWAY_TRANSACTIONS=y` and run with
`KYTHIRA_REDIS_TRANSACTIONS=on` turns the multi-key commands into
transactions across shards (`.kiro/specs/multi-raft-transactions/`, task 9).
The build option wraps the key/value state machine in
`transactional_state_machine`, which changes its snapshot format, so every
node of a cluster must be built the same way. Only the in-memory store is
wrapped: transactions need `KYTHIRA_CACHE_PERSISTENCE=memory`, and a node
with file persistence refuses `KYTHIRA_REDIS_TRANSACTIONS=on` at startup.
With the option off, or
transactions off, nothing below exists: those commands answer `unknown
command`, and `DEL` takes one key, as before.

| Command | What it does |
|---|---|
| `MSET k v [k v ...]` | Every pair, all or none. Under `KYTHIRA_REDIS_IMMUTABLE_VALUES` a key that already holds the same bytes is left out, and one holding other bytes refuses the whole command |
| `MGET k [k ...]` | Each key's committed value at the connection's isolation, or a null |
| `DEL k [k ...]` | Deletes, in one transaction, every key that was present; replies with how many. `DEL` with one key is the plain single-key command |
| `MULTI` ... `EXEC` | Queues `GET`, `MGET`, `EXISTS`, `SET`, `SETEX`, `MSET` and `DEL`, then runs them as one transaction. Reads run at `EXEC` and see the transaction's own earlier writes. If the commit does not commit, `EXEC` answers with that error and nothing applied. A command refused while queuing (unsupported, bad arguments, not permitted) makes `EXEC` answer `EXECABORT`, as in Redis |
| `DISCARD` | Drops the queue |
| `KYTHIRA.ISOLATION [level]` | Shows or sets this connection's isolation |
| `WATCH k [k ...]`, `UNWATCH` | Snapshot build only (refused on the tier-1 build). The next `EXEC` commits only if no watched key changed since the first `WATCH`; otherwise it answers a null array and applies nothing. `EXEC`, `DISCARD` and `UNWATCH` clear the watch. See below |

Replies that are not Redis's own:

- `-ERR transaction aborted: <why>; nothing applied, retry`: the transaction
  met another one's lock (after `KYTHIRA_REDIS_TXN_MAX_RETRIES` automatic
  retries), or its own locks expired first. Safe to retry.
- `-ERR transaction <id> outcome unknown; it may have applied (...)`: the
  commit point was proposed and its result was lost, for example to a leader
  change. Read the keys before retrying a non-idempotent write.
- `-ERR key is locked by transaction <id>, retry` on a plain `SET`, `SETEX`
  or `DEL`: another transaction holds the key. The gateway first asks that
  transaction's primary shard to finish it, which aborts it if its lock TTL
  has passed, and tries once more; this is the answer when the lock is still
  live.

The coordinator runs on whichever gateway the client is connected to. Its
messages to each shard go to the local replica when it leads, and otherwise
over the forward link to the leader's gateway as an internal `KYTHIRA.TXN`
command. A gateway accepts `KYTHIRA.TXN` only on a connection authenticated
as `KYTHIRA_REDIS_INTERNAL_USER` and never forwards it again, so transactions
need forwarding configured the same way plain forwarding does (an internal
secret, and TLS or the plaintext opt-in). The host's background resolver uses
the same link to finish transactions whose coordinator died.

### Snapshot isolation

A daemon built with `CONFIG_REDIS_GATEWAY_SNAPSHOT_ISOLATION=y` as well
keeps every value multi-versioned (`redis_kv_mvcc_state_machine` under
`mvcc::on`, tasks 11 to 14) and runs a timestamp oracle group on every node
beside the shards. `KYTHIRA_REDIS_TXN_ISOLATION=snapshot` and
`KYTHIRA.ISOLATION snapshot` are then accepted, and an `MGET` (or the reads
of a `MULTI`) under `snapshot` reads every key at one start timestamp, so
the transfer above can never show up half done.

What changes with the build:

- **Every write takes two Raft rounds and a timestamp.** A plain `SET`,
  `SETEX` or `DEL` is a prepare, an oracle timestamp, and a commit, as is a
  single-shard transaction. A node that does not lead the oracle group asks
  its leader's gateway for timestamps over `KYTHIRA.TXN TS`.
- **Snapshot reads go to each shard's leader** after a read-index round, over
  `KYTHIRA.TXN READAT` when this node does not lead it. A key held by a
  transaction that started at or below the read's timestamp waits for that
  transaction (or resolves it once its lock TTL has passed); one that started
  later is invisible to the read.
- **Old versions are collected.** Each shard keeps versions newer than its
  safe point, an oracle timestamp at least ten minutes old. A snapshot older
  than that fails with `snapshot ... is older than the safe point`; retry it.
- **The snapshot format changes again**, so every node of a cluster must be
  built the same way, and a tier-1 cluster cannot be upgraded to this build
  in place.
- **Write-write conflicts are first committer wins**: a transaction that
  writes a key someone else committed after it started aborts with a write
  conflict, and is not retried automatically, since a retry would be a lost
  update.
- **`serializable` is offered too** (task 16). Every key a transaction reads
  is validated when it commits: if a version of it committed after the start
  timestamp, or another transaction holds it, the commit aborts with
  `a key it read changed after it started` and applies nothing. Validated
  keys stay read-locked until the commit finishes, so a writer meeting one
  waits or retries as it would for a write lock. A read-only transaction (an
  `MGET`) commits without any of this.
- **`WATCH`** takes an oracle timestamp at the first `WATCH`, and the next
  `EXEC` runs as a `serializable` transaction started there, with every
  watched key validated. Unlike Redis, a key changed between two `WATCH`
  commands also aborts the `EXEC` (they share the first timestamp), and the
  reads queued in the `MULTI` see the data as of the `WATCH`, not as of the
  `EXEC`. An `EXEC` with no writes therefore never aborts: its reads are
  one consistent snapshot taken at the `WATCH`.

### Isolation levels

Each level, and the anomaly it still allows. Every example uses a transfer
that `MSET`s `a/alice` from 100 to 90 and `z/bob` from 0 to 10; the two keys
are on different shards.

| Level | Built | A read sees | Still allowed |
|---|---|---|---|
| `read_committed` (default) | always | The last committed value. A key locked by an in-flight transaction reads as its value before that transaction | **Read skew.** The transfer commits at the instant its record on `a/alice`'s shard commits; `z/bob` is rolled forward a moment later. An `MGET a/alice z/bob` in between returns `90` and `0`: the money is briefly nowhere. Also non-repeatable reads, lost updates (two read-modify-write clients) and write skew |
| `read_committed_wait` | always | As above, but a read of a key another transaction holds waits until that transaction finishes (or resolves it once its TTL has passed), up to the command timeout | **Read skew** across reads that straddle a commit: the `MGET` above can no longer see `0` for `z/bob` once `a/alice` has committed, but if it reads `a/alice` before the transfer prepares and `z/bob` after it commits, it returns `100` and `10`. Also non-repeatable reads, lost updates and write skew |
| `snapshot` | `CONFIG_REDIS_GATEWAY_SNAPSHOT_ISOLATION=y` | Every read at one start timestamp | **Write skew.** Two transactions that each read both keys and write a different one both commit. Lost updates are not allowed: first committer wins |
| `serializable` | `CONFIG_REDIS_GATEWAY_SNAPSHOT_ISOLATION=y` | As `snapshot`, with reads validated at commit | Nothing |

Asking for a level the build does not offer is an error, at startup for
`KYTHIRA_REDIS_TXN_ISOLATION` and from `KYTHIRA.ISOLATION` for a connection.
It is never silently downgraded.

## Serializer

`KYTHIRA_WIRE_SERIALIZER` defaults to `cbor` and should stay there for this
workload. The tree's default JSON RPC serializer base64-encodes every log
entry's bytes, so an AppendEntries carrying an 8 MiB value is 10.7 MiB under
JSON and 8.0 MiB under CBOR — and takes 158 ms of leader CPU to serialize
against 5 ms. The measurement is in the design document's serializer section.

## Observability

`INFO` (admin) reports the shards this node holds and leads, key and expiring
key counts, approximate bytes, and counters for writes, deletes, value
conflicts, oversize rejections, auth failures, authorization denials,
forwards, forward failures and shed connections. Transactions add metrics
`redis.txn.committed`, `redis.txn.aborted` and `redis.txn.unknown` (each
labelled with the isolation level) and `redis.txn.lock_resolves`; the host's
own `kythira.multiraft.txn.*` metrics count resolver work. `DBSIZE` counts keys in
this node's replicas — a follower may lag the leader by a few entries, so ask
the leader when the exact number matters.

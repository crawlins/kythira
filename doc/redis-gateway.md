# The Redis-compatible KV gateway

`redis_gateway_node` is a replicated, sharded key/value store that speaks
enough of the Redis protocol to be sccache's `redis` cache backend. It is the
daemon built by `CONFIG_REDIS_GATEWAY` (`cmd/redis_gateway_node`), a RESP2/RESP3
front end (`include/raft/redis_gateway.hpp`) over the `multi_raft` sharding
layer, with a state machine that splits with its shard
(`include/raft/redis_kv_state_machine.hpp`) and an ACL with PBKDF2-hashed
secrets (`include/raft/redis_acl.hpp`).

It is deliberately not a Redis. The command set is exactly the closure sccache
puts on the wire through OpenDAL and redis-rs — `GET SET SETEX DEL EXISTS
STRLEN GETRANGE PING AUTH HELLO SELECT CLIENT SETINFO QUIT RESET` — plus the
diagnostics an operator reaches for (`COMMAND`, `INFO`, `DBSIZE`, `TTL`,
`ECHO`). Everything else is refused with Redis's own `unknown command` error.
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
export KYTHIRA_REDIS_PEER_GATEWAYS="1=kv1:6379,2=kv2:6379,3=kv3:6379"
redis_gateway_node
```

`docker/sccache-e2e-compose.yml` is a complete three-node example, and
`docker/redis_gateway_node/Dockerfile` packages the host-built binary the same
way the other daemons in `docker/` are packaged (`make docker-redis-gateway-image`).

### Environment variables

Raft and sharding:

| Variable | Default | Meaning |
|---|---|---|
| `KYTHIRA_NODE_ID` | required | This node's id, a positive integer |
| `KYTHIRA_PEERS` | required | `id=http://host:port,...` for every voter, this node included |
| `KYTHIRA_RAFT_BIND` | `0.0.0.0` | Bind address for Raft RPCs |
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
| `KYTHIRA_REDIS_PEER_GATEWAYS` | peer host + `LISTEN` port | `id=host:port,...` of every peer's RESP listener, for forwarding |
| `KYTHIRA_REDIS_IO_THREADS` | `2` | Listener/IO threads |
| `KYTHIRA_REDIS_WORKER_THREADS` | `8` | Command workers |
| `KYTHIRA_REDIS_LOG_COMMANDS` | `false` | Log every command (keys, never values or secrets) at debug |

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
  history. `nopass` accepts any password (use it only with `cert=`), and
  `disabled` in this position keeps the line but lets nobody authenticate
  as the user.
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
Failed `AUTH`s are rate-limited per source address, because each one costs a
PBKDF2 derivation.

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

`docker/sccache_runner/run.sh` does exactly this, and the acceptance test
`docker_sccache_e2e_test` (`make docker-sccache-e2e-tests`) asserts that a
build with every gateway stopped still succeeds. It also asserts the read-only
runner's builds succeed with a non-zero `cache_write_errors`, and that a user
scoped to another prefix gets an uncached build rather than a failed one.

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
forwards, forward failures and shed connections. `DBSIZE` counts keys in
this node's replicas — a follower may lag the leader by a few entries, so ask
the leader when the exact number matters.

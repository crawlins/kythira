# Redis gateway module

`redis_gateway_node` and everything only it uses: a RESP2/RESP3 front end
over Kythira's multi-Raft sharding layer, the replicated KV state machines
behind it (in memory, multi-versioned, and the disk-backed build cache), the
build cache's HTTP front end, the ACL and auth limiter, and their tests,
container images and scenarios. The design and the commands it accepts are
in `doc/redis-gateway.md`. The module was moved here from `include/raft/`,
`cmd/`, `tests/` and `docker/` by `.kiro/specs/redis-gateway-module/` task 4.
Files kept their basenames, so `git log --follow` reaches their history.

It follows the pattern in `services/README.md`. It is on by default
(`CONFIG_REDIS_GATEWAY`, `services/redis/Kconfig`), because the gateway was
on before it became a module.

## Layout

| Path | What |
|---|---|
| `include/kythira/redis/` | the library headers, included as `<kythira/redis/…>` |
| `cmd/redis_gateway_node/` | the daemon (the composition root) and the `kythira_node` module |
| `tests/` | unit and in-process integration tests, the transaction bench rows, `golden/redis/` |
| `tests/docker/` | the container scenarios (sccache end to end, the cache cluster, transactions under faults, engine storage under SIGKILL) |
| `docker/` | the images and compose files those scenarios use |
| `boundary-allow.txt` | the core `*_impl.hpp` includes the module may make, with reasons |

Test names, labels and timeouts are the ones they had in `tests/`, and test
binaries are still written to `build/tests/`. The container targets
(`docker-sccache-e2e-tests`, `docker-cache-cluster-tests`,
`docker-txn-chaos-tests` and their images) kept their names too.

## CI

`.github/workflows/redis-module.yml` runs on pull requests and pushes to
`main` that touch `services/redis/**` or the core files listed in its
`paths` (the multi-Raft host, the transaction wrapper, `asio_listeners.hpp`,
the boundary checker, the test helpers, the persistent state machine and
RocksDB engine), weekly, and on dispatch. Its jobs:

| Job | What it proves |
|---|---|
| `boundary` | the boundary check and its fixture tests pass |
| `module-off` | core builds and its ctest passes with `configs/ci_no_redis_defconfig`, and no module target or test is configured |
| `module-tests` | the module's targets build and every test under `services/redis/` passes |
| `containers` (docker, podman) | `docker-sccache-e2e-tests`, `docker-cache-cluster-tests`, `docker-txn-chaos-tests` and `docker-cache-rolling-upgrade-tests` pass under rootful Docker and rootless Podman |
| `engine-containers` (docker, podman) | `docker-engine-chaos-tests` passes under both runtimes: a daemon on engine storage (`configs/ci_ion_defconfig`, RocksDB) keeps a linearizable history while replicas are SIGKILLed and restarted, and a lagging follower cut off and killed mid snapshot transfer still catches up |

**It is not a required check.** GitHub reports nothing for a workflow its
`paths` filter skips, so requiring it would hang every unrelated pull
request, and the project's auto-merge waits on required checks only. If you
own a pull request that touches those paths, wait for this workflow to finish
green before merging, as well as for `ci.yml`. `ci.yml` still builds the
module and runs its ctest on every pull request.

### Rolling upgrade

`docker-cache-rolling-upgrade-tests` runs the cache cluster with two nodes
on an older gateway build and one on this tree's, using the override
`docker/cache-cluster/rolling-upgrade.yml`. An sccache build misses and
then hits. Then cache-1 moves to the new build and cache-3 to the old one,
each keeping its volume, and a warm build still hits every object the cold
build wrote. The older binary comes from
`-DKYTHIRA_REDIS_OLD_GATEWAY_BINARY=<path>`; `docker-cache-cluster-old-image`
stages it into `docker/cache-cluster/dist-old/` and tags
`kythira-cache-cluster:old` from the same Dockerfile. CI builds it from
`fe53e33`, the last commit before the module move. To run it locally:

```sh
git worktree add ../kythira-old fe53e33
cmake -S ../kythira-old -B ../kythira-old/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build ../kythira-old/build --target redis_gateway_node
cmake -B build -DKYTHIRA_REDIS_OLD_GATEWAY_BINARY="$(find ../kythira-old/build -name redis_gateway_node -type f -perm -u+x | head -n1)"
cmake --build build --target docker-cache-rolling-upgrade-tests
```

When a release line exists, point CI at the previous release's commit
instead of the pre-move commit.

The Podman setup lives in `.github/actions/setup-container-runtime`, which
`container-tests.yml` uses too. `container-tests.yml` no longer runs the
module's scenarios.

## Authentication seam

`include/raft/credential_verifier.hpp` is the core concept through which the
gateway asks something outside itself whether a credential is good: a
`credential_request` (service, user, credential, peer) in, a future of
`credential_accepted` (principal, expiry) or `credential_denied` out. It
names no IAM or AWS type. `credential_verifier_fn` is its type-erased form.

Both front ends take an optional verifier and a predicate that selects which
credentials go to it:

- RESP: `redis_gateway_config::_credential_verifier` and
  `_credential_verifier_users`. `AUTH` and `HELLO ... AUTH` for a selected
  user go to the verifier instead of the password check. Admission, the
  failure budget and the `WRONGPASS` reply are the password path's. The ACL
  still decides the identity: an accepted user takes its ACL role and key
  prefixes, a user the ACL does not have (or has disabled) is refused before
  the verifier is asked, and the internal user is never accepted this way.
  The call waits on the connection's worker, never on the I/O strand, for at
  most `_command_timeout`. The accepted expiry closes the connection: the
  connection's idle timer is armed for whichever of the idle timeout and the
  expiry comes first, and a command after the expiry is refused with
  `NOAUTH credential expired`. A password `AUTH` or `RESET` clears it.
- HTTP: `cache_http_config::_credential_verifier` and
  `_credential_verifier_selects`, through `verify_cache_http_credential`.
  For Bearer, which names no user, the accepted `principal` names the ACL
  user. Verified requests skip the success cache, so a cached entry can never
  outlive the credential.

With either field empty, which is every build today, both front ends behave
exactly as they did before the seam existed.

**IAM task 21** fills the seam in `redis_gateway_node` when it is built with
`CONFIG_REDIS_GATEWAY_IAM` (depends on `REDIS_GATEWAY && IAM`):

- The ACL's `auth=password|iam|both` is a reserved attribute like `cert=`
  (a repeated or unknown value is rejected). `redis_acl::authenticate`
  refuses an `auth=iam` user's password, and `auth_mode_of` tells the
  composition root which users are the verifier's: `redis_module.hpp` sets
  `_credential_verifier_users` to the `iam` and `both` users whenever a
  verifier is configured. A `both` user the verifier refuses is checked
  against its password in the same admission slot.
- `cmd/redis_gateway_node/iam_auth.cpp` is the only file that includes the
  IAM module, through three `module-adapter` lines in `boundary-allow.txt`.
  It builds IAM's embedded replica and `iam_redis_verifier` from the
  `KYTHIRA_IAM_*` variables; without the option neither the file nor the
  variables exist (`redis_gateway_node_iam_switch` checks `--help`).
- `doc/redis-gateway.md` documents `auth=`, the rolling-upgrade rule, and
  the variables.

## Beside other services in `kythira_node`

The same code runs as a module of the composite daemon
(`.kiro/specs/service-colocation/` task 7,
`cmd/redis_gateway_node/redis_module.hpp`), with `KYTHIRA_SERVICES` naming
`redis`. `redis_gateway_node` is that module alone on its own Beast
transport (`standalone_node.hpp`), so its variables, ports, wire, group ids
and data layout are as before. The serving side, `redis_serving.hpp`, is
one `multi_raft` stack per storage, each still in its own translation unit
(`run_cbor*.cpp` for the daemon, `redis_module_*.cpp` for `kythira_node`).

In `kythira_node`, Raft goes over the node's one listener, with mutual TLS
from `KYTHIRA_NODE_TLS_*`, on the CBOR wire whatever
`KYTHIRA_WIRE_SERIALIZER` says, and the gateway's groups sit at `1 << 56`
and up (the snapshot-isolation oracle at the same low bits as stand-alone).
A stand-alone data directory therefore cannot be opened by a composite
node; a cache is refilled rather than moved. The node's settings fill in
what the variables above leave unset (`composite_env` in `config.hpp`): the
peers from `KYTHIRA_NODE_PEERS`, Raft's bind address and port, and the RESP
TLS listener's certificate and key and the forwarding CA from
`KYTHIRA_NODE_TLS_*`, used only when `KYTHIRA_REDIS_TLS_LISTEN` asks for a
TLS listener. The data directory is the node's `redis/` in file and engine
mode; memory mode has none. Every other setting, the ACL file included, is
set as for the daemon; `redis_gateway_node_config_unit_test` checks the
mapping. The node's listener takes Raft requests up to 64 MiB, and a
`KYTHIRA_REDIS_MAX_VALUE_BYTES` or `KYTHIRA_CACHE_CHUNK_BYTES` that needs
larger ones is refused at start.

Two things differ from the daemon. A peer's RESP and HTTP addresses are not
learned from its Raft connections (the node's listener learns only Raft's),
so under rootless Podman a gateway that restarts at a new address is found
once its name resolves again. And IAM authentication: when IAM runs in the
same node (`KYTHIRA_SERVICES` names `iam`) and
`KYTHIRA_REDIS_IAM_ACCOUNT_ID`, `_REGION` and `_CACHE_NAME` name the cache
(all three or none), `AUTH` for the ACL's `auth=iam` and `auth=both` users
goes to the IAM module's in-process evaluator (`iam.evaluator`), which
checks `elasticache:Connect` as the embedded replica does. Otherwise a
`CONFIG_REDIS_GATEWAY_IAM` build keeps its embedded replica
(`KYTHIRA_IAM_*`), in `kythira_node` as stand-alone.

## What the module uses from core

This is the list a split to its own repository would have to export from a
Kythira package. The boundary check (`scripts/check_module_boundaries.py`)
keeps the C++ side of it honest; the CMake side is this list.

**Targets**

| Target | Used for |
|---|---|
| `kythira_core_headers` | the public headers under `include/` and the Kconfig header |
| `kythira_test_support` | the in-process test fabric's headers, by name (tests only) |
| `kythira_test_pch` | linked by `add_raft_completion_test` (tests only) |
| `certificate_authority` | `KYTHIRA_HAS_OPENSSL` for the IAM replica's TLS RPC headers (`CONFIG_REDIS_GATEWAY_IAM` builds of `redis_gateway_node` only) |
| `kythira_iam` (the IAM module) | `iam_auth.cpp`, the composition root's IAM side (`CONFIG_REDIS_GATEWAY_IAM` only) |
| `network_simulator`, `boost_json`, `kythira_host_node_id` | the daemon's links, as before the move |
| `multi_raft_bench_rows` | the module adds its two transaction rows to it |
| `multi_raft_performance_report` | the command of `multi_raft_performance_report_transactions` |

**Helpers** (`cmake/`, on `CMAKE_MODULE_PATH`)

- `kythira_test_helpers.cmake`: `add_raft_completion_test`,
  `add_docker_chaos_scenario_test`, `kythira_detect_container_runtime()`,
  `kythira_scale_test_timeouts()` and `KYTHIRA_IMAGE_CONTEXT`, the build
  context the images use (the source root: the cache client image also
  copies `scripts/oci-build-cache/start-sccache.sh`).
- `TestReports.cmake`: `kythira_attach_test_reports()`.

**Variables**

- The gate: `KYTHIRA_BUILD_REDIS_GATEWAY`, and the Kconfig values
  `KCONFIG_REDIS_GATEWAY_TRANSACTIONS` and
  `KCONFIG_REDIS_GATEWAY_SNAPSHOT_ISOLATION`.
- What the build found: `ENABLE_COVERAGE`, `KYTHIRA_FUTURE_BACKEND_AVAILABLE`,
  `folly_FOUND`, `Boost_FOUND`, `OpenSSL_FOUND`, `httplib_FOUND`,
  `FIU_FOUND`, `FIU_LDFLAGS`, `FIU_INCLUDE_DIRS`.
- `KYTHIRA_TEST_TIMEOUT_SCALE`, and the `heavy_tu` job pool.

**Core internals**: the files listed in `boundary-allow.txt` include
`multi_raft_impl.hpp` (and the daemon `beast_http_transport_impl.hpp`), as
any application instantiating the multi-Raft host must.

## What a split would still need

- A published Kythira package: the headers, a CMake config exporting the
  targets above, and the helpers in `cmake/`.
- The test fabric as an installable test-support target, rather than a path
  into Kythira's `tests/`.
- The container helpers and an image base that does not need Kythira's
  source tree as its build context.
- A CI workflow of its own. `redis-module.yml` (above) is the in-tree one,
  and would move with the module.

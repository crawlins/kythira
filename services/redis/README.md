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
| `cmd/redis_gateway_node/` | the daemon (the composition root) |
| `tests/` | unit and in-process integration tests, the transaction bench rows, `golden/redis/` |
| `tests/docker/` | the container scenarios (sccache end to end, the cache cluster, transactions under faults) |
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
the boundary checker, the test helpers), weekly, and on dispatch. Its jobs:

| Job | What it proves |
|---|---|
| `boundary` | the boundary check and its fixture tests pass |
| `module-off` | core builds and its ctest passes with `configs/ci_no_redis_defconfig`, and no module target or test is configured |
| `module-tests` | the module's targets build and every test under `services/redis/` passes |
| `containers` (docker, podman) | `docker-sccache-e2e-tests`, `docker-cache-cluster-tests` and `docker-txn-chaos-tests` pass under rootful Docker and rootless Podman |

**It is not a required check.** GitHub reports nothing for a workflow its
`paths` filter skips, so requiring it would hang every unrelated pull
request, and the project's auto-merge waits on required checks only. If you
own a pull request that touches those paths, wait for this workflow to finish
green before merging, as well as for `ci.yml`. `ci.yml` still builds the
module and runs its ctest on every pull request.

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

**For IAM task 21**, which fills the seam:

- Set the predicates from the ACL's `auth=password|iam|both` attribute, and
  parse `auth=` like `cert=`: a reserved attribute, not a key prefix, with a
  repeated or unknown value rejected. Today's parser reads `auth=iam` as the
  key prefix `auth=iam` (pinned by `redis_acl_unit_test`).
- Roll every gateway onto the new build before any ACL file uses `auth=`,
  and say so in `doc/redis-gateway.md`. An older gateway still allows
  password `AUTH` for an `auth=iam` user, scoped to keys starting
  `auth=iam`. Where a deployment needs a hard guarantee, give an IAM-only
  user the secret `disabled`, which an old binary refuses outright.
- Add `CONFIG_REDIS_GATEWAY_IAM` (depends on `REDIS_GATEWAY && IAM`) in
  `Kconfig`. The daemon's composition root then links IAM's adapter, which
  needs a `module-adapter` line in `boundary-allow.txt`.

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

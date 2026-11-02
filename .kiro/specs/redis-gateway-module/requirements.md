# Requirements Document — Redis Gateway as a Self-Contained Module

## Introduction

The Redis-compatible gateway (`.kiro/specs/redis-compatible-kv/`) and the
disk-backed build cache built on it (`.kiro/specs/disk-backed-build-cache/`)
are today spread through the core tree:

- 15 headers in `include/raft/` (`resp_protocol.hpp`, `redis_*.hpp`,
  `cache_*.hpp`), side by side with the Raft core;
- the daemon in `cmd/redis_gateway_node/`;
- 14 test files in `tests/`, registered from the middle of
  `tests/CMakeLists.txt`;
- two Kconfig symbols in the top-level `Kconfig`;
- container scenarios in `tests/docker_chaos/` and `docker/`, run only by the
  weekly `container-tests.yml` and the manual arm64 workflow.

Nothing stops core code from depending on the gateway, or the gateway from
reaching into core internals or the test tree. The daemon already does the
latter: `cmd/redis_gateway_node/run_host.hpp` includes
`tests/multi_raft_transport_harness.hpp` for its transport handles.

Clark asked on 2026-11-02 for Redis to become its own sub-module in the
Kythira tree, the way the new services are being laid out: the IAM service
(`.kiro/specs/iam-compatible-auth/`, PR #672) as a separately built,
Kconfig-gated daemon, and the verifiable ledger
(`.kiro/specs/verifiable-ledger/`, PR #674) as `services/ledger/` behind
`KYTHIRA_LEDGER`, limited by a CI check to Kythira's public headers so that it
stays separable and could move to its own repository later. This spec applies
that pattern to an existing service, which the other two never had to do.

**This is a move, not a rewrite.** No command, flag, environment variable,
wire format, log-entry or snapshot format, metric name, error string, test
name or container image changes. The one forward-looking piece is an
authentication seam, so that the IAM spec's Redis work (its tasks 21-22:
`AUTH` accepting both ACL passwords and IAM tokens) lands inside the module
without the module depending on IAM.

**Scope**: the module directory and its CMake and Kconfig wiring, moving the
gateway, the build-cache front end, the daemon, the tests and the container
scenarios into it; promoting the core pieces the module borrows from `tests/`
to public headers; the dependency-boundary check; the module's own CI
workflow; the authentication seam; docs. **Out of scope**: any behaviour
change; splitting the build cache into a second module; moving the module to
another repository; the IAM implementation itself.

## Glossary

- **Module**: a directory under `services/` with its own `Kconfig`,
  `CMakeLists.txt`, headers, sources, tests, daemon(s) and container
  scenarios, built only when its Kconfig symbol is on.
- **Core**: everything outside `services/`: `include/raft/`, `src/`, the
  other `cmd/` daemons, and `tests/`.
- **Public core header**: a header under `include/raft/` or
  `include/concepts/` whose name does not end in `_impl.hpp`. Concepts, value types, host APIs and the template
  declarations a consumer instantiates.
- **Composition root**: the daemon translation units that pick concrete
  transport, serializer and persistence types and instantiate the multi-raft
  host. They need template definitions from core `*_impl.hpp` headers.
- **Boundary check**: the CI script that enforces Requirement 3.
- **Credential verifier**: the core concept (Requirement 6) through which the
  gateway asks something outside itself whether a non-password credential is
  valid.

## Requirements

### Requirement 1: One directory for the whole service

**User Story:** As a maintainer, I want everything that is the Redis gateway
in one place, so I can see, build, test and review it as a unit.

#### Acceptance Criteria

1. The module SHALL live under `services/redis/` with this layout:
   - `include/kythira/redis/`: the library headers;
   - `cmd/redis_gateway_node/`: the daemon;
   - `tests/`: unit and in-process integration tests;
   - `tests/docker/`: the container scenarios;
   - `docker/`: the daemon, cache-cluster and sccache images and compose files;
   - `Kconfig`, `CMakeLists.txt` and `README.md`.
2. The build-cache pieces (`cache_chunk_store`, `cache_chunk_fetcher`,
   `cache_disk_guard`, `cache_http_auth`, `cache_http_front_end`,
   `cache_kv_state_machine`) SHALL move with the gateway. They are served only
   by `redis_gateway_node` and depend on the gateway's ACL, auth limiter and
   KV command codec.
3. Files SHALL move with `git mv` and keep their basenames, so
   `git log --follow` and a search for an old file name still find them.
4. Nothing Redis- or build-cache-specific SHALL remain in core, except
   comments that name the module.
5. Core headers whose only consumers are in the module SHALL move only if
   they are Redis- or cache-specific. Generic pieces stay in core.

### Requirement 2: Kconfig switch

**User Story:** As a maintainer, I want the module built only when its switch
is on, and a build without it to be the core alone.

#### Acceptance Criteria

1. `CONFIG_REDIS_GATEWAY` and `CONFIG_REDIS_GATEWAY_TRANSACTIONS` SHALL move,
   with their names, defaults, dependencies and help text unchanged, into
   `services/redis/Kconfig`. The top-level `Kconfig` SHALL `source` it.
2. The default SHALL stay `y`, unlike the new IAM and ledger modules. This
   spec changes where Redis lives, not who builds it.
3. Every existing defconfig SHALL load unchanged and without warnings
   (`scripts/kconfig/check_defconfigs.py`).
4. With `CONFIG_REDIS_GATEWAY=n`:
   - no target, test or install rule from `services/redis/` SHALL be
     configured;
   - `services/redis/CMakeLists.txt` SHALL not be entered at all;
   - the core's target and test lists SHALL equal those of a module-on build
     minus the module's.
5. With it on, the module's target and test lists SHALL equal today's Redis
   and cache targets and tests, name for name.
6. A new defconfig `configs/ci_no_redis_defconfig` SHALL turn the module off.
   It SHALL be applied by the module workflow (Requirement 5), so
   `check_defconfig_usage.py` passes.

### Requirement 3: Depends only on Kythira's public surface

**User Story:** As a maintainer, I want the module to use only what any
outside consumer of Kythira could use, so that core internals stay free to
change and the module could move to its own repository.

#### Acceptance Criteria

1. Headers and sources under `services/redis/include/` SHALL include only:
   - their own module's headers;
   - public core headers (no `*_impl.hpp`);
   - the standard library and third-party headers that core already requires.
2. The composition root (the daemon's `run_host.hpp` and `run_*.cpp`) MAY also
   include core `*_impl.hpp` headers. Each one SHALL be listed with its reason
   in `services/redis/boundary-allow.txt`. No other module file may appear
   there.
3. No module file SHALL include anything under the top-level `tests/` or
   `cmd/` directories. Module tests MAY include the in-process test fabric
   only through the `kythira_test_support` interface target (Requirement 4.2),
   never by a relative path.
4. No core file SHALL include a module header, and no core CMake file SHALL
   name a module target, except the top-level `add_subdirectory` and the
   documented install and workflow hooks.
5. `services/redis/CMakeLists.txt` SHALL not reference `${CMAKE_SOURCE_DIR}`
   outside `services/redis/`. It SHALL reach core only through exported CMake
   targets and variables listed in its `README.md`.
6. A boundary check (`scripts/check_module_boundaries.py`) SHALL enforce
   criteria 1-5 for every directory under `services/`. It SHALL run on every
   pull request in `ci.yml`, needs no build, and SHALL fail with the offending
   file, line and rule.
7. The check SHALL have its own test: a fixture tree with one violation per
   rule, each of which it must report.

### Requirement 4: Core pieces the module borrows become public

**User Story:** As a maintainer, I want the daemon's dependencies on test
code replaced by real core APIs.

#### Acceptance Criteria

1. The transport handles the daemon takes from
   `tests/multi_raft_transport_harness.hpp` (`transport_client_handle`,
   `transport_server_handle` and the Beast transport-types bundle) SHALL move
   to a public core header. `tests/multi_raft_transport_harness.hpp` and
   `cmd/multi_raft_bench` SHALL include that header instead, and their
   behaviour SHALL not change.
2. Core SHALL export an interface target, `kythira_test_support`, that carries
   the include path of the in-process fabric (`multi_raft_test_fabric.hpp`
   and friends) and the `add_raft_completion_test` helper. Module tests SHALL
   use the fabric through this target.
3. Core SHALL export an interface target, `kythira_core_headers`, carrying
   `include/`, so the module does not rely on the top-level
   `include_directories()` call.
4. `add_raft_completion_test` and `add_docker_chaos_scenario_test` SHALL be
   callable from the module's CMake. They move into `cmake/` modules with
   their behaviour unchanged.
5. `container_runtime()` and `compose_prefix()` (CLAUDE.md, "Container
   runtime compatibility") SHALL stay the only way the module's container
   tests choose a runtime.

### Requirement 5: Its own path-filtered CI workflow

**User Story:** As a reviewer, I want a Redis change to run the Redis
container scenarios on the pull request, not a week later, and a core-only
change not to pay for them.

#### Acceptance Criteria

1. A workflow, `.github/workflows/redis-module.yml`, SHALL run on pull
   requests that touch `services/redis/**`, the workflow file itself, or the
   core headers the module includes most (the multi-raft host, the
   transaction wrapper and `asio_listeners.hpp`, listed in the workflow).
   It SHALL also run on pushes to `main` that touch those paths, weekly, and
   on manual dispatch.
2. It SHALL run:
   - the boundary check;
   - a module-off build with `configs/ci_no_redis_defconfig` and that build's
     ctest;
   - the module's ctest suites (labels `redis` and `cache`);
   - `docker-sccache-e2e-tests` and `docker-cache-cluster-tests` under rootful
     Docker and under rootless Podman, as a matrix.
3. Those two container targets SHALL move from `container-tests.yml` to this
   workflow, so they run once per trigger, not twice.
4. `ci.yml` SHALL keep building the module in its existing configurations and
   running its ctest suites on every pull request, so a core change that
   breaks Redis still fails the main gate.
5. The workflow SHALL NOT be a required status check. A path-filtered
   workflow that is skipped never reports, so requiring it would block every
   PR it skips. If it is later required, it SHALL first gain an always-run
   gate job that passes when the paths did not change.
6. Under Podman the workflow SHALL use the same setup steps as
   `container-tests.yml`. Compose files SHALL keep wiring containers by
   service name, with no static IPs and no privileged or host-network
   containers.

### Requirement 6: An authentication seam for IAM

**User Story:** As the IAM implementer, I want to add IAM tokens to Redis
`AUTH` inside the module without the module depending on the IAM module, so
both stay separable and Redis builds without IAM.

#### Acceptance Criteria

1. Core SHALL gain a small public concept, the credential verifier: given a
   service name, a user name, a credential string and the connection's peer,
   it returns a future of either a denial (with a reason) or an accepted
   principal with an expiry time. It SHALL carry no IAM or AWS types.
2. The gateway SHALL take an optional credential verifier. With none
   configured, which is the only possible state while IAM is not built,
   `AUTH` and `HELLO … AUTH` SHALL behave exactly as today.
3. The ACL file format SHALL be ready for a per-user `auth=password|iam|both`
   attribute, which the IAM spec's Requirement 13.1 defines. This spec does
   not add the attribute; IAM task 21 does. Today the parser reads any word
   after the role that is not `cert=…` or `*` as a key prefix, so
   `auth=iam` would silently become a prefix. This spec SHALL pin that
   behaviour with a test, and the design records what IAM task 21 has to do
   about it.
4. Wiring an IAM verifier into `redis_gateway_node` SHALL be a composition-root
   change behind a new symbol, `CONFIG_REDIS_GATEWAY_IAM`, which depends on
   `REDIS_GATEWAY && IAM`. The symbol is added with IAM task 21, not by this
   spec.
5. Whatever authenticates the user, the ACL user's role and key prefixes
   SHALL apply afterwards (IAM Requirement 13.4). Forwarded internal links
   SHALL keep using the internal ACL user (IAM Requirement 13.7).
6. The build cache's HTTP front end (`cache_http_auth`) SHALL take the same
   optional verifier, so Basic and Bearer credentials can later be checked by
   IAM too. Its behaviour is also unchanged until a verifier is configured.

### Requirement 7: Behaviour is provably unchanged

**User Story:** As an operator of a running cache cluster, I want to upgrade
across this change without noticing it.

#### Acceptance Criteria

1. The `redis_gateway_node` binary name, build output directory, environment
   variables, `--help` text, defaults, listeners, metric names and log lines
   SHALL be unchanged. A test SHALL compare `--help` and the parsed default
   configuration before and after.
2. The RESP wire behaviour, the KV log-entry encoding and the snapshot formats
   of `redis_kv_state_machine`, `cache_kv_state_machine` and their
   transactional wrappers SHALL be byte-identical. Golden-byte tests SHALL be
   added before the move and SHALL pass unchanged after it.
3. Every existing Redis and cache test SHALL keep its ctest name, labels and
   timeout, so the retry allowlist, the no-folly allowlist and workflow
   filters that name tests keep matching.
4. Container images SHALL keep their names, entrypoints and in-image paths.
   The `docker/cache-cluster` and `docker/redis_gateway_node` build contexts
   may move; the images may not change.
5. A mixed cluster of old-tree and new-tree `redis_gateway_node` binaries
   SHALL serve the sccache scenario. A rolling upgrade is then known safe.

### Requirement 8: Extractable later

**User Story:** As a maintainer, I want to be able to move the module to its
own repository later without redesigning it, as the ledger spec plans for
`services/ledger/`.

#### Acceptance Criteria

1. `services/redis/CMakeLists.txt` SHALL configure against an installed or
   `add_subdirectory`'d Kythira through the targets and variables named in
   its `README.md`, and against nothing else (Requirement 3.5).
2. The module `README.md` SHALL list what a split would still need: a
   published Kythira package (headers and CMake config), the test-support
   target, the container helpers, and the CI workflow.
3. This spec SHALL NOT move the module out of the tree. Staying in-tree keeps
   one CI, one vcpkg baseline and atomic changes across core and module, while
   the API is still moving (multi-raft transactions tier 2, persistent state
   machine). The boundary check keeps a later split cheap.

### Requirement 9: Documentation

#### Acceptance Criteria

1. `doc/redis-gateway.md` SHALL stay where the docs site expects it, with
   updated paths and a short "Where the code lives" section.
2. `services/README.md` SHALL describe the module pattern: the layout, the
   Kconfig and CMake conventions, the boundary rules and the CI pattern. The
   ledger and IAM modules SHALL be able to follow it unchanged.
3. The redis-compatible-kv and disk-backed-build-cache specs SHALL gain a
   one-line note pointing at the new paths. Their history is otherwise left
   as written.

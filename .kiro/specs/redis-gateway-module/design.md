# Design Document — Redis Gateway as a Self-Contained Module

## Overview

The gateway is already close to a module in practice. A survey of `main` at
`5134391` (2026-11-02) found:

- The 15 library headers include only public core headers:
  - `types`, `shard_types`, `splittable_state_machine`, `multi_raft`;
  - `txn_state_machine`, `txn_envelope`, `txn_coordinator`,
    `shard_exceptions`;
  - `asio_listeners`, `http_connection_gate`, `bounded_resolver`;
  - `future_default`, `logger`, `fault_injection`, `crc32`.

  None of them includes a core `*_impl.hpp`.
- No core header or source depends on them. The only core mention is a
  comment in `ca_http_helpers.hpp`.
- The daemon's composition root includes two core `*_impl.hpp` headers
  (`multi_raft_impl.hpp`, `beast_http_transport_impl.hpp`), which is
  legitimate. It also includes `tests/multi_raft_transport_harness.hpp`,
  which is not.

So the work is mostly mechanical: move the files, give the module its own
CMake and Kconfig, promote the one borrowed test header, add the check that
keeps it this way, and give it its own CI. The design keeps the diff that
touches behaviour at zero, and makes the move one large `git mv` PR that is
trivially reviewable, with the risky parts (new public header, check, CI) in
small PRs either side of it.

```
                      ┌──────────────────────── core ─────────────────────────┐
                      │ include/raft/  (public headers, *_impl.hpp)           │
                      │ cmake/kythira_test_helpers.cmake                      │
                      │ kythira_test_support (fabric include path, helpers)   │
                      │ credential_verifier concept (new, Req. 6)             │
                      └──────▲──────────────────▲───────────────────▲─────────┘
         public headers only │   *_impl.hpp,    │ kythira_test_support│
                             │   allowlisted    │  (tests only)       │
 services/redis/ ┌───────────┴──────┐ ┌─────────┴────────┐ ┌──────────┴───────┐
                 │ include/kythira/ │ │ cmd/redis_       │ │ tests/,          │
                 │   redis/*.hpp    │◄┤  gateway_node/   │ │ tests/docker/    │
                 └──────────────────┘ └──────────────────┘ └──────────────────┘
         core never includes services/**  (boundary check, every PR)

 later, IAM task 21:  services/iam ──implements──► credential_verifier ◄── redis gateway
                      wired only in redis_gateway_node, behind CONFIG_REDIS_GATEWAY_IAM
```

## Layout

| Today | After |
|---|---|
| `include/raft/resp_protocol.hpp`, `redis_*.hpp` (9) | `services/redis/include/kythira/redis/` (same basenames) |
| `include/raft/cache_*.hpp` (6) | `services/redis/include/kythira/redis/` (same basenames) |
| `cmd/redis_gateway_node/*` | `services/redis/cmd/redis_gateway_node/*` |
| `tests/resp_protocol_unit_test.cpp`, `tests/redis_*`, `tests/cache_*` (14) | `services/redis/tests/` |
| `tests/docker_chaos/sccache_e2e_test.cpp`, `cache_cluster_test.cpp` | `services/redis/tests/docker/` |
| `docker/redis_gateway_node/`, `docker/cache-cluster/`, `docker/sccache_e2e/`, `docker/sccache_runner/`, `docker/sccache-e2e-compose.yml` | `services/redis/docker/` |
| `Kconfig` `REDIS_GATEWAY`, `REDIS_GATEWAY_TRANSACTIONS` | `services/redis/Kconfig`, `source`d from `Kconfig` |
| Redis block of `tests/CMakeLists.txt`, Redis targets in `tests/docker_chaos/CMakeLists.txt` | `services/redis/CMakeLists.txt` and `services/redis/tests/CMakeLists.txt` |
| — | `services/redis/boundary-allow.txt`, `services/redis/README.md`, `services/README.md` |
| `doc/redis-gateway.md` | unchanged location (docs site), paths updated |

**Include spelling.** `#include <raft/redis_acl.hpp>` becomes
`#include <kythira/redis/redis_acl.hpp>`. The redundant `redis_` in the path
is the cost of keeping basenames (Requirement 1.3), and it is cheaper than a
rename that breaks `git log --follow` and every grep in old specs and
threads. The `kythira::` namespace and every type name stay as they are.
Namespaces carry no behaviour, and renaming them would turn a reviewable move
into a rewrite.

**Why `services/` and not `modules/`.** The ledger spec already uses
`services/ledger/`. IAM (`iam_node`) should follow too. See "Follow-ups".

**Why the build cache moves too.** `cache_http_front_end` and
`cache_kv_state_machine` are served only by `redis_gateway_node`, select it
with `KYTHIRA_CACHE_*` variables, and depend on `redis_acl`,
`redis_auth_limiter` and the `redis_kv_*` command codec. Splitting them out
would need either a third, shared "kv-auth" module or the cache depending on
the Redis module. Both are more structure than one daemon needs. If the cache
ever gets a daemon of its own, it can become `services/build-cache/`
depending on `services/redis/`, using the same rules.

## Kconfig

`Kconfig` gains, where the two symbols are now:

```
source "services/redis/Kconfig"
```

The block moves with its help text byte for byte, apart from the paths it
names. Kconfiglib resolves `source` relative to `$srctree`, and
`cmake/Kconfig.cmake` and `scripts/kconfig/genconfig.py` already run from the
source root. Task 1 checks this, and that the generated `autoconf.hpp` is
identical before and after.

## CMake

The top level replaces today's three Redis blocks with one:

```cmake
kythira_kconfig_gate(REDIS_GATEWAY)          # unchanged
...
if(KYTHIRA_BUILD_REDIS_GATEWAY)
    add_subdirectory(services/redis)
endif()
```

The `add_subdirectory` sits after the core targets, `tests/` and
`tests/docker_chaos/`, so the targets and helper functions it uses exist.
`services/redis/CMakeLists.txt`:

- defines `kythira_redis`, an INTERFACE library whose include directory is
  `services/redis/include`, linking `OpenSSL::SSL`, `OpenSSL::Crypto` and
  `kythira_core_headers`. That last one is a new core INTERFACE target
  (task 2) carrying `include/`. Today the core relies on a global
  `include_directories(${CMAKE_SOURCE_DIR}/include)`, which a module that may
  one day build outside the tree cannot rely on;
- adds `cmd/redis_gateway_node` under today's guard (folly, Boost, OpenSSL,
  future backend available, not a coverage build). Same output directory
  (`${CMAKE_BINARY_DIR}`), `heavy_tu` pool and compile definitions;
- adds `tests/`, which registers every test with today's name, labels and
  timeout through `add_raft_completion_test`, plus a `kythira_test_support`
  link for the fabric include path;
- adds `tests/docker/` with the `docker-sccache-e2e-tests` and
  `docker-cache-cluster-tests` custom targets. They are built from the same
  `add_docker_chaos_scenario_test` and image-build helpers, keep the same
  target names, and still forward `KYTHIRA_CONTAINER_RUNTIME` /
  `KYTHIRA_COMPOSE_COMMAND`.

The helpers move out of `tests/CMakeLists.txt` and
`tests/docker_chaos/CMakeLists.txt` into
`cmake/kythira_test_helpers.cmake`, unchanged, and both files `include()` it.
The docker-chaos image targets the Redis scenarios depend on
(`docker-chaos-image` and friends) are already global targets, so they are
referenced by name.

**Proof of no change** (tasks 1 and 4). Before the move, a script records:

- `ninja -t targets all` (target names);
- `ctest --show-only=json-v1` (test names, labels, timeouts, commands with the
  source root replaced by `<src>`);
- `redis_gateway_node --help`.

It does this in the module-on and module-off configurations of
`ci_full_defconfig`. After the move the same script must report:

- module on: target and test sets equal. Commands differ only in the source
  paths of moved files. `--help` is identical.
- module off: target and test sets equal to today's module-off sets.

## Boundary check

`scripts/check_module_boundaries.py` is pure Python, with no build and no
compiler, like `check_defconfigs.py`. For every `services/<m>/` it:

1. Scans each C++ file's `#include` lines. Quoted includes resolve relative to
   the file; angle includes resolve against the module's include directory,
   then core `include/`.
2. Classifies every target: same module, other module, public core header,
   core `*_impl.hpp`, `tests/`, `cmd/`, or system / third-party (not found in
   the tree).
3. Applies the rules:
   - another module's header is never allowed;
   - `*_impl.hpp` is allowed only in files listed in `boundary-allow.txt`;
   - `tests/` and `cmd/` are never allowed, except that the fabric headers are
     allowed from `services/<m>/tests/**`;
   - no core file under `include/`, `src/`, `cmd/` or `tests/` may include
     anything under `services/`.
4. Greps each module's `CMakeLists.txt` files for `CMAKE_SOURCE_DIR`
   references outside the module, and the core CMake files for module target
   names, except the allowed hooks.

`services/redis/boundary-allow.txt` starts as:

```
# file                                   allowed core *_impl.hpp      why
cmd/redis_gateway_node/run_host.hpp      multi_raft_impl.hpp          instantiates the multi-raft host
cmd/redis_gateway_node/run_host.hpp      beast_http_transport_impl.hpp instantiates the Beast transport
```

It is a cross-module rule, not a Redis rule, so the ledger (task 1) and IAM
reuse it unchanged. It runs in `ci.yml`'s `kconfig-check` job, which takes
seconds and runs on every PR. Its fixture tests run in the same step.

**Cross-module dependencies.** None are allowed today. When a module needs
something from another, the shared piece becomes a core concept (as in
"Authentication seam" below) and the composition root wires the two together.
This keeps every module buildable alone, which is what makes a later split
possible.

## Promoting the transport handles

`run_host.hpp` uses `kythira::testing::transport_client_handle`,
`transport_server_handle` and `harness_transport_types` from
`tests/multi_raft_transport_harness.hpp` (2,271 lines, which drags in the
test fabric, workloads, CoAP and Proxygen bundles).

Task 2 moves the handle types and the Beast bundle into
`include/raft/multi_raft_transport_handles.hpp`, in namespace `kythira`. The
harness keeps `using` aliases in `kythira::testing`, so its users
(`cmd/multi_raft_bench`, the multi-raft performance tests) compile
unchanged. The daemon then includes the public header and loses `tests/` from
its include path. This is the one change outside the move that touches
compiled code. It is a pure extraction, so the bench and every multi-raft
test are the regression check.

## Authentication seam

A new public core header, `include/raft/credential_verifier.hpp`:

```cpp
struct credential_request {
    std::string_view service;     // "redis", "cache-http"
    std::string_view user;        // AUTH user / Basic user; may be empty for Bearer
    std::string_view credential;  // the password slot, verbatim
    std::string_view peer;        // remote address, for audit and rate keys
};

struct credential_accepted {
    std::string principal;                          // for logs and metrics only
    std::chrono::system_clock::time_point expires;  // connection deadline
};

struct credential_denied { std::string reason; };   // never sent to the client verbatim

template <typename V>
concept credential_verifier = requires(V& v, const credential_request& r) {
    { v.verify(r) } -> kythira::future<std::variant<credential_accepted, credential_denied>>;
};
```

It carries no IAM or AWS types. IAM's `iam_authorizer` adapts to it (presigned
`elasticache` URL, `elasticache:Connect` on the user and replication-group
ARNs), and a future OIDC/JWT or mTLS verifier could too.

**In the gateway** (task 6), the config gains an optional type-erased
verifier (`std::function` over the concept, so the gateway does not become a
template on it). No verifier means every code path is today's. With a
verifier, the place IAM task 21 fills is:

```
AUTH user cred  ─► ACL lookup(user)
                   ├─ user absent                  → today's decoy verify, WRONGPASS
                   ├─ user allows password, cred verifies → today's path
                   ├─ user allows iam, verifier set ─► verifier.verify(...)
                   │     accepted → ACL role + prefixes apply; deadline = min(expires, …)
                   │     denied   → WRONGPASS, counted by redis_auth_limiter
                   └─ otherwise                    → WRONGPASS
```

`verify()` returns a future so that the strand never blocks. Its completion
is posted back to the connection's strand, the same way forwarded commands
complete today. The IAM verifier answers from its local replica and needs no
network call (IAM Requirement 13.6), but the seam does not assume it.

Task 6 adds only:

- the config field;
- the deadline plumbing (a per-connection `expires` checked by the existing
  idle-timeout timer);
- a test verifier in the module's tests that accepts and denies on command.

Even so, no ACL user can select it until IAM task 21 adds `auth=`, so
production behaviour is unchanged.

**The ACL `auth=` attribute.** The parser today treats any word after the
role that is not `cert=…` or `*` as a key prefix (`redis_acl.hpp`,
`parse`). Task 6 pins this with a test. IAM task 21, when it adds
`auth=password|iam|both`, must:

- parse `auth=` like `cert=` (a reserved attribute, not a prefix), and reject
  a repeated or unknown value;
- note in `doc/redis-gateway.md` that an older gateway reads `auth=iam` as
  the key prefix `auth=iam` and still allows password AUTH for that user. So
  every gateway in a cluster must run the new build before any ACL file uses
  `auth=`. Where a deployment needs a hard guarantee, an IAM-only user can be
  given secret `disabled`, which an old binary refuses outright.

**Build cache HTTP.** `cache_http_auth::authenticate` takes the same optional
verifier for Basic and Bearer credentials. It is not wired to anything in this
spec.

**Kconfig.** IAM task 21 adds `CONFIG_REDIS_GATEWAY_IAM` (depends on
`REDIS_GATEWAY && IAM`) in `services/redis/Kconfig`. With it on,
`redis_gateway_node`'s composition root links the IAM module's adapter. That
is a composition-root include of another module, which the boundary check
allows only through an explicit `boundary-allow.txt` line of a new kind
(`module-adapter`), added by that task.

## CI

`.github/workflows/redis-module.yml`:

```yaml
on:
  pull_request:
    paths:
      - services/redis/**
      - .github/workflows/redis-module.yml
      - scripts/check_module_boundaries.py
      - cmake/kythira_test_helpers.cmake
      - include/raft/multi_raft.hpp
      - include/raft/multi_raft_impl.hpp
      - include/raft/txn_*.hpp
      - include/raft/asio_listeners.hpp
      - include/raft/credential_verifier.hpp
      - include/raft/multi_raft_transport_handles.hpp
  push:
    branches: [main]
    paths: [ ...same list... ]
  schedule:
    - cron: '0 8 * * 1'    # weekly, after container-tests
  workflow_dispatch:
    inputs: { runtime: { type: choice, options: [both, docker, podman], default: both } }
```

Jobs:

| Job | Runs | Cost |
|---|---|---|
| `boundary` | `check_module_boundaries.py` and its fixture tests | seconds |
| `module-off` | configure with `ci_no_redis_defconfig`, build, ctest | one build leg, sccache-warm |
| `module-tests` | `ci_full_defconfig`, build the module's targets, `ctest -L 'redis\|cache'` | one build leg |
| `containers` (matrix: docker, podman) | `docker-sccache-e2e-tests`, `docker-cache-cluster-tests` | ~the two steps' share of today's container-tests legs |

The Podman leg's setup (rootless service, `aardvark-dns`,
`KYTHIRA_CONTAINER_RUNTIME=podman`) is copied from `container-tests.yml` as a
composite action, `.github/actions/setup-container-runtime`, which
`container-tests.yml` then uses as well, so the two cannot drift.
`container-tests.yml` drops the two Redis steps.

**Not required.** GitHub reports nothing for a workflow its `paths` filter
skips, so a required check from it would hang every unrelated PR.
`ci.yml` keeps building the module and running its ctest on every PR, which
is the gate. This workflow adds the container proof on the PRs that need it.
The project's auto-merge rule fires on required checks only. So a thread
that owns a Redis PR must also wait for this workflow before merging. The
module `README.md` says so (the repo has no PR template to carry it).

## Behaviour-unchanged evidence

| Property | How it is shown | Task |
|---|---|---|
| Wire, log-entry and snapshot bytes | Golden-byte tests added *before* the move for RESP replies, every `redis_kv_*` command encoding, and `get_state()` of both state machines (plain and transaction-wrapped), on a fixed seed | 1 |
| Targets, tests, labels, timeouts | Target/test snapshot script, before vs after, module on and off | 1, 4 |
| Daemon surface | `--help` and default-config dump compared | 1, 4 |
| Images | Image build context moved, Dockerfile `COPY` paths updated, image digest of the non-binary layers compared | 4 |
| Rolling upgrade | Cache-cluster scenario with two old-tree nodes and one new-tree node, sccache warm hit | 7 |

The golden files are generated once from `main` and checked in. A golden
file that changes in a later PR means a behaviour change, and that PR must
say so.

## Sequencing and conflicts

The move PR (task 4) rewrites the path of every Redis file. Any open PR that
touches those files will conflict, and rebasing it is mostly mechanical but
not free. At the time of writing, the work most likely to overlap is:

- `disk-backed-build-cache` (item D) tasks 9-13, PR #659 in CI;
- `multi-raft-transactions` task 11, the multi-version Redis KV (item T);
- IAM tasks 21-22 (item Z, wave 6, weeks away).

Plan: tasks 1-3 land any time, since they touch no moved file except to add
tests. Task 4 waits for a window with no open PR under the moved paths. The
owning thread checks `list_prs` and the D and T threads, and asks them to
hold new Redis PRs for the day. Tasks 5-7 follow. IAM tasks 21-22 then target
`services/redis/` and the seam from task 6. The IAM spec's file references
(`include/raft/redis_gateway_impl.hpp` etc.) get a one-line path note in
task 8.

## Alternatives considered

- **A separate repository now.** The ledger spec weighs this and stays in
  tree. The same reasons apply more strongly here:
  - the gateway's API dependencies (multi-raft host, transaction wrapper,
    soon the persistent state machine) are still moving;
  - a split would need a published, versioned Kythira package that does not
    exist;
  - every core change that touches the host would become a two-repo change.

  The boundary check gives most of the benefit (an enforced public surface)
  at none of that cost, and keeps the split a mechanical step later
  (Requirement 8).
- **Keep `<raft/…>` include spelling** by putting the module headers under
  `services/redis/include/raft/`. That is a zero-edit move, but it makes the
  module's headers indistinguishable from core in every include line, and
  the boundary check would have to rely on file location alone.
- **Default the module off**, like IAM and the ledger. That would change what
  every existing build produces (Requirement 2.2). It can be revisited
  separately once the module stands alone.
- **A git submodule.** "Sub-module" here means an in-tree module, as with IAM
  and the ledger. A git submodule would be the separate-repository option
  plus pinning overhead.

## Follow-ups (not in this spec)

- The IAM spec places its code under `include/raft/` and `cmd/iam_node/`.
  Moving it to `services/iam/` under the same rules, before its task 8 adds
  `iam_node`, would cost nothing then and would make the three services
  uniform.
- The ledger's task 1 should reuse `scripts/check_module_boundaries.py` and
  `services/README.md` rather than add its own check.

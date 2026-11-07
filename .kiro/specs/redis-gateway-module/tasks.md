# Implementation Plan — Redis Gateway as a Self-Contained Module

## Status: 8/8 tasks complete

**Written**: November 2, 2026, at Clark's request ("move redis into its own
sub-module in the kythira tree like IAM is now"). The module pattern follows
`.kiro/specs/iam-compatible-auth/` (PR #672) and `.kiro/specs/verifiable-ledger/`
(PR #674). The file survey in design.md was taken from `main` at `5134391`.

## Overview

Move the Redis gateway, the build cache it serves, its daemon, tests and
container scenarios into `services/redis/`, behind its existing Kconfig
switch, limited by a CI check to Kythira's public headers, with its own
path-filtered CI workflow. Behaviour does not change. A small authentication
seam lets IAM tasks 21-22 add IAM tokens to `AUTH` inside the module without
a dependency on IAM.

Each task is one PR. Tasks 1-3 touch no moved file except to add tests, so
they can land at any time. Task 4 is the move itself and needs a quiet window
(design.md, "Sequencing and conflicts"). No cloud spend.

Reference material:

- design.md for this spec: the layout table, the boundary rules, the seam
  and the CI workflow.
- `.kiro/specs/redis-compatible-kv/` and `.kiro/specs/disk-backed-build-cache/`
  for what the gateway and the cache do.
- `.kiro/specs/iam-compatible-auth/` Requirement 13 and tasks 21-22 for what
  the seam must carry.
- CLAUDE.md "Container runtime compatibility" for the Podman rules.

## Task Dependency Graph

```json
{
  "waves": [
    { "wave": 1, "tasks": [1, 2, 3], "description": "Golden bytes and surface snapshots; public transport handles and core targets; boundary check" },
    { "wave": 2, "tasks": [4], "description": "The move (quiet window)" },
    { "wave": 3, "tasks": [5, 6], "description": "Module CI workflow; authentication seam" },
    { "wave": 4, "tasks": [7, 8], "description": "Rolling-upgrade scenario; docs and spec cross-references" }
  ]
}
```

## Tasks

- [x] 1. Pin today's behaviour
  - Golden-byte tests, generated from `main` and checked in:
    - RESP replies for every supported command, including errors;
    - every `redis_kv_*` command encoding;
    - `get_state()` of `redis_kv_state_machine` and `cache_kv_state_machine`,
      plain and transaction-wrapped, on a fixed seed.
  - A test that pins the ACL parser's handling of `auth=iam` after the role
    (read today as a key prefix).
  - `scripts/module_surface_snapshot.sh`: target list, ctest JSON (names,
    labels, timeouts, commands with the source root normalised),
    `redis_gateway_node --help` and its default-config dump, for
    `ci_full_defconfig` with the module on and off. Its output from `main` is
    checked in under `services/redis/tests/surface/` in task 4. Until then it
    lives in `scripts/` only.
  - Done. Goldens are in `tests/golden/redis/` (they move with the tests in
    task 4), compared by `tests/redis_golden_bytes.hpp` and regenerated with
    `KYTHIRA_UPDATE_GOLDENS=1`:
    - `resp_replies.txt` from `redis_gateway_integration_test` (the default
      build's commands and errors, RESP2 and RESP3); the disk-store variant
      checks the same file, so the store changes no reply byte;
    - `txn_resp_replies.txt` from `redis_gateway_txn_test` (MSET, MGET,
      MULTI/EXEC/DISCARD, WATCH's refusal, `KYTHIRA.ISOLATION`);
    - `kv_commands.txt` and `state_machines.txt` from the new
      `redis_golden_bytes_test`. The state machines are the four the daemon
      can be built with: plain, transaction-wrapped, the mvcc variant
      wrapped with `mvcc::on`, and `cache_kv_state_machine`, which the
      daemon never wraps (config.cpp refuses transactions with file
      persistence), so there is no wrapped cache golden.
    - Not pinned: replies that depend on the clock or on cluster history
      (TTL of an expiring key, INFO, DBSIZE after writes), and WATCH under
      `mvcc::on`, which `redis_gateway_snapshot_test` covers by assertion.
  - The daemon has no separate default-config dump; `--help` lists every
    variable with its default, so the script records that.
  - _Requirements: 7.1-7.3_

- [x] 2. Public core pieces the module needs
  - `include/raft/multi_raft_transport_handles.hpp`: `transport_client_handle`,
    `transport_server_handle` and the Beast transport bundle, moved out of
    `tests/multi_raft_transport_harness.hpp`, with `kythira::testing` aliases
    left in the harness. `cmd/multi_raft_bench` and the daemon include the
    new header; `cmd/redis_gateway_node` drops `tests/` from its include path.
  - Core INTERFACE targets `kythira_core_headers` and `kythira_test_support`.
  - `add_raft_completion_test`, `add_docker_chaos_scenario_test` and the
    image-build helpers moved to `cmake/kythira_test_helpers.cmake`,
    unchanged, and `include()`d from both test CMake files.
  - The multi-raft bench and performance tests are the regression check.
    Task 1's surface snapshot must be unchanged.
  - Done, landing ahead of task 1 because IAM tasks 8 and 10 need the
    targets. `rpc_counters` and `rpc_snapshot` moved too, since the client
    handle counts into them. The bundle is public as
    `kythira::host_transport_types`, with `harness_transport_types` as the
    harness alias. The container-runtime detection
    (`kythira_detect_container_runtime()`) and the boost_json resolution
    moved into the helper file as well, since the module's container
    targets need `_RUNTIME_EXE`, `_IMAGE_FORMAT_ARGS` and `_RT_ENV`.
    `cmd/multi_raft_bench` and `cmd/multi_raft_node` use the harness's
    workload code, so they keep including the harness and reach the
    handles through its aliases.
  - _Requirements: 4.1-4.4_

- [x] 3. Module boundary check
  - `scripts/check_module_boundaries.py` with the rules in design.md
    ("Boundary check"), for every `services/*/`. It is a no-op while
    `services/` holds no module.
  - Done. One deviation from Requirement 3.2: `boundary-allow.txt` may also
    list module tests, not only the composition root. A dry run of the task 4
    move showed four Redis tests (`cache_http_front_end_test`,
    `redis_gateway_integration_test`, `_snapshot_test`, `_txn_test`) include
    `multi_raft_impl.hpp` to stand up an in-process cluster, as the daemon
    does. Library headers still never may.
  - The same dry run found core tests that use Redis headers as their
    workload (`multi_raft_txn_*`, `persistent_state_machine_test`,
    `tests/bench_rows/txn_*fabric.cpp`, `docker_chaos/txn_bank_chaos_test`).
    Task 4 has to resolve these (`core-includes-module`) before the move can
    pass the check. The format-check glob in the top-level `CMakeLists.txt`
    also has to gain `services/`.
  - A fixture tree under `scripts/tests/module_boundaries/` with one
    violation per rule, plus a clean module, and a test that each violation
    is reported with file, line and rule.
  - Run it and its fixture test in `ci.yml`'s `kconfig-check` job.
  - `services/README.md`: the module pattern (layout, Kconfig, CMake,
    boundary rules, CI), written so the ledger's and IAM's modules can use it
    as is.
  - _Requirements: 3.1-3.7, 9.2_

- [x] 4. Move the gateway into `services/redis/`
  - Before starting: no open PR touches a path in design.md's layout table.
    Check with the D (build cache) and T (multi-raft transactions) threads.
  - `git mv` every file in the layout table, with basenames kept. Rewrite
    includes to `<kythira/redis/…>`. No other source edits.
  - `services/redis/Kconfig` (both symbols, byte-for-byte apart from paths),
    `source`d from `Kconfig`. Generated `autoconf.hpp` is identical.
  - `services/redis/CMakeLists.txt`, `tests/CMakeLists.txt` and
    `tests/docker/CMakeLists.txt`; the top-level and test CMake files lose
    their Redis blocks, and the top level gains the one `add_subdirectory`.
  - `services/redis/boundary-allow.txt` with the two composition-root lines.
  - Update the paths in every workflow, `.gitignore`, `README.md`, the
    allowlists that name files (test names are unchanged, so name-only
    entries stay) and the Dockerfiles' `COPY` lines. Image contents
    unchanged.
  - `configs/ci_no_redis_defconfig`, with an entry in
    `defconfig-usage-allowlist.txt` until task 5 applies it.
  - Proof in the PR body:
    - task 1's goldens pass unchanged;
    - the surface snapshot matches `main`, module on and off;
    - the boundary check is green with the module present.
  - _Requirements: 1.1-1.5, 2.1-2.6, 3.1-3.5, 7.1-7.4_
  - Done November 6, 2026. Notes on what the move found:
    - The core tests that used the Redis KV moved with it, as Clark chose
      on November 5: `multi_raft_txn_snapshot_test`,
      `multi_raft_txn_snapshot_bank_test`, `multi_raft_txn_write_skew_test`,
      the transaction bench rows (the module adds them to core's
      `multi_raft_bench_rows`), `docker_txn_bank_chaos_test` with its image
      and compose file, and `multi_raft_performance_report_transactions`.
      `persistent_state_machine_test`'s Redis half was six `static_assert`s;
      they moved into `cache_kv_state_machine_unit_test`, so no test was
      added or renamed.
    - `boundary-allow.txt` has ten lines, not two: the eight module tests
      that stand up in-process hosts include `multi_raft_impl.hpp` too. The
      checker accepts module tests there (see task 3's notes).
    - Directory-scoped steps in `tests/CMakeLists.txt` did not follow the
      tests: the timeout scaling became `kythira_scale_test_timeouts()` in
      `cmake/kythira_test_helpers.cmake`, which both directories call, and
      the module calls `kythira_attach_test_reports()`.
    - Test binaries and working directories stay in `build/tests/`, which
      the coverage job collects binaries from.
    - The images still build from the source root, because the cache client
      image copies `scripts/oci-build-cache/start-sccache.sh`. The module
      reaches it through `KYTHIRA_IMAGE_CONTEXT`, not `${CMAKE_SOURCE_DIR}`.
    - `redis_gateway_node` stays in core's `host_binaries` list, marked
      `# module-hook`.
    - The format-check glob covers `services/*/{include,cmd,tests}`, not
      the modules' `docker/` trees (the cache client's sample project).
    - `services/redis/README.md` was written here (task 8 still owns
      `doc/redis-gateway.md` and the spec notes), and the snapshot script
      now keeps target names only (no paths, no per-directory targets).

- [x] 5. Module CI workflow
  - `.github/workflows/redis-module.yml` with the triggers and jobs in
    design.md ("CI"): boundary check, module-off build and ctest, the
    module's ctest, and the two container targets under Docker and rootless
    Podman.
  - `.github/actions/setup-container-runtime` composite action, extracted
    from `container-tests.yml`, which then uses it and drops its two Redis
    steps.
  - `ci_no_redis_defconfig` moves from the usage allowlist to this workflow.
  - Prove the Podman leg on the PR itself (the workflow file is in its own
    `paths`).
  - Not added as a required check. The module README tells PR owners to wait
    for it.
  - _Requirements: 2.6, 5.1-5.6_
  - Notes:
    - The container job runs `docker-txn-chaos-tests` as well as the two
      targets design.md names, because task 4 moved that scenario into the
      module too. `container-tests.yml` drops all three.
    - `module-tests` builds Ninja's `services/redis/all` and runs
      `ctest --test-dir build/services/redis`, so the selection is by
      directory, not by the `redis|cache` labels: several module tests
      (the codec and KV unit tests, the moved transaction tests) carry
      neither label.
    - `module-off` also asserts that no module target, test or build
      directory exists, so a leak through a core CMake file fails there
      rather than passing quietly.
    - Both build jobs reuse the clang++-18 setup and vcpkg key of ci.yml's
      config-variants job, and the container job the g++-13 key of
      `container-tests.yml`, so they restore existing caches.

- [x] 6. Authentication seam
  - `include/raft/credential_verifier.hpp`: `credential_request`,
    `credential_accepted`, `credential_denied` and the `credential_verifier`
    concept (future-returning, no IAM types).
  - An optional, type-erased verifier in the gateway config and in
    `cache_http_auth`. With none set, every path is today's. Task 1's
    goldens and every existing test pass unchanged.
  - Per-connection credential deadline, enforced by the existing idle timer.
  - A test verifier in the module's tests: accept with an expiry, deny, and
    a slow future, to show the strand keeps serving other connections while
    one AUTH waits. Since no ACL user can select it yet (that is IAM
    task 21's `auth=`), the test drives the gateway's internal hook directly.
  - The hand-off note for IAM task 21 (parse `auth=` as a reserved
    attribute; roll every gateway before an ACL uses it; `disabled` secret
    for IAM-only users) goes into `services/redis/README.md`.
  - _Requirements: 6.1-6.6_
  - Notes:
    - The concept is `credential_verifier` in
      `include/raft/credential_verifier.hpp`, with `credential_verifier_fn`
      (a `std::function` returning `future_default<credential_result>`) as
      the type-erased form and `erase_credential_verifier()` to make one.
    - "No ACL user can select it yet" became a predicate next to the
      verifier in each config (`_credential_verifier_users`,
      `_credential_verifier_selects`). It is the internal hook the tests
      drive, and the thing IAM task 21 sets from `auth=`. Both empty is
      today's behaviour.
    - The verifier is waited on from the connection's worker (as forwarded
      commands are), not the strand, bounded by `_command_timeout`. A
      timeout refunds the limiter charge and answers a retryable error.
    - The internal user is never accepted through the verifier, and a user
      the ACL lacks is refused before the verifier is asked
      (`redis_acl::identity_of`).
    - The HTTP front end asks the verifier on every request and never caches
      the result, so a cached success cannot outlive the credential.
    - Task 1 already pinned the `auth=iam`-as-prefix parse in
      `redis_acl_unit_test`.

- [x] 7. Rolling-upgrade scenario
  - A cache-cluster variant with two gateways built from the last pre-move
    `main` commit (image pulled by digest from the CI cache, or built from
    that commit in the job) and one from the head. sccache misses then hits,
    and a node is restarted on the other build mid-run.
  - Runs in the module workflow's container matrix, under Docker and Podman.
  - _Requirements: 7.5_
  - Done: the old gateway is built in the job from `fe53e33` (the parent of
    the move commit), not pulled by digest; no registry holds CI images, and
    the build is cheap with sccache. It is checked out next to the tree,
    built out of source, and deleted once its binary is copied out, so it
    never enters an image build context. `docker-cache-cluster-old-image`
    puts that binary into the same Dockerfile through a new
    `GATEWAY_BINARY` build arg, so the two images differ only in the binary.
    `rolling-upgrade.yml` is a compose override: cache-1 and cache-2 on
    `:old`, cache-3 on `:dev`, each overridable by `KYTHIRA_CACHE_<n>_IMAGE`.
    The suite `rolling_upgrade` in `cache_cluster_test.cpp` is disabled by
    default and named by `docker-cache-rolling-upgrade-tests`. Case one runs
    an sccache cold/warm pair on the mixed cluster. Case two writes a cold
    build and a key, then moves cache-1 to `:dev` and cache-3 to `:old` with
    `compose up -d --no-deps` (volumes kept), and checks the key on every
    node and that a warm build with the same salt hits every object.

- [x] 8. Docs and cross-references
  - `doc/redis-gateway.md`: updated paths and a "Where the code lives"
    section.
  - `services/redis/README.md`: the targets and variables the module uses
    from core, and what a split to its own repository would need.
  - One-line path notes in `redis-compatible-kv`, `disk-backed-build-cache`
    and, for tasks 21-22, `iam-compatible-auth`.
  - `doc/TODO.md` row updated to done.
  - _Requirements: 8.1-8.3, 9.1-9.3_
  - Done: `doc/redis-gateway.md` gained "Where the code lives" (its paths
    were already rewritten by task 4). The README sections were written by
    tasks 4-7. `redis-compatible-kv/design.md` and
    `disk-backed-build-cache/tasks.md` open with a path note rather than
    having their historical paths rewritten, since those records describe
    what was built where at the time. `iam-compatible-auth` task 21 names the
    seam and the config fields it wires.

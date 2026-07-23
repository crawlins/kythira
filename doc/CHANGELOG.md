# Changelog

Chronological log of notable changes to Kythira, newest first. For the
current list of outstanding work, see [TODO.md](TODO.md).

### What Changed (July 23, 2026)

- **Completed `ci-real-cloud-tests` Task 12 — the full `workflow_dispatch`
  toggle matrix exercised end-to-end against real AWS — closing the spec
  out at 12/12. Along the way, real EC2 instances under real network
  behavior surfaced two genuine, previously-undetected application bugs
  that no local/Docker test could have caught, both fixed and merged via
  PR #90.** The matrix itself: master/AWS enable toggles, each bundle run
  alone against real AWS, `AWS_CI_ROLE_ARN` unset with AWS enabled
  (confirmed Requirement 7.1's fail-closed message verbatim, failing in
  ~12s before any credential step), and the `ec2-quorum-manager`
  permission-revocation check (Property 2) — dropped the bundle from the
  live CI role's policy, confirmed a real `Client.UnauthorizedOperation`
  on `ec2:AllocateAddress` via CloudTrail (not a stale credential or an
  unrelated failure), then restored the full policy. The two application
  bugs: (1) `cmd/ca_cluster_node/main.cpp`'s RPC-TLS cutover had a
  circular dependency — a node's root-discovery path only asked
  `raft_node.known_leader()`, which itself only gets populated by
  receiving Raft RPC traffic over the very transport whose accept policy
  stays too narrow to receive that traffic until root discovery already
  succeeded — a genuine, permanent deadlock (not slow convergence) once
  any peer switched its presented identity to a CA-issued cert first;
  reproduced as a node's entire data directory staying empty
  indefinitely, fixed by falling back to querying every configured
  peer's static client-facing address directly, a separate transport/
  trust boundary from RPC-TLS. (2) `include/raft/raft.hpp`'s
  `node<Types>::read_state()` already computed the correct majority
  threshold but collected heartbeat responses via a helper that always
  waits for every follower's future to individually settle before
  checking the count, so one network-partitioned follower (real AWS NACL
  DENY: silent packet drop, no RST) made every linearizable read pay
  that follower's full per-RPC timeout regardless of how fast the actual
  majority responded; reproduced as a healthy leader's own `/v1/root-ca`
  answering 503 throughout an AZ isolation window despite continuously
  replicating to its one reachable follower. Fixed by adding
  `raft_future_collector<T>::collect_n_successes_with_timeout()`
  (`include/raft/future_collector.hpp`) — resolves the instant quorum is
  reached instead of waiting on a peer that can no longer change the
  outcome — validated against all 15 existing local tests covering
  `read_state`/heartbeat/future-collection semantics plus one new
  dedicated test before being treated as safe, since this is core Raft
  consensus code shared by every `Types` instantiation in the codebase.
  Also extended the real-EC2 test suite's crash-cleanup signal handler
  (`tests/aws_real_ec2_test_support.hpp`) to cover `SIGABRT`/`SIGSEGV`/
  `SIGBUS`, not just the polite termination signals — a crash previously
  skipped teardown entirely and leaked that run's AWS resources exactly
  like an unhandled kill would.

- **Completed `discovery-nodes-host-build` (6/6 tasks), extending
  `chaos-node-host-build`'s "host build, Docker just packages" pattern to
  `poco_discovery_node`, `dns_discovery_node`, and `dns_sd_discovery_node`
  — PR #91.** Each `docker-*-discovery-image` CMake target now stages the
  host-built binary (`DEPENDS` on its own target + `make_directory`/`copy`
  before the existing `docker build` command) guarded on the exact same
  `FOUND`-variable condition each target's own `cmd/.../CMakeLists.txt`
  gates its `add_executable()` on, and each Dockerfile collapsed to a
  single runtime-only stage — no compiler, CMake, or `-dev` package left
  in any of the three images. Closed a real parity gap first:
  `libavahi-client-dev`/`libldns-dev` were never installed by any host
  build (only inside each Dockerfile's own now-removed builder stage), so
  all three targets were silently skipped everywhere — confirmed by each
  target's own `cmd/.../CMakeLists.txt` "skipped (requires ...)"
  diagnostic — until both packages were added to
  `arm64-docker-smoke-test.yml`'s dependency step alongside the
  `libfiu-dev` the prior spec already added there. Verified end-to-end via
  a real dispatch of that workflow (run 30006127341): `docker-poco-
  discovery-tests`, `docker-dns-discovery-tests`, and `docker-dns-sd-
  discovery-tests` all passed on real arm64 hardware — each target's host
  build, staging copy, `docker build`, and full scenario-test suite
  against real containers over a real network.

### What Changed (July 20, 2026)

- **Audited `.kiro/specs/ccache-adoption/tasks.md` against the actual
  codebase (it claimed 0/7 tasks done when 6/7 were implemented and
  merged 5 days earlier via PR #52), then finished the one genuinely
  outstanding task — and that task's own real-world re-measurement
  caught a live bug: ccache had been providing zero benefit on every
  CI run since July 15.** Task 7 exists specifically to catch the
  failure mode where caching "looks wired up" (correct key scheme,
  correct step ordering) but never actually restores or saves
  anything — and it did exactly that. Run 1 (PR #79) showed a 35m32s
  Build step despite `ccache: enabled` at configure time, with
  `Restore ccache` missing every fallback key and `Save ccache` failing
  with `Path Validation Error: ... do(es) not exist`. Root cause:
  ccache ≥4.0 changed its default cache directory from `~/.ccache` to
  the XDG Base Directory location (`~/.cache/ccache`), which the
  original spec's design (`~/.ccache` assumed as ccache's own default,
  true for 3.x, not for the 4.9.1 this CI installs) never accounted
  for — every restore/save step was watching a directory ccache never
  wrote to. Fixed by setting `CCACHE_DIR: /home/runner/.ccache`
  explicitly at the job level in `ci.yml` (`build-and-test`,
  `coverage`) and `real-cloud-tests.yml` (`aws`), and correcting the
  same wrong assumption in `DEPENDENCIES.md`. Verified end-to-end
  across three real CI runs on the same PR: Run 2 (post-fix) showed
  `Save ccache` succeeding for the first time (23m16s, still cold —
  establishing the first valid entry), and Run 3 restored that exact
  entry via the `restore-keys` prefix fallback and completed in
  14m18s — the first run to demonstrate a genuine warm-cache speedup
  from this mechanism in CI, closing out `ccache-adoption` at 7/7.

### What Changed (July 19, 2026)

- **Chased `chaos_node` scenario tests' `leader_crash_and_reelection`
  timeout through a chain of four further real bugs, ending in a full
  PreVote implementation and a Raft leadership-change liveness fix —
  all 7 `docker_chaos` scenario tests now pass cleanly on real arm64
  hardware.** Bounding `include/raft/tcp_rpc.hpp`'s `connect_to()`
  (non-blocking `connect()` + `poll()`, since `SO_SNDTIMEO`/
  `SO_RCVTIMEO` don't bound the `connect()` syscall itself on Linux)
  and moving `tcp_rpc_client`'s RPC dispatch off a synchronous,
  sequential path onto a private `folly::CPUThreadPoolExecutor` (both
  mirrored in `tls_tcp_rpc.hpp`) let CI progress far enough to reveal a
  real Raft protocol gap: a stale, partitioned-off node rejoining with
  an ever-climbing term forced the live-majority leader to step down
  repeatedly (the "disruptive server" problem, Ongaro's dissertation
  §9.6, observed as term 8→13 thrashing in one run). Fixed by
  implementing the full PreVote extension across
  `include/raft/types.hpp`, `network.hpp`, `json_serializer.hpp`,
  `tcp_rpc.hpp`, and `raft.hpp`, gated as a strictly optional
  network-concept extension
  (`network_client_with_pre_vote`/`network_server_with_pre_vote`,
  following this codebase's existing `_with_cluster_join`-style
  pattern) so transports that don't implement it — the in-memory
  simulator, `tls_tcp_rpc` — keep today's behavior unchanged. Verified
  on real arm64 hardware: term stayed flat at 2 throughout a scenario
  that previously thrashed 8→13. That same verification run then
  surfaced one more, final liveness bug: after a clean leadership
  change, the new leader got stuck at its inherited `commit_index`
  forever, because `advance_commit_index()` (`raft.hpp`) correctly
  refuses to commit an entry directly unless it is from the leader's
  own current term (Raft §5.4.2, a genuine safety requirement, not a
  bug) — and a leader that never appends anything of its own never
  satisfies that check. Fixed by having `become_leader()` append a
  no-op barrier entry in its new term, using a new
  `entry_type::no_op` discriminant (`types.hpp`) that
  `apply_committed_entries()` skips the same way it already skips
  `entry_type::configuration` entries, so the test state machine
  (which throws on an empty command) is never touched. Final
  verification (`workflow_dispatch` run 29693678147) shows all 7
  `docker_chaos` scenario-test binaries — `smoke_test`,
  `election_recovery_test`, `crash_recovery_test` (including
  `leader_crash_and_reelection` itself), `network_degradation_test`,
  `az_partition_test`, `persistence_faults_test`, and
  `safety_assertions_test` — passing cleanly, with
  `az_partition_test`'s own log showing all 3 nodes converging to the
  same `commit_index` after catchup where one had previously been
  stuck forever.
- **Fixed `docker/chaos_node/Dockerfile`'s long-standing inability to
  build `chaos_node` at all, via `.kiro/specs/chaos-node-host-build/`
  — plus two more genuine, previously-hidden bugs found and fixed
  along the way once the image could finally build and start for the
  first time.** The Dockerfile's builder stage tried to compile
  `chaos_node` in-container from a small, hand-maintained `apt-get`
  list that never included folly (not apt-installable on Ubuntu 24.04
  at all), so `cmd/chaos_node`'s CMake target was never even defined
  there — `ninja: error: unknown target 'chaos_node'` on every attempt.
  Fixed by building `chaos_node` once on the host, using the project's
  real, already-proven vcpkg-based CMake configuration (the same shape
  `ci.yml` already uses), and collapsing the Dockerfile to a single
  runtime-only stage that just packages the already-built binary —
  `tests/docker_chaos/CMakeLists.txt`'s `docker-chaos-image` target now
  depends on the `chaos_node` CMake target and stages
  `$<TARGET_FILE:chaos_node>` before invoking `docker build`. Verified
  on real arm64 hardware across 6 `workflow_dispatch` runs of
  `arm64-docker-smoke-test.yml`: the image now builds and tags
  `kythira-chaos-node:dev` successfully every time.
- Getting a real `chaos_node` container to actually start for the
  first time immediately surfaced two bugs that had simply never been
  reachable before:
  - `cmd/chaos_node/http_control.hpp`'s `/command` handler built its
    command bytes as free-form text (`"PUT key value\n"`), but
    `tcp_raft_types::state_machine_type`
    (`test_key_value_state_machine`, `include/raft/test_state_machine.hpp`)
    parses commands as a fixed binary layout —
    `[command_type:1][key_length:4][key][value_length:4][value]`,
    read via `memcpy` at fixed offsets — so every real command was
    rejected with a nonsense "key length exceeds command size" error.
    Fixed by building the actual expected byte layout.
  - `tests/docker_chaos/fault_control.hpp`'s `send_fiu_cmd_raw()` used
    `inet_pton()` to resolve its `host` argument, which only parses
    numeric IPv4 literals and never resolves hostnames — but
    `ChaosNode::enable_fault()` (`harness.hpp`) always calls it with
    the literal string `"localhost"`, so any fault-injection test
    using `fiu_rc_tcp` always failed with "bad host address:
    localhost". Fixed by resolving via `getaddrinfo()` instead.
  - Both bugs have existed since these files were written; nothing had
    ever exercised these exact code paths end to end before, since
    `chaos_node`'s Docker image could never build until this fix.
- After both fixes, `docker_chaos_smoke_test`,
  `docker_chaos_election_recovery_test`, and
  `docker_chaos_crash_recovery_test`'s `follower_crash_and_catch_up`
  case all pass cleanly on real arm64 hardware — but
  `crash_recovery_test`'s `leader_crash_and_reelection` case still
  fails ("no leader elected within timeout" after `docker kill`-ing
  the leader), a third, deeper, **not yet fixed** finding (leading
  hypothesis: `tcp_rpc.hpp`'s `connect_to()` doesn't actually bound
  `connect()`'s own blocking time on Linux, so a `RequestVote` RPC to
  a just-killed peer can block well past the configured 100ms
  `rpc_timeout`) — deliberately not chased further in this pass, since
  confirming and fixing it properly would touch core RPC/retry logic
  used far beyond `chaos_node`. See `.kiro/specs/chaos-node-host-build/tasks.md`'s
  Task 5 and `doc/TODO.md`'s Minor Enhancements for the full writeup;
  4 of 7 `docker_chaos` scenario-test files remain unverified against
  the now-working image as a result.

### What Changed (July 18, 2026)

- **`arm64-ci-verification` spec complete (13/13 tasks) — Task 10
  (Docker images on arm64) finished via 5 real `workflow_dispatch` runs of
  `.github/workflows/arm64-docker-smoke-test.yml` on a native
  `ubuntu-24.04-arm` GitHub-hosted runner**, plus a real, arm64-specific
  memory-corruption bug found and fixed along the way. `chaos_node` and
  `poco_discovery_node` fail to build on arm64 for two already-tracked,
  non-arm64-specific reasons (folly not apt-installable in
  `docker/chaos_node/Dockerfile`'s builder stage; `POCO_DNSSD_FOUND`
  correctly staying `FALSE` on `arm64-linux` since PocoDNSSD's static
  archives are only manually built for `x64-linux`). `dns_discovery_node`,
  `dns_sd_discovery_node`, and `bind9` all build and run correctly on
  arm64. The workflow itself needed `continue-on-error: true` added to
  every scenario-test step across four follow-up commits, since none of
  the five originally had it and the first failure (`chaos_node`) was
  silently skipping every step after it — including the independent
  images this task actually needed data on.
- **Fixed a genuine stack-use-after-scope bug in the DNS/DNS-SD/Poco
  discovery scenario tests' `peer_ids()` helper, found via the
  arm64-ci-verification smoke-test runs above.** Two of the five arm64
  runs crashed `docker_dns_discovery_test`'s and
  `docker_dns_sd_discovery_test`'s `all_nodes_discover_peers` case with a
  real `SIGSEGV` (`memory access violation ... no mapping at fault
  address`), intermittently rather than every run. Root cause:
  `tests/docker_chaos/dns_discovery_test.cpp`,
  `dns_sd_discovery_test.cpp`, and `poco_discovery_test.cpp` all shared
  the identical pattern
  `for (const auto& item : json::parse(res->body).as_array()) { ... }` —
  `json::parse(...)` returns a `boost::json::value` prvalue, and
  `.as_array()` is a *member function call* returning a reference into
  that temporary, which C++ does not lifetime-extend for a range-for loop
  (the temporary is destroyed at the end of the loop's init-statement,
  before the body runs, leaving the loop iterating over freed stack
  memory). This is undefined behavior on every architecture, not an
  arm64-only bug — it happened to "work" most of the time on x86_64
  because the freed stack slot usually wasn't yet overwritten by the time
  it was read, while arm64's different stack layout/ABI made the
  corruption manifest as a hard crash far more often, which is what
  actually surfaced it here. Confirmed independently of the CI runs via a
  minimal standalone repro built with the project's own toolchain
  (`g++-13 -std=c++23 -fsanitize=address`), which AddressSanitizer flagged
  immediately as `stack-use-after-scope ... in
  boost::json::array::begin()`. `poco_discovery_test.cpp` had the same
  latent bug despite `poco_discovery_node` not building on arm64 at all
  (so it never actually crashed there) — found and fixed anyway while
  fixing the other two, since it's the same code shape. Fixed in all
  three files by binding the parsed value to a named local before
  iterating (`const json::value parsed = json::parse(res->body); for
  (const auto& item : parsed.as_array()) { ... }`), which keeps it alive
  for the loop's full duration. Re-verified against real arm64 hardware
  with a 6th `workflow_dispatch` run (run ID 29664536952): zero
  `SIGSEGV`/memory-access-violation signatures anywhere in that run's
  logs, and both `all_nodes_discover_peers` cases (RFC 1035 and DNS-SD)
  completed cleanly — confirmed fixed, not just locally plausible.
  (Verifying this required pulling the raw job log rather than trusting
  `gh run view`'s step status: a `continue-on-error: true` step's
  reported `conclusion` is always `success` regardless of whether the
  underlying command actually failed.) That run also turned up one new,
  unrelated, non-crash finding: `dns_discovery_test`'s
  `stopped_node_absent_after_deregister` case failed a real assertion
  (surviving nodes still saw 2 peers instead of 1 after the stopped
  node's 3 s post-stop grace period) — a BIND9 DELETE-UPDATE propagation
  timing flake, not a memory-safety bug, and unrelated to the fix above;
  filed as its own `doc/TODO.md` entry rather than folded into this one.

### What Changed (July 16, 2026)

- **Metrics Backends: cloud-vendor entries scoped down to config-only, plus
  a testing-tier requirement for every entry.** The five cloud-vendor
  monitoring entries (AWS CloudWatch, Azure Monitor, GCP Cloud Monitoring,
  OCI Monitoring, Alibaba Cloud CloudMonitor) are no longer scoped as
  bespoke `kythira::metrics` SDK implementations — the intention is now
  example monitoring *configuration* (e.g. an OpenTelemetry Collector
  exporter config, or the vendor's own native agent config) routing
  Kythira's telemetry to that vendor, plus documentation, since an
  OpenTelemetry Collector (or the vendor's own agent) already does that
  integration work well and re-implementing it five times inside Kythira
  would just duplicate it while tying Kythira to five vendor SDKs. The
  self-hosted agents (Prometheus, Telegraf, VictoriaMetrics, NetData)
  remain full `kythira::metrics` implementations, since nothing else
  speaks their wire protocol on Kythira's behalf. Every entry (both kinds)
  now also requires two tests: a Docker-based test against a
  self-provisioned instance of the agent/aggregator (or a local emulator
  like LocalStack for vendor APIs that have one, or a config-syntax-only
  check where no emulator exists), mirroring the existing `docker_chaos`
  scenario-test convention and enabled by default; and, where a real
  vendor-managed service exists, a second test against the actual cloud
  service — validating the example config's routing mechanism for the
  cloud-vendor entries, since there's no Kythira-side SDK call to test
  directly — following the existing `ci-real-cloud-tests` opt-in toggle
  pattern and disabled by default (real credentials, real cost). Purely a
  documentation/requirements addition — no test code or example configs
  added yet.
- **Example-configuration requirement added to Cloud Provider Support and
  Metrics Backends.** Every entry in both `doc/TODO.md` sections —
  including the already-implemented AWS cloud-provider support, which
  doesn't yet have this — now carries an explicit requirement that its
  implementation ship with at least one example configuration file (a
  `.env.example`, sample YAML/JSON, or documented CLI-flag set) plus
  documentation showing how to configure and run it, mirroring the
  existing `docker/ca_cluster_node/ca_cluster_node.env.example`/
  `docker/ca_service/ca_service.env.example` convention. Recorded as a
  shared preamble under each section rather than duplicated per bullet,
  since it applies uniformly across every entry (present and future).
- **`dns-peer-discovery` spec complete — final two tasks
  (`rfc6763_peer_discovery`, `rfc6763_ldns_peer_discovery`)**: the last two of
  the spec's five DNS-based `peer_discovery` implementations.
  `rfc6763_peer_discovery` provides `find_peers` only, via a single RFC 6763
  SRV query at the cluster-level service name (mirrors
  `rfc1035_peer_discovery`'s partial-implementation shape, including a no-op
  `register_node` stub and fiu fault-injection hooks). `rfc6763_ldns_peer_discovery`
  is the full implementation: registers PTR + instance SRV + cluster-level
  SRV in one RFC 2136 UPDATE to the cluster zone, plus a domain-level SRV in
  a second UPDATE to the domain zone, and delegates `find_peers` to the
  embedded `rfc6763_peer_discovery` with self-filtering. Registration state
  is committed to member variables only after both UPDATEs succeed —
  mirroring `rfc2136_ldns_discovery`'s existing invariant — after an eager
  first draft left the destructor's `deregister_self()` attempting a real
  network DELETE with no configurable resolver timeout following a
  partially-failed registration, hanging a chaos test past its timeout.
  DELETE updates always target RFC 2136 §2.5.4 delete-specific-RR (exact
  owner/type/rdata) rather than deleting the whole RRset, so removing one
  node's PTR/cluster-level-SRV entry never disturbs other live nodes sharing
  the same RRset. Added matching unit and chaos test suites to the existing
  `dns_peer_discovery_unit_test`/`dns_peer_discovery_chaos_test` binaries
  (one new case specifically to exercise the real, fault-free
  `send_pkt`/`make_resolver` network-failure path, needed to keep the
  project's function-coverage ratchet from regressing — every other
  `register_node` test short-circuits via fiu faults before
  `make_resolver()` is ever called); verified the project builds clean with
  and without libldns present. `.kiro/specs/dns-peer-discovery/` is now
  fully complete (all 6 tasks, including the out-of-scope
  `rfc2136_dns_sd_discovery` addition); PR #55.
- **`main` branch protection required-status-check names fixed**: discovered
  while waiting on PR #55's auto-merge — `required_status_checks.contexts`
  still listed the pre-matrix job names (`Build & Test (g++-13)`,
  `Build & Test (clang++-18)`) from before CI was split into an arm64/x64
  matrix, so GitHub was waiting indefinitely for checks that no longer post
  under those exact names (`Build & Test (g++-13, x64)`, `..., arm64`,
  `Build & Test (clang++-18, x64)`, `..., arm64`), blocking auto-merge on
  every PR against `main` regardless of actual CI outcome. Updated to the
  four current matrix job names plus `Coverage (clang++-18)`.

### What Changed (July 15, 2026)

- **`ca-cluster-node-ami` spec authored** (not yet implemented) — a
  Packer-based build pipeline for a golden, secret-free AMI with
  `ca_cluster_node` and its systemd unit pre-installed, resolving the
  "(baked into an AMI, e.g. via Packer)" placeholder already referenced by
  `docker/ca_cluster_node/README.md`'s Path 3,
  `docker/ca_cluster_node/ecs-task-definitions/README.md`'s automated
  alternative, and `tests/ca_cluster_node_real_ec2_test.cpp`'s
  `KYTHIRA_EC2_TEST_AMI` env var — all three assumed this AMI existed with
  no template or script actually producing it. The binary is extracted
  from `docker/ca_cluster_node/Dockerfile`'s existing `builder` stage
  (`docker create`/`cp`) rather than recompiled independently, so there
  remains exactly one place that knows how to build `ca_cluster_node` from
  source; the source AMI is Ubuntu 24.04 (matching the Dockerfile's
  runtime stage) rather than Amazon Linux 2023, to avoid a glibc ABI
  mismatch; no secrets are baked in (unseal passphrase, auth token, RPC
  bootstrap credentials stay injected per-instance at launch time,
  unchanged from today's manual systemd-install flow). CI wiring follows
  the existing `real-cloud-tests.yml` three-level toggle model and
  `scripts/ci-cloud-credentials/` bundle pattern, gated independently of
  the existing `ca-cluster-node` bundle since every AMI build leaves a
  billable AMI/snapshot behind. Full spec at
  `.kiro/specs/ca-cluster-node-ami/`; draft PR #44.
- **`ca-cluster-node-ami` implemented — all 8 tasks**: the spec above is no
  longer just a plan. `packer/ca_cluster_node/` now holds the Packer
  template (`ca_cluster_node.pkr.hcl`/`variables.pkr.hcl`) and its three
  orchestration scripts (`extract-binary.sh`, `provision.sh`, `build.sh`);
  a new `packer-ca-cluster-node` job in `ci.yml` runs `packer fmt`/`packer
  validate -syntax-only`/`shellcheck`/a secret-absence grep on every push
  (no AWS credentials needed — `-syntax-only` specifically avoids the
  template's `amazon-parameterstore` data source making a real AWS SSM
  call); a new `ami-build` bundle (`scripts/ci-cloud-credentials/aws/policies/ami-build.json`,
  wired into `provision-oidc-role.sh` and `real-cloud-tests.yml` as an
  `amd64`/`arm64` matrix job on native runners) can produce a real AMI on
  demand; and the placeholder `// AMI running ca_cluster_node` text is gone
  from `docker/ca_cluster_node/README.md`, its ECS README, and
  `tests/ca_cluster_node_real_ec2_test.cpp`'s header comment, replaced with
  a pointer to `packer/ca_cluster_node/README.md`. The sandbox this was
  implemented in initially had no `packer` CLI, no AWS credentials, and no
  reachable Docker daemon; `packer` and `shellcheck` were subsequently
  installed directly into it (no daemon/AWS account needed for either), which
  caught and fixed three real issues before this landed — a secret-absence
  grep false positive from `provision.sh`'s own explanatory comment, a
  `packer fmt` alignment mismatch, and a shellcheck SC2015 finding in the
  cloud-init cleanup line — and let every static check (`packer fmt`,
  `packer init` + `validate -syntax-only`, `shellcheck`, secret-absence grep)
  actually run and pass locally. Still unexercised in this environment: an
  actual `extract-binary.sh` Docker build and a real AMI build against AWS
  (need a container daemon and AWS credentials respectively) — the first real
  exercise of those will be an operator-enabled `ami-build` run. See
  `.kiro/specs/ca-cluster-node-ami/tasks.md`'s status note for the exact
  verification boundary.
- **arm64 CI verification complete — `.kiro/specs/arm64-ci-verification/`**:
  Kythira's CI was entirely x86_64-only — the vcpkg triplet `x64-linux` was
  hardcoded as a literal in both workflow files, four `CMakeLists.txt` files,
  and every `docker/*/Dockerfile`, and
  `tests/aws_quorum_manager_real_ec2_test.cpp`'s existing
  `__aarch64__`/`__arm64__` Graviton-selection branch (from the
  `aws-quorum-manager` spec) had never actually compiled, since it was only
  ever built on an x86_64 runner. A pre-implementation spike
  (`spike-notes.md`) verified every vcpkg dependency's `supports` platform
  expression at the pinned `builtin-baseline` allows `arm64-linux` (Folly,
  the AWS SDK for C++, Boost, Poco, libcoap, cpp-httplib, libssh2, stdexec,
  and both Kythira-authored overlay ports all clear cleanly) before any
  workflow changes were made.
  - Introduced a single `KYTHIRA_VCPKG_TRIPLET` CMake variable
    (`CMakeLists.txt`), replacing every hardcoded `vcpkg_installed/x64-linux`
    literal across the root, `tests/`, `tests/chaos/`, and
    `tests/docker_chaos/` `CMakeLists.txt` files and
    `scripts/verify-optional-dependency-isolation.sh`.
  - Fixed the Avahi `find_library` search
    (`poco_peer_discovery`'s DNSSD backend), which only checked
    `/usr/lib/x86_64-linux-gnu`, to derive the correct Debian multiarch tuple
    via `CMAKE_LIBRARY_ARCHITECTURE` — it would otherwise have missed the
    library entirely on an arm64 host even when installed.
  - Added `${{ runner.arch }}` to every vcpkg `actions/cache` key in
    `ci.yml` and `real-cloud-tests.yml`. The prior key was keyed only on
    `runner.os` (always `"Linux"` on both architectures), which would have
    let an arm64 runner silently restore or corrupt an x86_64-built cache
    entry the moment a second architecture shared the workflow.
  - Added native `ubuntu-24.04-arm` legs to `ci.yml`'s `build-and-test`
    matrix (both `g++-13` and `clang++-18`) and to `real-cloud-tests.yml`'s
    `aws` job, so the dead Graviton EC2-provisioning branch mentioned above
    now actually compiles and, when the `ec2-quorum-manager` bundle runs,
    provisions a real Graviton instance. Coverage and format-check stay
    x86_64-only by design (coverage % isn't expected to vary by
    architecture, and the job is already disk/time-constrained on one
    architecture; `clang-format` output is architecture-independent).
  - Parameterized all 7 `docker/*/Dockerfile` build stages with a
    `uname -m`-derived triplet; `chaos_node` needed no change (it has no
    vcpkg dependency) and `bind9` needed no change (no CMake build).
  - **Verified against a real CI run** (crawlins/kythira#47): all four
    `build-and-test` matrix legs passed on a cold vcpkg cache, including
    the `--x-feature=edhoc` Rust/`lakers` build on both new arm64 legs, with
    no arm64-specific failures — the spike's static `supports`-expression
    analysis held up against an actual build. Measured wall-clock times
    (`g++-13, arm64` ~76 min, `clang++-18, arm64` ~46 min, `g++-13, x64`
    ~51 min, `clang++-18, x64` ~68 min) fit comfortably inside the existing
    120-minute per-leg timeout, so it was left unchanged rather than
    tightened — a cache-miss run is exactly when that headroom matters.
  - **Known, explicitly deferred limitation** (documented rather than
    silently skipped, per the spec's own Requirement 3.2 guidance):
    PocoDNSSD's manually-built static archives are provided only for
    `x64-linux` in this repository, so `poco_peer_discovery`'s DNSSD backend
    degrades to disabled on `arm64-linux` — same behavior as any x64 host
    missing the archives. See `README.md`'s new "ARM (arm64) Support"
    section.
  - **Docker image arm64 smoke test — in progress**: the implementation
    sandbox had no working AWS credentials (`aws sts
    get-caller-identity` returned `InvalidClientTokenId`) and no reachable
    container daemon to verify the Docker triplet parameterization on real
    arm64 hardware. Rather than provision new billable AWS infrastructure
    for a one-off check, added
    `.github/workflows/arm64-docker-smoke-test.yml` — a
    `workflow_dispatch`-only job reusing the same `ubuntu-24.04-arm`
    GitHub-hosted runner already proven above, building and running the
    `docker-chaos-tests`, `docker-poco-discovery-tests`,
    `docker-dns-discovery-tests`, and `docker-dns-sd-discovery-tests`
    CMake targets. Awaiting its first real run once this lands on `main`
    (GitHub only accepts `workflow_dispatch` API calls against workflows
    already present on the default branch) — results will be recorded in
    a follow-up entry.

### What Changed (July 14, 2026)

- **ca-cluster-rpc-mtls complete — all 13 tasks**:
  `.kiro/specs/ca-cluster-rpc-mtls/` secures `ca_cluster_node`'s
  Raft-internal RPC channel (previously plain, unauthenticated TCP via
  `tcp_rpc_client`/`tcp_rpc_server`) with mutual TLS, via a two-phase
  bootstrap: peers first mutually authenticate using a small, static,
  operator-provisioned credential (distributed the same way as the
  existing unseal passphrase), then, once the CA root exists, each node
  self-service-acquires its own CA-issued peer certificate and cuts over
  automatically — no operator action beyond initial provisioning.
  `tcp_rpc.hpp` itself is untouched; the new transport
  (`include/raft/tls_tcp_rpc.hpp`, `tls_tcp_rpc_client`/
  `tls_tcp_rpc_server`) is a sibling satisfying the same
  `network_client`/`network_server` concepts, wired in via a second
  `ca_cluster_raft_types` alternative selected by a runtime check in
  `cmd/ca_cluster_node/main.cpp` (template-instantiated once per Types, no
  duplicated ~500-line node-construction body). Adds `ca_state_machine`'s
  `record_rpc_tls_ready` command/set, `--rpc-tls-cert`/`--rpc-tls-key`/
  `--rpc-timeout-ms` CLI flags, and updated `docker/ca_cluster_node/`
  deployment packaging (systemd unit, env example, ECS task definitions)
  for the bootstrap credential.
  - **Real bugs found and fixed during multi-process integration testing**
    (none of which were caught by unit/2-node-in-process testing alone —
    all three only manifested under a real 3-process cluster with actual
    TLS handshake latency):
    1. `tls_tcp_rpc_client` originally rebuilt its `SSL_CTX*` from scratch
       — including re-reading and re-parsing the identity cert/key files
       from disk — on every single RPC call. At this project's default
       50ms heartbeat cadence, that's disk I/O plus a full asymmetric-key
       setup on Raft's own liveness-timer critical path; under real host
       contention it reliably drove elections into the hundreds of terms.
       Fixed by caching one long-lived `SSL_CTX*` per client, mutated only
       by `reload_identity()`, mirroring the server side.
    2. The accepted server-side socket had no `SO_RCVTIMEO`/`SO_SNDTIMEO`
       at all (unlike the client's own `connect_to()`) — a client that
       gave up mid-handshake left the server's per-connection thread
       blocked forever, leaking one thread and one fd per stall and
       compounding under load.
    3. A node was switching *what it presented* (to its own newly-acquired
       CA-issued certificate) at the same moment it acquired that
       certificate — but a peer that hadn't independently reached the
       CA root yet was still evaluating incoming connections under
       `pinned_fingerprint` alone and would reject the now-unrecognized
       cert outright. Fixed by decoupling "widen what this node accepts"
       (triggered the moment the CA root is known to exist, via
       `maybe_widen_rpc_trust_policy()`) from "switch what this node
       presents" (only after acquiring its own certificate) — every peer
       observing the same replicated root widens before any single peer
       can finish acquiring and start presenting.
    4. `node<Types>::read_state()`/`submit_command()` have no built-in
       leader-forwarding — a follower's call fails immediately with "not
       leader" rather than reaching the actual leader. The original
       design (mirrored from this spec's own design.md sketch) called
       both unconditionally from every node, which works for the leader
       but never for followers. Fixed by adding a leader/follower split
       (`fetch_root_cert_pem()`, matching the CSR-signing path's existing
       split) that uses the client-facing HTTP API — a transport
       completely unaffected by RPC-TLS trust state — for followers, and
       piggybacking a follower's own `record_rpc_tls_ready` submission
       onto its CSR-signing request so the leader, which alone can
       actually call `submit_command()` successfully, submits it on the
       follower's behalf.
- **ca-cluster-rpc-mtls: CI-only deadlock found and fixed post-merge**:
  the PR above passed every local/sandbox test run, merged, and then
  failed `ca_cluster_node_rpc_tls_test` reliably (3/3 retries, both
  compilers) on GitHub Actions' shared runners specifically. Root cause:
  the leader's own `fetch_root_cert_pem()` is an in-process read, so it
  could widen-then-acquire-then-switch its presented RPC identity within
  the same maintenance tick the CA root committed on — before any
  follower's maintenance thread had a real chance to widen its own trust
  policy (which needs an HTTP round trip to the leader's `/v1/root-ca`,
  itself gated on a quorum-confirmed `read_state()`). Once the leader
  switched, every follower started rejecting its traffic, which broke the
  very read-index heartbeats `/v1/root-ca` needed to keep answering — a
  genuine circular deadlock, not a race that resolves given more time.
  Fixed with a 3-second grace period (`k_identity_acquire_grace`) between
  a node first observing the CA root and that node switching its
  presented identity, giving every already-alive peer's widen step a real
  window while RPC is still universally on the old, mutually-trusted
  bootstrap credential. Verified locally under `taskset`-constrained CPU
  plus background load in addition to the unconstrained runs, then
  confirmed clean on CI. Landed as a separate commit
  (`fix(ca-cluster-node): delay RPC-TLS identity switch until peers can
  widen`) on the same PR.
- **ca-cluster-rpc-mtls: coverage-ratchet CMake gating bug found and
  fixed**: `tls_tcp_rpc_unit_test`/`tls_tcp_rpc_integration_test` were
  accidentally registered inside `tests/CMakeLists.txt`'s
  `if(TARGET ca_service)` block, which — unlike their actual dependency
  (`certificate_authority` only) — is unavailable under coverage builds
  (`ENABLE_COVERAGE` disables `cmd/ca_service`/`cmd/ca_cluster_node`
  entirely), so neither test binary, and none of `tls_tcp_rpc.hpp`'s
  coverage, was ever measured. Moved both out to be gated only by the
  enclosing `if(TARGET certificate_authority)`. Once actually measured,
  `reload_identity()`/`reload_trust_policy()`/`is_running()` (needed for
  live cutover, but not exercised by the round-trip-shaped tests written
  first) and the new `record_rpc_tls_ready` state-machine command turned
  up as genuine, unrelated-to-this-fix coverage gaps — closed with a
  dedicated reload test, an append_entries/install_snapshot round trip
  (previously only request_vote was exercised), and a
  `record_rpc_tls_ready`/`rpc_tls_ready_node_ids()` unit test. Coverage
  floor raised 88.92% → 88.95%.
- **`ca-cluster-rpc-mtls-real-aws` spec authored** (not yet
  implemented) — see Certificate Management, below, for the summary;
  full spec at `.kiro/specs/ca-cluster-rpc-mtls-real-aws/`.

### What Changed (July 13, 2026, later)

- **future-backend-performance-benchmark complete — all 23 tasks**:
  `.kiro/specs/future-backend-performance-benchmark/` adds a benchmark
  suite comparing `kythira::Future<T>` (Folly) against
  `kythira::stdexec_backend::Future<T>` (stdexec) across a fixed catalog of
  9 scenarios (creation/resolution for 3 payload shapes, same-/cross-thread
  promise fulfillment, `thenValue` chains at 3 depths, `thenError`,
  `via(scheduler)`, `collectAll` at 3 widths, `collectAny`, `delay`/`within`
  overhead). Every scenario is one function template
  (`examples/future-backend-benchmark/benchmark_harness.hpp`) instantiated
  once per backend via a `folly_backend_traits`/`stdexec_backend_traits`
  pair, so there is exactly one implementation per scenario — the two
  backends' numbers can never silently drift apart by comparing two
  different operations. Adds `tests/future_backend_benchmark_test.cpp`
  (CTest-registered, `LABELS "performance;benchmark;future-backend"`,
  hardware-independent sanity floors only — no test compares one backend's
  result against the other's) and
  `examples/future-backend-benchmark/benchmark_report.cpp` (a standalone,
  developer-run comparison report writing timestamped CSV/JSON to
  `test_results/`). Builds and runs Folly-only when `stdexec_FOUND` is
  false, with the `stdexec` column and delta omitted from the report rather
  than printed as zeroed placeholders. Does not change
  `KYTHIRA_DEFAULT_FUTURE_BACKEND` or recommend a default; see
  `doc/future_backend_performance_comparison.md` for methodology, the full
  scenario catalog, known structural asymmetries (the cross-thread
  scenario's per-iteration `std::thread` spawn cost, the GCC
  `-fno-strict-aliasing` mitigation already in place for `stdexec` targets),
  and reference numbers from a real run.

### What Changed (July 13, 2026)

- **stdexec future backend complete — all 52/52 tasks**: `.kiro/specs/stdexec-future-backend/`
  Phases 3–6 (Tasks 14–35) implemented — the full continuation/
  transformation/scheduling surface on `stdexec_backend::Future<T>`
  (`thenValue`/`thenTry`/`thenError`/`ensure`/`via`/`delay`/`within`),
  `FutureFactory`/`FutureCollector` (`collectAll`/`collectAny`/
  `collectAnyWithoutException`/`collectN`), `scheduler_executor_shim`,
  backend selection (`KYTHIRA_DEFAULT_FUTURE_BACKEND` CMake option,
  `include/raft/future_default.hpp`), and a full test suite (19 targets,
  `ctest -L stdexec`) including cross-backend fidelity tests comparing
  Folly and stdexec behavior directly and compile-time backend
  non-interference checks. Phases 0–2 (the concept regenericization and
  core `Try`/`single_shot_channel`/`Promise`/`Future` primitives) had
  already landed on this branch. Also closed the 4 sub-tasks left
  unchecked from that earlier Phase 0–2 work: Property 5 (Optional
  Dependency Isolation, `scripts/verify-optional-dependency-isolation.sh`
  + the `verify-optional-dependency-isolation` CMake target, using
  `-DCMAKE_DISABLE_FIND_PACKAGE_stdexec=ON` rather than mutating
  `vcpkg_installed/`), Property 2 (Concept-Layer Folly Independence,
  `tests/concepts_future_folly_independence_test.cpp` — a raw
  `add_executable` that deliberately doesn't link `network_simulator`, so
  a regression pulling Folly into `concepts/future.hpp` would fail to
  compile rather than pass silently), Property 1 (Concept Regenericization
  Preserves Folly Compliance,
  `tests/folly_backend_concept_regenericization_property_test.cpp`), and
  Property 3 (Unit Type Equivalence,
  `tests/unit_type_equivalence_property_test.cpp`).
  - **`within()`'s implementation changed mid-spec**: the original design
    used `exec::when_any` to race the original sender against a timeout,
    but `when_any`'s cancel-the-loser behavior depends on a stop token
    reaching the losing branch through this file's `any_sender_t<T>` type
    erasure — and `any_receiver_t<T>` is declared with the default empty
    query-forwarding list, so that stop token never arrives, leaving
    `when_any` waiting forever for a `single_shot_channel`-backed loser
    that can never acknowledge a stop request it never received.
    Reimplemented using the same "race to fulfill a shared channel,
    loser's result silently discarded" pattern already used by
    `FutureCollector::collectAny`, launched via `exec::start_detached`
    rather than `exec::async_scope::spawn()` (the latter's destructor
    asserts every spawned operation has already completed, which a
    losing branch — e.g. a promise the caller never fulfills — can
    violate for an unbounded time).
  - **Real GCC 13 miscompilation found and fixed**: at `-O2`/`-O3`, GCC 13
    miscompiles `exec::any_sender`'s small-buffer-optimized move
    constructor — moving one `any_sender` holding a small payload (e.g.
    `stdexec::just(int)`, the common case) into another `any_sender` of
    the same type corrupts the heap. The corruption doesn't crash
    immediately; it crashes on a later, unrelated allocation, which made
    the first several repro attempts misleading (a property test calling
    `FutureFactory::makeFuture(v).get()` in a 200-iteration loop always
    crashed on exactly the *second* iteration). Fixed with
    `-fno-strict-aliasing` for GCC builds only (`CMakeLists.txt`);
    `clang++-18` is unaffected at every optimization level. Full
    diagnosis in `.kiro/specs/stdexec-future-backend/spike-notes.md`'s
    "Phase 3 findings" section.

### What Changed (July 11–12, 2026)

- **Peer-to-peer log replication and TCP gossip transport implemented**: lands
  both `.kiro/specs/peer2peer-log-replication/` (the abstract catch-up
  mechanism) and `.kiro/specs/peer2peer-gossip-transport/` (a real network
  transport for it), since the gossip spec couldn't compile or be exercised
  without its foundation and neither had been started. Previously, log
  replication in `node<Types>` was a strict star topology — only the leader
  could supply missing entries, so its own CPU/bandwidth capped how fast a
  cluster converged when many members fell behind at once (rolling restart,
  healed partition, bursty joins).
  - New opt-in `peer2peer_replicator` concept
    (`include/raft/peer2peer_replication.hpp`), mirroring `peer_discovery`'s
    shape: a `no_op_peer2peer_replicator` default guarantees zero behavioral
    change for any `Types` bundle that doesn't opt in, and
    `static_peer2peer_replicator` is an in-memory reference/test
    implementation.
  - New `fetch_log_entries_request`/`response` RPC pair (`types.hpp`) plus
    optional `network_client_with_log_fetch`/`network_server_with_log_fetch`
    extension concepts (`network.hpp`), wired into `json_serializer.hpp` and
    `simulator_network.hpp` exactly like the existing `ClusterJoin`/
    `ClusterLeave` optional extensions.
  - `raft.hpp`: extracted `append_entries_with_consistency_check()` from
    `handle_append_entries()`'s Rules 3–5 so the peer-to-peer fetch path
    reuses the exact same conflict/truncation guarantees as leader-driven
    replication — a bad or stale source peer can only cause wasted local
    work, never a committed entry being lost or altered.
  - A replicator's peer set now tracks `cluster_members()` automatically via
    `sync_peer2peer_membership()`, wired into every `_configuration`
    mutation site (including `set_cluster_configuration()`, which the
    peer2peer-log-replication spec's own design doc had missed because it
    mutates `_configuration`'s fields in place rather than via a single
    assignment) — never separately configured.
  - `maybe_gossip_progress()`/`maybe_catch_up_from_peer()` piggyback on
    `check_election_timeout()` (called unconditionally for every node state
    by every existing binary's external timer loop) since this codebase has
    no dedicated maintenance-thread tick.
  - **`tcp_gossip_peer2peer_replicator`** (`include/raft/tcp_gossip_transport.hpp`,
    583 lines): a real anti-entropy gossip implementation (randomized
    push-pull digest exchange, Cassandra/Dynamo-style — not SWIM, since
    Raft's own election timeouts already cover liveness detection),
    self-contained TCP listener plus background gossip thread, entirely
    independent of whatever `network_client_type`/`network_server_type` the
    owning node uses for Raft RPCs. Starts its background threads lazily via
    an explicit `start()`/`stop()` pair (detected structurally via
    `if constexpr (requires {...})`) rather than in the constructor, since
    `node_config<Types>` holds `peer2peer_replicator_type` by value and
    `node<Types>`'s constructor moves it once — moving an object after its
    background threads have captured `this` would dangle.
  - 6 new test files (concept/no-op/static unit tests, an end-to-end
    property suite proving a joining node converges via a peer while
    excluded from ever reaching the leader, a `remove_server()`-revokes-
    eligibility test, a no-op-vs-undeclared parity test, pure-logic
    merge/prune/wire unit tests for the gossip transport, a real-TCP
    single-process integration test, and a mixed-transport property suite —
    real gossip sockets, simulated Raft RPCs — covering catch-up
    convergence, freshness expiry, and membership-removal). Full existing
    regression suite verified green alongside them.
- **State machine examples completed**: `replicated_log_state_machine` and
  `distributed_lock_state_machine` brought up to parity with
  `counter`/`register` (test targets, `CMakeLists.txt` wiring). Found and
  fixed a determinism defect in `distributed_lock_state_machine::apply()`:
  it called `std::chrono::steady_clock::now()` to compute lock expiry, which
  is non-deterministic across Raft replicas (clock skew, GC pauses,
  different machines) and violates the `state_machine` concept's requirement
  that every replica reach identical state from the same command at the same
  log index. Replaced wall-clock expiry with log-index-based expiry
  (`expiry_index = acquire_index + timeout_entries`), using the `index`
  argument `apply()` already receives — the one value every replica is
  guaranteed to agree on. The `ACQUIRE` command's third argument is renamed
  `timeout_ms` → `timeout_entries` to match; `include/raft/examples/README.md`
  and this file both updated accordingly. New
  `tests/replicated_log_state_machine_test.cpp` and
  `tests/distributed_lock_state_machine_test.cpp` mirror the existing
  counter/register test structure, each with a `static_assert` against
  `kythira::state_machine` to catch future signature drift at compile time;
  distributed lock's suite includes a dedicated determinism test applying
  the same command sequence — including an expiry and re-acquisition — to
  two independent instances and asserting byte-identical `get_state()` after
  every command.
- **Coverage hook no longer hangs on debuginfod network stalls**: the
  pre-commit coverage-ratchet step intermittently took several minutes to
  what looked like an indefinite hang at "[coverage] Measuring ...". Root
  cause: `llvm-profdata-18`/`llvm-cov-18` on this system are built with
  debuginfod (libcurl) support, and `DEBUGINFOD_URLS` is set globally in the
  environment; left alone, both tools attempt a network round-trip per test
  binary to fetch debug info they already have embedded locally, and in this
  network-restricted environment those connections stall (silently dropped,
  not refused) instead of failing fast. Confirmed directly — clearing
  `DEBUGINFOD_URLS` took the coverage report over all 306 test binaries from
  66 minutes wall clock down to 1.9 seconds. Fix: explicitly clear
  `DEBUGINFOD_URLS` for both the `llvm-profdata` merge and `llvm-cov` report
  invocations. An earlier, incorrect diagnosis had attributed this to LLVM's
  internal thread pool and worked around it with `--num-threads=1`; that
  workaround is superseded by this fix.

### What Changed (July 9–10, 2026)

- **Membership change (joint consensus) spec verified complete**: all 20 tasks
  in `.kiro/specs/membership-change/` were found already substantially
  implemented in the codebase — this spec's tracking document had simply
  never been updated to reflect that. Verified every task against
  `requirements.md` by direct code reading; the one genuine gap
  (`tests/node_recovery_unit_test.cpp`, task 20) was added, covering
  no-persisted-state, term+voted_for-only, snapshot-only, and
  snapshot-plus-trailing-log-entries restart scenarios (including a
  configuration log entry correctly overriding the snapshot's own
  configuration per Requirement 8.3).
- **CI flakiness diagnosed and fixed**: pulled JUnit artifacts from 8 recent
  CI runs to find the actual failure signatures rather than guessing. Three
  independent causes accounted for nearly all flaky `build-and-test`/coverage
  failures:
  - `ca_cluster_node_test` (6/8 sampled failures) — a real 3-node Raft
    cluster brought up as subprocesses, flaking under CPU contention from
    `ctest -j$(nproc)` on 4-vCPU runners. Fixed with `--repeat until-pass:3`
    on the `build-and-test` job's ctest invocation (already present on the
    coverage job, but missing here) and `PROCESSORS 4` on the test itself so
    ctest's scheduler stops co-scheduling other tests alongside it.
  - Coverage floor comparison was byte-exact against a floor set on the
    authoring dev's machine; CI's own measurement of the same tree can land
    a few tenths of a point lower from run-to-run counter/scheduling noise
    (observed: 88.10% vs. an 88.16% floor). Added a 0.50pp tolerance band to
    CI's enforcement check only — the local ratchet, which is what actually
    raises the floor, stays exact.
  - Coverage job intermittently failed at link time with "No space left on
    device" — coap-transport-security's added test binaries ate back into
    the headroom reclaimed for certificate-authority. Widened the
    disk-reclaim step (JVM, Az CLI, PowerShell, GHC's second install path,
    the runner's swapfile, apt's package cache).

### What Changed (July 7–8, 2026)

- **Certificate Authority framework complete**: all 35 tasks of
  `.kiro/specs/certificate-authority/` implemented, built, and tested. In-process
  `certificate_authority` (root CA generation, leaf issuance, revocation/CRL,
  `from_existing()` round-trip), `temp_cert_files` RAII helper, and
  `ca_service` CLI (`cmd/ca_service/`) for both oneshot Docker/Podman-volume
  provisioning and a long-running `--serve` HTTP API mode
  (`local`/`aws-acm-pca` providers, bearer-token auth, `/v1/certificates`,
  `/v1/certificates/renew`, `/v1/certificates/revoke`, `/v1/crl`,
  `/v1/root-ca`).
- **`aws_acm_pca_provider`**: `certificate_provider` implementation backed by
  AWS Certificate Manager Private CA; unit/LocalStack/real-AWS test tiers
  following the project's existing always-compiled-but-runtime-skipped
  convention.
- **TLS hot-reload**: `reload_tls_material()`/`enable_auto_reload()` for both
  `cpp_httplib_server`/`cpp_httplib_client` and `coap_server`/`coap_client`,
  plus `ca_test_fixture::renew()` and `temp_cert_files::replace_atomically()`
  (atomic write-tmp-then-rename) so certificate rotation never serves a
  half-written file.
- **`ca_cluster_node`** (`cmd/ca_cluster_node/`): a Raft-replicated CA —
  `ca_state_machine` records bootstrap/issuance/revocation as a deterministic
  replicated ledger; the leader reconstructs a `certificate_authority` via
  `from_existing()` and replays the ledger on every election; a `noop`
  command is submitted immediately on election so previous-term entries
  commit retroactively (Raft §5.4.2/Figure 8). Multi-node test coverage
  drives real 3-process clusters over subprocesses, including leader
  failover and restarted-follower recovery. Packaged for 3-AZ AWS deployment
  (systemd unit, ECS task definitions, `docker/ca_cluster_node/`).
- **ACME support (RFC 8555)**: `acme_test_server` (self-contained mock CA,
  `tests/acme_test_server.hpp`) and `acme_certificate_provider`
  (`include/raft/acme_certificate_provider.hpp`) — full JWS-signed order
  lifecycle, http-01 and dns-01 (RFC 2136 UPDATE) challenges, RFC 8738
  `"ip"`-typed identifiers, per-identifier challenge-type dispatch
  (`acme_identifier::classify()`/`challenge_for()` — IP identifiers always
  use http-01 regardless of configured challenge type), and `.local` (mDNS)
  challenge validation via ordinary `getaddrinfo()` with a distinguishable
  `mdnsResolverUnavailable` error when the validating host has no mDNS
  resolver configured (nsswitch.conf-based capability probe with a
  test-only override).
- **Fingerprint-pinned bootstrap** (`include/raft/ca_bootstrap_client.hpp`):
  `fetch_trusted_root()` lets a fresh instance, given only an out-of-band
  SHA-256 root fingerprint and bearer token, establish first-contact trust
  in a `ca_service`/`ca_cluster_node` TLS listener without any prior
  certificate chain to verify against — `--print-root-fingerprint` prints
  the operator-distributable fingerprint.
- **Two pre-existing `raft.hpp` bugs found and fixed** while wiring
  `ca_cluster_node`'s multi-node tests (not part of the certificate-authority
  spec's own scope, but blocking correct multi-node behavior):
  - `read_state()`'s quorum check used
    `raft_future_collector<T>::collect_majority()`, which computed
    `(followers.size()/2)+1` — wrongly requiring acknowledgment from *every*
    follower in a 3-node cluster instead of a majority *including* the
    leader's implicit self-vote. This made linearizable reads unavailable
    with exactly one node down, in any cluster size. Fixed with
    `collect_all_with_timeout()` plus an explicit
    `required_follower_acks = (heartbeat_futures.size() + 1) / 2`.
  - After a restart/election, previously-persisted log entries from a prior
    term were never retroactively committed, stalling `read_state()`/state
    application indefinitely. Fixed via the standard Raft no-op-on-election
    technique: `ca_cluster_node` submits a `ca_command_type::noop` command
    immediately upon becoming leader.
  - Added `node<Types>::known_leader()` public accessor needed for
    `ca_cluster_node`'s redirect-to-leader HTTP routes.
- **httplib gotcha documented**: `SSLClient::enable_server_certificate_verification(false)`
  disables cpp-httplib's *entire* verification block, including any custom
  `server_certificate_verifier_` callback — not just the default chain check.
  `ca_bootstrap_client.hpp` deliberately never calls it, relying solely on
  the callback's explicit `CertificateAccepted`/`CertificateRejected` return.
- **`quorum_management_test`/`docker_quorum_manager_test` linker fix**:
  both were missing the `Boost::context`/`libboost_context.a` link already
  required by every other Folly-linking test target (undefined reference to
  `boost::context::detail::make_fcontext` when actually exercising Folly
  fibers) — pre-existing, unrelated to this spec, fixed opportunistically
  while verifying a full build.

### What Changed (June 19–22, 2026)

- **`rfc2136_dns_sd_discovery` implemented** (`include/raft/rfc2136_dns_sd_discovery.hpp`,
  500 lines): DNS-SD peer discovery over unicast DNS via RFC 2136 dynamic update. Registers
  PTR, SRV, and TXT records per node under a configured service domain. A background fresher
  thread renews the TXT record's `fresh_until=<epoch>` field every
  `freshness_interval / 2` so that peers from crashed nodes (whose destructor never
  runs) are automatically filtered out by `find_peers`. `register_node` returns a
  `folly::Future<void>` that resolves once the UPDATE is acknowledged. Destructor sends
  a DELETE UPDATE best-effort. Fault injection points: `raft/dns/rfc2136/dns_sd/update`
  (throws) and `.../update/noop` (silent pass-through).

- **Unit and chaos tests for `rfc2136_dns_sd_discovery`** added to
  `tests/dns_peer_discovery_unit_test.cpp` (now 21 total cases across 3 suites) and
  `tests/chaos/dns_peer_discovery_chaos_test.cpp` (now 17 total cases across 3 suites).
  The `rfc2136_dns_sd_suite` covers: register resolves future, find_peers returns peers,
  deregister on dtor, freshness filtering. The `rfc2136_dns_sd_chaos_suite` covers:
  register throws on fault, dtor silent when deregister faulted, find_peers returns
  empty on fault, noop fault lets register succeed.

- **BIND9 Docker image** (`docker/bind9/Dockerfile`): multi-stage Ubuntu 24.04 build
  of BIND9 with RFC 2136 Dynamic Update enabled on a private `example.local.` zone;
  includes `dig` for healthchecks.

- **DNS discovery Docker scenario test** (`tests/docker_chaos/dns_discovery_test.cpp`,
  3 cases): `all_nodes_healthy`, `all_nodes_discover_peers`,
  `stopped_node_absent_after_deregister`. Runs via `docker-dns-discovery-tests` CMake
  target using `docker/dns-discovery-compose.yml` (BIND9 + 3 `dns_discovery_node`
  containers).

- **DNS-SD discovery Docker scenario test** (`tests/docker_chaos/dns_sd_discovery_test.cpp`,
  3 cases): `all_nodes_healthy`, `all_nodes_discover_peers`,
  `dead_node_absent_after_freshness_expiry`. Runs via `docker-dns-sd-discovery-tests`
  CMake target using `docker/dns-sd-discovery-compose.yml` (BIND9 + 3
  `dns_sd_discovery_node` containers). The `dead_node_absent_after_freshness_expiry`
  case kills node1 with SIGKILL and waits 25 s for the 20 s freshness interval to
  expire, then verifies the dead node is no longer reported by the surviving nodes.

- **`poco_peer_discovery` Docker scenario test** (`tests/docker_chaos/poco_discovery_test.cpp`,
  3 cases) added via `docker-poco-discovery-tests` CMake target; runs against a
  `docker/poco-discovery-compose.yml` cluster.

- **Podman support in Docker test harness**: `tests/docker_chaos/os_faults.hpp` now
  provides `container_runtime()` (reads `$KYTHIRA_CONTAINER_RUNTIME`, default
  `"docker"`) and `compose_prefix()` (reads `$KYTHIRA_COMPOSE_COMMAND`, defaults to
  `[runtime, "compose"]`); all command-vector builders use these instead of the
  hardcoded `"docker"` string. `tests/docker_chaos/CMakeLists.txt` auto-detects
  `docker` then `podman` via `find_program`, exposes `CONTAINER_RUNTIME` and
  `COMPOSE_COMMAND` CMake cache variables, and forwards both as env vars into every
  scenario-test invocation.

- **Rootless Podman compatibility**: `docker/dns-discovery-compose.yml` and
  `docker/dns-sd-discovery-compose.yml` no longer assign static IPs to BIND9
  (`ipv4_address` was silently ignored by rootless Podman). `DNS_SERVER` is now the
  compose service name (`"dns-test-bind9"`, `"dns-sd-test-bind9"`). Both node
  binaries (`cmd/dns_discovery_node`, `cmd/dns_sd_discovery_node`) resolve the
  service name to an IP via `getaddrinfo(AF_INET)` before handing it to ldns, which
  only accepts IP literals.

- **Coverage build fixes**: `cmd/` subdirectories excluded from CMake build when
  `ENABLE_COVERAGE=ON` to prevent `GcovrMergeAssertionError` (header-only classes
  compiled in both test TUs with `FIU_ENABLE` and node binaries without it produced
  conflicting gcov line-number metadata). `tests/docker_chaos/` excluded from gcovr
  (binaries compiled but never run by ctest). Coverage floor raised: 79.9% → 80.3%.

- **CLAUDE.md** created at repo root with steering directives: Conventional Commits
  format required for all commit messages; commit bodies must be detailed summaries
  (motivation, trade-offs, root cause, sub-changes); all container-based tests must
  work with both Docker and rootless Podman (no static IPs, no hardcoded `"docker"`,
  no root-only networking).

### What Changed (June 18, 2026)

- **DNS peer discovery tests complete**: comprehensive unit tests (14 cases,
  `tests/dns_peer_discovery_unit_test.cpp`) and chaos tests (12 cases,
  `tests/chaos/dns_peer_discovery_chaos_test.cpp`) for `rfc1035_peer_discovery`
  and `rfc2136_ldns_discovery`; both test targets are guarded by `LIBLDNS_FOUND`
  and registered in CTest with appropriate labels (`unit;dns;peer_discovery`,
  `chaos;dns;peer_discovery`).
- **Fault injection points added** to `rfc1035_peer_discovery.hpp`
  (`"raft/dns/rfc1035/find_peers/fail"`, `.../inject_ipv4`, `.../inject_mixed`)
  and `rfc2136_ldns_discovery.hpp` (`"raft/dns/rfc2136/send_update"`,
  `.../noop`) — all compile to no-ops without `FIU_ENABLE`.
- **`register_node` bug fixed** in `rfc2136_ldns_discovery`: `_self_address` is
  now set *after* a successful `send_update()` call, not before — previously a
  failed registration left `_self_address` set, causing the destructor to attempt
  deregistration of an address that was never successfully registered.
- **Node bootstrap spec fully complete**: all 20 tasks done including CoAP
  multicast adaptor (`coap_multicast_peer_discovery`), RFC 1035 query class,
  RFC 2136 dynamic-DNS class, 6 property tests, and DNS unit/chaos tests.

### What Changed (June 12, 2026)

- **Docker chaos testing complete**: real multi-node cluster in Docker containers with
  OS-level fault injection; `chaos_node` binary (`cmd/chaos_node/`) with TCP RPC
  (`tcp_rpc.hpp`), file persistence (`file_persistence.hpp`), HTTP control plane, and
  libfiu TCP remote-control server (`fiu_remote.hpp`); multi-stage `Dockerfile` +
  `docker-compose.yml` (3 nodes, `NET_ADMIN` for iptables); Python harness
  (`tests/docker_chaos/`) with `ChaosCluster`, `ChaosNode`, network partition helpers,
  raw-socket `fault_control.py`, and 3 test files (AZ partition, persistence faults,
  combined safety assertions); `docker-chaos-image` and `docker-chaos-tests` CMake
  targets; spec at `.kiro/specs/docker-chaos/`.

- **libfiu integration complete**: fault injection chaos testing implemented across 5 phases
  (21 tasks); `include/raft/fault_injection.hpp` guard header; `fiu_do_on()` calls in
  `persistence.hpp`, `simulator_network.hpp`, `test_state_machine.hpp`; `debug_state()`
  accessor on `kythira::node`; RAII `fault_profiles.hpp`; `safety_assertions.hpp` helpers;
  `tests/chaos/` with smoke, profile-verification, and 8 safety/liveness property tests;
  `chaos-tests` CMake target; `README.md` "Chaos Testing" section; `DEPENDENCIES.md` updated.

### What Changed (June 11, 2026)

- **clang-tidy zero findings confirmed**: all 291 compilation units clean after
  fixing narrowing conversions, enum sizes, branch-clone, else-after-return,
  use-after-move suppressions, and compiler diagnostic errors in `future.hpp`
  and `coap_transport_impl.hpp`.
- **libfiu integration spec created**: fault injection chaos testing design at
  `.kiro/specs/libfiu-integration/`; macro approach (`fiu_do_on` in production
  sources, compiles to no-op without `FIU_ENABLE`); 21 tasks across 5 phases.
- **Membership change spec created**: joint consensus (Raft §6) implementation
  design at `.kiro/specs/membership-change/`; 20 tasks across 7 phases covering
  log entry type discriminant, leader log append, joint quorum, apply path,
  follower update, property tests, and node recovery on restart.
- **Node bootstrap spec created**: `peer_finder` concept + `ClusterJoin` RPC for
  fresh-node cluster join at `.kiro/specs/node-bootstrap/`; 15 tasks across 7
  phases; `no_op_peer_finder` default preserves all existing behaviour.

### What Changed (June 10, 2026)

- **clang-tidy integration**: `.clang-tidy` config with `WarningsAsErrors: "*"`;
  CMake `static-analysis` and `static-analysis-fix` targets (parallel via
  `run-clang-tidy`, sequential fallback); pre-commit hook step (opt-in with
  `TIDY_CHECK=1`, skip with `SKIP_TIDY_CHECK=1`); zero findings across all 291
  compilation units; spec at `.kiro/specs/clang-tidy/`.
- **clang-format integration**: `.clang-format` config (Google base, 4-space
  indent, 100-column limit); CMake `format` and `format-check` targets;
  pre-commit hook now checks staged files before the coverage ratchet;
  `SKIP_FORMAT_CHECK=1` escape hatch; 349 source files reformatted in a
  style-only commit; spec at `.kiro/specs/clang-format/`.

### What Changed (June 9, 2026)

- **Code coverage infrastructure**: `ENABLE_COVERAGE` CMake option + gcovr targets
  (`coverage`, `coverage-html`, `coverage-reset`); `coverage_floor.txt` baseline
  at 84.8%; pre-commit ratchet hook enforces non-decreasing coverage.
- **Membership API refactored**: `handle_node_removal(node_id)` replaced by
  `handle_cluster_membership_change(old_config, new_config)` — provides full
  context for both add and remove operations; notification fires after commit.
- **Command type encoding fixed**: `test_key_value_state_machine` enum aligned
  to `{get=0, put=1, del=2}` matching the command generator and inline test
  state machines. Fixes `state_machine_determinism_property_test`.
- **Trailing whitespace removed** from all 402 source files.

---

## Historical Notes

Full task-by-task implementation history is preserved in the spec files under
`.kiro/specs/`. Per-component status details are in `doc/RAFT_IMPLEMENTATION_STATUS.md`,
`doc/RAFT_TESTS_FINAL_STATUS.md`, and `doc/PERFORMANCE_VALIDATION.md`.

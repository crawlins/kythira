## TODO: Outstanding Tasks and Improvements

**Last Updated**: August 4, 2026

For a dated history of what changed and why, see [CHANGELOG.md](CHANGELOG.md).

## Current Status

The project is **PRODUCTION READY** ✅ with a 99%+ test pass rate.

- **403/403 tests passing, 0 failures, 0 skipped** on the full
  `ci_full_defconfig` suite, on all four `Build & Test` legs (`g++-13`/
  `clang++-18` × x64/arm64). Read off the JUnit artifacts of a specific
  green run rather than asserted — run
  [30947491385](https://github.com/crawlins/kythira/actions/runs/30947491385),
  commit `1a52e1f`, August 4, 2026. Counting note: the `ion_*` binaries are
  **not** in that 403, since the `ion` vcpkg feature is opt-in and absent
  from the default CI install; the `grpc_*` binaries **are**, since `grpc`
  is an unconditional `vcpkg.json` dependency.
- The `ca_cluster_node_test`/`ca_cluster_node_rpc_tls_test`/
  `ca_cluster_node_rpc_tls_restart_test` family's own intermittent SIGTERM-
  shutdown hang (see "Known Follow-ups" below) is fixed and verified, not
  just believed fixed
- All specifications complete across all 8 feature areas (membership change now complete),
  plus peer-to-peer log replication/gossip catch-up, state machine examples, the
  stdexec future backend, the Folly-vs-stdexec performance benchmark suite,
  RPC-internal mTLS for `ca_cluster_node`, Kconfig-based build configuration,
  Boost.Beast and Proxygen as two additional opt-in HTTP transports, a fourth
  (gRPC/HTTP2) transport, three additional RPC serializers (CBOR, Protocol
  Buffers, Amazon Ion) alongside the default JSON one, and Azure and GCP
  joining AWS as supported cloud providers
- `.kiro/specs/` now holds **45** per-feature spec directories, of which two
  are outstanding — see "Pending Specifications" below
- Build clean with no errors or warnings
- Both Folly-decoupling follow-up gaps closed for `tests/`/`certificate_authority`:
  test-bootstrap backend-conditional gating (PR #93) and per-target rather than
  subdirectory-level Folly CMake gating (PR #94) — see "Known Follow-ups" below
- Coverage floor: 89.09% (non-decreasing ratchet, see `coverage_floor.txt` —
  that file is the authority; this line has drifted from it before)

---

## Completed Specifications (All 8/8 Complete)

| Spec | Tasks | Status |
|------|-------|--------|
| Raft Consensus | 287/287 | ✅ Complete — includes Phase 5 multi-node testing (700–731) |
| HTTP Transport | 17/17 | ✅ Complete — A+ SSL/TLS, 931K+ ops/sec |
| CoAP Transport | 26/26 | ✅ Complete — DTLS, block transfer, 30K+ ops/sec |
| Folly Concept Wrappers | 55/55 | ✅ Complete — full wrapper ecosystem |
| Network Simulator | 26/26 | ✅ Complete — connection pooling, lifecycle management |
| Network Concept Template Fix | all | ✅ Complete — unified single-parameter concepts |
| Certificate Authority | 35/35 | ✅ Complete — local CA, `ca_service`/`ca_cluster_node`, ACME (RFC 8555/8738), fingerprint-pinned bootstrap; task 31's LocalStack/real-EC2 tests compile-verified only (no AWS access in this environment) |
| Membership Change | 20/20 | ✅ Complete — joint consensus (Raft §6) add/remove server, joint quorum, config-entry apply path, follower update, node recovery on restart; found already substantially implemented, `tests/node_recovery_unit_test.cpp` added to close the one real gap |

---

## Pending Specifications

The table above tracks the original 8 major feature areas; `.kiro/specs/`
has since grown to 45 per-feature spec directories, most now complete (see
`doc/CHANGELOG.md` for their individual completion entries). The specs
below are the ones that are not, split into two tables since they're
different kinds of "not done": genuinely never started (only a design/
requirements doc exists, zero implementation commits) versus a real,
ongoing partial state (some tasks done, specific ones outstanding) — kept
separate rather than folded into one list so each entry's actual status is
unambiguous at a glance.

### Not Started

| Spec | Tasks | Notes |
|------|-------|-------|
| `oci-cloud-provider` | 0/8 | Task 0 spike only (`spike-notes.md`); `oci_instance_pool_quorum_manager`/`oci_certificates_provider` |

`protobuf-rpc-serializer` came **off** this table on August 4, 2026 — it was
listed here as "0/44 design-only" long after it had actually shipped. Its
own `tasks.md` reads 44/44, `include/raft/protobuf_serializer.hpp` exists,
and five `protobuf_*` test binaries run in CI (present by name in run
30947491385's JUnit artifacts). Same correction, same day, applied to
`gcp-cloud-services` (13/13, live-verified), `ion-rpc-serializer` (45/45),
and `grpc-transport` (see below) — four specs that had all shipped while
this table still called them unstarted.

`transport-multi-serializer` moved to "Partially Implemented" on August 6,
2026 — the opposite direction, and while the work was happening rather than
long after, which is the point. Its `tasks.md` carries the detail, including
two decisions the design doc left open (an empty `Accept` means "no
preference", and the peer's ordering wins) and one bug found and fixed inside
the same change: `Accept: */*` — curl's default header — initially matched
nothing and would have been answered with 406.

### Partially Implemented

| Spec | Tasks | Notes |
|------|-------|-------|
| `transport-multi-serializer` | 12/16 tasks | Came **off** the "Not Started" table on August 6, 2026. The protocol-independent core is implemented and unit-tested: `media_type()` on the `rpc_serializer` concept and on all four shipped serializers (Task 1), the `serializer_registry` concept (2), `single_`/`multi_serializer_registry` in `include/raft/serializer_registry.hpp` (3), the tagged test serializer (4), both media-type exceptions (6), `parse_accept_header` in `include/raft/http_content_negotiation.hpp` (8), and 29 cases in `tests/serializer_registry_unit_test.cpp` (12). Later the same day: `transport_types` gained a checked `serializer_registry_type` (5) — across 8 production bundles and 16 test-local ones, since a hard concept requirement binds every model of it, not just the ones the task named — and CoAP got its media-type-keyed Content-Format table plus registry validation (7). Later still: HTTP content negotiation landed for both cpp-httplib and Beast (9, 10) — `Accept`/`Content-Type` on the wire, 415/406 rejection *before* the handler runs, a shared `peer_capability_cache`, and a `media_type` metrics dimension. What remains is CoAP's half of the wiring (11) and the regression/interop/negative suites (13-16). One behaviour change to know about: a cpp-httplib peer POSTing without an explicit `Content-Type` now gets 415, because httplib labels such a request `text/plain`; node-to-node traffic is unaffected since both clients set the header. Note the old "0/27" denominator here did not match `tasks.md`, which has 16 top-level tasks and 25 leaf items; this row counts top-level tasks |
| `grpc-transport` | 12.5/13 phases | Tasks 1–12 are implemented and **CI-verified**: `proto/raft.proto`, the `raft_grpc_transport` target, `include/raft/grpc_transport{,_impl}.hpp`, `grpc_exceptions.hpp`, `grpc_message_conversion.hpp`, plus `grpc_transport_conversion_property_test`, `grpc_transport_integration_test` and `grpc_transport_example_test` — all three of which pass on all four `Build & Test` legs. Its `tasks.md` had flagged Tasks 1.5/13 as needing "a build machine with gRPC/Protobuf present"; CI *is* one, because `grpc` is an unconditional `vcpkg.json` dependency (which is why nothing in `ci.yml` mentions gRPC by name, and why this went unnoticed). What genuinely remains of Task 13 is narrower: the `KYTHIRA_KCONFIG_STRICT` and graceful-degradation configure checks, and a performance sanity pass |

`boost-beast-http-transport` and `proxygen-http-transport` both reached full
completion (18/18 and 17/17) on July 30, 2026 via
[PR #117](https://github.com/crawlins/kythira/pull/117) and its immediate
follow-up commit (a second ThreadSanitizer pass against Beast's newly-split
test binaries caught four further real data races); both are off this
table entirely now. See `doc/CHANGELOG.md` for those entries.

`cbor-rpc-serializer` is likewise off this table, now 49/49. The two items
this row previously listed as outstanding optional follow-ups both exist:
the usage example is `examples/cbor_serializer_example.cpp`, and the
end-to-end CoAP sanity check (Task 10.3) is
`tests/coap_cbor_end_to_end_test.cpp`, registered via `add_network_test` and
exercising a real `coap_server`/`coap_client` pair over a live socket rather
than a stubbed payload cycle. Both had landed while this row still read
45/49 — an instance of exactly the drift the note below warns about, found
by checking the tree rather than the status header.

`gcp-cloud-services` is off this table too, now 13/13. The row's last open
item was exercising the real-GCE/real-CAS integration tests against **live**
GCP; both suites have since run against a real project with Workload Identity
Federation credentials provisioned by `scripts/ci-cloud-credentials/gcp/` —
`gcp_quorum_manager_real_gce_test` passes 11/11 and
`gcp_privateca_provider_real_test` passes while provisioning and tearing down
its own CA pool, with a post-run audit showing no leaked instances, disks,
MIGs, or CA pools. That first live run is what surfaced the three defects
fixed alongside it: a bare network short name rejected by `instances.insert`,
the CAS suite silently skipping while CTest reported the skip as a pass, and
`provision_timeout_cleanup` never timing out while leaking its instance.

`ion-rpc-serializer` is off this table too, now 45/45. The row's last open
items were task 9's final validation (9.1-9.4) — never actually run,
because the opt-in `ion` vcpkg feature had never actually been built: the
overlay portfile's `SHA512` was a `0` placeholder, so `ion_rpc_serializer`
had only ever been validated against a hand-written `ion-c` API stub
(`-fsyntax-only`), not the real library. Actually installing `ion-c` and
building the real `ion_*` test targets against it surfaced four genuine
bugs — three of them in `ion-c` itself, not this codebase (a `git describe`
version-generation fallback ion-c's own build lacks, a CMake config
filename-casing mismatch, a missing `DECNUMDIGITS` propagation, and —
the notable one — `ion-c`'s own `ASSERT()` macro spinning forever under
`-DNDEBUG` instead of no-op'ing, a real hang on malformed/truncated input
found by this spec's own "never crashes" property test). All fixed (three
vcpkg overlay-port patches plus one `CMakeLists.txt` fix); all 6 `ion_*`
CTest binaries now pass, including a new end-to-end test
(`tests/ion_http_coap_end_to_end_test.cpp`, task 9.4) driving a real
RequestVote/AppendEntries/InstallSnapshot cycle over both HTTP and CoAP.
See `.kiro/specs/ion-rpc-serializer/tasks.md`'s own "Known Follow-ups" for
the full accounting.

**A note on how this table stays honest**: `proxygen-http-transport/tasks.md`
was found, in the same week, drifted in *both* directions in turn — first
saying "Not Started (0/17)" after the feature had actually been fully
implemented, then (after a reconciliation pass) undercounting real gaps in
Tasks 12-15 that a same-day CI run then closed for real. Prefer
re-verifying a spec's `tasks.md` against its actual `include/`/`tests/`
files and a real CI run over trusting either a stale status header or an
unverified completion claim.

---

## Known Follow-ups

- **Beast suites segfault under the stdexec future backend, and CI cannot see
  it (found August 6, 2026 — RESOLVED same day, PR #169).** The diagnosis
  below was right that no CI job covered it and wrong about where the bug
  lived: the crash was not "inside the stdexec backend", it was Beast handing
  that backend an executor of the wrong type. Three separate defects, fixed
  together:
  1. **Silent UB, not an stdexec bug.** `asio_strand_executor` derived from
     `folly::Executor` and every `async_*_kf` took a `folly::Executor*`. That
     should have been a compile error under another backend, but
     `future_continuation` requires a `via(void*)` overload,
     `folly::Executor*` converts implicitly to `void*`, and stdexec's
     `via(void*)` `static_cast`s straight to `scheduler_handle*` —
     reinterpreting a vtable pointer as a `shared_ptr`. Confirmed by
     instrumenting `via(void*)`: probe fires, fault lands at `0x1b`.
     `asio_strand_executor` is now backend-conditional (three variants with a
     `handle()` accessor, mirroring `kythira::executor_default`).
  2. **Lazy senders never started.** Once the crash was fixed the same tests
     *hung*: `server_session` built continuation chains and stored them in a
     `_pending` member, which was an attempt at backend-neutrality with the
     dependency backwards — holding a Future alive is what the *eager*
     backends don't need, and is not what the lazy one needs. Both chains now
     end in `.detach()`; `_pending` is deleted.
  3. **Two boost-backend bugs**, hidden because Beast never compiled under
     `=boost` at all: `thenError` lacked `thenValue`'s Future-flattening
     overload, and `via()` was not sticky — continuations after the first fell
     back to bare `then()`, which Boost.Thread runs on a freshly spawned
     thread, taking Beast's session state off-strand.

  Verified one host / one compiler / one build type, varying only the backend:
  folly 4/4, stdexec 8/8, boost 8/8 (twice each locally, then confirmed in CI
  by test name under both non-default backends). The `future-backend-compat`
  job now builds and runs the `beast_*` targets and asserts an exact test
  count. Original diagnosis retained below for the reasoning trail.

  Five tests fail on unmodified `main` in a
  local `build/` configured `-DKYTHIRA_DEFAULT_FUTURE_BACKEND=stdexec`:
  `beast_ssl_test`, `beast_server_test`, `beast_integration_test`,
  `beast_cross_transport_equivalence_test` and
  `three_way_http_transport_equivalence_test`. `beast_ssl_test` fails 5 runs
  out of 5 — deterministic, not flaky. Found while verifying an unrelated
  change; **confirmed pre-existing** by rebuilding from `origin/main` and
  observing an identical failure, so it is not a regression from the HTTP
  content-negotiation work (PR #167).
  - **The crash is inside the stdexec backend, not Beast.** Backtrace from a
    `main`-built binary: `SIGSEGV` in
    `stdexec::__let::__opstate<...>::__start_next()`, reached from
    `kythira::stdexec_backend::single_shot_channel<unit>::set_value()`, called
    from `async_connect_kf`'s Asio connect-completion lambda, running on an
    `io_thread_pool` thread. A second signature seen on the same binary is a
    glibc abort — `pthread_mutex_lock.c:94: assertion failed:
    mutex->__data.__owner == 0` — which together with the above reads like the
    type-erased operation state being destroyed before its completion fires.
  - **Why no CI job catches it.** `Build & Test` passes no
    `-DKYTHIRA_DEFAULT_FUTURE_BACKEND`, so it takes the `CMakeLists.txt:674`
    default, **folly**. The `[stdexec, boost]` matrix legs exist but build only
    `--target proxygen_transport_test`. So **no job anywhere runs a Beast test
    under stdexec**, and Beast's green ticks certify one backend only. Same
    shape as the recurring theme below: a check that passes without covering
    the thing it appears to cover.
  - **Supporting evidence, short of proof.** Every frame in the backtrace is
    stdexec or Asio, and CI runs the same five tests under **folly** — on both
    arm64 and x64, PR #167's run `31106119481` — where all five pass. So folly
    passes and stdexec crashes on the same sources. That comparison is
    confounded: CI also differs in architecture, OS image and compiler build,
    so it is strong support for "stdexec-specific" rather than a controlled
    result.
  - **Step 1 is therefore a clean local comparison, not a fix.** Configure a
    folly and a boost build on *this* machine, build `beast_ssl_test` in each,
    and run it — same host, same compiler, one variable. No existing build
    directory serves: `build-clang` is folly but stale (it predates the Beast
    test split and knows only `beast_transport_test`), and neither
    `build-boost` nor `build-gcp` has any Beast target built. If folly and
    boost both pass, the bug is stdexec's; if folly also fails locally, the
    environment is implicated and local-vs-CI becomes the question instead. Do
    this before reading any stdexec code.
  - Repro: `cmake -B build-stdexec -DKYTHIRA_DEFAULT_FUTURE_BACKEND=stdexec
    -DCMAKE_BUILD_TYPE=Release && cmake --build build-stdexec --target
    beast_ssl_test && ./build-stdexec/tests/beast_ssl_test`. Under `gdb -batch
    -ex run -ex "bt 25"` the trace above appears within seconds.

- **Possible improvement: split `Build & Test` into one build job feeding N
  sharded test jobs (measured August 6, 2026 — deliberately deferred, not
  started).** `Build & Test` currently builds and tests in the same job.
  Uploading the built test binaries once and fanning them out to parallel
  `ctest` shards would cut leg latency. The mechanism is ordinary `needs:` +
  `upload-artifact`/`download-artifact`; sharding is `ctest -I <k>,,<N>`.

  Measured on run `31123519182`, `Build & Test (clang++-18, arm64)`:

  | | measured |
  | --- | --- |
  | Build vs Test | test share **35.8%–57.4%** across the four legs (see per-leg table) |
  | Test binaries | 361 binaries, **7.83 GB** raw |
  | After `strip` + gzip | **~600 MB** — 12.99× measured on a 40-binary sample (731.5 MB → 56.3 MB, 9% of total bytes) |
  | Linkage | statically linked, **0** vcpkg/`$HOME` deps → portable across same-image runners |
  | ctest registry | 12 × `CTestTestfile.cmake`, **204 KB** |
  | Fixture staging | **none** — 0 `WORKING_DIRECTORY`/`configure_file`/`file(COPY)` in `tests/CMakeLists.txt`, plus 1 MB of data files |

  That last row is what usually kills this pattern and it is clean here: the
  shippable payload is binaries + 204 KB + 1 MB.

  Per-leg, from run `31123519182` (the whole matrix, not one leg — an earlier
  draft of this entry generalised from `clang++-18, arm64` alone and got the
  shape wrong; the legs are not alike):

  | leg | build | test | test share | projected N=4 |
  | --- | --- | --- | --- | --- |
  | g++-13, x64 | 912s | 1229s | 57.4% | 2141s → ~1304s (**−39%**) |
  | clang++-18, x64 | 991s | 553s | 35.8% | 1544s → ~1214s (−21%) |
  | g++-13, arm64 | 672s | 609s | 47.5% | 1281s → ~909s (−29%) |
  | clang++-18, arm64 | 592s | 546s | 48.0% | 1138s → ~814s (−28%) |

  **The number that matters is the critical path, not the average.** The four
  legs run in parallel, so the matrix finishes when the slowest finishes —
  `g++-13, x64` at 2141s. Sharding takes that long pole to ~1304s, so PR
  latency drops **~39%** (36 min → 21 min). Total runner minutes go 6104s →
  ~6743s (**+10%**). Projections assume ~60s strip+upload on the build job and
  ~25s download per shard.

  There is still a hard floor: no leg goes below its own build time, so ~900s
  for the slowest.

  Three gotchas specific to this repo:
  - **It multiplies per matrix cell, not across it.** Binaries are not
    portable across `x64`/`arm64` or `g++`/`clang++` stdlibs, so you need 4
    build jobs and the matrix goes 4 → 4 + 4N (20 jobs at N=4).
  - **`retention-days: 1` is mandatory, not optional.** Without it this adds
    ~600 MB per run to an artifact store currently holding 2.37 GB total.
  - **Striding ignores test cost.** The suite has long multi-process tests;
    naive `-I k,,N` leaves one shard carrying the tail while others idle.
    Cost-aware partitioning is what makes the 28% real rather than notional.

  **Why deferred.** `ctest -j$(nproc)` is already in use, so this buys
  cross-machine parallelism on top of intra-machine — a real but modest win. A
  20-minute leg was not the bottleneck on the day this was measured; a 2-hour
  Actions queue and cache pressure were, and this change *adds* ~600 MB/run of
  artifact traffic. It also does nothing about `--repeat until-pass:3` hiding
  first-attempt failures in these same jobs (see the "did the job actually do
  the work?" entry below). Revisit when leg latency is the actual complaint.

- **Actions cache is at GitHub's 10 GB per-repository limit (measured
  August 6, 2026 — open).** `actions/cache/usage` reported **10.44 GB across
  34 caches**, against a documented 10 GB per-repo ceiling, so GitHub is
  already evicting entries LRU. Split: **ccache 5.61 GB** (31 entries) and
  **vcpkg 4.83 GB** (3 entries). vcpkg permanently occupies ~4.8 GB, leaving
  ~5 GB for 31 ccache entries across four compiler×arch cells plus the
  backend-matrix legs — they are evicting each other, so some builds start
  cold, a plausible contributor to 25–40 minute `Build & Test` legs. Note the
  `future-backend-compat` legs now compile eight targets instead of one
  (PR #169), which adds pressure. Levers: prune stale `ccache-*` keys, narrow
  the restore-key fan-out, or scope vcpkg caching more tightly. Separately —
  and on the artifact quota, not the cache quota — `coverage-report` is
  **2.29 GB across 456 copies, 97% of all 2.37 GB of artifact storage**.

- **CI now matrices every `future_default` backend across the whole suite
  (August 7, 2026 — resolved).** `future_default<T>` resolves to one of three
  backends (`folly`, `stdexec`, `boost` — `CMakeLists.txt:676`), and **118 of
  the 390 test files** under `tests/` resolve through it. The
  `backend: [stdexec, boost]` legs used to build an explicit list of eight
  HTTP-transport targets, so **113 of those 118 had never executed under
  stdexec or boost even once** — all 40 `raft_*`, all 21 `coap_*`, plus the
  `kythira_*`, `learner_*`, `peer2peer_*` and `membership_*` suites. Two of
  three backends were certified by the HTTP transports alone.

  The legs now build the default target set — **no `--target` list at all** —
  and run the whole registered suite with the same `-LE` label exclusions
  `build-and-test` uses, so a compat leg and a Build & Test leg differ in
  exactly one variable. A derived list (`grep -l future_default tests/*.cpp`)
  would have fixed the staleness but not the premise, and would still have
  needed a source-name → test-name mapping; building everything needs neither
  and covers a test added tomorrow.

  **Results of the first run, which is the point of the whole exercise:**
  - **stdexec: 409/409 pass.** The entire suite compiles *and* passes.
  - **boost: 411/412.** The entire suite compiles; two `chaos_*` tests
    segfault. See the new follow-up below.
  - The old prediction that this would "surface *more* than the Beast bug" was
    right in kind but much smaller in degree than feared: two tests, one
    backend, both in the same suite.

  **Three things worth knowing before reading these legs' output:**
  - **The two legs legitimately register different test counts** — 412 under
    boost, 409 under stdexec. The three extras are
    `boost_future_concept_compliance_property_test`,
    `boost_future_continuation_and_collector_property_test` and
    `boost_backend_migration_guide_example_test`, which
    `tests/CMakeLists.txt:3505` registers only when the boost backend is
    enabled. Verified by diffing the two runs' actual `Test #N: <name>` sets,
    not inferred: the stdexec set is a strict subset, nothing is missing.
    `check-test-run.sh` derives its expectation per build dir, so this is
    self-consistent rather than something to special-case.
  - **Cost, measured cold:** the Build step is **~55 minutes** per leg, against
    the 4-5 minutes the old eight-target legs took warm. `timeout-minutes`
    raised 60 → 180 to match `build-and-test`. Both legs run in parallel with
    the four Build & Test legs, so they set latency only when slower. If this
    proves too expensive per-PR, move them to a nightly schedule — do **not**
    narrow the target list again, which is the change that created this hole.
  - **Neither leg is in `main`'s required status checks**, so a red one reports
    without blocking. That is deliberate and is the sanctioned ordering, but it
    is one step from the failure mode this file exists to prevent: an ignored
    red check is worth exactly as much as a green one that tests nothing.
    Whoever reads a red compat leg has to act on it or record why not.

- **Retry continuations outlived their owning `node` — a real use-after-free
  (found and fixed August 7, 2026 — resolved).** The first output of the
  widened backend matrix above, and it was not what the symptom suggested.

  **Symptom:** `chaos_state_machine_safety_test` segfaulted 3/3 in CI under
  boost, and `chaos_persistence_degradation_recovery_test` 1/3. Both passed
  under folly and stdexec on the same commit.

  **What it actually was:** not a Raft bug and not a boost-backend bug. Both
  tests *passed* — `*** No errors detected` appears in the log **before** the
  crash. The fault was in teardown. `error_handler::execute_with_retry_impl`'s
  `thenTry` continuation captured `this` with no lifetime guard, and the future
  backend runs it on a thread nothing joins (under Boost.Thread, a freshly
  spawned detached one). By the time it ran, the owning `node` had been
  destroyed by its `unique_ptr` at the end of the test, and the continuation
  read `_rng` out of the freed `error_handler` from `calculate_delay`.

  **Confirmed by AddressSanitizer, not by inference** — a `heap-use-after-free`
  naming the freed object (`node<chaos_raft_types>`, freed at
  `chaos_state_machine_safety_test.cpp:96`), the read
  (`error_handler.hpp:648`), and the thread (`boost … thread_proxy`). Worth the
  detour: gdb was useless here because each run under it took minutes and the
  crash is timing-dependent, whereas ASan caught it on the first run because it
  flags the *access*, not the crash.

  **This was latent under every backend, not just boost.** `delay()` resolves
  to a process-wide timer under all three — boost's `timer_service::instance()`
  singleton, stdexec's `global_timed_context()`, folly's `Timekeeper` — and
  none of them is tied to the owner's lifetime. Only the fault-injection tests
  generate retries at all, which is why only the chaos suite ever hit it, and
  boost's scheduling is simply what made it reproducible. Do **not** file this
  mentally under "boost is flaky".

  **Fix:** `error_handler` now carries the same
  `shared_ptr<std::atomic<bool>>` stop flag the rest of this codebase uses for
  async callbacks, and both retry continuations check it *before* touching
  `this`. `node::start()` shares its own flag with all four handlers, re-shared
  on each start so continuations pending from a previous run keep seeing the
  old flag as true.

  **The limitation that used to be here is closed for `node`'s own callbacks.**
  A stop flag is a check, and a check that passes only says the owner was alive
  a moment ago. `include/raft/async_scope.hpp` adds the missing half: a
  continuation holds a `ticket` for the duration of its body, and
  `node::stop()`'s `drain_async_continuations()` refuses new tickets and then
  waits for outstanding ones. All 18 guarded sites in `raft.hpp` plus both in
  `error_handler.hpp` take one. The drain runs **last** in every `stop()` exit
  path — after the network server is down — because draining earlier could
  block on work a later shutdown step would have completed, which would trade
  the memory bug for a shutdown hang.

  **Verified:** ASan reported on run 1 before the fix and 0/8 after; the
  plain boost binaries went from 1/3 (local) and 3/3 (CI) failing to 10/10
  clean on each of the two tests; the three `error_handler_*` suites pass and
  still exercise the full retry ladder (attempts 1→2→3 with backoff), so the
  guard does not short-circuit retries when the flag is false.



- **`simulator_network_client` continuations outlive the `NetworkSimulator`.
  Root-caused and fixed; the fix is NOT merged because it regresses an
  unrelated raft test (August 7, 2026 — open).**

  The attempt lives on branch `fix/simulator-node-lifetime` (commit `93703cf`),
  pushed with no PR because it is not proposable as-is. **This entry is the
  durable half; that branch is disposable.** Everything needed to redo the work
  from scratch — the ownership analysis, the measurements, and the dead ends
  already paid for — is deliberately recorded here rather than there, because a
  defect documented only on an unmerged branch is one nobody will read about.
  If the branch is ever pruned, nothing of value is lost.

  **The defect.** `NetworkNode` holds a raw `simulator_type* _simulator` and
  guards every use with `if (!_simulator)`. Nothing ever nulls that pointer, so
  the check only ever caught a node built without a simulator — never a
  simulator since destroyed, which is the case that actually crashes.
  `receive()` then calls `retrieve_message()`, whose first statement is
  `std::unique_lock lock(_mutex)` on freed memory. Confirmed by ASan: freed
  allocation is the `NetworkSimulator` the *test* owns
  (`chaos_state_machine_safety_test.cpp:36`), read at
  `include/network_simulator/simulator_impl.hpp:487`, from a
  `simulator_network_client` continuation on a boost `Future<bool>::thenValue`.

  **Why neither obvious ownership fix works.** A `shared_ptr` back-pointer
  cycles: `NetworkSimulator::_nodes` owns its nodes by `shared_ptr`. A
  `weak_ptr` needs `enable_shared_from_this`, but **roughly 130 call sites
  construct the simulator on the stack**, where `weak_from_this()` is empty and
  would silently fail every node operation in the suite. Hence the attempt used
  `async_scope` (which works for either storage duration).

  **The attempt works, and costs too much.** Measured on
  `chaos_state_machine_safety_test` under boost + ASan: **0/30 use-after-free
  reports, against 2/30 before.** But `raft_commit_implies_replication_property_test`
  regresses:

  | configuration | clean runs |
  | --- | --- |
  | baseline (no change) | **24/25** |
  | tickets on, drain off | 20/25 |
  | tickets on, drain on | **22/35** |

  Baseline vs the full change is statistically solid; the ticket-only arm is
  ambiguous at this N. **The drain is the main contributor.**

  **Two hypotheses already ruled out by measurement — do not re-run these:**
  - *CPU starvation.* Reproduces on a fully idle box.
  - *Mutex contention in `enter()`.* Wall-clock is identical to the tenth of a
    second (43.3-45.0s vs 43.7-45.0s), and instrumenting the guard recorded
    **zero** refusals across full runs. The guard neither rejects anything nor
    costs measurable time, yet the pass rate still moves.

  **Most promising next step**, given the drain is the main contributor:
  `retrieve_message()` blocks in `_msg_available.wait_for(lock, timeout, pred)`
  holding its ticket, so a drain waits out that timeout rather than cancelling
  it. Add a "closing" flag to the wait predicate and `notify_all()` on close, so
  blocked receives return immediately instead of being waited out. That should
  remove the teardown stall without weakening the guarantee. Re-measure both
  arms at N>=25 before believing any of it.


- **A PR that does not target `main` runs no CI at all, and reports itself
  green (found August 7, 2026 — open).** `ci.yml` triggers on
  `pull_request: branches: [main]`, so a **stacked PR** — one whose base is
  another feature branch — skips the entire workflow. PR #178 was opened that
  way and showed a single passing check (GitGuardian) with
  `mergeStateStatus=CLEAN`, because with no required checks present there is
  nothing to be missing. It looked greener than a PR that had actually built.
  - This is the house failure mode wearing a new hat: **machinery reporting
    success while doing nothing.** Worse than usual, because the signal is not
    merely absent — the absence renders as approval.
  - Retargeting the base to `main` afterwards does **not** start a run. The
    workflow's `pull_request` trigger uses the default activity types
    (`opened`, `synchronize`, `reopened`) and a base change is `edited`. A push
    is required.
  - Options, unresolved: widen to `pull_request: branches: ['**']` so stacked
    PRs are covered — honest, but every stacked PR then pays for the full
    matrix, which since the backend-matrix widening is ~55 minutes of cold
    build per compat leg; or add an `edited` type so at least a retarget
    re-runs; or simply do not stack PRs in this repo and say so somewhere a
    reader will find it. The first is the only one that removes the trap rather
    than documenting around it.
  - Until then: **check `gh pr checks <n>` returns more than one row** before
    believing a PR is green. One row is not a pass, it is an absence.

- **Systematise "did the job actually do the work?" as CI jobs.** This repo's
  single most repeated failure is machinery that reports success while doing
  nothing — the count is past fifteen, and *every* instance so far was caught by
  a human reading a log on a hunch. That does not scale and it is not reliable:
  the ones nobody happened to look at are, by construction, still green. Turn
  the manual check into automated assertions that fail the build.
  - **Assert a test-count floor.** `Build & Test` runs 409 tests. A configure
    change that silently drops a target — an `if(TARGET Folly::folly)` that
    stops matching, a `find_package` that quietly fails — reduces the count and
    still reports 100% passed. Record the expected count (or a floor) and fail
    when the run comes in under it. This alone would have caught several past
    incidents.
  - **Assert the skip list, do not just tolerate skips.** CMake emits
    `message(STATUS "<test>: skipped (requires Folly)")` and ctest reports
    `Test suite ... is skipped because disabled`. Both currently read as
    success. Compare the actual skip set against a checked-in allowlist and
    fail on anything new, so a broken precondition cannot masquerade as a pass.
  - **Surface first-attempt failures.** `ctest --repeat until-pass:3` hides
    flakes completely: the summary says passed and only the raw log shows the
    first attempt failed. **Two live examples found August 6, 2026 in PR #167's
    run** — `httplib_server_validation_test` failed 4 assertions on attempt 1
    and passed on attempt 2, and `grpc_transport_example_test` failed all three
    attempts on a hardcoded-port collision (`51702`, `Address already in use`).
    Emit a "passed only on retry" report as a job summary, and fail (or open an
    issue) when it is non-empty. A flake nobody sees is a bug nobody fixes.
  - **Assert each job's configuration actually took effect.** A previous
    session verified *by hand* that the `Proxygen transport (boost future
    backend)` leg really configured
    `-DKYTHIRA_DEFAULT_FUTURE_BACKEND=boost` and that all 12 of
    `proxygen_transport_test`'s cases entered and ran, precisely because a
    green tick alone did not establish it. Both are one-line greps against the
    CMake cache and the ctest log; make every matrix leg do them, so the
    assertion lives in the workflow rather than in whoever last thought to
    check.
  - **Fail a "ran nothing" real-cloud run.** `real-cloud-tests.yml` treats
    `run_real_cloud_tests=true` as a master switch, and omitting it reports
    `skipped` — success with nothing run. A job that claims to have exercised
    real cloud resources and registered zero cases should be a failure, not a
    tick.
  - Related, and the reason this item sits third: the Beast × stdexec gap above
    is exactly this class of problem. The fix for that one gap is a wider
    matrix; the fix for the *class* is making "this job covered what it claims"
    a checked property rather than an assumption.

- **Proxygen content negotiation — answered and implemented (August 7, 2026 —
  resolved).** The open question was "is Proxygen a first-class transport?"
  **It is**, so it got Tasks 9/10's full treatment as spec Task 10a rather than
  the minimal "just fix the hardcoded label" alternative.
  - The defect was real and exactly as recorded: `application/json` hardcoded
    on the server response (`rpc_request_handler::onEOM`) and on both client
    request paths (`send_on_session`, `send_on_session_folly`), so a Proxygen
    node configured with `cbor_rpc_serializer` labelled every message JSON and
    lied on the wire. Requirement 9.4 names precisely this.
  - **The spec was amended before the code was written**, which is what the
    previous entry asked for. `requirements.md` and `design.md` had mentioned
    Proxygen zero times; both now enumerate it, the six acceptance criteria
    that name transports individually now name it, and `tasks.md` carries Task
    10a with a dated note saying the omission was an omission. Numbered `10a`
    rather than `17` because 11-16 are referenced by number from this file and
    from Task 10's own write-up.
  - **Two findings worth carrying forward, both specific to Proxygen:**
    - **Proxygen has *two* client send paths**, the generic bridge
      (Requirement 14) and the Folly fast path (Requirement 16), chosen by an
      `if constexpr` on the future backend, with separate function bodies and
      separate transaction bridges. Negotiating in one and not the other would
      have made a node's wire behaviour depend on which future backend it was
      compiled with — invisible under the default build, and surfacing only
      under the backend matrix the item above widens. Anything else that edits
      a Proxygen client path has the same trap.
    - **The response `Content-Type` had nowhere to live.** The shared
      `http_response` struct carried a status code and a body only, so neither
      bridge could see the header. It now carries the media type, filled by one
      `message_media_type()` helper that both bridges *and* the server's
      `RequestHandler` call — three hand-rolled copies of "strip the
      parameters, treat absent as empty" is the shape that ends up right in two
      places and subtly wrong in the third.
  - `tests/proxygen_negotiation_integration_test.cpp` pins the behaviour, and
    is **the first test anywhere that pairs one HTTP implementation's client
    with a different implementation's server** (raw `httplib::Client` against a
    live `proxygen_server`) — see the interop-grid item below, which this
    partially opens.

- **Test the client-implementation × server-implementation interop grid.**
  Until August 7, 2026 there was **no test anywhere** that paired one HTTP
  implementation's client with a different implementation's server. The two
  tests whose names suggest otherwise do not:
  `beast_cross_transport_equivalence_test` runs httplib client → httplib server
  on port 18220 and Beast client → Beast server on 18221 and compares the
  *results*; `three_way_http_transport_equivalence_test` says so outright in its
  header — "against each transport's **own** client/server pair". Three
  implementations means nine cells; three were covered, all on the diagonal.
  - Equivalence is not interoperability. Each implementation's client is only
    ever exercised against a server that shares its assumptions, so two
    transports can pass every equivalence assertion and still fail to talk to
    each other. The same grid-with-only-a-diagonal shape as the backend matrix
    item above, on a different pair of axes.
  - **One off-diagonal cell now exists.**
    `tests/proxygen_negotiation_integration_test.cpp` (Task 10a) drives a live
    `proxygen_server` with a raw `httplib::Client`. It was written for the
    negotiation branches rather than for this item — a foreign client is the
    only way to send the headers that reach 415/406 — but it is the httplib →
    Proxygen cell, and it is the reason the response-label defect became
    testable at all. Seven cells remain.
  - **The Proxygen blocker is gone.** This item used to depend on deciding
    whether Proxygen negotiates; it does, as of the entry above, so the
    Proxygen cells can now be written against specified behaviour rather than
    encoding today's accident as the expectation. Same caveat as the backend
    matrix — expect red on day one, so land fixes first or mark the new cells
    non-blocking with a dated note, never silently allowed-to-fail.
  - Note spec Task 15 already covers *serializer* interop (multi- ↔
    single-serializer, both directions, HTTP and CoAP). This item is the
    orthogonal axis — same serializer, different transport implementation — and
    the two should not be conflated when writing the tests.

- **Real-GCE suite: three silent no-ops found during the first live run
  (August 5, 2026) — all three fixed August 6, 2026.** All surfaced while
  verifying the zone-laddering escalation (PR #158) against real GCE, none
  affects that verification's result, and all are the same shape as the
  failures that run kept hitting — machinery that reports nothing and
  therefore reads as fine.
  - **`CostSummaryFixture` produced no output — fixed.** The
    `gcp_quorum_manager_real_gce_test` run that provisioned six VMs across
    three zones (`31055737271`) printed no cost summary at all — not a zero,
    nothing. Its early-out was `if (reps.empty()) return;`, which made "no
    reports registered" and "fixture never ran" indistinguishable. **It was
    the former**, settled by running a case against live GCE and watching the
    new warning print: no GCP case ever called `g_cost_accumulator.add()`
    (Azure's suite does so eleven times, AWS's three, GCP's zero), so the
    accumulator was always empty. Fixed by an RAII `case_cost_recorder` that
    files a report per case — RAII so a throwing case still reports, and so a
    newly added case cannot forget — wired into every provisioning case in
    both the Compute and MIG suites. The empty branch is now a loud WARNING
    rather than silence, in all three providers' fixtures. The summary
    renderer was extracted to a free `format_cost_summary()` so both of its
    branches are unit-testable offline; seven cases in
    `gcp_spot_escalation_test.cpp` cover it, needing no credentials.
  - **`BOOST_TEST_MESSAGE` output never appeared in real-cloud logs — fixed.**
    Boost's default log level (`error`) suppresses it, so every diagnostic
    written that way in the real-cloud suites was invisible — including
    `provision_escalates_past_zone_stockout`'s own record of which rung was
    refused and which one took over. Only the `std::cerr` `trace()` markers
    survived, which is the sole reason that case's behaviour could be
    confirmed at all. Fixed by exporting `BOOST_TEST_LOG_LEVEL=message` from
    `scripts/run-real-cloud-suite.sh` — an env var rather than a
    `--log_level` argument because ctest invokes each test through its
    registered command line, with no way to append one. Applies to the AWS
    and Azure real-cloud suites identically. The cost summary itself was
    additionally moved to `std::cerr`, so the spend record does not depend on
    a runner flag being set.
  - **Related, larger, and *not* test-only: `make_options()` set no retry,
    backoff or deadline policy on any client — fixed.** Every call in
    `gcp_compute_quorum_manager` inherited whatever google-cloud-cpp's
    default was, and `gcp_client_config::api_timeout` — documented as
    "maximum time allowed for a single GCP API call" — was consulted only by
    `gcp_operation_wait`, never by a client. The identical defect was present
    in all three copies of `make_options()` (`gcp_compute_detail`,
    `gcp_mig_detail`, `gcp_privateca_detail`), which were byte-identical; they
    are now one shared `include/raft/gcp_client_options.hpp`. Each client sets
    its service-specific `LimitedTimeRetryPolicy` (bounded by `api_timeout`)
    plus an exponential backoff and, for the LRO clients, a polling policy.
    Two transport bounds are set for the REST clients: `ServerTimeoutOption`
    and `rest_internal::TransferStallTimeoutOption` — the latter being the
    only one that aborts a stalled socket client-side, at the cost of
    depending on an internal-namespace option, which `__has_include` makes
    non-fatal to lose. PrivateCA is gRPC-backed and so gets the retry
    policies but not the REST bounds.

- **CoAP/Proxygen test-reliability sweep — four fixes, one item still open
  (August 4, 2026).** Full investigation record, with every
  before/after measurement, in
  [doc/coap-flake-investigation.md](coap-flake-investigation.md).
  - **`coap_thread_safety_property_test`'s "memory access violation at
    address: 0x1ac" — fixed** (`121f5ae`). Not a null dereference in the
    case Boost blames, which only calls `is_dtls_enabled()` (`return
    _config.enable_dtls;`) and cannot fault. A case that exceeds its
    `*boost::unit_test::timeout()` is unwound by Boost via `siglongjmp()`
    (`execution_monitor.ipp:873`), which **runs no destructors**: its worker
    threads are orphaned rather than joined, and go on reading a
    `coap_client` whose stack the *next* case has already reused. Fixed by
    heap-owning the transport and counters and capturing them by value as
    `shared_ptr`. Measured with the case forced to overrun: 11 crashes in 15
    runs before, 0 after, with SIGALRM firing in every run of both arms as
    the control.
  - **The same pattern in `coap_connection_reuse_property_test` and
    `coap_concurrent_processing_property_test` — fixed** (`1a52e1f`), 10
    crashes in 12 runs before, 0 after. These used a `[&]` catch-all
    capture, which is why an initial grep for `[&client` missed them.
  - **`coap_client::generate_message_token()` exceeded CoAP's 8-byte token
    cap — fixed** (`763f43b`). **A production bug, not a test issue.**
    Tokens were `"token_" + std::to_string(n)`, which reaches 9 bytes at
    n=100; `coap_add_token()` accepts an over-long token and `coap_send()`
    then drops the PDU, logging only a warning while `send_rpc()` returns a
    future that can never complete. A client silently stopped transmitting
    after its 100th request. Latent because nothing exercised the real
    libcoap path until `d54bc46` wired it into the non-stub build. Now a
    fixed-width 8-hex-digit encoding, with `send_rpc()` throwing on any
    over-long token so the failure can never be silent again, and
    `coap_max_token_length` `static_assert`ed against libcoap's own
    `COAP_TOKEN_DEFAULT_MAX`. Verified end-to-end: 101 dropped PDUs before,
    0 after, over the 200 requests `coap_thread_safety_property_test` issues.
  - **`proxygen_transport_test` TLS fixtures — reduced, still open**
    (`dd041bf`). RSA-2048 keygen via the `openssl` CLI cost 741-2966ms under
    load, against a 3000ms RPC deadline the test hardcodes in nine places;
    switched to P-256 (worst case 2321ms → 192ms under identical load; 3.09s
    → 0.72s in CI artifacts) and adopted `tests/test_timeout_scale.hpp`,
    which this suite uniquely lacked. **The failure was never reproduced
    locally** across 70 runs, so this reduces fragility rather than proving
    a root cause — tracked as still open in
    `.kiro/specs/proxygen-http-transport/tasks.md`.
  - **A correction worth keeping.** Three places in the tree recorded that an
    overrunning case aborts via `~std::thread()`'s `std::terminate()` on a
    still-joinable thread. That is true of a normal exception unwind and
    false of the signal path that actually occurs, and it drove four previous
    "fixes" (`92d824b`, `bc39d04`, `9727d38`, `5a9c5ff`) that shrank
    workloads or raised timeouts — each lowering the odds of the crash
    without removing it. It also nearly produced a fifth: restoring an RAII
    thread-joiner, which would have been inert for exactly that reason.
    Reading Boost's own source rather than the comments in the tree is what
    broke the cycle.

- **`ca_cluster_node_test` intermittent hang — fixed and verified (July 30,
  2026).** Root cause, found in commit `19b05e2` (July 29, 2026):
  `run_ca_cluster_node()`'s shutdown sequence joined `http_thread`,
  `election_timer`, `heartbeat_timer`, and `maintenance_thread` *before*
  calling `raft_node.stop()`. Every `submit_command()`/`read_state()`
  future is only ever resolved two ways: it completes normally, or
  `check_heartbeat_timeout()` calls
  `CommitWaiter::cancel_timed_out_operations()` on its next tick.
  `raft_node.stop()`'s `CommitWaiter::cancel_all_operations()` is the only
  other path that resolves a pending future, but it ran last, after every
  `join()`. If SIGTERM landed while `maintenance_thread` was blocked
  inside a `submit_command().get()` call (e.g. the leader-transition no-op
  commit in `ensure_signer()`/`maybe_bootstrap()`) and the commit could no
  longer land (this node losing quorum precisely because it and/or its
  peers were shutting down), with `heartbeat_timer` already stopped
  ticking by the time `maintenance_thread.join()` was reached, nothing was
  left running to ever unblock it — `maintenance_thread.join()`, and the
  whole process, hung forever. This matched the originally-documented
  symptom exactly (a follower `stop()`'d cleanly, the leader retrying
  `AppendEntries` against it forever, and the test's own `waitpid()` on
  the child never returning) but had not been connected to it at the time
  this entry was first written; the investigation described below (no
  `ptrace` access, ~12 attempts via `/proc/<pid>/task/*/wchan` alone)
  never found it. Fix: call `raft_node.stop()` immediately after
  `server->stop()`, before any thread `join()`, so a blocked `.get()` call
  is force-rejected right away instead of racing an already-stopped
  timeout mechanism. `cmd/chaos_node/main.cpp` was checked for the same
  pattern and doesn't have it (no `maintenance_thread`/HTTP handlers that
  ever block on `submit_command()`).
  **Verification** (July 30, 2026, `fix/ca-cluster-node-hang`): 25
  iterations of `ctest -j3 -R
  'ca_cluster_node_test|ca_cluster_node_rpc_tls_test|ca_cluster_node_rpc_tls_restart_test'`,
  each wrapped in `timeout --signal=KILL 120` — the same heavy concurrent
  load that originally triggered the ~1-in-12-15 hang rate. Zero hangs
  (zero `timeout`-forced kills) across all 25; two ordinary assertion
  failures under load (`certificate issuance failed with only 2 of 3
  nodes up`, a real but *different* quorum-timing flake, not a hang —
  each failing iteration's suite still completed normally afterward,
  confirming it isn't a recurrence of this bug). Also added defense in
  depth: `cluster_node_process::stop()` in all three test files now polls
  `waitpid(..., WNOHANG)` with a 30-second bounded timeout
  (`tests/ca_cluster_node_process_wait.hpp`'s `wait_for_exit_or_kill()`)
  instead of a plain blocking `waitpid()`, escalating to `SIGKILL` and
  raising a `BOOST_ERROR` if exceeded — so a *future* regression of this
  exact shutdown-ordering bug would surface as a diagnosable test failure
  instead of silently going back to an indefinite `ctest` hang.
  July 24, 2026 (kept for history): the resulting failure mode before the
  above fix — an abnormally-terminated test process orphaning a spawned
  `ca_cluster_node` child, which then held the test's stdout/stderr pipe
  open indefinitely and wedged `ctest`'s own output capture (turning one
  flaky test into a hung whole suite) — was fixed independently via
  `PR_SET_PDEATHSIG`, applied to all three files sharing this
  `posix_spawn`-based subprocess pattern. Verified via 10 further runs
  with no orphans left behind at the time; superseded in relevance by the
  root-cause fix above, but kept working alongside it.

- **Folly decoupling is complete at the header level; two independent
  gaps remain out of scope** (July 24, 2026) — `future_default.hpp` no
  longer unconditionally pulls in Folly (`include/raft/future.hpp`)
  regardless of `KYTHIRA_DEFAULT_FUTURE_BACKEND`; ~20 production headers
  that hardcoded raw `kythira::Future`/`FutureFactory` (certificate/AWS/
  ACME providers, DNS peer discovery, RPC transports) were converted to
  `future_default`/`future_factory_default`; two genuinely non-future
  Folly dependencies masquerading as future-backend ones
  (`folly::Synchronized` in `peer2peer_replication.hpp`/
  `tcp_gossip_transport.hpp`, `folly::CPUThreadPoolExecutor` in
  `tcp_rpc.hpp`/`tls_tcp_rpc.hpp`) were replaced with portable
  equivalents (`kythira::synchronized<T>`, new
  `include/raft/synchronized.hpp`; `kythira::executor_default::submit()`,
  extending the existing `executor_default`). Verified for real: every
  affected header compiles under `KYTHIRA_FUTURE_BACKEND_STDEXEC` with
  `-H` header-tracing confirming zero Folly headers touched (not just "the
  Kconfig symbol allows it"), plus full `cmake --build` + `ctest` runs
  under all three backends (folly 389/394, stdexec 389/391, boost
  388/394 — only pre-existing, unrelated failures: the 5 documented
  LocalStack/real-EC2 tests, plus two timing-threshold-sensitive tests
  this sandbox's stdexec overhead trips that the same tests don't touch
  under folly/boost, both in files untouched by this work
  — `performance_equivalence_property_test.cpp`,
  `future_backend_benchmark_test.cpp`).
  Also found and fixed, mid-verification: `ldns/common.h` (`libldns`,
  used by the DNS peer-discovery headers and `acme_certificate_provider`'s
  DNS-01 challenge support) `#define`s `true`/`false` as plain macros
  with no `__cplusplus` guard, silently corrupting any C++20
  `concept`/`requires` code parsed afterward in the same translation unit
  — harmless standalone, but a real, reproducible compile break the moment
  a file pulls in both `<ldns/ldns.h>` and stdexec's headers (`"atomic
  constraint must be of type bool (found int)"`). Fixed with a defensive
  `#undef true` / `#undef false` immediately after every direct
  `<ldns/ldns.h>` include (7 files) — always safe in C++, since `true`/
  `false` are keywords regardless of macro state.
  **Two things deliberately left out of scope**, both documented directly
  in `CONFIG_FOLLY`'s own Kconfig help text: (1) roughly 30 test files call
  `folly::init()`/construct `folly::Init` for Boost.Test process bootstrap
  — a real Folly dependency, but entirely unrelated to which future
  backend is selected, so converting it isn't a "decouple the future
  backend" change; (2) the root `CMakeLists.txt` still gates
  `certificate_authority`, most of `examples/`, and the whole `tests/`
  subdirectory on `folly_FOUND` at the subdirectory level rather than
  per-target — meaning `CONFIG_FOLLY=n` today stops CMake from *probing*
  for Folly, but doesn't yet make those targets actually *buildable*
  without it, since the ~20-30 genuinely Folly-specific test files (by
  design, e.g. `folly_concept_wrappers_*`, `*_future_returning_callback_*`)
  are mixed into the same subdirectories as everything else. Restructuring
  that into fine-grained per-target gating across 100+ targets is a
  separate, much larger undertaking than this pass.

- **Gap 1 above (test-file `folly::init()` bootstrap) closed** (July 25,
  2026) — the "roughly 30 test files" estimate was low: a full grep found
  135 files calling `folly::init()`/constructing `folly::Init`, of which
  120 had no other Folly usage at all (the rest — `folly_concept_wrappers_*`,
  `*_future_returning_callback_*`, etc. — genuinely need Folly and were left
  untouched). Of those 120, 13 are `cmd/*/main.cpp` and `examples/*.cpp`
  standalone executables calling `folly::init()` in a real `main()` — out of
  scope, since that's legitimate production usage, not vestigial Boost.Test
  bootstrap. The remaining 106 (105 `tests/*.cpp` files plus the shared
  `tests/chaos/chaos_test_types.hpp` fixture) had their
  `FollyInitFixture`/`GlobalFixture`/`chaos_test_fixture` boilerplate gated
  behind `#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) &&
  !defined(KYTHIRA_FUTURE_BACKEND_BOOST)` — the same backend-conditional
  pattern used throughout the rest of the Folly-decoupling work — rather than
  deleted outright.
  Deletion was the first approach tried, and it was wrong: it broke 37 tests
  at runtime (`SIGABRT`, `"Singleton folly::Timekeeper ... requested before
  registrationComplete()"`). Under the Folly backend (still the project
  default), `kythira::future_default<T>` *is* `folly::Future<T>`, so any
  test that transitively touches Folly's async timer machinery — retry
  backoff delays in `error_handler.hpp`, `future_collector.hpp` timeouts,
  etc. — needs `folly::init()` to have run first, even though the test's own
  source never mentions `folly::` by name. A static grep for the literal
  string `folly::` in each file's own text (the heuristic used to classify
  "boilerplate-only" vs "genuinely needs Folly") can't see that transitive,
  runtime-only dependency, so it isn't a safe signal for whether the init
  call can simply be deleted. Caught by actually running the full test
  suite after the change (not just rebuilding), which is why that step is
  never skippable for this kind of change. The conditional-gating fix
  restores identical behavior under the Folly backend (still selected by
  default today) while genuinely removing the Folly dependency once
  `CONFIG_STDEXEC_BACKEND`/`CONFIG_BOOST_FUTURE_BACKEND` is selected instead.
  `tests/chaos/chaos_test_types.hpp`'s shared fixture needed hand-editing
  rather than the scripted pass, since it does double duty (`folly::Init`
  *and* `fiu_init(0)` fault-injection setup in the same constructor) — only
  the Folly half is gated, `fiu_init(0)` stays unconditional.
  `tests/minimal_network_test.cpp` (a standalone smoke-test `main()`, not a
  Boost.Test file at all) was left untouched for the same reason as the
  `cmd/`/`examples/` files.
  Verified with a full rebuild + the same label-filtered `ctest` subset the
  pre-commit coverage hook and CI both use
  (`-LE ^(slow|performance|verbose|benchmark|docker)$`,
  `--repeat until-pass:3`): 382/382 passed, 0 failed — confirmed via
  `ctest`'s own exit code and `Testing/Temporary/LastTest.log`, not just the
  wrapping shell pipeline's exit code (which had earlier masked the
  37-failure regression, since piping through `tee | tail` reports the exit
  code of `tail`, not `ctest`).
  Gap 2 (per-target `folly_FOUND` CMake gating across 100+ targets) remains
  deliberately out of scope, unchanged from the note above.

  **Unrelated, found and fixed along the way**: the repo-root
  `vcpkg_installed/x64-linux` dependency cache (gitignored, local-only) had
  silent file-level corruption in three `boost/intrusive` headers
  (`pack_options.hpp`, `detail/mpl.hpp`, `detail/function_detector.hpp`) —
  stray characters inserted into macro definitions (e.g. `template< class
  (TYPE)>` instead of `template< class TYPE>`), breaking any target that
  pulls in Folly's futures headers. Root-caused by diffing against the
  known-good copy in `build-clang/vcpkg_installed`; fixed for good via a
  full `vcpkg install` reinstall from the manifest (binary-cache-backed, a
  few seconds) after moving the corrupted directory aside. That reinstall
  doesn't manage everything under `vcpkg_installed/`, though: the
  `libPocoDNSSD.a`/`libPocoDNSSDAvahi.a` static archives documented in
  README.md's ARM-support section are manually built and placed there by
  hand (DNSSD isn't a vcpkg feature), so they were lost when the corrupted
  directory was deleted rather than kept. The project's own graceful-
  degradation path (`POCO_DNSSD_FOUND=FALSE` when the archives are absent)
  handled it correctly once `build-clang` was reconfigured — the affected
  targets (`poco_peer_discovery*`, a handful of `coap_*` tests) simply
  disable that backend, exactly as an x86_64 host without the prebuilt
  archives already would. Rebuilding those archives from scratch, if ever
  needed, isn't documented anywhere yet — worth adding if this bites again.

- **Gap 2 above (per-target `folly_FOUND` CMake gating) closed for
  `tests/`/`certificate_authority`** (July 25, 2026) — scoped empirically
  with a real probe build (`-DCMAKE_DISABLE_FIND_PACKAGE_folly=ON
  -DKYTHIRA_DEFAULT_FUTURE_BACKEND=stdexec`), the only reliable way to find
  what actually breaks rather than guessing from inspection. Three
  independent problems, found in that order:
  1. **The subdirectory-level gates were checking the wrong condition.**
     `certificate_authority`/`tests/`/three `cmd/*` groups were gated on
     `folly_FOUND` specifically, when the real requirement is "the
     *selected* future backend's dependency is satisfied" — not the same
     thing once a non-folly backend is chosen. Replaced with
     `KYTHIRA_FUTURE_BACKEND_AVAILABLE` (`folly_FOUND OR NOT
     KYTHIRA_DEFAULT_FUTURE_BACKEND STREQUAL "folly"`), reusing the
     invariant CMake already enforces via `FATAL_ERROR` for stdexec/boost
     when *they're* selected without being found. `examples/` and the 5
     `cmd/*` discovery-node/ca_cluster_node executables were deliberately
     left `folly_FOUND`-gated, not relaxed: every one of them calls
     `folly::init()` directly in its own `main()` to demonstrate/require
     real Folly usage, unlike `tests/`.
  2. **A handful of test targets silently ignored `KYTHIRA_DEFAULT_FUTURE_
     BACKEND` entirely.** `quorum_management_test`,
     `docker_quorum_manager_test`, `state_machine_commutativity_property_
     test`, `state_machine_crash_recovery_property_test`,
     `raft_multi_node_fixture_test`, `raft_cluster_initialization_unit_test`
     used a legacy hand-rolled `add_executable`/`target_link_libraries`
     pattern that never linked `network_simulator` (the target that
     carries the `KYTHIRA_FUTURE_BACKEND_STDEXEC`/`_BOOST` compile
     definitions) — meaning they always compiled `future_default.hpp`'s
     Folly branch regardless of which backend Kconfig actually selected. A
     latent, pre-existing bug (silently wrong under boost/stdexec even
     with Folly present) that gap 2's stress-testing surfaced as a hard
     failure instead. Fixed by adding `network_simulator` to each one's
     link libraries.
  3. **A real, substantial minority of tests are genuinely Folly-only by
     design**, not something to "fix": HTTP/CoAP transport tests
     (`include/raft/http_transport.hpp`/`coap_transport.hpp` hardcode
     `folly::Future<T>` as `future_template` — a real, header-level Folly
     dependency the original decoupling pass never covered, since that
     pass scoped to the Raft core and RPC transports, not HTTP/CoAP),
     Folly concept-wrapper tests (`folly_concept_wrappers_*`,
     `kythira_*_concept_compliance_*`, etc.), and cross-backend comparison
     tests that need Folly present to compare against
     (`future_backend_benchmark_test`, `cross_backend_concept_compliance_
     property_test`). Decoupling HTTP/CoAP transport from Folly the way
     the Raft core already is would be a separate, much larger feature —
     explicitly out of scope here. Instead, all 70 such targets (found by
     rerunning the probe after fixes 1–2 and reading off the real
     remaining failure list, not guessed up front) were wrapped in
     `if(TARGET Folly::folly) ... else() message(STATUS "...: skipped
     (requires Folly)") endif()` within `tests/CMakeLists.txt`, so they're
     cleanly skipped rather than hard-failing when Folly is absent.
  Verified clean on all three backends, each with Folly genuinely absent
  (`-DCMAKE_DISABLE_FIND_PACKAGE_folly=ON`) in a real (non-tmp) build
  directory — a first probe run under `/tmp/.../scratchpad/` produced two
  spurious failures (`complete_conversion_validation_property_test`,
  `namespace_consistency_property_test`) that turned out to be a
  source-root path-resolution artifact of that unusual nested location,
  not a real regression; both pass cleanly once built from a normal path,
  confirmed by rerunning from scratch: stdexec 244/247 passed (only
  `performance_equivalence_property_test`, an already-documented
  pre-existing stdexec-timing sensitivity), boost 245/247 passed (that
  same pre-existing test, plus `membership_change_leader_crash_property_
  test`, confirmed flaky rather than a regression by 4 clean standalone
  reruns immediately after). The default Folly-enabled build was also
  fully re-verified after all these changes: 382/382 passed, 0 failed —
  zero regressions for the common case.
  **Two things remain deliberately out of scope**: (1) decoupling HTTP/
  CoAP transport from Folly at the header level, the same way the Raft
  core already is — a separate, much larger feature, not "finishing" gap
  2; (2) `examples/` and the 5 standalone `cmd/*` production executables
  stay Folly-only, since their own `main()`s call `folly::init()` directly
  by design.

- **`three_way_http_transport_equivalence_test` segfaulted under
  ThreadSanitizer — FIXED** (July 30, 2026, PR #117 introduced the
  workaround; fixed shortly after) — this test passed cleanly under every
  ordinary CI leg but reproduced an identical SIGSEGV on two separate real
  runs of the `tsan` CI job (`.github/workflows/ci.yml`), roughly 2.1s in,
  with zero diagnostic output either time — not even Boost.Test's own crash
  handler fired, i.e. it crashed before `main()`'s test framework was
  running. Root cause: it was the only Folly-touching Boost.Test binary in
  the suite that never called `folly::init()`. Folly's registration-gated
  singletons — notably `folly::Timekeeper`, reached through the RPC future
  timeouts (`.get()` with an `rpc_timeout`) — abort if accessed before
  `folly::init()` runs `registrationComplete()`. As the only binary that
  combines the Beast (boost::asio) and Proxygen (Folly) transports in one
  process, its thread interleavings differ from the sibling
  `beast_*`/`proxygen_transport_test` suites, and TSan's perturbed pthread/
  TLS ordering turned that latent, timing-dependent abort into the
  output-less early crash (it fires from a background Folly/IO thread before
  Boost.Test's handler is in place, which is why no stack was printed). Fix:
  install the same `FollyInitFixture` (a `BOOST_GLOBAL_FIXTURE` constructing
  a `folly::Init`) that the ~118 other Folly-dependent tests already use, so
  Folly's singletons are registration-complete before any transport runs.
  The test is re-enabled in the `tsan` job (build target + `ctest -R`) so CI
  now verifies the fix holds, and it shares `tests/tsan_suppressions.txt`
  with the other suites.

- **`.kiro/specs/boost-beast-http-transport/tasks.md`'s Task 13 is now
  closed.** Both gaps this entry previously tracked are done:
  `max_request_body_size` is actually enforced (`server_session` reads
  through a `beast_http::request_parser` with `.body_limit()` set, returning
  413, with oversized-body and truncated-request coverage), and
  `tests/beast_transport_test.cpp` has been split into the
  one-file-per-concern layout (`beast_client_test.cpp`/
  `beast_server_test.cpp`/`beast_integration_test.cpp`/`beast_ssl_test.cpp`)
  the rest of the suite uses. A subsequent ThreadSanitizer pass over the
  split binaries closed four further pre-existing races.

- **`http_transport_impl.hpp`'s `make_future_with_exception` no longer
  slices derived exceptions.** It took `const std::exception&`, so
  `std::make_exception_ptr(e)` deduced `e`'s *static* type and reduced
  every `http_timeout_error`/`serialization_error`/`http_client_error`/
  `http_server_error` to a plain `std::exception` — discarding the derived
  type, its message, and `status_code()` across five call sites. Now
  templated on the concrete exception type
  ([PR #114](https://github.com/crawlins/kythira/pull/114)), which also let
  `tests/beast_cross_transport_equivalence_test.cpp` drop its workaround and
  assert that both transports surface the same exception type and status
  code. The trailing `catch (const std::exception&)` fallback still deduces
  the base type by nature; that is the generic catch-all, not a typed-error
  path. See that spec's "Known Follow-ups" section for the fuller writeup.

---

## Remaining Work (All Optional)

### Build Tooling

- [x] **clang-format integration** — `.clang-format` config (Google base, 4-space
  indent, 100-col); CMake `format`/`format-check` targets; pre-commit hook
  checks staged files first; `SKIP_FORMAT_CHECK=1` escape hatch
- [x] **clang-tidy integration** — `.clang-tidy` config with `WarningsAsErrors: "*"`;
  CMake `static-analysis`/`static-analysis-fix` targets; pre-commit opt-in hook
  step; zero findings across 291 source files
- [x] **Code coverage** — CMake `ENABLE_COVERAGE` option using Clang/LLVM
  source-based instrumentation (`llvm-profdata`/`llvm-cov`, switched from an
  earlier gcovr approach that undercounted template-heavy headers via
  COMDAT folding), HTML reports, pre-commit ratchet hook, and a dedicated CI
  coverage job (`coverage_floor.txt` = 88.99%); code-coverage spec now
  20/20 tasks complete
- [x] **CI reliability (flaky build-and-test/coverage jobs)** — `ca_cluster_node_test`
  (real multi-process Raft cluster, flaked under `ctest -j$(nproc)` CPU
  contention) now retries via `--repeat until-pass:3` and is isolated from
  co-scheduling via `PROCESSORS 4`; coverage floor check has a 0.50pp
  tolerance band for CI-vs-local measurement noise; coverage job's
  disk-reclaim step widened after intermittent "No space left on device"
  link failures
- [x] **stdexec future backend** — a second, `stdexec` (P2300 sender/receiver)
  backed `Future`/`Promise`/`Try`/`Executor` implementation alongside the
  default Folly one, for new code wanting direct access to `stdexec`
  schedulers/algorithms; `include/raft/future_stdexec.hpp`, backend
  selection via `KYTHIRA_DEFAULT_FUTURE_BACKEND` CMake option
  (`include/raft/future_default.hpp`); spec at
  `.kiro/specs/stdexec-future-backend/`; 52/52 tasks complete;
  found and fixed a real GCC 13 `-O2`/`-O3` miscompilation of
  `exec::any_sender`'s small-buffer-optimized move constructor along the
  way (`-fno-strict-aliasing` for GCC builds, `clang++-18` unaffected).
  **Update, July 24, 2026**: "no existing production call site converted"
  above is now stale — see that day's `CHANGELOG.md` entry. Production Raft/
  RPC code and the full test suite were converted to `future_default` and
  now run cleanly under this backend end to end (373/373 `ctest`); the
  conversion surfaced and fixed a real double-execution bug in this
  backend's `thenTry` Future-returning overload and a widespread
  discarded-continuation footgun (`kythira::Future<T>::detach()` added to
  address it). Folly remains the default and a required dependency
  regardless.
- [x] **boost future backend** — a third, `boost::thread`-backed
  `Future`/`Promise`/`Try`/`Executor` implementation alongside the default
  Folly one and the `stdexec` one, for new code wanting the
  `boost::asio`-backed timer primitives (`delay`/`within`) without pulling
  in Folly's Timekeeper; `include/raft/future_boost.hpp` (guarded behind
  `KYTHIRA_HAS_BOOST_FUTURE`), backend selection via
  `KYTHIRA_DEFAULT_FUTURE_BACKEND=boost` CMake option
  (`include/raft/future_default.hpp`); spec at
  `.kiro/specs/boost-future-backend/`.
  **Update, July 24, 2026**: "no existing production call site converted"
  above is now stale — see stdexec's update note just above and that day's
  `CHANGELOG.md` entry; the same conversion covered both backends together.
  Also found and fixed two real bugs specific to this backend
  (`collectAnyWithoutException<void>` and a `nullptr`-`exception_ptr`
  crash in `set_exception_from_std`) and one missing overload
  (`makeReadyFuture(T value)`). Folly remains the default and a required
  dependency regardless. A Phase 0 spike (throwaway
  compile against the real vendored Boost headers) found `BOOST_THREAD_
  PROVIDES_EXECUTORS` is gated on `BOOST_THREAD_VERSION>=5`, not `>=4` as
  originally assumed from source inspection alone — defined explicitly
  alongside `BOOST_THREAD_VERSION=4` instead; also found `boost::
  exception_ptr` is a distinct type from `std::exception_ptr` whose
  implicit converting constructor compiles but silently breaks rethrow,
  requiring a genuine catch-and-rethrow bridge at every exception boundary;
  two property-test binaries (31 cases) plus an extended
  `backend_non_interference_compile_fail_test.cpp` (now unconditional
  rather than `stdexec_FOUND`-gated, since it needs to validate Folly-only,
  boost-only, stdexec-only, and both-enabled configurations)
- [x] **Remove unused includes** — `http_transport_impl.hpp`'s own
  `#include <future>` was provably redundant (it includes
  `raft/http_transport.hpp` first, which already includes `<future>`
  unconditionally); `simulator_impl.hpp` had two genuine literal
  duplicates (`#ifdef FOLLY_FUTURES_AVAILABLE #include <folly/futures/
  Future.h> #endif` appeared twice back to back, `#include <thread>`
  appeared twice). Verified by building representative consumers of
  both headers.
- [x] **Folly CMake detection** — confirmed by actually building with
  Folly hidden (`-DCMAKE_DISABLE_FIND_PACKAGE_folly=ON`) that
  `certificate_authority`, all of `examples/`, and the overwhelming
  majority of `tests/` transitively require Folly via
  `include/raft/future.hpp` (no `#ifdef` of its own) but weren't gated
  on `folly_FOUND`, so a Folly-absent configure looked fine (just a
  mild warning) until the build itself died deep inside a confusing
  `GLOG_EXPORT` error with no indication Folly was the real cause. All
  three now gated at the top level; full build with Folly hidden
  completes cleanly (exit 0) instead of failing catastrophically; no
  regression in the normal (Folly present) configuration
- [x] **Kconfig integration** — a declarative front end (via
  [Kconfiglib](https://github.com/ulfalizer/Kconfiglib), the same
  configuration language as the Linux kernel/Zephyr/Buildroot/coreboot)
  layered over the ad hoc per-dependency `find_package`/`KYTHIRA_HAS_*`
  pattern: a root [`Kconfig`](../Kconfig) file declaring every optional
  dependency (OpenSSL, HTTP transport TLS, CoAP transport, EDHOC, DNS/Poco
  peer discovery, the AWS SDK core and ACM Private CA components, libssh2,
  libfiu, and the stdexec/boost future backends) with `depends on`
  constraints (`AWS_ACM_PCA` needs `AWS_SDK`, `EDHOC` needs `COAP_TRANSPORT`,
  `HTTP_TRANSPORT_TLS` needs `HTTP_TRANSPORT` and `OPENSSL`);
  `scripts/kconfig/genconfig.py` translating a resolved `.config` into
  `build/generated/autoconf.cmake` (`KCONFIG_<NAME>` variables) and
  `build/generated/kythira/autoconf.hpp` (the existing macro names,
  unchanged, so no `#ifdef` call site needed to change); `cmake/Kconfig.cmake`
  wiring those into the root `CMakeLists.txt` via two new macros
  (`kythira_find_optional()` for single-call dependencies,
  `kythira_kconfig_gate()`/`kythira_kconfig_require()` for the hand-written
  multi-step ones: libcoap, libldns, libfiu, Poco DNSSD, the AWS ACM PCA
  two-step re-probe); `menuconfig`/`guiconfig`/`savedefconfig`/
  `kconfig-check` CMake targets; checked-in `configs/ci_full_defconfig` and
  `configs/minimal_defconfig`. `KYTHIRA_KCONFIG_STRICT=ON` turns "wanted but
  not found" from a silent skip into a hard `find_package(... REQUIRED)`-
  style configure failure — verified directly by selecting `CONFIG_EDHOC=y`
  with the `lakers` vcpkg feature genuinely not installed in this
  environment and confirming CMake's own standard not-found error fires,
  naming `lakers`. folly, Boost, and stdexec deliberately stay outside
  Kconfig's control (hard-required or already-unconditionally-probed, per
  the design doc's "Kconfig expresses intent; CMake still does detection"
  principle) — only genuinely optional dependencies are gated. Zero-config
  behavior (no `-DKYTHIRA_KCONFIG`, no prior `menuconfig`) is byte-for-byte
  unchanged from before this feature, verified both with and without
  `kconfiglib` installed by diffing the full `cmake --build --target help`
  target list against a pre-Kconfig baseline configured from the same
  `vcpkg_installed/` tree — identical apart from the four new Kconfig
  targets themselves. `configs/ci_full_defconfig` deliberately leaves
  `CONFIG_COVERAGE` at its default (off) despite selecting every other
  optional feature: forcing it on would require Clang and
  `CMAKE_BUILD_TYPE=Debug` project-wide, breaking the g++-13 CI jobs that
  also apply this defconfig — coverage remains a separate build variant
  with its own dedicated CI job and direct `-DENABLE_COVERAGE=ON`, verified
  to produce identical `ENABLE_COVERAGE` cache state whether set that way or
  via `CONFIG_COVERAGE=y`.

### New Transport Implementations

- [x] **Boost.Beast HTTP transport** — a second `network_client`/
  `network_server` implementation alongside cpp-httplib
  (`include/raft/beast_http_transport.hpp`), driven by a caller-owned
  `boost::asio::io_context` (this project's first genuinely asynchronous
  transport); connection pooling with per-connection `net::strand`,
  configurable timeouts, TLS (mutual and server-only) with hot reload,
  cross-transport equivalence tests against cpp-httplib; spec at
  `.kiro/specs/boost-beast-http-transport/`
- [x] **Proxygen HTTP transport** — a third `network_client`/
  `network_server` implementation (`include/raft/proxygen_http_transport.hpp`),
  backed by Meta's Proxygen driven directly by Folly's `EventBase`/
  `IOThreadPoolExecutor`; connection-to-thread pinning falls out of
  Proxygen's own architecture rather than requiring a manually-built
  `net::strand` equivalent; adds an optional Folly-native fast path
  (Requirement 16) that skips the generic `kythira::promise_default<T>`
  bridge entirely when `KYTHIRA_DEFAULT_FUTURE_BACKEND=folly` (this
  project's default) by wrapping a `folly::Promise<T>` directly into
  `kythira::Future<T>` — a shortcut neither cpp-httplib nor Beast has any
  equivalent of, since neither is built on Folly internally; TLS (mutual
  and server-only, via `folly::SSLContext`/`wangle::SSLContextConfig`)
  with hot reload; three-way cross-transport equivalence tests against
  cpp-httplib and Beast; see
  `doc/http_transport_performance_comparison.md` for measured
  cpp-httplib-vs-Beast-vs-Proxygen throughput/latency numbers; spec at
  `.kiro/specs/proxygen-http-transport/`

### Protocol Completeness

- [x] **Peer-to-peer log replication (gossip catch-up)** — opt-in
  `peer2peer_replicator_type` extension so a lagging member pulls missing log
  entries from another member that already has them instead of exclusively
  from the leader, addressing the single-leader `replicate_to_followers()`
  fan-out bottleneck for catch-up scenarios (rolling restarts, healed
  partitions, bursty joins); leader remains sole commit authority (no change
  to `_commit_index`/election safety), no-op default (`no_op_peer2peer_replicator`)
  preserves today's leader-only behavior exactly, activates only for catch-up
  (steady-state replication unchanged); the replicator's own peer set tracks
  `node<Types>::cluster_members()` — the replicated log's core cluster
  membership (`_configuration.nodes()`, unioned with `old_nodes()` during
  joint consensus, excluding learners) — pushed via `sync_peer2peer_membership()`
  at every `_configuration` mutation site, not separately maintained
  configuration; spec at `.kiro/specs/peer2peer-log-replication/`;
  21 tasks across 4 phases complete
- [x] **Peer-to-peer gossip transport** — `tcp_gossip_peer2peer_replicator`,
  a real anti-entropy gossip implementation (randomized push-pull digest
  exchange, Cassandra/Dynamo-style, not SWIM — Raft's own election timeouts
  already cover liveness detection) of the `peer2peer_replicator` concept
  above; self-contained TCP listener + background gossip thread,
  independent of the Raft RPC transport (`network_client_type`/
  `network_server_type` untouched); current membership comes exclusively
  from `sync_peer2peer_membership()` (driven by the log, per the spec above),
  never separately configured — only node-ID-to-address resolution
  (`address_book`) remains static, since addresses aren't log data; depends
  on `.kiro/specs/peer2peer-log-replication/`; test strategy deliberately
  avoids subprocess-spawning tests (single-process, real-TCP, multi-instance
  instead) after `ca_cluster_node_test` was diagnosed as this project's
  dominant CI-flake source; spec at `.kiro/specs/peer2peer-gossip-transport/`;
  14 tasks across 4 phases complete
- [x] **Membership change (add/remove server)** — joint consensus (Raft §6):
  log entry type discriminant, leader append of C_old+new, joint quorum
  (commit-index and election), `apply_committed_entries()` config-entry
  handling, C_new append after joint commit, leader step-down on
  self-removal, follower configuration update and truncation revert, node
  recovery on restart (log/snapshot/config reload); property tests for add
  server, remove server, and leader-crash-mid-change; spec at
  `.kiro/specs/membership-change/`; 20 tasks across 7 phases complete
- [x] **Node bootstrap** — `peer_discovery` concept + `ClusterJoin` RPC so a fresh
  node can locate an existing cluster and request membership without out-of-band
  `set_cluster_configuration()` calls; spec at `.kiro/specs/node-bootstrap/`;
  20 tasks across 7 phases complete; `no_op_peer_discovery` default preserves all
  existing behaviour; includes `rfc1035_peer_discovery`, `rfc2136_ldns_discovery`,
  `coap_multicast_peer_discovery` adaptors; 6 property tests + 14 unit tests +
  12 chaos tests; `register_node` ordering bug fixed (set `_self_address` after
  successful `send_update`)
- [x] **`poco_peer_discovery`** — registers and discovers nodes via the platform
  DNS-SD daemon (Avahi on Linux) using Poco DNSSD; TXT-record freshness with
  background renewal thread; Docker scenario test (`docker-poco-discovery-tests`);
  spec at `.kiro/specs/dns-peer-discovery/`
- [x] **`rfc2136_dns_sd_discovery`** — DNS-SD over unicast DNS via RFC 2136; publishes
  PTR + SRV + TXT records per node; background fresher thread renews `fresh_until`
  TXT field so stale entries from crashed nodes expire; 6 unit tests + 4 chaos tests;
  Docker scenario test (`docker-dns-sd-discovery-tests`) with BIND9; spec at
  `.kiro/specs/dns-peer-discovery/`
- [x] **`rfc6763_peer_discovery`** (partial) — SRV-query-only peer discovery via a
  single RFC 6763 SRV query at the cluster-level service name; building block for
  `rfc6763_ldns_peer_discovery`; spec at `.kiro/specs/dns-peer-discovery/`
- [x] **`rfc6763_ldns_peer_discovery`** (full) — registers PTR + instance SRV +
  cluster-level SRV (one RFC 2136 UPDATE to the cluster zone) + domain-level SRV
  (a second UPDATE to the domain zone) per node; delegates `find_peers` to the
  embedded `rfc6763_peer_discovery` with self-filtering; registration state is
  only committed after both UPDATEs succeed so a partially-failed registration
  leaves the destructor's cleanup a true no-op instead of attempting a real
  network DELETE with no configurable resolver timeout; deletes always use RFC
  2136 §2.5.4 delete-specific-RR (exact owner/type/rdata) so removing one
  node's entry never disturbs other live nodes sharing the same PTR/SRV
  RRset; spec at `.kiro/specs/dns-peer-discovery/`, now fully complete (all 6
  tasks, including the out-of-scope `rfc2136_dns_sd_discovery` addition)

### Certificate Management

- [x] **Certificate authority framework** — in-process `certificate_authority`
  (root CA generation, leaf issuance, revocation/CRL, `from_existing()`),
  `temp_cert_files` RAII helper, `ca_service` CLI (oneshot Docker/Podman
  provisioning + `--serve` HTTP API mode with `local`/`aws-acm-pca`
  providers); spec at `.kiro/specs/certificate-authority/`; 35 tasks complete
- [x] **`aws_acm_pca_provider`** — `certificate_provider` backed by AWS
  Certificate Manager Private CA; unit/LocalStack/real-AWS test tiers
- [x] **TLS hot-reload** — `reload_tls_material()`/`enable_auto_reload()` for
  `cpp_httplib_server`/`client` and `coap_server`/`client`; atomic
  write-tmp-then-rename certificate rotation via
  `temp_cert_files::replace_atomically()`
- [x] **`ca_cluster_node`** — Raft-replicated CA (`ca_state_machine` ledger of
  bootstrap/issuance/revocation, leader reconstructs via `from_existing()`
  and replays the ledger on election); multi-node subprocess test coverage
  including leader failover; packaged for 3-AZ AWS deployment (systemd, ECS
  task definitions); LocalStack/real-EC2 tests compile-verified only (no AWS
  access in this environment)
- [x] **ACME support (RFC 8555/8738)** — `acme_test_server` mock CA,
  `acme_certificate_provider` (JWS order lifecycle, http-01/dns-01
  challenges, `"ip"`-typed identifiers, per-identifier challenge-type
  dispatch), `.local` (mDNS) challenge validation with a distinguishable
  `mdnsResolverUnavailable` error when unavailable
- [x] **Fingerprint-pinned bootstrap** — `ca_bootstrap_client::fetch_trusted_root()`
  establishes first-contact TLS trust from an out-of-band SHA-256 root
  fingerprint + bearer token, before any certificate chain exists to verify
  against
- [x] **`ca_cluster_node` RPC mTLS** — secures the Raft-internal RPC channel
  between `ca_cluster_node` peers (previously plain TCP via
  `tcp_rpc_client`/`tcp_rpc_server`, itself untouched) with mutual TLS via
  a new sibling transport (`include/raft/tls_tcp_rpc.hpp`,
  `tls_tcp_rpc_client`/`tls_tcp_rpc_server`); two-phase bootstrap — a
  static, operator-provisioned shared credential (mirroring the existing
  unseal-passphrase distribution) authenticates peers before the CA root
  exists, then each node self-service-acquires its own CA-issued
  certificate and cuts over automatically once a Raft-replicated
  `rpc_tls_ready` readiness set shows every configured peer has done the
  same; spec at `.kiro/specs/ca-cluster-rpc-mtls/`, 13/13 tasks complete;
  4 real concurrency bugs (persistent client `SSL_CTX*`, server-side
  socket timeouts, accept/present trust-widening ordering, follower
  RPC-forwarding gaps) plus a 5th, CI-only deadlock (leader switching
  identity before any follower had time to widen its own trust policy,
  which broke the read-index heartbeats the follower's own widen step
  needed — closed with a 3-second grace period) found and fixed via
  multi-process and real-CI testing, none of which loopback/2-node-
  in-process testing alone caught; real-AWS validation tracked separately
  — see below
- [x] **`ca_cluster_node` RPC mTLS — real-AWS validation** — extends
  `certificate-authority`'s existing real-EC2 harness
  (`tests/ca_cluster_node_real_ec2_test.cpp`, plain-TCP) to run RPC TLS
  across three real, separate EC2 instances via a new sibling fixture
  (`tests/ca_cluster_node_rpc_tls_real_ec2_test.cpp`): bootstrap-and-cutover
  with the bootstrap credential deleted afterward, staggered node join,
  restart without the bootstrap credential, and a network-isolation
  recovery scenario (subnet-level deny-all NACL swap, reusing the same
  proven technique `aws_quorum_manager_real_ec2_test.cpp` already
  implements, rather than the per-instance security-group reassignment
  originally speced but never actually used anywhere in this codebase) —
  the last of which no loopback test can exercise at all. Directly
  motivated by the CI-only deadlock above: loopback/CI-container testing
  already missed one real race once, so this adds a second,
  environment-specific line of defense on real infrastructure. Also
  generalizes `aws-quorum-manager`'s cost-tracking and signal-driven-cleanup
  apparatus (previously only in `tests/aws_quorum_manager_real_ec2_test.cpp`)
  into a shared header (`tests/aws_real_ec2_test_support.hpp`) so every
  real-EC2 test binary gets both — including
  `ca_cluster_node_real_ec2_test.cpp`, which had neither before and would
  have leaked a VPC and running EC2 instances if killed mid-run; fixed an
  unrelated pre-existing bug found along the way in that same file (its
  `--peers`/curl checks used `https://` against a server the test never
  actually configures with `--tls-cert`/`--tls-key`). Spec complete,
  9/9 tasks implemented (`.kiro/specs/ca-cluster-rpc-mtls-real-aws/`); full
  project builds cleanly and every fixture was confirmed to fail gracefully
  with a clear "skip" message when AWS credentials are absent — same
  compile-verified-only limitation `ca_cluster_node_real_ec2_test.cpp`
  already had (no AWS account available in this environment to actually run
  any of the three real-EC2 binaries).
- [x] **`ca_cluster_node` custom AMI (Packer build pipeline)** — produces a
  golden, secret-free AMI with `ca_cluster_node` and its systemd unit
  pre-installed, giving `aws_ec2_quorum_manager_config.image_id` and
  `KYTHIRA_EC2_TEST_AMI` a real, scripted producer instead of a manually
  hand-built AMI; `packer/ca_cluster_node/` (template + `extract-binary.sh`/
  `provision.sh`/`build.sh`), a static-checks CI job, and an on-demand
  `ami-build` CI bundle; spec at `.kiro/specs/ca-cluster-node-ami/`, all 8
  tasks complete (statically verified — `packer fmt`/`init`/`validate
  -syntax-only`/`shellcheck` all run and pass locally; see the spec's
  tasks.md status note for exactly what still needs a container daemon or
  AWS credentials to exercise)

### Cloud Provider Support

**Requirement (applies to every entry below, including AWS):** each cloud
provider's support SHALL ship with at least one example configuration file
(e.g. a `.env.example`, sample YAML/JSON config, or documented CLI-flag
set) and accompanying documentation showing how to configure and run it —
mirroring the existing `docker/ca_cluster_node/ca_cluster_node.env.example`/
`docker/ca_service/ca_service.env.example` convention. **All three shipped
providers — AWS, Azure and GCP — are currently missing it**; those two files
are still the only `.env.example`s in the tree. Tracked here as an
outstanding documentation gap rather than three separate checklist entries,
since the underlying features are implemented and this is
example/documentation work, not a missing capability. It is the single
largest unmet *stated requirement* in this document, and grows by one every
time a provider lands, so it is worth clearing before OCI or Alibaba start.

- [x] **AWS** — `aws_ec2/asg_quorum_manager` (node ID = EC2 instance ID hex,
  `DescribeInstanceStatus` liveness, consistency poll) and
  `aws_acm_pca_provider` (`certificate_provider` backed by AWS Certificate
  Manager Private CA); `aws-sdk-cpp` features: `acm-pca`, `autoscaling`,
  `ec2`, `iam`, `s3`, `sts`
- [x] **Microsoft Azure** — `azure_vm_quorum_manager` (direct ARM
  `Microsoft.Compute/virtualMachines` PUT/DELETE, tag-scan `next_node_id()`
  since ARM requires the caller to choose the VM name up front,
  `instanceView` power state for liveness) and `azure_vmss_quorum_manager`
  (VMSS `sku.capacity` PATCH, production-grade, rejects
  `upgradePolicy.mode=Automatic` scale sets) and `azure_key_vault_ca_provider`
  (`certificate_provider` backed by Azure Key Vault Keys — CSR
  parsing/TBSCertificate assembly happen locally, only the final signature
  comes from Key Vault's `Sign` operation via a custom OpenSSL `RSA_METHOD`);
  `azure-core-cpp`/`azure-identity-cpp`/`azure-security-keyvault-keys-cpp`.
  Like AWS, does not yet have the example-config-file
  documented above — tracked as the same kind of outstanding documentation
  gap, not a missing capability. The CA provider's local signing-assembly
  path currently only covers `rs256` (RSA PKCS#1v1.5/SHA-256); `rs384`/
  `rs512`/`ps256`/`es256`/`es384` are forwarded to Key Vault correctly but
  not yet supported end-to-end (see `azure_key_vault_ca_provider.hpp`'s
  `azure_key_vault_signing_algorithm` doc comment). This spec's own
  real-Azure integration test file
  (`tests/azure_quorum_manager_real_test.cpp`) now implements its design
  doc's full test list (10 VM + 5 VMSS cases) and compiles/skip-paths
  cleanly, but none of it has run against a live Azure subscription yet —
  treat the real assertion logic as unverified until that first run happens.
  **Update, August 4, 2026**: those cases now launch through a spot-first SKU
  escalation ladder (`tests/azure_real_test_support.hpp`), mirroring
  `aws_quorum_manager_real_ec2_test.cpp`'s `spot_first_launch_options()` and
  staying, like it, entirely in the test layer — `azure_vm_quorum_manager` is
  untouched. Live retail prices are joined against `Microsoft.Compute/skus`
  availability, since ranking on price alone puts SKUs this subscription
  cannot purchase at the top of the ladder. Two filters the original design
  didn't anticipate turned out to be load-bearing and were found by probing
  live eastus data rather than by inspection: **CPU architecture** (the
  cheapest SKUs in the region are Ampere/Arm64 and undercut every x64 spot
  price, but cannot boot the suite's x86-64 image) and **Hypervisor
  generation** (every cheap modern family is Gen2-only, so the fixture's
  image moved from `22_04-lts` to `22_04-lts-gen2`; that mismatch is a hard
  `BadRequest`, which escalation deliberately does *not* retry). The image
  change also un-breaks CI's own `AZURE_TEST_VM_SIZE=Standard_D2s_v7`, which
  is likewise Gen2-only and could not have booted the Gen1 image. The
  `Standard_D2s_v5` default that appeared ~10 times in this file is gone; it
  is `NotAvailableForSubscription` here (confirmed against live ARM) and so
  could never have launched. Escalation's own logic — parsing, ranking, the
  capacity/quota-vs-fatal classifier, and the walk — is verified offline and
  deterministically by `tests/azure_spot_escalation_test.cpp` (19 cases), the
  only way to exercise a path that a healthy live run never reaches; the
  live fetch/rank path was separately confirmed against the real
  subscription (42-rung ladder, cheapest spot `Standard_F1als_v7` at
  $0.01118/hr vs $0.0605/hr on-demand).
  **The whole VM suite has now run green against live Azure** — the first
  time any case in this file has executed its real assertion logic, closing
  the "treat as unverified" caveat above for the 8 VM cases (the 2 needing a
  pre-provisioned PPG/Availability Set still skip, and the VMSS suite still
  needs `AZURE_TEST_VMSS_NAME`). Escalation was verified *under the failure
  condition*: with `AZURE_TEST_FORCE_FIRST_VM_SIZE=Standard_D2s_v5` the run
  advanced on a real `SkuNotAvailable` and still provisioned, while the
  control run (variable unset) recorded zero escalations — so the
  precondition is proven to have fired. Total suite cost: $0.036.
  Getting there surfaced **four pre-existing bugs**, each fixed in its own
  commit and none related to escalation: (1) `next_node_id()`'s ARM `$filter`
  used the `tagName`/`tagValue` form, which is valid only on the generic
  `/resources` endpoint — and which ARM rejects *only once the resource group
  contains a VM*, so provisioning worked for a cluster's first node and 400'd
  for every node after, meaning `azure_vm_quorum_manager` could never have
  built a multi-node cluster (this supersedes `870cfc0`'s encoding
  diagnosis: the request 400s identically whether minimally or fully
  percent-encoded); (2) no `osDisk` was sent on create, so ARM defaulted
  `deleteOption` to `Detach` and every VM ever provisioned orphaned a
  permanently-billing managed disk; (3) the fixture set neither
  `ssh_public_key` nor an `adminPassword`, so ARM refused every VM outright;
  (4) `provision_and_assess_single_zone` asserted `healthy` on a 1-node
  topology, which `classify_status()` makes unreachable (`live == majority`
  → `critical`). Two ladder refinements also came out of the live run: a
  `LowPriorityCores` refusal now jumps straight to the on-demand rung
  (region-wide spot quota dooms every spot rung identically — observed as 40
  consecutive identical refusals), and confidential-compute SKUs are excluded
  since they reject any create lacking `securityProfile.securityType` with a
  fatal `BadRequest` that aborts the walk.
  Two further fixes closed the loop. `provision_timeout_cleanup` leaked the
  VM it deliberately times out on, every run: the manager *does* call
  `best_effort_delete_vm` on that path, but with a 1-second timeout the VM is
  still mid-create, ARM refuses to delete a resource with an operation in
  flight, and the failure is logged and swallowed by design — so the NIC
  delete then failed too, the NIC still being attached. Fixed in
  `AzureIntegrationFixture::teardown()` rather than in the manager, since the
  manager's delete is necessarily racing an ARM operation it does not control
  while the fixture runs after the test body, when the create has settled;
  the sweep matches the fixture's own `kythira:cluster` tag (unique per test
  case), retries past 409s, and logs every VM it deletes so a future leak
  stays visible instead of being silently absorbed. Because `teardown()` is
  also what the signal handler calls, an interrupted run now cleans up too,
  which it never did before.
  Separately, `external_arm_post_action` treated ARM's `202 Accepted` as
  completion. Deallocation is asynchronous, so callers got back a
  still-running VM and then immediately asserted on `assess_quorum`'s live
  count — racing ARM rather than testing the manager, and *usually winning*,
  which is worse than always losing: two consecutive full-suite runs with no
  code change between them came out green and then failed with `1 != 0` and
  `6 != 5`. It now polls instanceView until the VM leaves
  `PowerState/running`. The earlier green run recorded above was therefore
  weaker evidence than it looked; the suite has since been confirmed green on
  two consecutive full runs, each ending with the resource group holding
  nothing but the permanent VNet/NSG/Key Vault.
- [x] **Google Cloud Platform (GCP)** — `gcp_compute_quorum_manager` (direct
  GCE `instances.insert`/`list`/`delete`, node ID = GCE instance ID,
  `instances.get` status for liveness) and `gcp_mig_quorum_manager` (Managed
  Instance Group, production-grade) and `gcp_privateca_certificate_provider`
  (`certificate_provider` backed by Certificate Authority Service);
  `google-cloud-cpp` with the `compute` and `privateca` components, gated
  behind the independent `KYTHIRA_HAS_GCP_SDK`/`KYTHIRA_HAS_GCP_PRIVATECA`.
  Spec at `.kiro/specs/gcp-cloud-services/`, 13/13. **Verified against live
  GCP**, not just compile-verified: `gcp_quorum_manager_real_gce_test` passes
  11/11 and `gcp_privateca_provider_real_test` passes while provisioning and
  tearing down its own CA pool, using Workload Identity Federation
  credentials provisioned by `scripts/ci-cloud-credentials/gcp/`, with a
  post-run audit confirming no leaked instances, disks, MIGs, or CA pools.
  That first live run is what surfaced three real defects (a bare network
  short name rejected by `instances.insert`, the CAS suite silently skipping
  while CTest reported the skip as a pass, and `provision_timeout_cleanup`
  never timing out while leaking its instance). Like AWS and Azure, still
  missing the example config file documented at the top of this section.
- [ ] **Oracle Cloud Infrastructure (OCI)** — quorum manager backed by an
  Instance Pool and a `certificate_provider` backed by OCI Certificates
  Service
- [ ] **Alibaba Cloud** — quorum manager backed by an Auto Scaling Group and
  a `certificate_provider` backed by Alibaba Cloud SSL Certificates Service

### RPC Serializer Implementations

`kythira::rpc_serializer` (`include/raft/types.hpp`) is a compile-time
concept — `serializer_type` is a template parameter on `raft_types`/
`tcp_raft_types` (default `json_rpc_serializer`,
`include/raft/json_serializer.hpp`), so adding a concrete alternative needs
no change to that seam, only a new type satisfying the concept plus
`serialize`/`deserialize` overloads for every RPC message type (RequestVote,
RequestPreVote, AppendEntries, InstallSnapshot, ClusterJoin, and their
responses — see `tests/rpc_serializer_concept_test.cpp` for the exact
surface a conforming implementation must cover).

- [x] **Protocol Buffers** — `.proto`-defined messages for the existing RPC
  request/response types, compiled via `protoc`; a schema-driven alternative
  to today's hand-rolled `boost::json` construction, with generated
  accessors instead of manual field-by-field (de)serialization. Implemented
  as `protobuf_rpc_serializer<Data>` (`include/raft/protobuf_serializer.hpp`);
  spec at `.kiro/specs/protobuf-rpc-serializer/`, 44/44. Five test binaries
  (`protobuf_rpc_serializer_concept_test`, `..._serialization_property_test`,
  `..._malformed_message_property_test`, `protobuf_rpc_integration_test`,
  `protobuf_json_benchmark_test`) run in CI; wire-size/throughput numbers in
  `doc/protobuf_serializer_performance_comparison.md`. Deliberately
  independent of the gRPC transport below — distinct `.proto` packages, no
  gRPC dependency
- [x] **gRPC / protobuf-binary** — layers gRPC's binary wire format (protobuf
  payloads over HTTP/2) on top of the Protocol Buffers message definitions
  above; needs its own `network_client_type`/`network_server_type`
  transport pairing (gRPC owns framing/HTTP/2 itself, unlike the
  serializer-only JSON path used over `tcp_rpc`/`tls_tcp_rpc`), not just a
  new `serializer_type`. Implemented as `grpc_client`/`grpc_server`
  (`include/raft/grpc_transport{,_impl}.hpp`, `src/grpc_transport_impl.cpp`)
  over `proto/raft.proto`, with TLS/mTLS, the callback API, metrics and the
  optional network-concept extensions; `GRPC_TRANSPORT` Kconfig symbol,
  gracefully degrading when gRPC/Protobuf are absent. Spec at
  `.kiro/specs/grpc-transport/`; see the "Partially Implemented" table above
  for the narrow remainder of Task 13 (strict-mode/graceful-degradation
  configure checks and a performance sanity pass) — the transport itself and
  its three test binaries are CI-verified. Docs in
  `doc/grpc_transport_README.md`
- [x] **CBOR (RFC 8949)** — compact binary JSON-equivalent encoding; same
  logical structure as `json_rpc_serializer`'s `boost::json::object` field
  layout, smaller wire size and no text-parsing overhead, likely the
  lowest-effort binary alternative to implement first. Implemented as
  `cbor_rpc_serializer<Data>` / `cbor_serializer` (`raft/cbor_serializer.hpp`):
  hand-rolled RFC 8949 codec (definite-length uint/byte-string/text-string/
  array/map/bool subset), byte fields carried as CBOR byte strings (no base64),
  absent optionals omitted, `name()` → `"cbor"` for CoAP `application/cbor`;
  no `vcpkg.json`/`Kconfig` change
- [x] **Amazon Ion** — Amazon's self-describing binary/text data format
  (binary encoding for wire efficiency, with the text encoding available for
  debugging/logging the same messages). Implemented as
  `ion_rpc_serializer<Data>` (`include/raft/ion_serializer.hpp`) over an
  `ion-c` vcpkg overlay port (`vcpkg-overlays/ion-c`), behind the **opt-in**
  `ion` vcpkg feature and `ION_SERIALIZER` Kconfig symbol; binary and text
  encodings with encoding-agnostic deserialize, `application/ion` CoAP
  Content-Format (65000) and HTTP `Content-Type`. Spec at
  `.kiro/specs/ion-rpc-serializer/`, 45/45. All 6 `ion_*` CTest binaries pass
  against the **real** `ion-c` library — a first, which surfaced four genuine
  bugs, three of them upstream in `ion-c` itself (notably its `ASSERT()`
  macro spinning forever under `-DNDEBUG` on malformed input, fixed by an
  overlay patch). Because the `ion` feature is opt-in, these binaries are
  **not** part of the default CI build's 403 — enabling them needs
  `vcpkg install --x-feature=ion`

### Metrics Backends

Most entries below are `kythira::metrics`-concept implementations
(`include/raft/metrics.hpp`) — today only satisfied by `noop_metrics`, a
zero-cost stub. `metrics_type` is a compile-time template parameter on
`raft_types`/`tcp_raft_types` (default `noop_metrics`), so adding a concrete
backend needs no change to that seam, only a new type satisfying the
concept (`set_metric_name`/`add_dimension`/`add_one`/`add_count`/
`add_duration`/`add_value`/`emit`, all non-blocking — I/O deferred to a
background emitter).

**Cloud-vendor monitoring services (AWS CloudWatch, Azure Monitor, GCP
Cloud Monitoring, OCI Monitoring, Alibaba Cloud CloudMonitor) are
intentionally out of scope for a bespoke `kythira::metrics` implementation
each.** The intention for these five is to provide example monitoring
*configuration* — e.g. an OpenTelemetry Collector exporter config for that
vendor, or the vendor's own native agent config — routing whatever
telemetry Kythira already emits through a shared exporter to that vendor's
ingestion API, plus documentation, rather than a full custom SDK-based
`kythira::metrics` type per vendor. Writing and maintaining five separate
vendor-SDK integrations inside Kythira itself would duplicate integration
work already done well by an OpenTelemetry Collector (or the vendor's own
agent), and would tie Kythira's own dependency footprint to every vendor
SDK it wants to support. The self-hosted agents below (Prometheus,
Telegraf, VictoriaMetrics, NetData) remain full `kythira::metrics`
implementations — they have no equivalent "someone else already wrote the
integration" story, so Kythira has to speak their wire protocol directly.

**Requirement (applies to every entry below):** each metrics/logging agent
integration SHALL ship with at least one example configuration file (e.g.
an agent config snippet, scrape config, or `.env.example`) and accompanying
documentation showing how to point it at a real backend — same convention
as the Cloud Provider Support requirement above.

**Testing requirement (applies to every entry below):**

- A test that sends real data to a **self-provisioned** instance of that
  agent/aggregator via Docker (a real Prometheus/OTel Collector/Telegraf+
  backend/NetData container, or a local emulator such as LocalStack for a
  vendor API that has one, mirroring the existing
  `aws_quorum_manager_localstack_test.cpp` pattern) SHALL be added, mirroring
  the existing `docker_chaos` scenario-test convention (e.g.
  `docker-dns-sd-discovery-tests`, `docker-poco-discovery-tests`) and
  following `CLAUDE.md`'s container-runtime-compatibility rules. This test
  SHALL be **enabled by default** — it runs in every environment with a
  container runtime available, the same as every other `docker_chaos`
  scenario test, including on GitHub-hosted CI runners (which are
  themselves cloud-hosted infrastructure, but that is incidental — this
  requirement does not itself call for provisioning any separate, billable
  cloud resource). For a cloud-vendor entry with no self-hostable emulator
  for its specific monitoring API, this tier MAY instead validate just the
  example config's syntax/schema (e.g. `otelcol validate` or equivalent)
  rather than a full data round-trip, since there is nothing to emulate
  the vendor's ingestion endpoint against locally.
- A second test exercising the **actual vendor-managed service** SHALL be
  added, following the existing `.kiro/specs/ci-real-cloud-tests/` toggle
  pattern already used by
  `aws_quorum_manager_real_ec2_test.cpp`/`ca_cluster_node_real_ec2_test.cpp`.
  For the five cloud-vendor monitoring entries (config-only per the
  section-level note above), this test stands up the routing mechanism
  described by the example config — e.g. a real OpenTelemetry Collector
  configured per that example, or the vendor's own agent configured per
  it — against the real service, and confirms a known metric arrives; it
  is not a direct SDK call from Kythira's own code, since none exists for
  these five. Self-hosted-only agents (Prometheus/Telegraf/NetData) have no
  vendor-managed counterpart and so need only the Docker-based test above.
  This test SHALL be **disabled by default** — real credentials and real
  cost are required, so it only runs when explicitly opted into, exactly
  like every other real-cloud test in this project.

- [x] **OTLP (OpenTelemetry Protocol)** — `otlp_metrics`
  (`include/raft/otlp_metrics.hpp`) and `otlp_logger`
  (`include/raft/otlp_logger.hpp`), covering both metrics and logging (this
  section's Requirement/Testing-requirement language above applies to
  logging integrations too, not metrics alone); OTLP/HTTP JSON only, no
  gRPC/protobuf-binary; shared non-blocking batching exporter
  (`include/raft/otlp_exporter.hpp`); wired into `chaos_node` as opt-in via
  `OTLP_ENDPOINT`; spec at `.kiro/specs/otlp-telemetry-backend/`. Because
  OTLP is vendor-neutral, a suitably configured OpenTelemetry Collector
  reaches many of the vendor-specific backends still listed below
  (CloudWatch, Prometheus, etc.) indirectly — those entries are not
  themselves considered done by this one; they remain useful as direct,
  Collector-free integrations.
- [ ] **AWS CloudWatch** — example monitoring configuration (e.g. an
  OpenTelemetry Collector `awscloudwatch`/`awsemf` exporter config, or the
  CloudWatch agent's own config) routing Kythira's exported telemetry to
  CloudWatch, plus documentation — not a bespoke `kythira::metrics`
  implementation (see the section-level note above); natural pairing with
  `aws_ec2_quorum_manager`/`aws_asg_quorum_manager`, already implemented
  (Cloud Provider Support, above)
- [ ] **Azure Monitor** — example monitoring configuration (e.g. an
  OpenTelemetry Collector `azuremonitor` exporter config) routing to Azure
  Monitor's custom-metrics API, plus documentation — not a bespoke
  `kythira::metrics` implementation; pairs with the Azure quorum
  manager/certificate provider entry above
- [ ] **GCP Cloud Monitoring** — example monitoring configuration (e.g. an
  OpenTelemetry Collector `googlecloud` exporter config) routing to Cloud
  Monitoring's `timeSeries.create` API, plus documentation — not a bespoke
  `kythira::metrics` implementation; pairs with the GCP quorum
  manager/certificate provider entry above
- [ ] **OCI Monitoring** — example monitoring configuration routing to OCI
  Monitoring's `PostMetricData` API, plus documentation — not a bespoke
  `kythira::metrics` implementation; pairs with the OCI quorum
  manager/certificate provider entry above
- [ ] **Alibaba Cloud CloudMonitor** — example monitoring configuration
  routing to CloudMonitor's custom-metrics API, plus documentation — not a
  bespoke `kythira::metrics` implementation; pairs with the Alibaba Cloud
  quorum manager/certificate provider entry above
- [ ] **Prometheus** — exposes an HTTP `/metrics` scrape endpoint (text
  exposition format); the dominant pull-based backend for Kubernetes/
  container deployments
- [ ] **Telegraf** — emits to a local Telegraf agent (StatsD or InfluxDB
  line protocol over UDP/TCP), letting operators fan metrics out to
  whatever Telegraf output plugin they already run (InfluxDB, Graphite,
  Kafka, etc.) without Kythira needing to pick one
- [ ] **VictoriaMetrics** — Prometheus-remote-write-compatible, so likely
  shares most of the Prometheus implementation's wire format rather than
  needing a wholly separate one
- [ ] **NetData** — via NetData's StatsD-compatible collector or its own
  REST API, for operators already running NetData for host-level
  monitoring who want Kythira's Raft/RPC metrics in the same dashboard

### Minor Enhancements

- [x] **State machine examples** — counter, register, replicated log, and
  distributed lock examples for documentation/demonstration purposes
  (all four now have test targets)
- [x] **libfiu integration** — fault injection chaos testing; spec at
  `.kiro/specs/libfiu-integration/`; 21 tasks complete: CMake detection,
  `fiu_do_on` fault points in persistence/network/state machine, RAII fault
  profiles, safety assertion helpers, smoke + profile + 8 safety/liveness tests
- [x] **Docker chaos testing** — real multi-node `chaos_node` cluster; TCP RPC +
  file persistence + HTTP control plane + libfiu TCP remote; Docker packaging;
  C++ harness (`harness.hpp`, `os_faults.hpp`, `fault_control.hpp`) with
  injectable `CmdExecutor`/`HttpGet`/`HttpPost` stubs for unit testing;
  32 harness unit tests (`docker_chaos_harness_unit_tests`,
  `docker_chaos_fault_control_unit_tests`) registered in CTest; 7 chaos scenario
  tests (election recovery, crash recovery, network degradation, AZ partition,
  persistence faults, safety assertions, leadership stability) + 3 DNS discovery
  scenario tests + 3 DNS-SD discovery scenario tests + 3 poco_peer_discovery
  scenario tests; Podman runtime support and rootless Podman compatibility;
  25 original tasks + 5 expansion tasks complete;
  spec at `.kiro/specs/docker-chaos/`
- [x] **`docker/chaos_node/Dockerfile` couldn't actually build `chaos_node`**
  — fixed via `.kiro/specs/chaos-node-host-build/`: `chaos_node` is now
  built once on the host (the project's real, vcpkg-based CMake
  configuration, same as `ci.yml`) and `docker/chaos_node/Dockerfile`
  packages the already-built binary into a single runtime-only stage —
  no in-container compiler/CMake/folly-apt-install attempt left to
  fail. Confirmed on real arm64 hardware: `docker-chaos-image` now
  builds and tags `kythira-chaos-node:dev` successfully. Also unblocks
  `docker-otlp-collector-tests`, which reuses this image.
- [x] **`poco_peer_discovery_unit_test` / `poco_discovery_node` couldn't
  actually build** — `poco_peer_discovery_unit_test` showed as
  `***Not Run` in `ctest` rather than a real pass/fail, which turned
  out to be masking two stacked bugs. First, its object file and
  executable were stale 0-byte artifacts left by an interrupted build
  days earlier, silently never rebuilt since (Ninja's mtime tracking
  never noticed). Forcing a genuinely clean rebuild surfaced the real
  compile error: a malformed computed `#include` —
  `boost/mpl/aux_/preprocessed/(gcc/or.hpp)`, literal parentheses
  included. Root cause: `POCO_DNSSD_INCLUDE_DIRS` pointed at the
  *source tree's* `vcpkg_installed/.../include` (where Poco/DNSSD's
  manually-built headers actually live, since DNSSD isn't a real vcpkg
  port), which also contains a full, independently-extracted second
  copy of every other vcpkg package — including an older, unpatched
  `boost/mpl`/`boost/config` whose parenthesized computed-include
  macros (`#define BOOST_SLIST_HEADER (<ext/slist>)` etc.) don't
  survive `BOOST_PP_STRINGIZE` on Clang. The build tree's own
  `vcpkg_installed/` (populated fresh per build) has the patched,
  working version of the same files; exposing the source tree's copy
  via an extra `-I` let the compiler pick up the broken one instead.
  `cmd/poco_discovery_node/main.cpp` — the actual production binary,
  not just this test — had the identical latent gap (never added a
  Poco DNSSD include path at all, per an incorrect comment claiming
  vcpkg's toolchain covered it automatically) and could not compile on
  `main` either, masked the same stale-artifact way. Fixed by pointing
  `POCO_DNSSD_INCLUDE_DIRS` at a build-tree shim directory containing
  only a symlink to the `Poco/DNSSD` subtree specifically, never
  exposing the rest of that directory (boost included) to any
  consumer's search path. Verified: both targets build and link from a
  genuinely clean rebuild; the test's 32 cases pass; full local `ctest`
  (mirroring CI's exact filter) went from 379/380 to a clean 380/380.
  PR #76.
- [x] **PreVote extended to `tls_tcp_rpc_*` and the in-memory
  simulator** — `network_client_with_pre_vote`/
  `network_server_with_pre_vote` (`include/raft/network.hpp`) were
  previously implemented for `tcp_rpc_client`/`tcp_rpc_server` only;
  TLS-backed (`ca_cluster_node`) and simulator-backed clusters fell
  back to the pre-PreVote behavior via `if constexpr`. Extended to
  both: `tls_tcp_rpc.hpp`'s `server_impl` gained a `pv_fn`/
  `register_request_pre_vote_handler()`/dispatch branch mirroring its
  existing RequestVote handling, and the public `tls_tcp_rpc_client`/
  `tls_tcp_rpc_server` handles gained `send_request_pre_vote()`/
  `register_request_pre_vote_handler()` wrappers around
  `client_impl::call()`'s existing generic template (no transport-level
  change needed there). `simulator_network.hpp`'s client/server got the
  same treatment, including the server's try-each-deserializer dispatch
  loop. Confirmed genuinely exercised, not just concept-satisfying:
  `ca_cluster_node_rpc_tls_restart_test`'s log shows real
  `Starting pre-vote round` / `Granting pre-vote` traffic over the mTLS
  transport.

  Extending PreVote to the simulator surfaced one real, previously-latent
  bug: `peer2peer_catch_up_stale_source_safety_property_test.cpp`
  gave nodes 4/5 an artificially long (~10 minute) "dormant"
  `election_timeout` purely to stop them from spontaneously campaigning
  — but `handle_request_pre_vote()`'s leader-stickiness check
  (`raft.hpp`) also keys off that same per-node `_election_timeout`,
  so an effectively-infinite dormant value meant those nodes could
  never grant node 3's later, legitimate pre-vote either, once the
  simulator started actually enforcing PreVote (test previously fell
  back to real elections, which don't have this check). Fixed by
  switching nodes 4/5 to the test's normal fast config — safe because
  neither node ever has its own `check_election_timeout()` called
  anywhere in this file, so the dormant trick was never actually needed
  for its stated purpose here, only harmful once PreVote reached this
  transport. Four other test files share the identical
  `make_dormant_config()` pattern
  (`peer2peer_catch_up_membership_sync_property_test.cpp`,
  `peer2peer_catch_up_partition_reconnect_property_test.cpp`,
  `peer2peer_catch_up_property_test.cpp`,
  `tcp_gossip_transport_catch_up_property_test.cpp`) but *do* drive
  their "dormant" node's timer directly, relying on the timeout gap for
  deterministic race-losing — each verified to pass reliably as-is (3
  runs each) since none currently ask a recently-contacted dormant node
  to grant a pre-vote to a new candidate, but they carry the same
  latent design tension and could need the same fix if a future change
  to any of those scenarios introduces that interaction.

  Verified: full local `ctest` (CI's exact filter,
  `--repeat until-pass:3`) 380/380 passing; the newly-fixed test alone
  run 5× and each of the four still-passing sibling files run 3× each
  to confirm reliability, not just a lucky single pass.
- [x] **`dns_discovery_test`'s `stopped_node_absent_after_deregister` was
  never actually a timing flake** — first suspected as one (found while
  re-verifying the `peer_ids()` `SIGSEGV` fix, see the July 18, 2026
  changelog entry, via a real `arm64-docker-smoke-test.yml`
  `workflow_dispatch` run, ID 29664536952: after `docker stop`-ing
  node1, a fixed 3 s wait then a single `/peers` check on the survivors
  saw 2 peers instead of 1). A poll-with-timeout rewrite (20 s via a new
  `wait_peer_count()` helper, mirroring the file's existing
  `wait_all_healthy()` shape) is a real improvement on its own merits
  regardless, but re-verifying *that specific fix* on real arm64
  hardware showed the exact same symptom even after the full 20 s —
  proof this was never about BIND9 being slow. Diagnostics (peer-ID
  trajectory logging, timing the `docker stop` call itself, dumping
  node1's own container logs, and finally logging the previously
  bare-`catch (...) {}`-swallowed deregistration exception) traced it
  through two wrong turns to the actual bug, all in
  `include/raft/rfc2136_ldns_discovery.hpp`'s `send_update()`:
  - The per-RR `CLASS` field was never explicitly set, defaulting to
    `IN` (correct for an add, which is why registration always worked)
    but making a delete request malformed — `CLASS IN` + empty RDATA +
    `TTL 0` matches none of RFC 2136's three valid update-record
    shapes. BIND9 correctly rejected it with a non-`NOERROR` rcode,
    previously invisible since nothing logged the caught exception.
  - Fixing that to `CLASS ANY` (RFC 2136 §2.5.2 "Delete An RRset") let
    the DELETE succeed, but with the wrong scope: `shared_name` is one
    DNS name shared by all three nodes (a round-robin RRset, one A
    record per node), and an RRset-wide delete wiped out *every*
    node's registration, not just node1's — survivors' peer counts
    went from a stuck 2 to an immediate but still-wrong 0.
  - The actual fix: RFC 2136 §2.5.4 "Delete An RR From An RRset" —
    `CLASS NONE`, `TTL 0`, RDATA set to the exact value being removed
    (previously skipped entirely for deletes, only ever pushed for
    adds). Confirmed on real arm64 hardware (run 29758502129): both
    survivors report the correct peer count *immediately* (`t=0ms`),
    no polling delay needed at all, `*** No errors detected`.
  Also fixed as a permanent (not just diagnostic) improvement:
  `rfc2136_ldns_discovery`'s destructor now logs a caught deregistration
  exception to stderr instead of silently swallowing it — this exact
  class of failure was completely invisible in production, not just in
  this test, before that. `dns_sd_discovery_test.cpp`'s analogous
  `dead_node_absent_after_freshness_expiry` case was checked and
  doesn't share any of this — different mechanism entirely (local TTL/
  freshness-timestamp comparison, no DNS UPDATE involved).
- [x] **`chaos_node` scenario tests: leader re-election after `docker
  kill` can time out** — found while verifying the Dockerfile fix
  above (real `arm64-docker-smoke-test.yml` runs,
  `.kiro/specs/chaos-node-host-build/` Task 5), then chased through a
  chain of four further real bugs, each surfaced only because the
  previous fix let CI get one step further than before:
  - `/command`'s wire format didn't match
    `test_key_value_state_machine::apply()`'s parser; `fiu_rc_tcp`'s
    client used `inet_pton()`, which never resolves the hostname
    `"localhost"` it's actually called with (both fixed first, see
    that spec's Task 5 writeup).
  - `include/raft/tcp_rpc.hpp`'s `connect_to()` genuinely did not
    bound `connect()`'s own blocking time on Linux (`SO_SNDTIMEO`/
    `SO_RCVTIMEO` don't apply to the `connect()` syscall itself) —
    fixed with a non-blocking `connect()` + `poll()` pair that
    actually enforces the configured timeout, mirrored in
    `tls_tcp_rpc.hpp`.
  - `tcp_rpc_client`'s RPC dispatch was synchronous and sequential
    (one call blocked the next) — fixed to dispatch via a private
    `folly::CPUThreadPoolExecutor`.
  - Fixing the connection timeout let a real Raft protocol gap
    surface: a stale, partitioned-off node rejoining with an
    ever-climbing term forced the live-majority leader to step down
    repeatedly (the "disruptive server" problem, Ongaro's
    dissertation §9.6) — fixed by implementing the full PreVote
    extension (`types.hpp`/`network.hpp`/`json_serializer.hpp`/
    `tcp_rpc.hpp`/`raft.hpp`), gated as a strictly optional
    network-concept extension so other transports are unaffected.
  - PreVote's own verification then surfaced one more liveness bug: a
    newly-elected leader got stuck at its inherited `commit_index`
    forever, since `advance_commit_index()` correctly refuses to
    commit an entry directly unless it's from the leader's own
    current term (Raft §5.4.2) and a leader that appends nothing of
    its own never satisfies that check — fixed by having
    `become_leader()` append a no-op barrier entry
    (`entry_type::no_op`) in its new term.
  - **Result**: `workflow_dispatch` run 29693678147 shows all 7
    `docker_chaos` scenario-test binaries — including the 3 previously
    unreachable ones (`az_partition_test`, `persistence_faults_test`,
    `safety_assertions_test`) plus `network_degradation_test` and
    `leader_crash_and_reelection` itself — passing cleanly on real
    arm64 hardware.
- [ ] **Memory usage profiling** — optional optimization pass

---

## Historical Notes

Full task-by-task implementation history is preserved in the spec files under
`.kiro/specs/`. Per-component status details are in `doc/RAFT_IMPLEMENTATION_STATUS.md`,
`doc/RAFT_TESTS_FINAL_STATUS.md`, and `doc/PERFORMANCE_VALIDATION.md`. A dated log of
what changed and why is kept in [CHANGELOG.md](CHANGELOG.md).

# Changelog

Chronological log of notable changes to Kythira, newest first. For the
current list of outstanding work, see [TODO.md](TODO.md).

### What Changed (August 13, 2026)

- **The five cloud-vendor monitoring entries (AWS CloudWatch, Azure Monitor, GCP Cloud Monitoring, OCI Monitoring, Alibaba CloudMonitor) delivered as the config-only integrations the TODO prescribed** — example configs under `docker/cloud-monitoring/`, operator docs in `doc/cloud_vendor_monitoring.md`, and both testing tiers wired. No new `kythira::metrics` code: the node's existing OTLP (or, for OCI, Prometheus) emission is routed by an OpenTelemetry Collector or the vendor's own agent.
  - **Routing choices, each pinned to a verified fact**: CloudWatch = `awsemf` + `awscloudwatchlogs` exporters; Azure = `azuremonitor` (metrics + logs into Application Insights); GCP = `googlecloud`; Alibaba = `prometheusremotewrite` into CloudMonitor 2.0's Prometheus ingestion **because the entry's original target, the legacy custom-metrics upload API, was deprecated by Alibaba in September 2024** — the TODO entry now records the substitution; OCI = the vendor's Management Agent PrometheusEmitter scraping `PROMETHEUS_METRICS_PORT`, **because collector-contrib (v0.116) has no OCI Monitoring exporter at all**. OCI/Alibaba logging documented out-of-scope (no vendor log-ingestion path via these mechanisms) — same doc-only reasoning as the NetData logging leg.
  - **Docker tier follows the requirement's emulator split**: CloudWatch is the one vendor with a self-hostable emulator of its ingestion API, so `docker-cloudwatch-metrics-tests` round-trips a real chaos_node → Collector running the **unmodified example config** → LocalStack, asserting through the CloudWatch Logs API (`exec awslocal` — EMF envelope + known counter present); the other four get `docker-cloud-monitoring-config-tests`, which runs the Collector binary's own `validate` against each config (dummy env supplied; schema-level errors fail) and required-key checks for the OCI `.properties`. Both steps added to the arm64 smoke workflow, unmasked.
  - **Real-cloud tier: five lightweight `<provider>-monitoring` jobs** in real-cloud-tests.yml (no C++ build — a host Collector binary, one synthetic OTLP probe, the vendor's own query API as the oracle; `scripts/real-cloud-monitoring/`), each disabled by default behind `REAL_CLOUD_TESTS_<PROVIDER>_MONITORING_ENABLED` + the master switch, each failing closed by naming its missing variables. AWS adds a `cloudwatch-monitoring` IAM bundle scoped to the `/kythira/chaos-node/*` log groups. Honest status recorded in every doc that touches them: AWS/Azure/GCP are runnable once their documented monitoring-specific values are set; **OCI (Management Agent install key/installer URL) and Alibaba (no account exists) are wired but have never run against the live services**. — three `kythira::diagnostic_logger` implementations, paired with their ecosystems and verified against real agents** (the same-day follow-up the TODO item prescribed; before this only OTLP had a logging half).
  - **`loki_logger`** (pairs with Prometheus, `LOKI_ENDPOINT`): Loki push API via `otlp_http_batch_exporter` reused wholesale — same queue, retry, and injectable poster; one stream per severity labelled {job, instance, level}; structured pairs as logfmt in the line.
  - **`victorialogs_logger`** (pairs with VictoriaMetrics, `VICTORIALOGS_ENDPOINT`): ND-JSON to `/insert/jsonline` on the shared `metrics_line_exporter` with an HTTP sender; structured pairs become first-class LogsQL fields. One measured correction en route: VictoriaLogs v1.0.0 rejects bare nanosecond `_time` integers (parses them as milliseconds — its container log said so verbatim in the first verification dispatch), so `_time` is RFC3339-with-nanos, and the header comment plus unit test now encode the measured behavior, not the assumption.
  - **`telegraf_logger`** (pairs with the Telegraf metrics leg, `TELEGRAF_LOGS=on`): each log call is one line-protocol record (`kythira_log`, `level` tag, `msg` + structured pairs as string fields) on the SAME socket_listener the metrics ride — zero new agent config, fan-out inheritance applied to logs.
  - **Pairing enforced at startup**: an unpaired logger env var is rejected with a message naming the required metrics variable — three logger-bearing bundles exist, not a logger×metrics matrix.
  - **NetData deliberately gets documentation, not code**: it has no app-facing log ingestion API (its log story consumes host journals via systemd-journal), so `doc/netdata_metrics_backend.md` documents the journald pairing and why a container-tier test would have to fake exactly the part that matters.
  - Verified: 14 unit cases across three binaries, and the three scenario tests each gained a logs case asserting through the agent's own query API/output — all green in smoke run 31676479371 (a fully green dispatch). The second dispatch's failure was the test's own sampling assumption (startup lines crowded out of a `limit=100` stream sample by heartbeat-debug spam) — fixed by querying for the content directly, which is also the better example of how to assert against a log store.
- **Four self-hosted metrics backends — Prometheus, VictoriaMetrics, Telegraf, NetData — implemented, unit-tested, and verified end-to-end against real agent containers**, closing four of doc/TODO.md's "Metrics Backends" entries (the four that need only the Docker test tier — they have no vendor-managed counterpart). All four are `kythira::metrics`-conforming **copyable handles over shared state**, deliberately not `otlp_metrics`' move-only shape: the HTTP transports copy the handle per emission (`auto metric = _metrics;`), so a move-only backend silently cannot be used with them — `tests/recording_metrics.hpp` had already documented the intended idiom, and these are the first production backends to follow it.
  - **Prometheus** (`include/raft/prometheus_metrics.hpp`): pull-based — a shared `prometheus_registry` (counters with `_total` suffixing, gauges, `le`-bucketed histograms; sorted labels so `add_dimension` order can't split a series), a text-exposition renderer, and a `prometheus_scrape_server` on httplib. No I/O on the recording path at all.
  - **VictoriaMetrics** (`victoriametrics_metrics.hpp`): the TODO entry's "likely shares most of the Prometheus implementation" prediction held exactly — same registry, same renderer, plus a push loop POSTing the cumulative exposition to `/api/v1/import/prometheus`. Cumulative push means a failed push loses resolution, not data; failures are counted, never retried. `job`/`instance` identity travels as constant labels.
  - **Telegraf** (`telegraf_metrics.hpp`): InfluxDB line protocol (dimensions as first-class tags) over UDP/TCP via a new shared `metrics_line_exporter` (bounded queue, background sender, drop-oldest, `dropped_line_count()`) — whose unit tests caught a real bug before it ever shipped: failed sends counted zero dropped lines because the payload's line count was never incremented.
  - **NetData** (`netdata_metrics.hpp`): StatsD over UDP with DataDog-style tags, on the same line exporter. Documented honestly: NetData aggregates per metric NAME, tags surface as chart labels, not per-dimension series.
  - `chaos_node` gains one env-var opt-in per backend (`PROMETHEUS_METRICS_PORT`, `VICTORIAMETRICS_ENDPOINT`, `TELEGRAF_ENDPOINT`, `NETDATA_STATSD_ENDPOINT`), checked in fixed precedence after `OTLP_ENDPOINT`. Operator docs in `doc/<backend>_metrics_backend.md`; example agent configs under `docker/`.
  - **Verified per the section's own testing requirement**: 34 unit-test cases across five binaries, plus four `docker_chaos`-convention scenario tests (`docker-<backend>-metrics-tests`) that each assert through the *agent's own API or output* — Prometheus and VM answer queries for the sample, Telegraf's file output re-serializes what it parsed, NetData's REST API exposes the chart — all four green in one arm64 smoke-workflow dispatch (run 31652592215, August 13 UTC). Getting there took five instrumented dispatches and surfaced four real environment defects the tests now document in place: the chaos_node entrypoint's iptables setup dies without `NET_ADMIN` (the new compose files now grant it, like docker-compose.yml always has), the telegraf image drops root regardless of `user:`, NetData's statsd listener binds localhost only, and the debugging pattern that cracked all of them was making the fixture dump `compose ps` + every container's logs + the agent-side evidence on ANY failure — a CI scenario failure with no agent logs is exactly the "silent machinery" this changelog keeps writing about.
  - **A pre-existing defect found by the same sweep: the OTLP Collector scenario test has never verifiably passed on the arm64 smoke workflow.** Its step was `continue-on-error: true`, so the workflow reported it green while (a) its compose file lacked the `NET_ADMIN` its own node container needs — the node died at the entrypoint — and (b) its read-back ran `docker exec … cat` against a distroless collector image that contains no `cat` to exec. Fixed: compose grants the capability, the read-back copies the file out with `docker cp` (no in-container binary needed), the step is unmasked and made independent so one failure can't skip the others. With all three fixes in, run 31653717200 is the OTLP scenario's first verifiable pass here — and the whole smoke workflow's first fully green dispatch, every scenario step included.

### What Changed (August 12, 2026)

- **CoAP client lock starvation — root-caused and fixed the suite's dominant flakiness source, including the 720 s budget exhaustion** (`0040eea`, PR #227). The client's `_io_thread` held `_mutex` around `coap_io_process(ctx, 20)` — a 20 ms *blocking* wait — in a tight loop, owning the lock for ~100% of wall time; `send_rpc()` could only acquire it in the instant between unlock and relock, and a non-fair mutex under a ~100%-duty-cycle holder produces a geometric waiting-time tail. That tail is exactly what the send-path probe (`0a5e489`, which split `send_ms` at the five places a stall could hide) measured: every stalled millisecond was `lock_wait_ms` (min 40 ms / median 19,881 ms / max 372,109 ms on an idle CI runner), with resolve/session/encode/`coap_send` all at 0 in every sample. The fix holds `_mutex` only across `coap_io_process(ctx, COAP_IO_NO_WAIT)` — drain ready I/O without blocking — and paces the loop with a 5 ms sleep *outside* the lock. Stated trade-off: an incoming PDU can now wait up to 5 ms for dispatch where the blocking call woke immediately on socket activity; worst case on the chattiest path (block transfer, ~128 round trips) is ~0.6 s, inside every test budget. Measured before/after with the coap-flake-measure workflow per its own rule — 20 iterations each, same selection, same runner class: baseline had four tests failing at 45/40/25/25% (27 failed test-runs total); after, **0 failures in 20 runs** (a 45% → 0/20 shift is ~6e-6 under binomial noise). The starvation was not just the concurrent-processing stall's cause — it was carrying a broad share of the CoAP suite's flakiness. Earlier `yield()`-after-unlock was the same starvation, only narrowed. Not changed: the DTLS-handshake path's own `coap_io_process(_, 100)` calls (a different, pre-existing wait the probe did not implicate) and the server's pump loop, whose context no other thread touches.
- **Requirement 4.4 closed on real infrastructure: a pool instance running `oci_heartbeat_writer_node` under Instance Principal wrote its own heartbeat tag, and `assess_quorum` classified it live under `heartbeat_timeout=60s`** (`b3a7321`, PR #229, on `d8abc2e`/PR #222's node-side writer) — the first code from this tree ever to run ON an OCI instance, closing `oci_federation.hpp`'s last unexercised caveat. Delivery chain: CI builds the writer → bucket upload + 2 h read-only PAR (URL masked — it is a capability) → the suite writes a `kythira-artifact-url` freeform tag → static cloud-init on the pool's Ubuntu 24.04 image polls the metadata service for the tag, fetches through the service gateway (subnet stays private, no NAT/IGW), launches. Verified end to end twice before merge: a full local suite run against the live tenancy and CI dispatch run 31636107113 (green, post-run leak audit clean, heartbeat 75 s after tagging). Two bring-up defects, both invisible from the mock tier and both found by serial-console exfiltration on a no-SSH subnet (five instrumented boots, ~$0.01 total):
  - **The clang CI binary dynamically linked `libatomic.so.1`, which Canonical cloud images do not ship** — loader death before `main()`. Fixed with an archive-first static link plus explicit `--as-needed` (a link *option* cannot beat `network_simulator`'s INTERFACE `-latomic`, and `-static-libstdc++` collides with folly's ExceptionTracerLib interposition). `ldd` on the g++ build proves nothing about the clang build.
  - **cloud-init 26.1 on Ubuntu 24.04 silently drops a bare (non-MIME) user-data shellscript containing non-ASCII bytes** — valid UTF-8, correct `#!`, and the same version's `UserDataProcessor` classifies it correctly when run in isolation; the byte-identical content executes when MIME-wrapped, and pure-ASCII bare content executes. Established by paired boots after an hour of upstream source reading had pointed the wrong way. The cloud-init script is ASCII-only by hard rule (`LC_ALL=C grep -P '[^\x00-\x7F]'` gates it). Reusable technique from the same debugging: user_data is arbitrary code execution with console output, so a MIME pair (production part + ASCII exfiltrator part — ordered by *filename* lexically, not MIME part order) answers one question per boot at ~2 instance-minutes each.
  - Follow-up hardening the same day — **attempted, measured, and backed out**: narrowing the `kythira-ci-instance-hb` dynamic-group grants to self-only (`where request.principal.id = target.instance.id`) broke an *unrelated* principal — the CI group's `put_object` to the artifacts bucket started failing `BucketNotFound`/404 on two consecutive dispatches while the same UPST's compute calls kept succeeding, and the compartment audit log shows the policy update as the only mutation between green and failing. A condition variable inapplicable to a request *declines* the request rather than merely not matching (documented under "Variables that Aren't Applicable to a Request"), the services enforce this inconsistently (Compute tolerated it, Object Storage failed closed cross-principal), and variable-to-variable comparison is undocumented to begin with. The broad grant stands, with the full account and the re-attempt checklist in `scripts/ci-cloud-credentials/oci/policies/heartbeat.txt` — the sequencing rule (land broad, verify, *then* tighten and re-verify) is what caught this as a policy regression instead of leaving it to surface as a mystery bucket outage in some later run.

### What Changed (August 4, 2026)

- **CoAP/Proxygen test reliability — root-caused a crash that four previous "fixes" had only made rarer, and found one real production bug along the way.** Full record, with every before/after measurement, in [`doc/coap-flake-investigation.md`](coap-flake-investigation.md).
  - **`coap_thread_safety_property_test`'s `memory access violation at address: 0x1ac` — fixed** (`121f5ae`). The address is stable across machines, which is the tell: it is a fixed field offset off a zeroed base, not heap corruption. Boost blames `test_concurrent_configuration_checks`, but that case only calls `is_dtls_enabled()` — `return _config.enable_dtls;` — and cannot fault on its own. The crash belongs to the case *before* it. When a case exceeds its `*boost::unit_test::timeout()`, Boost's SIGALRM handler calls `siglongjmp()` (`boost/test/impl/execution_monitor.ipp:873`) back to a `sigsetjmp()` outside the case and only then throws — and **`siglongjmp` unwinds nothing**, so no destructor in that frame ever runs. The worker threads are neither joined nor detached, just orphaned, still holding a `coap_client` that lives in the abandoned frame; the next case's locals land on those same stack bytes, and an orphaned worker hands libcoap a `_coap_context` read out of the next case's data. gdb named it outright: the faulting thread's frame `#6` is `test_concurrent_rpc_requests::test_method()::$_0`, with a sibling thread blocked on `pthread_mutex_lock(mutex=0x7fffffffc1a0)` — a *stack* address. Fixed by heap-owning the transport and the counters the workers touch and capturing them by value as `shared_ptr`, so an orphaned worker keeps its object alive instead of reaching into dead stack; a timed-out case then costs a bounded leak on an already-failing path. Measured with the rpc case forced to overrun so workers are guaranteed in flight when the alarm fires: **11 memory-access violations in 15 runs before, 0 after**, with SIGALRM firing in 14/15 and 15/15 respectively — the after arm firing in *every* run is the control that proves the fix was exercised under the failure condition rather than bypassed by it.
  - **The same defect in `coap_connection_reuse_property_test` (`:183`, `:242`) and `coap_concurrent_processing_property_test` (`:244`) — fixed** (`1a52e1f`), **10 crashes in 12 runs before, 0 after**. These had been *inferred* to be affected from the capture pattern and are now demonstrated. They used a `[&]` catch-all, which is why an initial grep for `[&client` missed them — a reminder that a capture-list search has to look for the catch-all form too. `grep` for `emplace_back([&`/`[=` across `tests/coap_*.cpp` now returns nothing.
  - **A wrong explanation, corrected in three places.** Both of the above files, and the closed PR #133, recorded that an overrunning case aborts via `~std::thread()`'s `std::terminate()` on a still-joinable thread. That is accurate for a normal exception unwind and simply false for the signal path that actually occurs. It is why every previous response — `92d824b`, `bc39d04`, `9727d38`, `5a9c5ff` — shrank a workload or raised a timeout, each lowering the *probability* of reaching the trap without removing it, and why the crash kept coming back. It also nearly produced a fifth such fix in this session: the first instinct here was to restore PR #133's `joining_thread_group` RAII helper, which would have been completely inert, because destructors are precisely what `siglongjmp` skips. Reading Boost's own source instead of the comments in the tree is what broke the cycle. The comments now say what actually happens; the reduced workloads are kept, but as a runtime choice rather than as the fix.
  - **`coap_client::generate_message_token()` exceeded CoAP's 8-byte token cap — fixed** (`763f43b`). **This one is in production code, not tests.** Tokens were built as `"token_" + std::to_string(counter)`: 7 bytes at counter 0, and **9 bytes from counter 100 onward**, past the 8-byte maximum RFC 7252 §5.3.1 gives the 4-bit Token Length field. Nothing rejected it — `coap_add_token()` accepts the over-long token, and it is `coap_send()` that drops the PDU, logging only `WARN coap_send: PDU dropped as token too long (9 > 8)` while `send_rpc()` goes on returning futures that can never complete. The observable behaviour is a node that **silently stops transmitting after its 100th request**, which in a live cluster reads as a peer going quiet after warm-up with a libcoap warning as the only evidence. It stayed latent because nothing exercised the real libcoap path until `d54bc46` (July 31) wired `LIBCOAP_AVAILABLE` into the non-stub build — before that every `coap_*_test` compiled the branch out and ran the stub, and this test's whole module finished in 87ms doing no I/O at all. Fixed with a fixed-width encoding — eight lowercase hex digits of the counter, exactly `coap_max_token_length` bytes for every possible value — so the failure is removed by construction rather than by a bounds check. Hex rather than raw bytes because the transport logs the token on ~45 paths and uses it as an `_pending_requests` key; truncating the 64-bit counter to 32 bits is deliberate, since tokens only need to be unique among *outstanding* requests. Two supporting changes, because the width was never the whole problem — the cap being exceeded **silently** is what let this survive: `send_rpc()` now throws `coap_transport_error` on an over-long token so any future regression is attributable to the call that caused it, and `coap_max_token_length` replaces three separately hardcoded `8`s and is `static_assert`ed against libcoap's own `COAP_TOKEN_DEFAULT_MAX` wherever libcoap is present. Verified end-to-end against real libcoap over the 200 requests `coap_thread_safety_property_test` issues: **101 dropped PDUs before, 0 after**. 101 is exactly the predicted count — `_token_counter` starts at 1, so `token_1`…`token_99` fit and requests 100 through 200 inclusive, 101 of them, did not — which confirms the mechanism and not merely the outcome. `tests/coap_token_length_test.cpp` covers the boundary and was confirmed to *fail* against the old encoding before being kept (`[7 != 8]` for low counters, `[9 != 8]` from 100), and carries a case pinning the old scheme's behaviour so the file cannot later be rewritten into something vacuous.
  - **`proxygen_transport_test`'s intermittent `ingress timeout` — reduced, not proven fixed** (`dd041bf`). The accompanying `Acceptor.cpp:247] Failed to re-configure TLS: couldn't read cert file` errors look like the cause and were this investigation's first diagnosis; **they appear on passing runs too**. They come from `server_reload_tls_material`/`client_reload_tls_material`, which delete cert files *on purpose* to test that `reload_tls_material()` fails all-or-nothing, and glog writes them to stderr unbuffered so they interleave into whatever Boost is printing. Asking "does this symptom also occur on a green run?" is what separated the two, and it is the check that had not been done when the lead was first written down. The real signature is the rest of that line: `timeout=3000ms` is a deadline the file hardcodes in nine places, and this suite had **no** timeout scaling at all (`scaled_timeout` usage was zero, while the CoAP suite adopted it in #140/#147), so CI had no lever to pull. What made 3000ms reachable is that the TLS fixtures generated RSA-2048 keys by shelling out to the `openssl` CLI — once per TLS case, three times per mutual-TLS case. Measured on 2 pinned cores under 8× busy-loop load: `rsa:2048` took **741–2966ms** (one sample alone consuming essentially the entire RPC budget) against `prime256v1`'s **65–170ms**. These tests want a real handshake, not a particular key type, so P-256 gives identical coverage ~15× cheaper and with a far tighter spread: worst observed `tls_request_vote_round_trip` fell from **2321ms to 192ms** under identical load — a 15× margin against the deadline instead of 1.3× — and CI artifacts confirm the test went from **3.09s to 0.72s**. `rpc_times_out_against_unresponsive_peer` stayed at ~513ms throughout as a control, confirming only the keygen-heavy cases moved. `keyUsage` on the mutual-TLS leaf certs drops `keyEncipherment`, an RSA key-transport usage that does not apply to an EC key and that a strict validator can reject; both mutual-TLS cases still pass, so the CA-signed chain validation they exist to exercise is intact. The same commit adopted `tests/test_timeout_scale.hpp` here and added `scaled_deadline()` for deadlines handed *into* the code under test rather than Boost's own SIGALRM budget — a distinction that matters, because scaling the case timeout alone would not have helped when the deadline that expired was the one passed to `send_request_vote()`. **Honest limit: the failure was never reproduced locally**, across 70 runs at 4, 2 and 1 cores and under heavy load, so this reduces fragility rather than proving a root cause; it stays open in `.kiro/specs/proxygen-http-transport/tasks.md`.
  - **A recurring hazard worth naming, since it cost more time this session than any of the bugs did: measurements that fail silently and read as results.** A forced-timeout harness reported `sigalrm=0` because its forcing function was too weak — both arms would have looked identical and the fix would have "worked" for the wrong reason. A `--run_test` filter that matched nothing reported as 25 failures, because the case lives inside a suite and the filter omitted it. A CI timing comparison used a *failing* run as the baseline and would have claimed a 32% improvement where the artifacts showed 4.3×. In each case the result looked plausible and was wrong, and what caught it was checking that the experiment's own precondition actually fired rather than that its output looked right. Related, and now consistent across this file's entries: `gh run view --log` returned an empty or single-line response on three separate occasions here, while the step-status API and uploaded artifacts were reliable every time — prefer artifacts for anything load-bearing.

### What Changed (August 3, 2026)

- **`ion-rpc-serializer` — actually built and ran against the real `ion-c` library for the first time, closing out task 9's final validation and finding four real bugs along the way.** `ion_rpc_serializer` had only ever been validated against a hand-written `ion-c` API stub (`-fsyntax-only`); the opt-in `ion` vcpkg feature had never actually been built because `vcpkg-overlays/ion-c/portfile.cmake`'s `SHA512` was still a `0` placeholder. Computed it directly (`curl -sSL <url> | sha512sum`) and installed `ion-c` for real (`vcpkg install --x-feature=ion`), which surfaced:
  - **A real upstream `ion-c` bug**: `cmake/VersionHeader.cmake` generates version macros by parsing `git describe`, with no fallback when there's no `.git` directory (exactly `vcpkg_from_github`'s own tarball extraction) — the regex never matches, and `IONC_VERSION_MAJOR`/`MINOR`/`PATCH` substitute as empty, failing `ion_version.c`'s build outright (`*major = ;`). Fixed with a new patch, `vcpkg-overlays/ion-c/0001-fix-version-header-without-git-describe.patch`, falling back to the version `ion-c`'s own `project(IonC VERSION 1.1.3 ...)` already declares.
  - **A CMake config-filename-casing mismatch**: `vcpkg_cmake_config_fixup(PACKAGE_NAME ionc ...)` only controls which `share/<name>` directory the config files land in, not the filenames themselves (`ion-c` still names them `IonCConfig.cmake`) — `find_package(ionc CONFIG)` never found it on this case-sensitive filesystem despite the directory being right. Fixed with an explicit `file(RENAME ...)` in the portfile.
  - **A missing `DECNUMDIGITS` propagation**: `ion-c`'s own `ion_decimal.h` `#error`s without it, and `ion-c`'s directory-scoped `add_definitions()` setting it is never exported on `IonC::ionc`'s usage requirements — added `DECNUMDIGITS=34` to `raft_ion_serializer`'s own interface compile definitions (root `CMakeLists.txt`).
  - **A real, serious upstream `ion-c` bug, found by this spec's own "never crashes" property test**: `tests/ion_malformed_message_property_test.cpp`'s truncated-input case hung indefinitely at 100% CPU. A gdb backtrace (started fresh under `gdb --args`, since this sandbox's `ptrace_scope` blocks attaching to an already-running process) pinned it to `ion-c`'s own `ionc/ion_internal.h`: `ASSERT(x)` is `while (!(x)) { ion_helper_breakpoint(), assert(x); }`, unconditionally — under `NDEBUG` (this port's Release build), `assert(x)` compiles away entirely, so a failed invariant's `while` condition never becomes false and it spins forever instead of aborting (debug) or no-op'ing (release, matching plain `assert()`'s own semantics). A real hazard for any consumer building `ion-c` in Release and feeding it malformed/truncated input, not specific to this codebase. Fixed with `vcpkg-overlays/ion-c/0002-fix-assert-infinite-loop-under-ndebug.patch`. Two replacement-macro attempts failed to even compile first, for instructive reasons the patch's own comment records: a bare `((void)0)` breaks call sites that invoke `ASSERT(x)` with no trailing semicolon (two adjacent expansions parse as one calling the other), and `do {} while (0)` has its own trailing semicolon as part of its grammar, which those same call sites don't provide either — `while (0) {}` (the original macro's own bare-`while` statement shape, just always-false) was what actually worked.
  - **Task 9.4 (end-to-end sanity check) added**: `tests/ion_http_coap_end_to_end_test.cpp`, mirroring `cbor-rpc-serializer`'s own `coap_cbor_end_to_end_test.cpp` precedent — real client/server round trips over both `cpp_httplib` HTTP and CoAP with `ion_rpc_serializer`, plus a unit check that `name()` maps to `application/ion` through both transports' shared `coap_utils`-based detection.
  - All 6 `ion_*`-labeled CTest binaries now pass. `.kiro/specs/ion-rpc-serializer/tasks.md`'s 45 checkboxes were all still unchecked despite `doc/TODO.md` already tracking ~40/45 as done (another instance of the tasks.md-drift pattern this changelog has documented before) — reconciled to 45/45 with the full accounting in that file's own "Known Follow-ups".

### What Changed (August 2, 2026)

- **Boost.Beast HTTP transport — root-caused and fixed the strand-serialization race the previous entry left open.** `Promise<T>::getFuture()` (`future.hpp`) routes its returned future through `SemiFuture::via(&folly::InlineExecutor::instance())`; Folly's Core resolves the race between attaching a `.thenValue()` continuation and fulfilling that promise by running the continuation inline, immediately, on *whichever thread wins* — which need not be the io-thread that's still unwinding the very same asio completion handler that just called `setValue()`. Reading Boost.Beast's own vendored source pinned the exact sequence: `connect_op::operator()` resets its internal `pending_guard` state, *then* invokes the completion handler where this codebase's `async_connect_kf` (etc.) fulfills the promise — so a continuation that wins the attach race runs concurrently with that same handler's own still-in-progress unwind on a different thread, touching the identical stream state.
  - **Fixed**: a new `beast_detail::asio_strand_executor` (`beast_http_transport.hpp`) — a minimal `folly::Executor` wrapping a stream's own executor (its strand), whose `add()` does `net::post(ex, func)`. Every `async_connect_kf`/`async_client_handshake_kf`/`async_server_handshake_kf`/`async_write_kf`/`async_read_kf` helper now takes a `folly::Executor*` and `.via()`'s its returned future onto it, instead of leaving it on `InlineExecutor` — Folly's `via()` binding persists across every subsequent `.thenValue()` in the chain, so every continuation that touches the stream is forced through that exact stream's strand, provably ordered after Beast's own internal bookkeeping regardless of which thread fulfills the promise or when. `plain_beast_connection`/`tls_beast_connection` (client) and `server_session` (server) each gained an `asio_strand_executor` member built from their stream's own `get_executor()`.
  - **A dead end worth recording**: an earlier attempt in this same session — deferring `promise.setValue()` itself via a raw `net::post()` inside the completion handler, rather than `.via()`-ing the future — looked plausible but measurably did not close the race (identical failure signature before and after, confirmed by a clean rebuild-and-rerun comparison). `.via()` binds *every* downstream continuation to the strand; deferring just the one promise's fulfillment does not.
  - **Confirmed by direct measurement**: the targeted race (0 occurrences, with or without `tests/tsan_suppressions.txt` applied, across 4+ full reruns) and the functional failures it caused are both gone — `beast_server_test`, `beast_integration_test`, and `beast_cross_transport_equivalence_test` each went from a hard, reproducible `end of stream` failure to Boost.Test's own `*** No errors detected` in isolated reruns.
  - **A nuance for anyone auditing `tsan` CI history**: `tests/tsan_suppressions.txt` already blanket-suppresses `race:boost::beast::`/`race:boost::asio::` for a *different*, genuine reason (those vcpkg-vendored binaries aren't themselves built with `-fsanitize=thread`). That pattern happened to also match this bug's TSan signature, even though this specific race is not a packaging artifact — so the `tsan` job was already green before this fix, and still is after it. This fix's actual value is eliminating the functional failures the race caused under real (non-TSan) contention, which no TSan suppressions file was ever positioned to hide from the `Coverage`/`Build & Test` jobs' own plain test runs.
  - **A scope caveat, not a further bug**: this fix only compiles/behaves correctly when `future_default<T>` is Folly-backed (the project default) — no CI configuration currently builds any `beast_*` target under a different `KYTHIRA_DEFAULT_FUTURE_BACKEND`, so this doesn't narrow anything actually exercised today, but it's a real gap if that ever changes. Full accounting, including one remaining intermittent ~182-second timeout in `beast_server_test` consistent with the July 30 entry's own scheduling-starvation caveat (not a new bug), is in `.kiro/specs/boost-beast-http-transport/tasks.md`'s "Known Follow-ups" (Round 4).

### What Changed (August 1, 2026)

- **Boost.Beast HTTP transport — verified the round-2 TSan segfault fix, found and fixed one more genuine bug, and surfaced one still-open TSan race affecting most of the suite.** Re-running `beast_cross_transport_equivalence_test` confirmed the July 30 fix (stripping `CPPHTTPLIB_USE_NON_BLOCKING_GETADDRINFO` under `KYTHIRA_SANITIZER STREQUAL "thread"`) does eliminate the segfault — it now fails cleanly with a catchable `boost::system::system_error: end of stream` instead of crashing the process. That clean failure was the first sign that "all five [beast-http binaries] pass cleanly" (the prior entry's closing line) didn't hold up under a full `ctest -L beast-http -j1` run in this same sandbox: 4 of 5 real beast-http binaries (everything except `beast_client_test`, which never drives a real client/server round trip) failed with dozens of TSan data races apiece and a hard functional error on the *first* RPC issued in the process.
  - **Fixed**: `boost_beast_client`'s connection-pool eviction paths (`get_or_create_connection()`'s idle-timeout and LRU-over-capacity evictions, plus `remove_connection()`'s error-path cleanup) destroyed a pooled connection via `close()` + `_connections.erase()` immediately, rather than retiring it to `_retired_connections` first the way `reload_tls_material()` already does for the identical reason: a concurrent in-flight RPC obtained from an earlier `get_or_create_connection()` call may still hold a raw `beast_connection*` into that same entry, making the immediate erase a use-after-free. Fixed all three sites to retire instead of erase-destroy, matching the already-reviewed `reload_tls_material()` pattern exactly (including not closing the connection from the evicting thread, since that itself bypasses the connection's own strand).
  - **Confirmed insufficient on its own**: an identical rebuild-and-rerun before and after this fix showed the exact same four binaries failing, with the exact same fatal-error test case names — ruling out connection-pool eviction as the cause of *these* particular failures (unsurprising in hindsight for e.g. `beast_integration_test`'s `request_vote_round_trip_and_connection_reuse`, which fails on its very first RPC, before the pool or its eviction logic is ever exercised). The fix is real and worth keeping regardless.
  - **Still open**: reading the actual TSan report for the recurring `basic_stream::expires_after()` race (not just its one-line summary) shows both racing accesses attributed to two different `io_thread_pool` worker threads — never the main thread — meaning two operations on the *same* connection's stream are running concurrently despite that stream being constructed on its own `net::strand`, which `boost_beast_client`'s own design comment says should serialize every operation issued on it. Every TSan trace collected this round includes `folly::InlineExecutor::add`/`folly::Executor::KeepAlive` — the exact machinery `folly::Future<T>::via()` introduces to move a `.thenValue()` continuation onto a specific executor — a pointed coincidence given this same branch's own prior commit (`f3f70a2`, routing `Promise<T>::getFuture()` through `SemiFuture` + `via()`). Leading, **unconfirmed** hypothesis: that routing change may let some `.thenValue()` continuations in the connect/send/read chain run off the connection's own strand thread, silently breaking the serialization invariant `set_timeout()`/`send()`'s re-arm-before-write logic (the July 30 entry's timeout fix) depends on. Not fixed this round — full accounting, including why this wasn't attempted blind, is in `.kiro/specs/boost-beast-http-transport/tasks.md`'s "Known Follow-ups" (Round 3).
  - **Out of scope, noted only**: `three_way_http_transport_equivalence_test` has no built executable in this sandbox's `build-tsan` (`ctest` reports "Not Run"); looks like a pre-existing build-configuration gap unrelated to the above, not investigated further.

### What Changed (July 30, 2026)

- **Proxygen HTTP transport — closed the remaining test-coverage and
  benchmark gaps flagged by the July 29, 2026 reconciliation of
  `.kiro/specs/proxygen-http-transport/tasks.md`** (that reconciliation
  itself has no separate changelog entry; see that file's own "Last
  Updated" history). Everything below is written and reviewed but **not
  compiled or run** — this session's environment could not obtain a
  working `vcpkg install` (a from-scratch bootstrap failed downloading
  `zlib`, a transitive `proxygen` dependency, from its upstream GitHub
  release archive with a 403 traced to this environment's GitHub access
  being scoped to this repository specifically, not a transient network
  error — confirmed via the proxy's own structured error response). See
  `.kiro/specs/proxygen-http-transport/tasks.md`'s "Known Follow-ups" for
  the full accounting; this entry is the short version.
  - **Property 12's forced-generic-bridge escape hatch**: `proxygen_client::send_rpc`'s
    generic-bridge branch is now a standalone method,
    `send_rpc_generic_bridge`, reachable unconditionally via a new public,
    explicitly test-only `send_rpc_via_generic_bridge_for_test` — the only
    way to exercise the generic bridge at all when the project's future
    backend is Folly, since `send_rpc`'s own `if constexpr` dispatch would
    otherwise always select the fast path.
  - **Test coverage added**: mutual TLS (a self-contained CA plus
    server/client leaf certs generated via the `openssl` CLI, no new test
    dependency), whole-RPC timeout enforcement against a raw-socket
    listener that accepts a connection and never responds, concurrent RPCs
    to the *same* target node (not just different ones), and the forced
    generic bridge producing identical results to the Folly fast path for
    the same RPC. The existing Folly-fast-path-taken test's assertion was
    also generalized (computed from `Types`'s own `future_template` member
    type, the same condition `send_rpc`'s dispatch itself uses) so the same
    test is correct whether the surrounding project is built with
    `KYTHIRA_DEFAULT_FUTURE_BACKEND=folly`, `=stdexec`, or `=boost`, instead
    of needing a second, backend-specific test.
  - **Three-way cross-transport equivalence test**
    (`tests/three_way_http_transport_equivalence_test.cpp`), built jointly
    with `.kiro/specs/boost-beast-http-transport/`'s own long-open Task 15
    (a two-way cpp-httplib-vs-Beast equivalence test that spec had never
    built, and that this spec's own Task 14 was blocked on) — one test file
    now satisfies both specs' equivalence requirements, extended to all
    three transports rather than duplicated as two separate two-way and
    three-way tests.
  - **Two new benchmark scenarios** in
    `examples/raft/http_transport_comparison_benchmark.cpp`: Proxygen's
    generic bridge vs. its Folly fast path for the same small RPC, and the
    same comparison at a 1 MiB `install_snapshot`-sized body (the concrete
    measurement Requirement 17.3 asked for to turn the zero-copy
    `folly::IOBuf` claim into a measured result instead of an unmeasured
    architectural expectation) — `doc/http_transport_performance_comparison.md`
    documents both scenarios as implemented-but-unmeasured rather than
    presenting invented numbers, per that document's own established
    "measure or label unmeasured" rule.
  - **Not attempted**: a ThreadSanitizer run/CMake preset (this project
    still has no such preset for *any* transport, and this session had no
    working build to validate a new one against).

### What Changed (July 30, 2026, continued)

- **Proxygen HTTP transport — CI-verified everything the entry above
  described as "written and reviewed but not compiled or run."**
  [PR #117](https://github.com/crawlins/kythira/pull/117) ran green across
  all four `Build & Test` legs (g++-13/clang++-18 × x64/arm64) plus
  `Coverage (clang++-18)`, on real GitHub Actions runners (unlike the
  session that wrote the code, GitHub Actions runners have normal internet
  access, so the earlier `vcpkg install` blocker didn't apply there).
  - **Two real bugs found and fixed by that first real compile**, neither
    caught by hand review: `kythira::Future<T>::thenTry`'s two
    non-flattening overloads (`include/raft/future.hpp`) didn't mark their
    wrapping closures `mutable`, breaking on the first-ever call site in
    this project's history to pass them a mutable callback (Property 12's
    new escape hatch, `send_rpc_generic_bridge`'s trailing `.thenTry()`) —
    fixed to match the adjacent flattening overloads, which already did
    this correctly, and confirmed no other call site anywhere in this
    codebase had ever hit the gap. And `temp_mtls_material` (the new
    mutual-TLS test's self-signed CA/leaf-cert generator,
    `tests/proxygen_transport_test.cpp`) produced a client leaf certificate
    with zero X.509v3 extensions, which a real TLS handshake rejected with
    a "bad certificate" alert — fixed by adding proper
    `basicConstraints`/`keyUsage`/`extendedKeyUsage` to both leaf certs,
    verified locally via `openssl verify` before the fix was even pushed.
  - **The two new benchmark scenarios are now measured**, via a temporary
    CI step added, run once, and reverted (this benchmark's CTest entry
    carries the `performance`/`slow` labels specifically so it's excluded
    from every normal CI run — there was no other way to capture real
    numbers short of a working local build). Result: no measurable
    fast-path advantage over the generic bridge at either the small
    RequestVote body (9,089 vs. 8,996 ops/sec) or the 1 MiB
    `install_snapshot` body (52 vs. 53 ops/sec) on this run — a genuine,
    reported-as-is result, not a fabricated one, with an honest
    interpretation in `doc/http_transport_performance_comparison.md` of
    why that's plausible rather than a sign the fast path doesn't work.
  - Still open: a ThreadSanitizer run/preset (no precedent for any
    transport in this project) and confirming the generic-bridge test
    coverage under `KYTHIRA_DEFAULT_FUTURE_BACKEND=stdexec`/`=boost`
    specifically (this project's CI has no future-backend axis for any
    feature yet). See `.kiro/specs/proxygen-http-transport/tasks.md`'s
    "Known Follow-ups".

### What Changed (July 30, 2026, further continued)

- **Proxygen HTTP transport — closed both remaining Known Follow-ups from
  the entry above.** [PR #117](https://github.com/crawlins/kythira/pull/117)
  added a `KYTHIRA_SANITIZER` CMake cache option (root `CMakeLists.txt`,
  mirroring `ENABLE_COVERAGE`'s existing shape) and two new CI jobs
  (`.github/workflows/ci.yml`): `tsan` (ThreadSanitizer over
  `beast_transport_test`/`proxygen_transport_test`) and
  `future-backend-compat` (a 2-leg matrix building `proxygen_transport_test`
  under `KYTHIRA_DEFAULT_FUTURE_BACKEND=stdexec` and `=boost`). Both are now
  green. Building and running under configurations this project had never
  actually exercised before found four more genuine, pre-existing bugs, on
  top of the two the entry above already documents:
  - The same missing-`mutable` gap already found in `thenTry` also existed
    in all four `thenValue` overloads of `include/raft/future.hpp` — fixed
    the same way.
  - `include/raft/proxygen_http_transport_impl.hpp`'s generic bridge
    hardcoded the Folly-specific `kythira::Try<Response>` where the
    backend-generic `kythira::try_default<Response>` was needed — harmless
    under the default Folly backend, a hard compile error under
    `stdexec`/`boost`.
  - `include/raft/future_stdexec.hpp`'s non-flattening `thenTry` overloads
    attached one callback to two separate continuations by copying it into
    each, which cannot compile for a move-only callback — fixed by wrapping
    the callback in a `shared_ptr` so both continuations copy that instead.
  - A real, pre-existing capacity limit of HTTP/1.1 session reuse: under
    genuine 16-way concurrent load against the `boost` backend,
    `HTTPUpstreamSession::newTransaction()` legitimately returned nullptr
    for whichever caller lost the race to reuse a busy pooled session
    (only one in-flight transaction per HTTP/1.1 connection) — fixed at the
    test level with a bounded retry on specifically that transient
    condition, since the test's own purpose (no cross-response
    interleaving) is unaffected by it. An initial theory that this was a
    thread-affinity bug (forcing the completion handler through
    `evb->runInEventBaseThread()`) did not change the observed crash at all
    and was reverted before the real root cause was identified.
  - `tests/tsan_suppressions.txt` documents the vendored Folly/Wangle/
    Boost races the `tsan` job intentionally suppresses (their prebuilt
    vcpkg binaries aren't themselves built with `-fsanitize=thread`) —
    every suppressed report was inspected and none had a `kythira::`
    function as the actual racing read or write.
    `three_way_http_transport_equivalence_test` is deliberately excluded
    from the `tsan` job: it reproduced an undiagnosable, zero-output
    SIGSEGV under TSan on two separate real CI runs while passing cleanly
    under ordinary CI, and its own test cases are entirely sequential (no
    concurrent RPC threads), so it was never exercising a concurrent code
    path that job exists to check.
  - `.kiro/specs/proxygen-http-transport/tasks.md` is now **Complete
    (17/17 tasks)**, and `.kiro/specs/boost-beast-http-transport/tasks.md`'s
    own identical ThreadSanitizer follow-up (its Task 14) is closed too,
    via the same `tsan` job.
### What Changed (July 30, 2026, yet further continued)

- **Boost.Beast HTTP transport: closed out the remaining 3 tasks** —
  `.kiro/specs/boost-beast-http-transport/` now stands at 18/18. See that
  spec's `tasks.md` for the full per-task writeup; summary below.
  - **Cross-transport equivalence test** (`tests/beast_cross_transport_equivalence_test.cpp`,
    Task 15/Property 9) — `cpp_httplib_client`/`server` and
    `boost_beast_client`/`server`, run through different `Types` bundles
    that share the same `serializer_type`, against the identical
    RequestVote/AppendEntries/InstallSnapshot request values, assert
    field-for-field equivalent responses. Kept alongside PR #117's own
    three-way `three_way_http_transport_equivalence_test.cpp` (which
    independently closed the same task) rather than deleted: it confirmed a
    genuine, pre-existing asymmetry the three-way test's own design doesn't
    happen to surface — `http_transport_impl.hpp`'s
    `make_future_with_exception` slices `http_client_error`/
    `http_server_error` down to plain `std::exception` before an async
    failure ever reaches a catch block (a `std::make_exception_ptr(e)` call
    where `e`'s *static* type is `const std::exception&`) — a real, separate
    bug this spec's Non-Goals rule out fixing here, so the new test works
    around it rather than papering over it.
  - **Malformed-request handling** (Task 13) — `boost_beast_server_config::
    max_request_body_size` was a validated-but-unenforced config field:
    `async_read_kf` read into a bare `beast_http::request<string_body>&`,
    whose default body limit is Beast's own internal one, not the
    configured value. `server_session::read_loop` now reads through a
    `beast_http::request_parser<string_body>` with `.body_limit()` set
    instead (a new `async_read_kf` overload taking a `parser`), responding
    413 when exceeded. New tests cover oversized bodies and a truncated-
    request regression for the accept loop's own resilience.
  - **Mutual TLS test coverage** (Task 13) — `require_client_cert` was
    already fully implemented both server- and client-side; only the test
    (`mutual_tls_client_certificate_enforcement`) was missing.
  - `tests/beast_transport_test.cpp` was split one-file-per-concern
    (`beast_client_test.cpp`/`beast_server_test.cpp`/
    `beast_integration_test.cpp`/`beast_ssl_test.cpp`), matching the layout
    the rest of this project's test suite already uses.

### What Changed (July 30, 2026, and yet further still)

- **Boost.Beast HTTP transport — a round-2 ThreadSanitizer pass against the
  now-split test binaries found four more genuine, pre-existing bugs**
  beyond the two [PR #117](https://github.com/crawlins/kythira/pull/117)'s
  own `tsan` job found against the (then still monolithic)
  `beast_transport_test` binary. Splitting the suite exercised
  connection-lifetime paths — a server stopping mid-keep-alive, a client
  torn down with an in-flight TLS handshake — the monolithic binary's own
  test cases didn't hit the same way. None of the four were
  Folly/Boost/Wangle packaging-mismatch false positives; all four are real:
  - Two of `beast_server_test.cpp`'s test cases joined `io_context` worker
    threads and a stop-thread by hand, in a way that could `std::terminate()`
    via a still-joinable `std::thread` destructor if an exception unwound
    past the join. Replaced with two small RAII helpers,
    `kythira::testing::io_thread_pool`/`joining_thread`
    (`tests/beast_test_thread_pool.hpp`), applied across all four split test
    files — a test-harness fix, not a production one.
  - `plain_beast_connection`/`tls_beast_connection` didn't re-arm
    `boost::beast::basic_stream::expires_after()` for a second, separate
    operation — that call covers one logical operation (or a consecutive
    read-then-write), and a later, separate operation needs its own fresh
    call. `send()`'s write and `server_session::handle_and_write()`/
    `handle_read_error()`'s response write were each running against
    whatever deadline the *previous* operation had armed, reading as an
    instantly-expired timeout once that deadline had passed. Fixed by
    re-arming `expires_after()` immediately before each of those operations.
  - `boost_beast_client`'s destructor could tear down a connection's
    `ssl::context` while a handshake was still in flight on another thread,
    racing `SSL_do_handshake()`'s own `CRYPTO_THREAD_write_lock`/
    `CRYPTO_THREAD_lock_free` calls. `boost_beast_server` already had a
    drain-on-stop pattern for this; added the client-side equivalent — an
    `in_flight_operations` counter guarded by a mutex/condition variable, via
    an RAII `in_flight_guard` held for the duration of `send_rpc`'s
    connection use, with the destructor closing every connection first and
    then waiting for the count to reach zero before destroying any
    `ssl::context`.
  - A ThreadSanitizer-only segfault in `beast_cross_transport_equivalence_test`,
    traced to cpp-httplib's `CPPHTTPLIB_USE_NON_BLOCKING_GETADDRINFO` compile
    flag: glibc's `getaddrinfo_a()` spins up its own internal worker threads
    outside any `pthread_create()` the application makes, which TSan's
    allocator interceptors didn't handle cleanly. Fixed in the root
    `CMakeLists.txt` — when `KYTHIRA_SANITIZER STREQUAL "thread"`, that one
    compile definition is stripped from `httplib::httplib`'s
    `INTERFACE_COMPILE_DEFINITIONS` before anything links against it.
  - All five `beast-http`-labeled CTest binaries now pass cleanly under
    `-DKYTHIRA_SANITIZER=thread` locally.

### What Changed (July 30, 2026, still further continued)

- **Fixed and verified the long-standing `ca_cluster_node_test` intermittent
  SIGTERM-shutdown hang** — root cause: `run_ca_cluster_node()`'s shutdown
  sequence joined `http_thread`/`election_timer`/`heartbeat_timer`/
  `maintenance_thread` *before* calling `raft_node.stop()`, the only thing
  (besides a normal timeout) that force-rejects a `submit_command()`/
  `read_state()` future still in flight — if SIGTERM landed while
  `maintenance_thread` was blocked in one such call and the commit could no
  longer land (this node losing quorum precisely because it and/or its
  peers were shutting down), with `heartbeat_timer` already stopped by the
  time `maintenance_thread.join()` was reached, nothing was left to ever
  unblock it, hanging the whole process forever (commit `19b05e2`, found by
  a different pass through this same code rather than the earlier
  no-`ptrace`-access investigation). Verified via 25 iterations of the
  affected tests under heavy concurrent `ctest -j` load (the condition that
  originally produced a ~1-in-12-15 hang rate): zero hangs. Also added
  defense in depth — `cluster_node_process::stop()` in all three affected
  test files now bounds its wait with a 30s timeout and `SIGKILL` escalation
  (`tests/ca_cluster_node_process_wait.hpp`) instead of a plain blocking
  `waitpid()`, so a future regression of this exact bug surfaces as a
  diagnosable test failure rather than silently hanging `ctest` again. See
  `doc/TODO.md`'s Known Follow-ups for the full writeup.

### What Changed (July 28, 2026)

- **Proxygen HTTP transport** — a third `network_client`/`network_server`
  implementation (`include/raft/proxygen_http_transport.hpp`/`_impl.hpp`),
  backed by Meta's Proxygen driven directly by Folly's `EventBase`/
  `IOThreadPoolExecutor`, alongside the existing cpp-httplib and
  Boost.Beast transports. Spec at `.kiro/specs/proxygen-http-transport/`,
  all 17 tasks across 12 phases complete.
  - Generic (any-`KYTHIRA_DEFAULT_FUTURE_BACKEND`) async bridge composed
    via `future_transformable`'s `thenValue`/`thenTry`, mirroring Beast's
    own bridge shape but adapted to Proxygen's callback interfaces
    (`HTTPConnector::Callback`, the 10-method `HTTPTransactionHandler` —
    `spike-notes.md` Finding 2 corrects `design.md`'s original 4-method
    sketch). One connection pooled per target node, with one
    `folly::EventBase` pinned per node for its whole connection lifetime
    (not round-robin per call — required for correctness, since
    `HTTPUpstreamSession` is permanently pinned to its creating
    `EventBase`) and an `HTTPSessionBase::InfoCallback`-based liveness
    tracker so a dead connection is transparently replaced on next use.
  - An optional Folly-native fast path: when the project's future backend
    is Folly (the default), `send_rpc` skips the generic
    `kythira::promise_default<T>` bridge entirely and wraps a raw
    `folly::Promise<T>`/`folly::Future<T>` directly into
    `kythira::Future<T>`, dispatched via `if constexpr` on `Types`'s own
    `future_template<T>` member type — never a
    `KYTHIRA_DEFAULT_FUTURE_BACKEND` macro check. Confirmed actually taken
    (not merely "the RPC succeeded") via a metrics path-label distinguishing
    it from the generic bridge.
  - Server side built on Proxygen's own higher-level `RequestHandler`/
    `RequestHandlerFactory`/`ResponseBuilder` API rather than a raw
    `HTTPTransactionHandler` — a deliberate refinement over the spec's
    original lower-level sketch (`spike-notes.md` Finding 6), since
    Proxygen itself documents this as the intended extension point for
    exactly this shape of server.
  - TLS (mutual and server-only) via `folly::SSLContext` (client) /
    `wangle::SSLContextConfig` (server), with hot reload
    (`reload_tls_material()`/`enable_auto_reload()`) matching both existing
    transports' contract.
  - A real, non-obvious bug found and fixed during development (not by a
    reviewer after the fact): an early draft captured the same move-only
    `kythira::promise_default<T>`/`folly::Promise<T>` into both a
    `.thenValue()` and a separate `.thenError()` continuation, moving from
    an already-moved-from promise on the error path. Fixed by settling the
    outer promise from a single trailing `.thenTry()`/`folly::Future<T>::thenTry()`
    instead.
  - Two real, pre-authoring discrepancies between the spec's assumptions
    and the actually-vendored `proxygen`/`folly` headers, found and
    recorded via direct header inspection rather than assumed:  the
    resolvable `proxygen` version at this project's pinned
    `builtin-baseline` is `2025.05.19.00`, not the `2026.02.23.00` the spec
    document originally stated; and `HTTPTransactionHandler` has 10
    pure-virtual methods, not the 4 the original design sketch showed.
    Full findings in `.kiro/specs/proxygen-http-transport/spike-notes.md`.
  - A genuine, general project-level CMake gap surfaced by this feature's
    new transitive dependency chain (fizz/mvfst pull in `unofficial-sodium`
    for the first time): this project configures vcpkg's manifest-mode
    output via `CMAKE_PREFIX_PATH` rather than vcpkg's own toolchain file,
    and `unofficial-sodium`'s generated CMake config is the first vcpkg
    port in this project's tree whose config script reads
    `_VCPKG_INSTALLED_DIR`/`VCPKG_TARGET_TRIPLET` directly (most ports
    instead derive their install prefix relative to the config file's own
    location) — those two variables are only ever defined by vcpkg's
    toolchain file, so left undefined they silently collapsed
    `unofficial-sodium::sodium`'s `INTERFACE_INCLUDE_DIRECTORIES` to a
    literal `"//include"`, failing CMake's imported-target path-existence
    check at generate time (misleadingly attributed by CMake's own
    diagnostic to `proxygen::proxygen`, the top-level target actually being
    linked, not the deeply-nested real source). Fixed generally in root
    `CMakeLists.txt` (defining both variables from `KYTHIRA_VCPKG_TRIPLET`
    when a caller hasn't already), not specific to this one port.
  - `doc/http_transport_performance_comparison.md`: a real, measured
    throughput/latency comparison of all three HTTP transports
    (`examples/raft/http_transport_comparison_benchmark.cpp`) doing the
    same `RequestVote` round trip. Notable finding: cpp-httplib measured at
    ~12 ops/sec (vs. Beast's 3,527 and Proxygen's 2,839) on this run,
    traced to cpp-httplib's vendored `CPPHTTPLIB_TCP_NODELAY` defaulting to
    `false` — a genuine Nagle/delayed-ACK interaction, not a benchmark
    artifact or a general "cpp-httplib is slow" claim.
  - `configs/ci_full_defconfig` was already missing `CONFIG_BOOST_BEAST_TRANSPORT=y`
    (a pre-existing gap predating this feature — Beast's own default is
    `n`, same as every other optional transport, so "every optional
    dependency selected" was never actually true for either HTTP
    transport); fixed alongside adding `CONFIG_PROXYGEN_TRANSPORT=y`, so
    CI's full-feature build now actually exercises both.
  - Full `ci_full_defconfig` regression suite: 393/395 passing (2 new
    targets from this feature, both passing); the 2 non-passing entries
    are the pre-existing, already-documented `ca_cluster_node_test`/
    `ca_cluster_node_rpc_tls_restart_test` intermittent-hang flake (Known
    Follow-ups), confirmed unrelated (both pass standalone; this feature
    touches neither `certificate_authority` nor `ca_cluster_node`) and
    reconfirmed rather than assumed. One real, transient regression found
    and fixed along the way: this feature's own new
    `http_transport_comparison_benchmark_test` (a genuine ~3-minute,
    CPU-heavy real-network run) caused `raft_comprehensive_performance_benchmark`'s
    latency-coefficient-of-variation check to fail under `ctest -j4`
    contention alone (confirmed by an isolated rerun passing cleanly) —
    fixed with the same `PROCESSORS`-matches-runner-cores CTest property
    `ca_cluster_node_test` itself already uses for the identical reason.

### What Changed (July 25, 2026, continued)

- **Closed gap 2 of the Folly-decoupling follow-ups for `tests/`/
  `certificate_authority`: per-target rather than subdirectory-level
  `folly_FOUND` CMake gating.** Scoped empirically with a real probe build
  (Folly's `find_package` disabled, stdexec selected as the default
  backend) rather than guessing from inspection, which surfaced three
  distinct problems: (1) the subdirectory-level gates checked `folly_FOUND`
  when the real question is "is the *selected* backend's dependency
  satisfied" — fixed with a `KYTHIRA_FUTURE_BACKEND_AVAILABLE` variable
  reusing the invariant CMake already enforces for stdexec/boost; (2) six
  test targets used a legacy linking pattern that never picked up the
  `KYTHIRA_FUTURE_BACKEND_STDEXEC`/`_BOOST` compile definition at all — a
  latent, pre-existing bug (silently always-Folly regardless of Kconfig
  selection) that gap 2 surfaced rather than caused, fixed by linking
  `network_simulator` properly; (3) 70 test targets are genuinely Folly-
  only by design (HTTP/CoAP transport tests — those headers hardcode
  `folly::Future<T>`, a real gap the original decoupling pass never
  covered since it scoped to the Raft core/RPC transports — Folly
  concept-wrapper tests, cross-backend benchmarks), now cleanly skipped
  via per-target `if(TARGET Folly::folly)` guards instead of hard-failing.
  Verified on all three backends with Folly genuinely absent from a real
  build directory (a first pass under a `/tmp/.../scratchpad/` path
  produced two false-positive failures traced to that location's own
  path-resolution quirk, not a real bug) — stdexec and boost each passed
  cleanly except for already-known/confirmed-flaky timing-sensitive
  tests unrelated to Folly's availability — and the default Folly build
  was fully re-verified afterward: 382/382 passed, 0 failed, no
  regressions. `examples/` and 5 standalone `cmd/*` production
  executables stay deliberately Folly-only (their own `main()`s call
  `folly::init()` directly); decoupling HTTP/CoAP transport from Folly
  at the header level remains explicitly out of scope, a separate,
  larger feature. See TODO.md's "Known Follow-ups" for the full
  breakdown.

### What Changed (July 25, 2026)

- **Closed the first of the two Folly-decoupling follow-up gaps: test files'
  vestigial `folly::init()` Boost.Test bootstrap.** A full grep found 135
  files calling `folly::init()`/constructing `folly::Init` (the prior
  estimate of "roughly 30" was low); 120 had no other Folly usage in their
  own source at all. Of those, 13 are `cmd/*/main.cpp` / `examples/*.cpp`
  standalone executables where the call is legitimate production usage and
  out of scope; the remaining 106 (105 `tests/*.cpp` files plus the shared
  `tests/chaos/chaos_test_types.hpp` fixture) had their Folly bootstrap
  fixture gated behind `#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) &&
  !defined(KYTHIRA_FUTURE_BACKEND_BOOST)`, matching the backend-conditional
  pattern already used throughout the rest of the decoupling work.
  The first attempt deleted the fixture outright instead of gating it, and
  that broke 37 tests at runtime with `SIGABRT` /
  `"Singleton folly::Timekeeper ... requested before
  registrationComplete()"` — under the still-default Folly backend,
  `kythira::future_default<T>` *is* `folly::Future<T>`, so any test
  transitively touching Folly's async timers (retry-backoff delays in
  `error_handler.hpp`, `future_collector.hpp` timeouts) needs `folly::init()`
  to have run, regardless of whether the test's own source mentions
  `folly::` by name — a transitive, runtime-only dependency invisible to a
  static text grep. Caught by running the full test suite after the change,
  not just rebuilding; fixed by gating instead of deleting, which restores
  identical Folly-backend behavior while genuinely dropping the Folly
  dependency once a different backend is selected. Re-verified clean:
  382/382 tests passed (`-LE ^(slow|performance|verbose|benchmark|docker)$`,
  `--repeat until-pass:3`), checked via `ctest`'s own exit code and
  `Testing/Temporary/LastTest.log` rather than a wrapping shell pipeline's
  exit code (piping through `tee | tail` had silently reported `tail`'s
  exit code instead of `ctest`'s during the initial, broken pass).
  Gap 2 (per-target, rather than subdirectory-level, `folly_FOUND` CMake
  gating across 100+ targets) remains out of scope — see TODO.md.
- **Unrelated, found and fixed along the way: silent corruption in the
  repo-root `vcpkg_installed/x64-linux` dependency cache.** Three
  `boost/intrusive` headers had stray characters inserted into macro
  definitions (e.g. `template< class (TYPE)>` instead of `template< class
  TYPE>`), breaking any target pulling in Folly's futures headers. Fixed via
  a full `vcpkg install` reinstall from the manifest after moving the
  corrupted directory aside (binary-cache-backed, seconds not minutes).
  That reinstall doesn't manage the manually-built `libPocoDNSSD.a`/
  `libPocoDNSSDAvahi.a` archives (DNSSD isn't a vcpkg feature; see
  README.md's ARM-support section), so those were lost along with the
  corrupted directory — handled correctly by the project's existing
  `POCO_DNSSD_FOUND=FALSE` graceful-degradation path once `build-clang` was
  reconfigured, same as any host without those archives.

### What Changed (July 24, 2026, continued further)

- **Reorganized the Kconfig "Futures" menu and made Folly genuinely
  optional at the header level.** Grouped `CONFIG_FOLLY` (new),
  `CONFIG_STDEXEC_BACKEND`, and `CONFIG_BOOST_FUTURE_BACKEND` into a
  dedicated `menu "Futures"`, added a Kconfig `comment` that becomes
  visible if all three are deselected simultaneously, and enforce "at
  least one backend must be selected" as a hard configure-time error in
  `cmake/Kconfig.cmake` (not just a UI hint) — verified directly by
  configuring with all three off and confirming a real `CMake Error`
  fires, and confirming the default state (`CONFIG_FOLLY=y`) configures
  cleanly. The `Default kythira::Future backend` choice's default
  resolution changed from a single unconditional `default
  DEFAULT_FUTURE_BACKEND_FOLLY` to a priority chain (`default
  DEFAULT_FUTURE_BACKEND_FOLLY if FOLLY`, then `STDEXEC if
  STDEXEC_BACKEND`, then `BOOST if BOOST_FUTURE_BACKEND`), so folly stays
  the default whenever it's enabled but the choice still falls through
  correctly if a user deselects it.
  `CONFIG_FOLLY=n` now gates `find_package(folly)` itself via
  `kythira_find_optional(FOLLY folly)`, the same mechanism already used
  for stdexec/boost's own dependencies -- CMake configure genuinely skips
  probing for Folly when deselected, and every existing downstream
  `folly_FOUND` guard (already present throughout `CMakeLists.txt` for
  the "Folly happens to be missing from the host" case) handles the
  Kconfig-driven case identically.
  Making this a *meaningful* toggle (not just a CMake-level no-op)
  required a substantial header-level decoupling pass, since
  `future_default.hpp` previously `#include`d the Folly-backed
  `future.hpp` *unconditionally*, regardless of which
  `KYTHIRA_FUTURE_BACKEND_*` macro was set -- meaning every one of the
  three backends silently still required Folly to compile. Fixed by
  moving the `future.hpp` include into the same `#if/#elif/#else` chain
  already used for `future_stdexec.hpp`/`future_boost.hpp`, so it's only
  reached when Folly is actually the selected backend. That single fix
  then surfaced (via real compile attempts, not just code review) three
  further layers of latent Folly coupling, all fixed in the same pass:
  - **~20 production headers hardcoded raw Folly types**, relying
    entirely on `future.hpp`'s old unconditional include to make
    `kythira::Future<T>`/`FutureFactory::` available -- none of them
    included `future.hpp` themselves, so a naive grep for direct
    `#include <raft/future.hpp>` users (the approach used for the
    original `future_default` migration) missed all of them. Converted
    to `future_default`/`future_factory_default` using the same
    mechanical playbook as that migration (raw exceptions wrapped in
    `std::make_exception_ptr`, bare `.get()` rewrapped as
    `std::move(f).get()`): `certificate_provider.hpp`,
    `docker_quorum_manager.hpp`, `peer2peer_replication.hpp`,
    `poco_peer_discovery.hpp`, the five `rfc*_*_discovery.hpp`/
    `rfc*_peer_discovery.hpp` DNS peer-discovery headers,
    `coap_transport.hpp`'s `coap_multicast_peer_discovery` class,
    `tcp_gossip_transport.hpp`, `acme_certificate_provider.hpp`/
    `_impl.hpp`, `aws_acm_pca_provider.hpp`/`_impl.hpp`,
    `aws_asg_quorum_manager.hpp`, `aws_ec2_quorum_manager.hpp`, and
    `cmd/ca_service/main.cpp`.
  - **Two non-future-related Folly dependencies were transitively
    reachable only through the future-backend headers**, despite having
    nothing to do with which future backend is selected:
    `folly::Synchronized<T>` (`peer2peer_replication.hpp`,
    `tcp_gossip_transport.hpp` -- mutex-guarded progress/membership
    tables) and `folly::CPUThreadPoolExecutor` (`tcp_rpc.hpp`,
    `tls_tcp_rpc.hpp` -- the private RPC-dispatch thread pool). Added
    `kythira::synchronized<T>` (new `include/raft/synchronized.hpp`,
    ~50 lines: `std::shared_mutex` + RAII `wlock()`/`rlock()` proxies
    matching `folly::Synchronized`'s API, with explicit copy/move
    constructors since `std::shared_mutex` itself isn't copyable/movable
    -- a real regression caught by the Folly-backend build after the
    naive version compiled fine under stdexec but broke
    `static_peer2peer_replicator`'s default-constructibility under
    Folly). Extended `kythira::executor_default`
    (`.kiro/specs/...`/`error_handler_async_retry_property_test.cpp`'s
    original test-only helper) with a portable `submit(Func)` method
    normalizing each backend's fire-and-forget dispatch primitive
    (`folly::Executor::add`, `boost::basic_thread_pool::submit`,
    `exec::start_detached` composed over the stdexec scheduler), then
    switched `tcp_rpc_client`/`tls_tcp_rpc`'s private executor from a
    hardcoded `folly::CPUThreadPoolExecutor` to
    `kythira::executor_default`.
  - **A genuine, reproducible system-library bug**: `<ldns/common.h>`
    (`libldns`, already a dependency for DNS peer discovery)
    `#define`s `true 1` / `false 0` whenever
    `__bool_true_false_are_defined` isn't already set, with no
    `__cplusplus` guard of its own -- a pre-existing C-compatibility
    shim that happens to be harmless in isolation, but corrupts any
    C++20 `concept`/`requires` code parsed afterward in the same
    translation unit. Never triggered before because no file previously
    combined `<ldns/ldns.h>` with stdexec's headers in one TU (every
    ldns-using file was hard-Folly via the old unconditional
    `future.hpp` include); surfaced immediately once
    `acme_certificate_provider_impl.hpp` (which both `#include`s
    `<ldns/ldns.h>` directly for its DNS-01 TXT record support, and now
    correctly resolves to whichever backend is selected) was exercised
    under stdexec for the first time, as `stdexec/__detail/__concepts.hpp`
    failing to parse `concept __true = true;` ("atomic constraint must
    be of type bool, found int"). Root-caused by isolating the exact
    failing target with `-j1` (parallel builds interleave error output
    across concurrent compiler invocations) and reading the resulting
    macro-expansion trace. Fixed with a defensive `#undef true` /
    `#undef false` immediately after every direct `<ldns/ldns.h>`
    include (7 files) -- always safe in C++, since `true`/`false` are
    keywords regardless of any macro's state.
  - **A handful of files relied on transitively-included standard
    library headers** that disappeared along with Folly:
    `std::async`/`std::launch` (the *standard library* `<future>`,
    unrelated to `kythira`'s own future types) in
    `raft_concurrent_read_efficiency_property_test.cpp` and
    `integration_test.cpp`, plus `folly::Executor` used directly as a
    `transport_types::executor_type` in 12 `coap_*` test files with no
    include of their own for it.
  - **Verification**: for every affected production header, confirmed
    with a real compile -- not just "the code looks portable" -- that it
    builds under `-DKYTHIRA_FUTURE_BACKEND_STDEXEC` with zero Folly
    headers touched, via `clang++ -H` (header-trace) output filtered for
    `folly/`. Then full `cmake --build` + `ctest -j4 --repeat
    until-pass:3` runs to completion under all three
    `KYTHIRA_DEFAULT_FUTURE_BACKEND` values: folly 389/394, stdexec
    389/391, boost 388/394 -- every failure in all three runs was one of
    the 5 already-documented LocalStack/real-EC2 tests, or (stdexec/boost
    only) a pre-existing, unrelated timing-threshold sensitivity in
    `performance_equivalence_property_test.cpp`/
    `future_backend_benchmark_test.cpp` (both untouched by this work;
    this sandbox's stdexec per-operation overhead is measurably higher
    than whatever machine those hardcoded thresholds were tuned
    against -- e.g. 949ms measured against a 500ms threshold for 50,000
    `makeFuture`+`.get()` round trips). Zero regressions introduced by
    this pass under any backend.
  - Two deliberately out-of-scope, non-future Folly dependencies are
    documented directly in `CONFIG_FOLLY`'s Kconfig help text and in
    `doc/TODO.md`'s "Known Follow-ups": ~30 test files' `folly::init()`
    process-bootstrap calls (unrelated to future backend selection), and
    the root `CMakeLists.txt` still gating `certificate_authority`/
    `examples/`/`tests/` on `folly_FOUND` at the subdirectory level
    rather than per-target -- so `CONFIG_FOLLY=n` stops CMake from
    probing for Folly, but doesn't yet make those targets buildable
    without it, since genuinely Folly-specific test files are mixed into
    the same subdirectories as everything else.

### What Changed (July 24, 2026, continued)

- **Implemented the `kconfig-integration` spec**
  (`.kiro/specs/kconfig-integration/`): a declarative front end, via
  [Kconfiglib](https://github.com/ulfalizer/Kconfiglib) (the same
  configuration language as the Linux kernel, Zephyr, Buildroot, and
  coreboot), layered over Kythira's existing `find_package()`-driven
  optional-dependency matrix. All 19 tasks across the spec's 5 phases
  complete.
  - **Root [`Kconfig`](../Kconfig) file**: one `config` symbol per optional
    dependency (`OPENSSL`, `HTTP_TRANSPORT`, `HTTP_TRANSPORT_TLS`,
    `COAP_TRANSPORT`, `EDHOC`, `DNS_DISCOVERY`, `POCO_DISCOVERY`, `AWS_SDK`,
    `AWS_ACM_PCA`, `LIBSSH2_TESTS`, `CHAOS_TESTS`, `COVERAGE`), grouped into
    `menu`s, with `depends on` expressing every prerequisite the existing
    CMake `if()` chains already encoded (`HTTP_TRANSPORT_TLS` needs
    `HTTP_TRANSPORT` and `OPENSSL`; `EDHOC` needs `COAP_TRANSPORT`;
    `AWS_ACM_PCA` needs `AWS_SDK`) — verified directly with Kconfiglib that
    deselecting the prerequisite correctly greys out the dependent symbol.
    Extended beyond the spec's original two-backend (`folly`/`stdexec`)
    `choice` to a three-way `folly`/`stdexec`/`boost` choice plus a
    `BOOST_FUTURE_BACKEND` symbol, since the `boost-future-backend` spec
    (and this session's earlier `future_default` migration) had since made
    boost a third real backend the original spec design predated.
  - **`scripts/kconfig/genconfig.py`**: translates a resolved `.config`
    into `build/generated/autoconf.cmake` (`KCONFIG_<NAME>` CMake
    variables for every symbol) and `build/generated/kythira/autoconf.hpp`
    (`#define`s using the *existing* macro names — `KYTHIRA_HAS_LDNS`,
    `LAKERS_AVAILABLE`, `LIBCOAP_AVAILABLE`, etc. — so no existing `#ifdef`
    call site needed to change). Unit-verified against `ci_full_defconfig`,
    `minimal_defconfig`, and bare Kconfig defaults (no `.config` at all),
    confirming the bare-defaults output exactly matches `ci_full_defconfig`
    except for the two/three symbols that deliberately default off
    (`EDHOC`, `STDEXEC_BACKEND`, `BOOST_FUTURE_BACKEND` — all gated behind
    toolchain/vcpkg features not assumed present).
  - **`cmake/Kconfig.cmake`**: `KYTHIRA_KCONFIG`/`KYTHIRA_KCONFIG_STRICT`
    cache variables; graceful degradation to "no `KCONFIG_*` variables
    defined" when `python3`/`kconfiglib` aren't available, so Kconfig
    remains purely additive tooling, never a build requirement; three new
    macros — `kythira_find_optional()` for dependencies resolved by a
    single plain `find_package()` call (OpenSSL, httplib, lakers, AWSSDK
    core, libssh2), and `kythira_kconfig_gate()`/`kythira_kconfig_require()`
    for the hand-written multi-step ones that don't fit that shape
    (libcoap's CONFIG-then-PkgConfig fallback, libldns and libfiu's
    `pkg_check_modules`, Poco DNSSD's vcpkg-tree-then-CMake-then-pkgconfig
    cascade, and `AWS_ACM_PCA`'s existing `unset(AWSSDK_FOUND)` re-probe
    trick, all preserved verbatim and just wrapped in the new gate).
    `CONFIG_COVERAGE` and `CONFIG_BOOST_FUTURE_BACKEND` pre-seed their
    legacy cache variables (`ENABLE_COVERAGE`,
    `KYTHIRA_BUILD_BOOST_FUTURE_BACKEND`) the same way — via a plain
    `set(... CACHE ...)` without `FORCE`, which only takes effect if the
    cache entry doesn't already exist, so an explicit `-D` on the command
    line always wins over the Kconfig-derived value, verified directly by
    configuring with both set and confirming the `-D` value survives.
  - **`menuconfig`/`guiconfig`/`savedefconfig`/`kconfig-check` CMake
    targets**, following the same stub-target-when-tool-missing pattern as
    the existing `format`/`static-analysis` targets — always listed by
    `cmake --build build --target help`, but print an actionable
    `pip install` hint and fail only themselves when `kconfiglib` is
    absent, verified not to block the rest of the build.
  - **`configs/ci_full_defconfig`** (every optional feature on) and
    **`configs/minimal_defconfig`** (every optional feature off, core
    folly/Boost/plain-HTTP transport untouched since those stay outside
    Kconfig's control entirely) — both minimal by construction (only
    symbols differing from their Kconfig-declared default), validated by
    `scripts/kconfig/check_defconfigs.py` (the `kconfig-check` target).
  - **Verification, all performed against a real build** (not just
    Kconfiglib-level symbol checks): zero-config regression — diffed the
    complete `cmake --build --target help` output between this branch and
    an origin/main baseline (same `vcpkg_installed/` tree, via a symlinked
    git worktree) both with and without `kconfiglib` installed; identical
    apart from the four new Kconfig targets. Strict-mode failure —
    selected `CONFIG_EDHOC=y` with `KYTHIRA_KCONFIG_STRICT=ON` against this
    environment's genuinely-not-installed `lakers` vcpkg feature and
    confirmed CMake's own standard `find_package(... REQUIRED)` not-found
    error fires, naming `lakers`, with the call stack pointing at
    `kythira_find_optional`; confirmed the identical `.config` without
    strict mode degrades gracefully instead. Minimal-config check —
    applied `minimal_defconfig` on a machine with every optional dependency
    actually installed and confirmed none of them link in
    (`KYTHIRA_HAS_POCO_DNSSD` etc. absent, `poco_discovery_node` etc.
    skipped) despite being available. `menuconfig` round-trip — simulated
    via Kconfiglib's API directly (deselect `POCO_DISCOVERY` from
    `ci_full_defconfig`, save, reconfigure): confirmed
    `KYTHIRA_HAS_POCO_DNSSD` disappears from the generated `autoconf.hpp`
    and `poco_discovery_node` is skipped, and that `savedefconfig`'s output
    differs from `ci_full_defconfig` by exactly that one symbol.
  - **Documentation**: new "Build Configuration (Kconfig)" section in
    [README.md](../README.md); `kconfiglib` documented in
    [DEPENDENCIES.md](../DEPENDENCIES.md) under its own subsection,
    explicitly distinct from the required/optional C++ library
    dependencies already there (it's Python tooling, not something the
    build itself links against).

### What Changed (July 24, 2026)

- **Converted the entire test suite and all production RPC/collection
  machinery it depends on from the Folly-hardcoded `kythira::Future`/
  `Promise`/`Try`/`FutureFactory`/`FutureCollector` to the backend-selectable
  `kythira::future_default`/`promise_default`/`try_default`/
  `future_factory_default`/`future_collector_default` family, so `ctest`
  passes cleanly under all three `KYTHIRA_DEFAULT_FUTURE_BACKEND` values
  (`folly`, `boost`, `stdexec`), not just the default.** Scope grew far
  beyond "convert test files": the boost and stdexec future backends
  (`.kiro/specs/boost-future-backend/`, `.kiro/specs/stdexec-future-backend/`)
  had shipped with 52+31 passing tests each but had never actually been
  exercised by the *production* Raft/RPC code path (`include/raft/raft.hpp`,
  `error_handler.hpp`, `future_collector.hpp`, `tcp_rpc.hpp`,
  `tls_tcp_rpc.hpp`, `configuration_synchronizer.hpp`,
  `network_simulator/*`), which was still hardcoded to Folly's `kythira::`
  namespace throughout — so this pass is the first real end-to-end proof
  that a non-Folly backend can run a real multi-node Raft cluster, not just
  satisfy the wrapper-level concept-compliance tests.
  - **Mechanical conversion** (regex/sed, verified via `-fsyntax-only`
    against all three backends before any build): ~115 test files plus the
    production headers above renamed `Future<`/`Promise<`/`SemiPromise<`/
    `Try<`/`FutureFactory::`/`FutureCollector::` to their `_default`
    counterparts; raw-exception `makeExceptionalFuture(std::runtime_error(...))`
    calls (a Folly-only implicit-conversion convenience) rewrapped in
    `std::make_exception_ptr(...)`; bare lvalue `.get()` calls (valid on
    Folly's unqualified `Future<T>::get()` but a compile error against
    boost/stdexec's `&&`-qualified one) rewrapped as `std::move(f).get()`
    — the single most common fix, needed in well over 100 places across the
    `network_simulator` test suite alone; `.hasException()`/`.value()` on
    `Future<T>` (Folly-only convenience methods never in the portable
    `future` concept) replaced with try/catch around `.get()` or
    `.isReady()`.
  - **Files judged genuinely Folly-specific were left untouched** rather
    than converted: `future_test.cpp`, the `folly_concept_wrappers_*` suite,
    and four `*_future_returning_callback_*` test files that specifically
    exercise Folly's `.thenError()`/`.thenTry()` overload accepting a
    `folly::exception_wrapper`-typed callback (an overload boost/stdexec
    never implemented) all stayed on the raw Folly types by design, not
    oversight — each confirmed via direct inspection (constructor calls
    into `folly::Try`/`folly::Promise`, or an `is_invocable_v<F,
    folly::exception_wrapper>` dependency) rather than assumed from
    filename.
  - **Two genuine production bugs found and fixed in `future_boost.hpp`**,
    surfaced only because this pass was the first time boost-backend code
    actually ran against real test scenarios rather than synthetic
    concept-compliance cases: `collectAnyWithoutException<void>` tried to
    construct an ill-formed `std::tuple<size_t, void>` (fixed with the same
    void/non-void split Folly and stdexec already had); and
    `set_exception_from_std` called `std::rethrow_exception(nullptr)`
    unconditionally, which is undefined behavior per the standard and
    segfaults on this libstdc++ — fixed with an explicit null check. Also
    added the missing `FutureFactory::makeReadyFuture(T value)` overload
    (boost only had the zero-arg void one; Folly and stdexec both had the
    value-taking overload already).
  - **A genuine stdexec-backend double-execution bug** in
    `Future<T>::thenTry`'s Future-returning ("automatic flattening")
    overload: it composed `_sender | ex::let_value(func) | ex::let_error(func)`
    — mirroring the shape of the non-Future-returning overload, which is
    safe there because it wraps `func`'s result in `ex::just(...)` (always a
    value, never an error). The Future-returning overload instead
    `.extract_sender()`s `func`'s *own* returned Future, which can itself
    complete with an error — and `ex::let_error` can't distinguish "the
    original antecedent failed" from "func's own later-spliced sender
    failed," so it fired a second, spurious call to `func` on the latter.
    Isolated with a minimal standalone repro (a nested Future-returning
    `thenTry` whose result fails was invoked twice; without `.delay()`,
    without recursion — pure two-level nesting was enough) before touching
    the real code. Fixed by normalizing both of `_sender`'s completion
    channels into a single value-channel-only `Try<T>` via `ex::just(...)`
    first, then calling `func` exactly once from one `let_value` stage with
    no outer `let_error` left to double-fire. Fixed identically for both
    `Future<T>` and the `Future<void>` specialization.
  - **Added `kythira::Future<T>::detach()`** (all three backends) after
    discovering the deepest and most widespread issue: stdexec's senders
    are lazy (nothing runs until something *starts* the composed chain),
    unlike Folly's and boost's eager futures (attaching a `.thenTry()`/
    `.thenValue()` continuation runs it regardless of whether the returned
    `Future` object is kept). Production and test code throughout this
    codebase has a long-standing "attach a continuation, discard the
    returned Future, fire-and-forget" idiom — safe under Folly/boost, a
    silent no-op under stdexec. `detach()` is a no-op on Folly/boost
    (the continuation is already running) and wraps `exec::start_detached`
    on stdexec. Applied to ~10 genuine fire-and-forget call sites in
    `raft.hpp` (election vote-outcome and pre-vote-outcome collection,
    heartbeat and AppendEntries response handlers, `add_learner`/
    `remove_server`/`promote_to_voter` triggered from RPC handlers,
    peer-to-peer progress-advertisement and catch-up chains) and one in
    `future_collector.hpp` (`collect_n_successes_with_timeout`'s per-future
    loop) — all previously either bare-discarded or wrapped in `(void)fut;`,
    which reads as intentional but has the identical silent-no-op problem
    under stdexec. Also fixed the same discard pattern in 11 test files
    (`membership_change_*`, `learner_*`, `quorum_promotion_capacity_fallback_test`,
    `peer2peer_catch_up_membership_sync_property_test`) that used a
    `std::move(fut).thenValue(...).thenError(...);` idiom to flip a
    `bool` flag polled via `wait_until(...)` — 28 occurrences fixed with a
    script, not by hand, after confirming the exact discard shape.
  - **Added `kythira::executor_default`** (`include/raft/executor_default.hpp`),
    a small owning N-thread pool whose `.handle()` is directly usable as the
    argument to `future_default<T>::via(...)` across all three backends,
    despite `.via()` itself taking a genuinely different type per backend
    (`folly::Executor&`, a duck-typed `boost::executors::basic_thread_pool&`,
    `stdexec_backend::scheduler_handle&`) — what's portable is the *call
    shape* (`future.via(executor.handle())`), not a shared type. Exists
    because `error_handler_async_retry_property_test.cpp` needed a real,
    bounded thread pool to prove its actual property (retry backoff doesn't
    starve other work queued on the same pool), which can't be written
    portably against a single backend's native executor type.
  - **Verification**: full `cmake --build` + `ctest` (373 non-chaos/AWS/
    real-EC2 tests) run to completion in all three
    `KYTHIRA_DEFAULT_FUTURE_BACKEND` configurations — boost 373/373, stdexec
    373/373, Folly (default) 370/373 with all 3 remaining failures
    independently reproduced as pre-existing CPU-contention flakiness under
    `ctest -j$(nproc)` (2 confirmed passing standalone; the third,
    `ca_cluster_node_test`, matches this project's already-documented flaky
    pattern under parallel test contention — see the CI-reliability entry
    below), not migration regressions.
  - **Two follow-up fixes for the three flaky tests found during
    verification above**: `state_machine_apply_performance_property_test`
    (measures real elapsed-time thresholds, same class of test as
    `ca_cluster_node_test`) got the same `PROCESSORS 4` CMake isolation
    `ca_cluster_node_test` already carried, so it no longer gets
    co-scheduled into CPU contention by `ctest -j$(nproc)`. Separately,
    investigating `ca_cluster_node_test`'s own flakiness surfaced a real
    bug distinct from the flakiness itself: it (and its two siblings,
    `ca_cluster_node_rpc_tls_test`/`ca_cluster_node_rpc_tls_restart_test`)
    spawn a real `ca_cluster_node` subprocess via `posix_spawn` and rely on
    RAII (a destructor sending SIGTERM) to clean it up — which never runs
    if the test process itself is killed by an external SIGTERM (from
    `timeout`, or ctest's own `TIMEOUT`), orphaning the child and leaving
    it holding the test's stdout/stderr pipe open indefinitely, capable of
    wedging the entire `ctest` invocation rather than just failing one
    test. Fixed by replacing `posix_spawn` with
    `fork()`+`prctl(PR_SET_PDEATHSIG, SIGKILL)`+`execve()` in all three
    files, so the kernel kills the child the instant its parent dies for
    any reason, independent of destructors running; verified via a 10-run
    local loop with zero orphans left behind afterward. The intermittent
    hang itself (reproduced directly, ~1 in 12-15 runs) remains unresolved
    — diagnosing it needs a live backtrace, and this sandbox has no
    `ptrace` access — and is tracked as a documented follow-up in
    `TODO.md`.
  - **Merged to `main` via [PR #92](https://github.com/crawlins/kythira/pull/92)**
    (rebase merge, 6 commits, all CI checks — 4 build/test matrix jobs,
    coverage, and the Packer AMI build — passing).

### What Changed (July 23, 2026, continued further)

- **Implemented `.kiro/specs/boost-future-backend/` end to end — the third
  `Future`/`Promise`/`Try`/`Executor` backend, `boost::thread`-based,
  alongside Folly (default) and `stdexec`.** `include/raft/future_boost.hpp`
  (~1000 lines, `namespace kythira::boost_backend`, guarded behind
  `KYTHIRA_HAS_BOOST_FUTURE`): `Try<T>`/`SemiPromise<T>`/`Promise<T>` direct
  wraps of `boost::promise<T>` (push-model like Folly, no hand-rolled
  bridge needed unlike `stdexec`'s pull model); `Future<T>` with
  `thenValue`/`thenError`/`ensure`/`via`/`delay`/`within`; a Meyers-singleton
  `timer_service` (one shared `boost::asio::io_context` + background
  thread) backing `delay`/`within`, since Boost.Thread has no built-in timed
  continuation; `FutureFactory`; `FutureCollector` with `collectAll`
  (`boost::when_all`), `collectAny`/`collectAnyWithoutException`
  (`boost::when_any` plus a shrinking-pool retry loop for the
  without-exception variant), and `collectN` (same shrinking-pool shape,
  Folly-matching semantics — first N to complete regardless of success,
  not N successes). Wired into `KYTHIRA_DEFAULT_FUTURE_BACKEND=boost` and
  `include/raft/future_default.hpp` alongside the existing `folly`/`stdexec`
  options; no existing production call site converted, Folly stays default.
  Two new property-test binaries (`boost_future_concept_compliance_property_test`,
  `boost_future_continuation_and_collector_property_test`, 31 cases total)
  plus a `boost-backend/migration_guide_example.cpp` comparison example,
  gated on the same `KYTHIRA_BUILD_BOOST_FUTURE_BACKEND`/
  `KYTHIRA_DEFAULT_FUTURE_BACKEND=boost` condition throughout.
  `tests/backend_non_interference_compile_fail_test.cpp` extended with
  boost-vs-Folly and boost-vs-`stdexec` non-interference checks and, in the
  process, converted from `stdexec_FOUND`-gated to unconditional (Folly is
  always present; `future_stdexec.hpp`/`future_boost.hpp` are both safe
  no-op headers when their backend isn't enabled, so the file's own
  internal `#ifdef` guards now do the gating instead of the CMake
  registration) so it validates whichever subset of backends is actually
  active in any given configuration.
  A pre-implementation spike (throwaway compiles against the real vendored
  Boost 1.89.0 headers, not documentation) found two real corrections to
  the spec's original design: `BOOST_THREAD_PROVIDES_EXECUTORS` is gated on
  `BOOST_THREAD_VERSION>=5`, not `>=4` as first read from source — fixed by
  defining it explicitly rather than relying on auto-definition; and
  `boost::exception_ptr` is a distinct type from `std::exception_ptr` whose
  *implicit* converting constructor compiles cleanly but silently rethrows
  a `clone_impl<std::exception_ptr>` wrapper instead of the original
  exception on `get()` — fixed with genuine catch-and-rethrow bridge
  functions used at every exception boundary, verified end-to-end to
  preserve the original exception's concrete type and message. A third
  finding surfaced while writing the non-interference test: unlike Folly's
  and `stdexec`'s `via()` (each fixed to one concrete executor type),
  `boost_backend::Future::via()` is deliberately templated on the executor
  type (Boost.Thread's own concrete executors, e.g. `basic_thread_pool`,
  don't share a common base), which makes a `static_assert(!requires{...})`
  -style cross-backend check vacuously pass for `via()` specifically — not
  a real interference bug, just undetectable through that particular SFINAE
  idiom, so those specific checks were omitted with an explanatory comment
  rather than kept as a false-negative test. All new/extended targets
  verified via the real CMake/CTest build in three configurations: boost
  backend enabled (with stdexec also available), and Folly-only (both
  optional backends disabled) for the now-unconditional non-interference
  test specifically.

### What Changed (July 23, 2026, continued)

- **Fixed Folly CMake detection so builds actually degrade gracefully when
  Folly is absent, instead of only appearing to.** Confirmed by actually
  building with Folly hidden (`-DCMAKE_DISABLE_FIND_PACKAGE_folly=ON`, the
  same technique `scripts/verify-optional-dependency-isolation.sh` already
  uses for `stdexec`) rather than just reading the CMake logic: configure
  succeeded with only a mild warning, but the build then died deep inside
  `certificate_authority.cpp`'s `#include` chain with a `GLOG_EXPORT`
  "unknown type name" error from `glog/flags.h` — no indication anywhere
  that the real cause was `include/raft/future.hpp` wrapping Folly types
  with no `#ifdef` of its own. `certificate_authority` was gated only on
  `TARGET OpenSSL::SSL`, `examples/` was added completely unconditionally
  (confirmed all ~20 example targets transitively need Folly), and
  `tests/` was gated only on `Boost_FOUND` (confirmed the overwhelming
  majority of the 391+ registered tests transitively need it too, even
  ones nominally about the `stdexec`-only backend). All three now gated on
  `folly_FOUND` at the point each is added; a full build with Folly hidden
  now completes cleanly (exit 0) instead of failing catastrophically, with
  no regression in the normal configuration.
- **Removed redundant/duplicate includes**: `http_transport_impl.hpp`'s own
  `#include <future>` was provably redundant (`raft/http_transport.hpp`,
  included first, already includes it unconditionally);
  `simulator_impl.hpp` had the entire `#ifdef FOLLY_FUTURES_AVAILABLE
  #include <folly/futures/Future.h> #endif` block duplicated back to back,
  plus a duplicate `#include <thread>`.
- **Added `.kiro/specs/boost-future-backend/` (requirements/design/tasks,
  spec only — no implementation yet).** Plans a third `kythira::Future`/
  `Promise`/`Try`/`Executor` implementation backed by `boost::thread`'s
  extended future API (`then()`/`when_all`/`when_any`, gated behind
  `BOOST_THREAD_VERSION>=4`), alongside the existing Folly (default) and
  `stdexec` backends. `boost-thread`/`boost-asio` are already required
  dependencies of this project for unrelated reasons and are not currently
  used for `boost::future`/`boost::promise` at all. Follows
  `.kiro/specs/stdexec-future-backend/`'s exact structural precedent, but
  is structurally simpler in one specific way documented up front:
  `boost::promise::set_value()`/`set_exception()` are already push-model
  (the same shape as `folly::Promise`), so — unlike `stdexec`'s pull model
  — no hand-rolled single-shot-channel bridge is needed; `Promise<T>` is a
  near-direct wrap. The real new risks are different in kind:
  `BOOST_THREAD_VERSION` is a project-wide ABI hazard if two translation
  units disagree (solved by making it one `INTERFACE` compile definition,
  mirroring how `KYTHIRA_HAS_STDEXEC` is already applied); `then()`'s
  callback receives the completed future itself, not the unwrapped value,
  and does not auto-flatten a `Future<U>`-returning callback; and
  Boost.Thread has no `when_n` or timed-continuation primitive, so
  `collectN`/`delay`/`within` are built on `when_any`-style repeated
  selection and a small shared `boost::asio` timer service respectively.

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

# Design Document

## Overview

This design turns `.kiro/specs/multi-raft-performance/requirements.md` into a
concrete harness: one workload definition, one cluster driver, a fixture per
transport, and a matrix of scenario instantiations over
(transport × RPC provider × payload × concurrency).

Two things shape every decision below.

**The first is that this is an instrument, not a feature.** Requirement 14
forbids touching `raft.hpp`, `multi_raft*.hpp`, the serializers and the
transports. Everything here therefore lives under `tests/`, and where the
production code makes something awkward — non-movable transports, a tick loop
with no timer of its own — the harness adapts rather than the library.

**The second is that the numbers have to survive being quoted.** A row that
cannot say which transport, which encoding, which durability mode and how many
samples its p99 came from is not usable in a comparison against somebody's
published figure, and Requirements 5, 6 and 9 are mostly about that. So the
result type carries its own provenance, and the statistics type refuses to
report a percentile it does not have the samples for.

### Status of the substrate

This design was written after a working spike of the Tier B substrate. That
spike has since landed: `tests/multi_raft_kv_workload.hpp`,
`tests/multi_raft_transport_harness.hpp` and
`tests/multi_raft_http_benchmark_test.cpp` are in the tree, and the boxes
`tasks.md` checks are checked against it rather than against this prose.

The smoke cases pass on all three transports: three hosts, four shards,
elections, committed PUTs and read-backs over real loopback sockets — the first
time anything in this tree has driven `multi_raft` through a socket.

Two defects came out of it, in the order that matters.

**The concurrency crash was real and is fixed.** `boost_beast_client`
segfaulted under concurrent RPCs to a single target; multi-Raft is the first
workload in this tree to issue them, since four groups on four executor stripes
replicate to the same two peers at once. Fixed by `fix(beast): check
connections out exclusively per RPC`, with the isolated reproduction living
beside the transport in `tests/beast_client_test.cpp`.

**The teardown abort — `corrupted double-linked list` after the test module had
already printed "No errors detected" — has not been observed since that fix.**
It shared the lifetime problem's shape: a Beast connection outliving the
executor its callbacks were scheduled on. Task 5 keeps the measured rate rather
than a claim, because the abort never reproduced under `gdb -batch -ex run`
either, and a defect that hides from a debugger is not one to declare closed on
a single clean run.

## Architecture

```
                    ┌──────────────────────────────────────────┐
                    │  multi_raft_http_benchmark_test.cpp      │
                    │  (and, later, a report binary)           │
                    │  — the matrix: one call per cell         │
                    └────────────────┬─────────────────────────┘
                                     │ instantiates
        ┌────────────────────────────┴─────────────────────────┐
        │  multi_raft_transport_harness.hpp                    │
        │                                                      │
        │  kv_cluster<Transport>      run_put_workload<T>()    │
        │       │                            │                 │
        │       │ owns                       │ drives          │
        │  ┌────┴──────────────┐             │                 │
        │  │ Transport fixture │             │                 │
        │  │  cpp_httplib /    │             │                 │
        │  │  beast / proxygen │             │                 │
        │  └────┬──────────────┘             │                 │
        │       │ hands out                  │                 │
        │  transport_client_handle<C>        │                 │
        │  transport_server_handle<S>        │                 │
        └───────┼────────────────────────────┼─────────────────┘
                │ satisfies                  │ uses
                │ network_client/server      │
        ┌───────┴────────────┐   ┌───────────┴──────────────────┐
        │ kythira::multi_raft│   │ multi_raft_kv_workload.hpp   │
        │ (unmodified)       │   │ commands, keys, values,      │
        └────────────────────┘   │ sampling, statistics         │
                                 └──────────────────────────────┘
```

The split between the two headers is deliberate and is itself a claim under
test: the workload header knows nothing about transports, so "the same work over
a different wire" is checkable rather than asserted. When CoAP arrives it touches
the transport header only.

## Components

### 1. The workload (`multi_raft_kv_workload.hpp`)

Owns everything that is true of the payload regardless of how it is carried.

**Command encoding.** Built to `test_key_value_state_machine`'s own format —
type byte, `u32` key length, key, and for PUT a `u32` value length and value.
`kv_put`, `kv_get`, `kv_del`. Deliberately the state machine every other
multi-Raft suite uses, per Requirement 1.1: a benchmark-only fixture would
measure a path nothing else covers.

**`kv_partitioner`.** Recovers the key from a command so
`multi_raft_config::partitioner` can perform the cross-shard admission check.
Without it a routing defect applies a command to a shard that does not own its
key and no invariant catches it — which would make a throughput number a
throughput number for the wrong system.

**Keys.** `kv_key(n)` is fixed-width decimal at ten digits, so lexicographic
order is numeric order and each shard range is a genuine contiguous slice
(Requirement 1.2). `kv_shard_ranges(count, key_count)` tiles `(-inf, +inf)` with
open outer bounds so `check_tiling()` has something true to say.

**Values.** `kv_value(seed, bytes)` is deterministic (reproducible runs) but not
constant (Requirement 1.4's sweep would otherwise measure the encoder's ability
to compress a repeated byte, not the payload).

**Key selection.** `key_sampler` over `key_distribution::{uniform, zipfian}`.
Zipfian uses the inverse-CDF approximation rather than a precomputed table:
at these key counts the table costs more than the draws. Uniform spreads load
across shards; Zipfian concentrates it, which is the only way the matrix reaches
H7's per-group lock.

**Statistics.** `latency_sample_set` returns `p99()` as `std::optional`, empty
below 1,000 samples, and `p999()` empty below 10,000 — Requirement 5.3, enforced
by the type rather than by discipline. `quantile()` is available for a caller
who wants a number anyway and is honestly named. `operation_tally` counts
failures by cause (`_not_leader`, `_epoch_mismatch`, `_merging`, `_timeout`,
`_transport`, `_other`) and keeps `_offered` distinct from `_completed`, so a
success rate is visible rather than folded into throughput (Requirement 4.4–4.5).

**Statistics across runs.** `repeated_result` holds every repetition of one cell
and is the only thing that yields a headline. `headline_ops_per_second()`
returns `std::optional` and is empty below `k_required_repetitions` (5) —
Requirement 6.2's "never report a single run as a result", enforced the same way
the percentile guard is. The headline is the **median run**, not an averaged
rate: `median_run()` names an actual window, so the p50/p95/p99 printed beside
the headline came from the window that produced it, and an averaged p95 that
belongs to no window that ever happened cannot be quoted by accident.
`spread()` is `max(max − median, median − min) / median` — the ± half-width
Requirement 6.3 is written in, rather than a full-width ratio, which would flag
a pair that sits inside the band. `verdict()` collapses the two guards into
`stable` / `unstable` / `inconclusive`, and `comparable()` is what a comparison
table is supposed to consult.

A repetition is a **whole** measurement, cluster construction and election
included. That is more expensive than re-running the workload against one
long-lived cluster, and it is the point: the ±21% Beast/JSON/128 B spread that
made Requirement 6.2 necessary appeared *between* freshly-elected clusters, so a
repetition that reused one would measure a narrower thing than the number gets
quoted as.

**Machine provenance.** `machine_description` / `describe_machine()` record
Requirement 6.4's fields — CPU model and logical count, memory, kernel,
filesystem *and device* for a named directory, compiler, build type and flags,
sanitizer, future backend — plus the one-minute load average and a
`_quiet_at_start` flag for Requirement 6.5. Build type and flags come from the
build (`KYTHIRA_BENCH_BUILD_TYPE` / `KYTHIRA_BENCH_CXX_FLAGS`) rather than from
`NDEBUG` and `__OPTIMIZE__`, which are proxies that a hand-edited flag list can
make wrong. Every field that cannot be read stays `"not stated"`, following the
comparison register's own rule: recorded, never inferred. The description is
captured by a **global fixture** rather than a first test case, because
`--run_test` can deselect a case and a number whose provenance was deselected
along with it is exactly what 6.4 exists to prevent — and because one
description per process is what makes Requirement 6.6's "same machine, same
session" hold structurally rather than by convention.

**`benchmark_result`.** One row, carrying transport, serializer media type,
scenario, cluster shape, value size, concurrency, ops/sec, p50/p95/optional p99,
the tally and the duration. A row is self-describing (Requirement 11.5).

### 2. The transport handles (`multi_raft_transport_harness.hpp`)

`multi_raft_config` holds `network_client` / `network_server` **by value** and
`multi_raft` moves them into place. None of the real transports is movable —
Beast and Proxygen delete their move constructors, cpp-httplib holds a
`std::mutex` and a `std::jthread`. So the host is handed
`transport_client_handle<Client>` / `transport_server_handle<Server>`: movable,
pointer-thin views onto a transport the fixture owns and outlives.

The three mandatory RPCs forward unconditionally. **Every optional RPC forwards
behind a `requires` clause on the underlying transport** (Requirement 17.8):

```cpp
auto send_timeout_now(...) requires kythira::network_client_with_timeout_now<Client>
{ return _client->send_timeout_now(...); }
```

This is the whole extension mechanism, and it is the same one
`group_scoped_client` already uses. Consequences, all of them wanted:

- The handle satisfies exactly the extension concepts its transport does.
- `multi_group_network_server::start()` already installs the optional handlers
  behind `if constexpr` on those concepts (`group_transport.hpp:257,272,313`),
  so an HTTP row registers three handlers and no more.
- A transport that never has TimeoutNow — every HTTP transport today, and CoAP
  tomorrow — produces a row where `transfer_leadership` reports `unsupported`,
  which is the advisory-operator contract working, not a build failure.

### 3. Transport fixtures

Each fixture owns its runtime and hands out references. They differ only in what
that runtime is, which is exactly the per-backend traits pattern
`.kiro/specs/future-backend-performance-benchmark/` established:

| Fixture | Runtime it owns | Client ctor | Server ctor |
|---|---|---|---|
| `cpp_httplib_transport` | none (server owns its thread) | `(url_map, cfg, metrics)` | `(addr, port, cfg, metrics)` |
| `beast_http_transport` | `boost::asio::io_context` + work guard + N threads | `(ioc, url_map, cfg, metrics)` | `(ioc, addr, port, cfg, metrics)` |
| `proxygen_http_transport` | `folly::IOThreadPoolExecutor` | `(*io, url_map, cfg, metrics)` | `(addr, port, cfg, metrics, io)` |

**Port assignment.** `reserve_port()` binds a socket to port 0, reads the
kernel's choice back with `getsockname`, and closes it. The alternative — letting
the server bind port 0 and reading `actual_port()` — cannot work here, because
every client needs every peer's URL at *construction* time, which is before
`multi_raft::start()` has started any server. Reserving up front keeps startup in
its natural order at the cost of a small TOCTOU window; any port chosen in
advance has that window, and this at least never collides with a port already in
use.

**Shutdown order is load-bearing.** `kv_cluster::shutdown()` joins the tick
drivers, then stops the hosts, then calls `_transport.shutdown()` — and
`_transport` is declared *before* `_hosts` so it is destroyed last. A host's
`stop()` still touches its server. The spike's teardown abort is a defect
somewhere in this ordering and is task 1.

### 4. The types bundles

Two bundles, and the distinction matters for Requirement 17.5.

`harness_transport_types<Serializer>` is the **transport's** bundle. Its
`future_template` is pinned to `kythira::future_default`, not to
`folly::Future`: `node<Types>` declares its RPC lambda as returning
`kythira::future_default<T>` and assigns the client's future straight into it
(`raft.hpp:1583`), so the folly-typed `http_transport_types` bundle would convert
under one backend and fail to compile under boost and stdexec. The serializer
here is the **swept** one.

`kv_host_types<Transport>` is the **host's** bundle: the KV state machine,
`memory_persistence_engine` (Tier B is `durability none`), and the two handles
as `network_client_type` / `network_server_type`. Its `serializer_type` — the
node-internal one for log entries and snapshots — is held at JSON across every
row, so the only thing moving on the serializer axis is the wire.

### 5. `kv_cluster<Transport>`

`_nodes` hosts, each with a replica of every one of `_groups` shards, one tick
driver thread per host. `multi_raft` has no timer of its own; the caller owns the
loop, exactly as `cmd/chaos_node/main.cpp` does for a single node.

Configuration held constant across every row (Requirement 17.11), with the
reasoning that fixes each value:

| Setting | Value | Why |
|---|---|---|
| nodes / groups | 3 / 4 | the shape the static-cluster suite already validates |
| stripes | 4 | one group per stripe, so a tick's blocking I/O does not serialize across groups |
| tick interval | 2 ms | fast relative to the heartbeat; swept separately for H6 |
| election timeout | 2000–4000 ms | a cpp-httplib tick's send phase blocks for a ~83 ms round trip per follower; a 300 ms timeout would have followers deposing a leader that is merely mid-heartbeat |
| heartbeat | 400 ms | as above |
| hibernation | off | a population that hibernated mid-window would be measuring hibernation |
| policy / split-merge | disabled, 1 h interval | so neither lands inside a timed window (Requirement 7.8) |

`run_command` measures from **before** the routing lookup, so the latency is what
the caller paid rather than what the last attempt took, and classifies every
failure by catching the specific shard exceptions. `run_read_state` is separate
because it is a different operation (Requirement 2.1a) and reports bytes
returned. `term_sum()` is the cheap steady-state probe: a window across which it
moved contained an election.

### 6. The workload driver

`run_put_workload` is **closed-loop**: `_in_flight` worker threads, each holding
exactly one operation outstanding, so the parameter is literally the concurrency.
Blocking on the future is safe here and only here — the tick runs on its own
driver threads, so the thing that settles the future is not the thing that is
blocked. (Doctrine: never `.get()` a future the tick loop you are responsible for
must settle.) `std::move(f).get()` throughout, since `get()` is rvalue-qualified
under the boost backend.

Open-loop with coordinated-omission correction (Requirement 4.2) belongs to the
report binary rather than the CTest-registered test: a CI-registered test would
have to pick an offered rate the runner can sustain, and it cannot know one.

## The matrix

One scenario implementation, instantiated per cell (Requirement 17.6).

| Axis | Values | Gate |
|---|---|---|
| Transport | cpp-httplib | always |
| | Boost.Beast | `KYTHIRA_BENCH_HAS_BEAST` |
| | Proxygen | `KYTHIRA_BENCH_HAS_PROXYGEN` |
| | CoAP | once `multi_raft` has a CoAP binding |
| RPC provider | JSON, CBOR | always |
| | protobuf | `KYTHIRA_BENCH_HAS_PROTOBUF` |
| | Ion | `KYTHIRA_BENCH_HAS_ION` |
| Group count | **1, 8, 64**, 256, 1000 | always; the first three are mandatory on every transport row |
| Value size | 16 B, 128 B, 1 KiB, 4 KiB | always |
| In-flight | 1, 8, 64 | always |
| Distribution | uniform, zipfian | always |

### The shared serialization point

Requirement 17a, and the reason the group-count axis has three mandatory values
rather than a free choice. Every shared transport here has a point at which many
groups become one queue, and it is usually what a per-group number is really
reporting:

| Transport | Serialization point | Status |
|---|---|---|
| CoAP | `std::recursive_mutex` across every libcoap call | required — libcoap's C API is not concurrency-safe on one context |
| cpp-httplib | one `httplib::Client` per target | inherent to the library |
| Beast | one pooled connection per target | **fixed** — exclusive checkout, `fix(beast): check connections out exclusively per RPC` |
| Proxygen | session pool per target | degrades gracefully; retries under contention |

This is where `.kiro/specs/coap-transport-multi-raft/` Requirement 7 is
discharged. That spec asked for N groups over one shared CoAP client at
N ∈ {1, 8, 64} with per-group send-path latency and lock-wait time — the same
question, asked where it bites hardest. Reconciling the two into one matrix is
what makes the CoAP answer comparable to the cpp-httplib and Beast ones rather
than a third unrelated number, and it is why the row consumes
`KYTHIRA_COAP_SEND_PROBE` instead of a second instrumentation scheme.

Beast is the worked example of why this axis earns its place: its serialization
point did not merely slow things down, it **crashed**, and nothing found it until
a workload issued concurrent RPCs to one peer. The axis exists to make that
class of thing visible as a number before it becomes visible as a segfault.

The full cross-product is not run in one sitting; the CTest cases cover a spine
through it (transport axis at fixed serializer, serializer axis at fixed
transport, value-size axis at fixed both), and the report binary covers the rest.
Every gate that is off announces the rows it dropped (Requirement 17.2).

**cpp-httplib gets a smaller operation budget**, and this is the only per-row
deviation. Its vendored header defaults `CPPHTTPLIB_TCP_NODELAY` to `false`, so
each small RPC body pays the classic Nagle/delayed-ACK round trip — measured at
12 ops/sec against Beast's 3,527 on a bare ping-pong
(`doc/http_transport_performance_comparison.md`, July 28 2026). Throughput stays
comparable because it is a rate; the tail does not, which is what
`latency_sample_set`'s optional `p99()` exists to say out loud.

## Correctness before measurement

Requirement 17.14. Each transport gets a smoke case that elects, commits a PUT on
every shard, reads each back **through the log as a `GET`** (one value, so a
command that landed in the wrong shard is a wrong answer rather than a smear in a
blob), and checks tiling. Only then does a throughput case run. Before this spec
nothing in the tree had driven `multi_raft` through a socket, so the alternative
was benchmarking an unexercised path.

## Assertions

Following the scale test's rule — a wall-clock threshold is a statement about the
machine, a ratio is a statement about the implementation:

- **Smoke cases** assert real outcomes: leader on every shard, PUT commits, GET
  returns the value written, tiling intact.
- **Throughput cases** assert only that something completed and that ops/sec
  clears a **sanity floor** low enough for a loaded runner and high enough to
  catch a structural regression. No cross-transport assertion: the relative
  ordering is a measurement, not a contract.
- **Elections during a window** are *reported*, not asserted. On a loaded machine
  an election is a fact about the machine, and failing on it would make the suite
  flaky in exactly the way doctrine 21 warns about.

## Error handling

- A failed operation is classified by catching `shard_not_leader_exception`,
  `shard_epoch_mismatch_exception`, `shard_merging_exception` and then
  `std::exception`, and counted; it never aborts the window.
- `f.wait(timeout)` before `get()` so a hung transport times out the operation
  rather than the suite.
- A cluster that cannot elect within `k_election_budget` fails the case with the
  transport named, rather than proceeding to measure a cluster with no leader.

## Testing strategy

| Concern | Where |
|---|---|
| multi-Raft over a real socket at all | smoke case per transport |
| Transport axis | `write_throughput_by_transport` |
| RPC-provider axis | `write_throughput_by_rpc_serializer` |
| Payload axis | `write_throughput_by_value_size` |
| Concurrency axis / H7 | `write_throughput_by_concurrency` (to add) |
| Read taxonomy (Requirement 2) | `read_taxonomy` (to add) |
| Statistical method (Requirement 6) | `repeated_result` around every throughput row; `machine_description` from a global fixture |
| Portability | compiles and runs under folly, boost and stdexec |

Registered as `multi_raft_http_benchmark_test` with labels
`performance;benchmark;multi-raft;http`, gated per transport rather than on all
of them, so the matrix shrinks gracefully.

## Real-cloud measurement

The local development machine is old, which makes every gap against a number
published on modern hardware ambiguous: implementation or CPU? Running the same
binary on a current cloud instance removes that ambiguity, and it is the cheapest
useful thing this spec can do about the comparison.

### The constraint that decides the shape

`.github/workflows/real-cloud-tests.yml` has **exactly 25 `workflow_dispatch`
inputs**. That is GitHub's hard cap, and exceeding it does not produce an error —
it silently invalidates the whole file, so the workflow stops existing. PR #257
did exactly this (24 → 27 inputs) and `ci.yml` now carries a guard job that
asserts the limit across every workflow file.

So the cloud performance runs go in **a new workflow file**,
`.github/workflows/perf-cloud.yml`, with its own small input set. Not a
preference — the alternative is unavailable. **Confirmed with the maintainer,
August 27 2026**, so this is settled rather than open.

### Two shapes, cheapest first

**Shape 1 — one instance, all three hosts (Tier B, then D).** The existing
`multi_raft_http_benchmark_test` runs unchanged: three hosts in one process, real
loopback sockets, real serializers. This is the primary deliverable because it
answers the actual question (is the gap the CPU?) for the cost of one instance
for a few minutes, and needs no cross-instance networking, no security-group
rules and no service discovery. Tier D follows on the same instance by swapping
`memory_persistence_engine` for `file_persistence` plus `tick_batch_controller`
and pointing the log at a real volume, whose class and IOPS become part of the
machine record.

**Shape 2 — one host process per instance (Tier E).** The only shape whose
latency resembles a published cluster number, and the expensive one: it needs a
host binary in `cmd/`, N instances, and a measured network. Before the measured
window it records inter-node RTT and bandwidth, because a cluster number without
its network is not comparable to anything (Requirement 18.8). Deferred behind
Shape 1.

### Provider

**AWS first**, and only AWS until it works end to end. It has the most mature
path in this tree — OIDC role assumption already provisioned, both x86_64 and
native arm64 runners already in CI, `aws_ec2_quorum_manager` already provisioning
and tearing down instances under test, and a Packer AMI pipeline that already
builds a node image. Five half-wired providers produce no numbers. GCP is the
natural second (Workload Identity Federation is already live and its real-run
audit pattern is the one this design borrows).

Graviton is worth a row in its own right rather than as an afterthought: the
arm64 leg already exists in CI, and a modern arm64 instance is a different point
in the design space from x86_64, not a portability check.

### Instance selection

Burstable types are disqualified for any published measurement — AWS `t*`, GCP
`e2-micro`/`f1`, Azure `B` series. A run that exhausts CPU credits mid-window
produces a number describing a credit balance, and it will do so silently. The
chosen type, its vCPU count, the underlying CPU model as reported by the guest,
memory, stated network performance, storage class and IOPS, image, kernel and
tenancy all go into the machine record (Requirement 18.4).

A GitHub-hosted runner is admissible as a *machine* but only as an **indicative**
row: 2–4 shared vCPUs whose variance no stability gate can be tuned around.
Useful as a free smoke test that the workflow works at all; not useful as a
number.

### Cost and safety

| Control | Mechanism |
|---|---|
| Pre-registered estimate | instance-hours × published rate, per run, in the spec's own doc — the pattern `doc/aws_acm_pca_test_cost_estimate.md` set |
| Wall-clock ceiling | a hard timeout on the measured phase, so a hung benchmark cannot bill indefinitely |
| Teardown | unconditional, in a step that runs even when the measurement fails |
| Leak audit | a post-run check that fails the job if any provisioned resource still exists — instances, volumes, addresses, security groups, placement groups. The live GCP run is the precedent: the audit is what *proved* nothing leaked, and it found three real defects doing it |
| Credentials | short-lived OIDC only, reusing what `scripts/ci-cloud-credentials/` already provisions |

### Reporting

Cloud rows are their own rows. They carry their machine record, they are never
merged with local rows, and they are never averaged with them. The stability gate
of Requirement 6.3 applies unchanged and is expected to fire more often — a
shared-tenancy instance has neighbours, and a run that would be marked unstable
locally is not made trustworthy by having cost money. Artifacts (CSV/JSON) are
uploaded from the job, so the number exists somewhere other than a log that
expires.

The comparison this unlocks is the honest one: **the same binary, the same
scenarios, the same configuration, two machines.** The delta between the local
row and the cloud row is the hardware confound, measured rather than argued
about, and every gap against an external number can then be quoted against
whichever of the two is the fairer basis.

## Deliberate omissions

- **Tiers C–E locally.** This design delivers Tier B (in-process hosts, real
  transport, memory persistence). Tier C onward needs a host process in `cmd/`,
  and Tier D needs `file_persistence` plus `tick_batch_controller` — both are
  their own tasks, and neither changes anything here. Tier B and Tier D on a
  single cloud instance come first, because they answer the hardware question
  without needing either.
- **Open-loop load.** Report binary, not CTest.
- **The external comparison itself.** Requirement 9's register and
  `doc/multi_raft_performance_comparison.md` come after there are Tier B numbers
  to put in them. Publishing a comparison from Tier B alone would violate
  Requirement 3.3, which forbids a like-for-like claim below Tier C.

## Open decisions

1. Should the value-size sweep extend past 4 KiB, into the regime where gRPC's
   measured 19–24× advantage over the HTTP transports on a 1 MiB payload starts
   to matter? It would need a gRPC row, which needs a gRPC multi-Raft binding.
2. Does the report binary belong in `examples/` (following
   `future_backend_benchmark_report`) or in `cmd/`? `examples/` matches
   precedent; `cmd/` is where a Tier C host process would have to live anyway.
3. Should the CI-registered subset drop to Tier A (the fabric) for stability, per
   Requirement 12.4's expectation, leaving Tier B to a developer-run invocation?
   The socket rows are the interesting ones, and they are also the ones a shared
   runner will perturb.

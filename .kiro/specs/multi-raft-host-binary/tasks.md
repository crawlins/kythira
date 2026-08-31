# Implementation Plan

Ordered so the shared workload seam exists before the second consumer of it,
and so the cheapest thing that proves the design — the driver agreeing with the
in-process harness at Tier B — happens before Tier C is claimed.

- [x] 1. Extract the workload seam
  - The key sampler, value construction, command mix, read-kind taxonomy,
    open/closed-loop scheduling and the statistics move behind an interface
    whose only variable part is the **submit** step
  - In-process submit is `submit_command` on a host; out-of-process submit is a
    data-path request. One function differs; nothing else may
  - The existing suite and the report generator keep working unchanged — this
    is a refactor with three consumers at the end of it, and
    `.kiro/specs/multi-raft-performance/` doctrine 115 is the reason it comes
    first
  - **Done.** `workload_target` in `tests/multi_raft_transport_harness.hpp` is
    the seam: five members, of which `submit_write` and `submit_read` are the
    only ones that differ between tiers. `run_put_workload_on` and
    `run_read_workload_on` take a target; `run_put_workload`,
    `run_read_workload` and `preload_keys` keep their signatures as thin
    adapters over `in_process_target`, so the CI suite, the report generator
    and every row definition in `multi_raft_benchmark_rows.hpp` compile
    unchanged
  - `fill_result` splits its labels: the deployment's come from
    `target.describe(row)`, the workload's from the options. That split is what
    lets one row description serve an in-process cluster and a cluster on the
    far side of a wire
  - `benchmark_result` gains `_internal_counters` and `_placement`, and the CSV
    and JSON writers emit **empty / null** for the replication and durability
    columns when the measuring process could not see them. A zero there would
    read as "no replication happened", which is a different claim from "nobody
    was counting" and the more dangerous one
  - The row *printer* moved too, to `tests/multi_raft_row_report.hpp`:
    Requirement 3.4 asks the driver to emit the same fields a Tier B row
    carries, and two printers would satisfy that on the day they were written
    and drift the week after
  - _Requirements: 4.1, 4.2_

- [x] 2. The client-facing data path
  - Put, get and delete, routed by key, over the three HTTP transports the
    Tier B matrix already sweeps, encoded through the existing serializer
    registry
  - A read-kind selector, because the three kinds differ by three orders of
    magnitude
  - **Not-leader is returned, never forwarded**, identifying the leader when
    known — forwarding would hide the routing cost Requirement 8.3 of the
    performance spec exists to measure
  - Kept separate from any control or fault-injection surface
  - **Done**, in `cmd/multi_raft_bench_common/kv_data_path.hpp` (the client and
    the wire contract) and `cmd/multi_raft_node/kv_data_server.hpp` (the host)
  - **The request body IS the state-machine command**, produced by the same
    `kv_put` / `kv_get` / `kv_del` the in-process harness submits, and the host
    forwards it to `submit_command` without parsing it. That is the strongest
    available reading of Requirement 2.3: there is exactly one encoding of the
    workload in this project rather than two that agree
  - Not-leader is 421 with the leader in a header, **never forwarded**, and the
    test's evidence that nothing was forwarded is the status code: a proxied
    request would have come back 200 with the value stored
  - The three read kinds are a query parameter. `local` is deliberately not
    checked for leadership — served by the leader a stale read is stale only in
    theory
  - Kept off the control surface by a **second listening socket** rather than a
    path prefix. A prefix would still let anything counting bytes on the data
    port see control traffic
  - _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5_

- [x] 3. `cmd/multi_raft_node`
  - One `multi_raft` per process, its own tick thread at a configurable
    cadence, every axis settable without a rebuild
  - Shards pre-split into N ranges, recorded on the row
  - **Shutdown follows `kv_cluster::shutdown()`**: stop the groups, drain the
    transport, then destroy. Getting this wrong terminates the process rather
    than failing an assertion
  - Documented as a measurement host and not a supported server; kept out of
    any default install target
  - **Done.** `--transport`, `--serializer`, `--persistence`, `--groups`,
    `--tick-interval`, `--data-threads` and the rest are all runtime, and
    `--config`/`--peers-from` read the same vocabulary from a file
  - Shutdown follows `kv_cluster::shutdown()` in order, and
    `the_data_path_starts_and_stops_repeatedly` exercises it three times per run
    because its failure mode is process termination rather than a failed
    assertion
  - `--transport proxygen` is **refused loudly** rather than silently
    substituted: its server needs a caller-owned `IOThreadPoolExecutor` and a
    shutdown sequence this binary does not implement, and a transport that
    half-works in a measurement host produces rows nobody can trust
  - Not in any install target, and the usage text says in its first line that
    this is not a supported server
  - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5, 4.4, 6.1, 6.2, 6.3_

- [x] 4. `cmd/multi_raft_bench`
  - The driver, hosting no Raft node
  - Both load modes with their controlling parameters; open loop measures from
    the **intended** start time and reports the mean schedule lag
  - Emits the same fields a Tier B row carries
  - **Done.** Both load modes come free from the shared loop, including the
    intended-start-time rule and `_mean_schedule_lag`
  - The driver hosts no Raft node and holds one `httplib::Client` per (worker,
    host): a shared client would serialise the driver's own concurrency into a
    queue and the row would report it as the cluster's latency, which is the
    exact confound this tier exists to remove
  - Leader routing is resolved **out of band** from each host's control port
    before the window and refreshed between repetitions. A not-leader answer is
    still counted in `_not_leader` and still updates the cache, so the routing
    cost stays visible rather than being hidden by a retry loop
  - _Requirements: 3.1, 3.2, 3.3, 3.4_

- [x] 5. The agreement test — the one that decides whether any of this is worth
      trusting
  - Run one row both ways at **Tier B**: the driver over loopback against an
    in-process host, and the existing in-process harness
  - Assert the ratios agree. Requirement 4.5 makes disagreement a defect in one
    of them rather than a tier effect, and without this test every cross-tier
    delta in the comparison document is unfalsifiable
  - Do this **before** claiming Tier C
  - **Done, and it passes.** `tests/multi_raft_driver_agreement_test.cpp`
  - **What is asserted is the thing that can agree**: a recording wrapper
    captures every `(key, command)` pair each path submits and the two
    sequences must match exactly. That is Requirement 4.1's property, it is
    deterministic, and it fails the moment either consumer starts building its
    own commands
  - **What is recorded rather than asserted is throughput**, and the task's
    "assert the ratios agree" is deliberately not read as an equality check on
    it: the two paths differ by an HTTP round trip *by construction*, so such an
    assertion would be either vacuous (a band wide enough to pass) or flaky (a
    band narrow enough to mean something). Measured at Tier B over beast:
    **in-process 1200.97 ops/sec at a 2902 µs p50, data path 1124.85 at 2800 µs**
    — 6% and 3.5% apart, with both completing 64 of 64
  - Also here: the not-leader case and the start/stop lifecycle case
  - _Requirements: 4.1, 4.5_

- [x] 6. Tier C rows
  - N host processes on one machine, driver on the same machine but its own
    process, static peer list
  - Every row labelled Tier C and stating, at the point of the number, whether
    that tier admits a like-for-like external comparison — it does, for a
    no-fsync number, which is the first time anything in this project has
  - **Done, and the row is real.** `scripts/run-tier-c-row.sh` starts N host
    processes and runs the driver in its own process against them
  - **A stable Tier C row: 1530.2 ops/sec, 6.1% spread, verdict `stable`, p50
    2160 µs, zero elections in all five windows** — three host processes, four
    pre-split shards, four operations in flight, beast, memory persistence, on
    a four-core development machine
  - Every row states its tier at the point of the number and now says that a
    like-for-like external comparison **is** permitted at this tier — the first
    time anything in this project has. `publishable_as_like_for_like` is no
    longer constant `false`, and its comment records what Tier C does and does
    not license: a no-fsync number, matched against a no-fsync number
  - _Requirements: 1.1, 4.3, 5.1_

- [x] 7. Lifecycle and not-leader tests
  - Start/stop the host repeatedly; the shutdown ordering is the most likely
    thing to be wrong and its failure mode is process termination
  - Assert not-leader identifies the leader and that nothing was forwarded
  - **Done**, both in `multi_raft_driver_agreement_test.cpp`
  - The lifecycle case starts and stops the whole stack three times, because
    the failure it guards against is a race and one clean pass proves very
    little about a race
  - The not-leader case waits for the follower to have *heard from* a leader
    before asserting that the answer names one. Requirement 2.2 says "if
    known", and electing is not the same event as every follower knowing —
    asserting on the first attempt would have been a flake dressed as a
    requirement. The 421 itself is required on every attempt
  - _Requirements: 1.4, 2.2_

- [x] 8. Discovery and placement for Tier E
  - Discovery that does not require every host to know every address up front,
    reusing what this project already has
  - `CLAUDE.md`'s container rules: no static IPs (rootless Podman ignores
    `ipam.config.ipv4_address` *silently*), no hardcoded `docker`, no
    privileged networking. Verified under both runtimes
  - **Done under both of the two networking models available here, and Docker's
    own engine is still not one of them.** `docker/multi-raft-tier-e-compose.yml`
    and `docker/multi_raft_node/`, verified end to end twice:
    - **rootless Podman 4.9.3 / podman-compose 1.0.6** — slirp4netns port
      publishing, aardvark-dns. Three containers, every group led, a row
      measured from a driver outside every container.
    - **rootful Podman** — netavark bridge, port publishing through the host's
      real netfilter rules, aardvark-dns. Architecturally the same shape as
      Docker's rootful bridge with its embedded DNS, which is the runtime
      CLAUDE.md names as CI's default. Three containers reported `healthy`,
      every group elected, and a row was measured from outside all of them.
  - **What that does and does not settle.** The rule exists because rootless
    Podman *silently ignores* things Docker honours, and the compose file uses
    none of them: no `ipam`/`ipv4_address` anywhere, service names for
    addressing, no `cap_add`, no privileged networking, no runtime hardcoded.
    Rootful Podman exercises the permissive networking model the rule is
    written against. What is still unverified is Docker's **engine** and its
    own `docker compose` implementation, and CI settles that
  - **Running it rootful found a defect the rootless run had hidden.** Podman
    defaults to the OCI image format, which has no healthcheck field: it warns
    "HEALTHCHECK is not supported for OCI image format and will be ignored"
    among the build noise and hands back a working image with no health check
    in it. The compose stack's readiness gate *is* that health check, so the
    stack came up `running` and never `healthy` and nothing reported an error.
    `scripts/build-tier-e-image.sh` now passes `--format docker` for Podman —
    Docker's builder emits that format by default and does not take the flag —
    and then **verifies the check survived** rather than trusting that it did,
    refusing to hand over an image that would fail silently
  - Discovery is the static peer list (Requirement 5.1), supplied by the
    compose file through the entrypoint. Requirement 5.2's "does not require
    every host to know every address in advance" is **not delivered**; see the
    task 10 note
  - _Requirements: 5.2, 5.3_

- [x] 9. Inter-node RTT and bandwidth, before the window
  - Measured and reported **before** the measured phase, with the placement
  - Task 11 of the performance spec established that the inter-round interval
    tracks the RPC round trip, so this is the axis most likely to explain a
    Tier E result — a cluster number without it is not reproducible
  - **Done.** Each host's control port serves `/probe`, which measures round
    trip and bandwidth to every peer's control port and returns them as JSON,
    and `run-tier-c-row.sh` calls it on every host **before** the driver starts
  - Round trip is the **median** of eleven samples, not the mean: one
    scheduling hiccup moves a mean and does not move a median, and a network
    figure a single outlier can move is not one to size a cluster from.
    Bandwidth is a real 1 MiB round trip through `/echo` rather than a latency
    multiplied by an assumed MTU
  - A peer with no `--peer-control` address is reported with **null** figures
    and a reason, never guessed at. Deriving a control port from the Raft URL by
    convention would silently probe whatever happened to be listening there
  - **It earned its place on its first run.** The probe read a 41 ms median RTT
    on loopback, which is not a network — it is Nagle's algorithm meeting the
    peer's delayed-ACK timer, because cpp-httplib defaults
    `CPPHTTPLIB_TCP_NODELAY` to false. Setting it on this spec's own sockets
    took the probe to 0.5–0.9 ms
  - _Requirements: 5.4_

- [x] 10. Tier E rows, and the handover
  - N machines or containers; report the placement and the network with every
    row
  - Hand Tiers C and E back to `.kiro/specs/multi-raft-performance/` tasks 20
    and 23: its like-for-like table can stop being empty for the no-fsync
    metrics, and its tier table can stop recording C and E as undelivered
  - **Tier D is still blocked** on `.kiro/specs/durable-append-barrier/` and
    this task must not claim otherwise
  - **Tier E rows: taken under both runtimes, and NOT claimed as publishable.**
    The placement is on each row verbatim, and both are over the 10% bar, so
    neither may enter a comparison table:
    - rootless Podman, driver outside every container: **335.4 ops/sec, 11.8%
      spread, p50 11.1 ms, zero elections in all five windows, 200 of 200**
    - rootful Podman, netavark bridge: **408.7 ops/sec, 84.5% spread, p50
      8.6 ms, one of five windows contained an election, 200 of 200**
    The second is far noisier, and the row says why rather than the reader
    having to guess: the machine was building at the time and one window
    contained an election. Neither number is evidence about the two networking
    models — they were not taken under comparable load, and saying so is
    cheaper than a comparison nobody could rely on
  - **This is not N machines.** Requirement 5.5's "several machines" is
    unreached: there is one machine here, and containers on it share a kernel,
    a scheduler and a memory bus. The row says `Tier E` because the hosts are
    in containers, and a reader should discount it accordingly. A real Tier E
    number needs the cloud harness `scripts/perf-cloud/` already provides for
    single instances, extended to N
  - **Tier D is still blocked, and this task does not claim otherwise** —
    except that its other blocker is gone: `.kiro/specs/durable-append-barrier/`
    landed, so a Tier C host with `--persistence file-barrier` is durable.
    Tier D is now a matter of running the rows, not of building anything
  - Handed back to `.kiro/specs/multi-raft-performance/` tasks 20 and 23
  - _Requirements: 3.5, 4.3, 5.4_

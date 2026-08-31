# Implementation Plan

Ordered so the shared workload seam exists before the second consumer of it,
and so the cheapest thing that proves the design — the driver agreeing with the
in-process harness at Tier B — happens before Tier C is claimed.

- [ ] 1. Extract the workload seam
  - The key sampler, value construction, command mix, read-kind taxonomy,
    open/closed-loop scheduling and the statistics move behind an interface
    whose only variable part is the **submit** step
  - In-process submit is `submit_command` on a host; out-of-process submit is a
    data-path request. One function differs; nothing else may
  - The existing suite and the report generator keep working unchanged — this
    is a refactor with three consumers at the end of it, and
    `.kiro/specs/multi-raft-performance/` doctrine 115 is the reason it comes
    first
  - _Requirements: 4.1, 4.2_

- [ ] 2. The client-facing data path
  - Put, get and delete, routed by key, over the three HTTP transports the
    Tier B matrix already sweeps, encoded through the existing serializer
    registry
  - A read-kind selector, because the three kinds differ by three orders of
    magnitude
  - **Not-leader is returned, never forwarded**, identifying the leader when
    known — forwarding would hide the routing cost Requirement 8.3 of the
    performance spec exists to measure
  - Kept separate from any control or fault-injection surface
  - _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5_

- [ ] 3. `cmd/multi_raft_node`
  - One `multi_raft` per process, its own tick thread at a configurable
    cadence, every axis settable without a rebuild
  - Shards pre-split into N ranges, recorded on the row
  - **Shutdown follows `kv_cluster::shutdown()`**: stop the groups, drain the
    transport, then destroy. Getting this wrong terminates the process rather
    than failing an assertion
  - Documented as a measurement host and not a supported server; kept out of
    any default install target
  - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5, 4.4, 6.1, 6.2, 6.3_

- [ ] 4. `cmd/multi_raft_bench`
  - The driver, hosting no Raft node
  - Both load modes with their controlling parameters; open loop measures from
    the **intended** start time and reports the mean schedule lag
  - Emits the same fields a Tier B row carries
  - _Requirements: 3.1, 3.2, 3.3, 3.4_

- [ ] 5. The agreement test — the one that decides whether any of this is worth
      trusting
  - Run one row both ways at **Tier B**: the driver over loopback against an
    in-process host, and the existing in-process harness
  - Assert the ratios agree. Requirement 4.5 makes disagreement a defect in one
    of them rather than a tier effect, and without this test every cross-tier
    delta in the comparison document is unfalsifiable
  - Do this **before** claiming Tier C
  - _Requirements: 4.1, 4.5_

- [ ] 6. Tier C rows
  - N host processes on one machine, driver on the same machine but its own
    process, static peer list
  - Every row labelled Tier C and stating, at the point of the number, whether
    that tier admits a like-for-like external comparison — it does, for a
    no-fsync number, which is the first time anything in this project has
  - _Requirements: 1.1, 4.3, 5.1_

- [ ] 7. Lifecycle and not-leader tests
  - Start/stop the host repeatedly; the shutdown ordering is the most likely
    thing to be wrong and its failure mode is process termination
  - Assert not-leader identifies the leader and that nothing was forwarded
  - _Requirements: 1.4, 2.2_

- [ ] 8. Discovery and placement for Tier E
  - Discovery that does not require every host to know every address up front,
    reusing what this project already has
  - `CLAUDE.md`'s container rules: no static IPs (rootless Podman ignores
    `ipam.config.ipv4_address` *silently*), no hardcoded `docker`, no
    privileged networking. Verified under both runtimes
  - _Requirements: 5.2, 5.3_

- [ ] 9. Inter-node RTT and bandwidth, before the window
  - Measured and reported **before** the measured phase, with the placement
  - Task 11 of the performance spec established that the inter-round interval
    tracks the RPC round trip, so this is the axis most likely to explain a
    Tier E result — a cluster number without it is not reproducible
  - _Requirements: 5.4_

- [ ] 10. Tier E rows, and the handover
  - N machines or containers; report the placement and the network with every
    row
  - Hand Tiers C and E back to `.kiro/specs/multi-raft-performance/` tasks 20
    and 23: its like-for-like table can stop being empty for the no-fsync
    metrics, and its tier table can stop recording C and E as undelivered
  - **Tier D is still blocked** on `.kiro/specs/durable-append-barrier/` and
    this task must not claim otherwise
  - _Requirements: 3.5, 4.3, 5.4_

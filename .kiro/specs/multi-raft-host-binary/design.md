# Design Document

## Overview

Two new binaries and one shared library seam:

- `cmd/multi_raft_node` — hosts one `multi_raft`, serves a client-facing
  key-value data path.
- `cmd/multi_raft_bench` — the driver, in its own process, offering load and
  measuring latency.
- A workload seam both the driver and the existing in-process harness use, so a
  Tier C row and a Tier B row differ in tier and nothing else.

The third item is the one that decides whether this effort is worth anything.

## Why the driver is its own process

Appendix B's question 2, answered. In-process is cheaper to build and produces
numbers that cannot be attributed: the load generator's key sampling, value
construction, latency bookkeeping and future settlement all run on the same
cores as the host's tick, transport and consensus. At sixteen operations in
flight on four cores that is not a rounding error.

The cost is a client-facing data path, which does not exist today and is most of
this spec's work. That is the honest price of Tier C.

## The workload seam, and the trap it avoids

`.kiro/specs/multi-raft-performance/` already learned this once, in its own
doctrine: *extract the measurement before writing the second consumer, not
after.* `tests/multi_raft_benchmark_rows.hpp` owns the cluster shape, budgets,
warm-up rule and repetition loop, and the CI suite and the report generator
differ only by a three-callback `row_observer`.

This spec adds a **third** consumer, and the same rule applies with more force,
because this one runs in a different process against a different transport. The
parts that must be shared, not reimplemented:

- the key sampler (uniform and Zipfian) and its parameters
- value construction and size handling
- the command mix and the read-kind taxonomy
- open- and closed-loop scheduling, including the intended-start-time rule
- `latency_sample_set`, `repeated_result`, the spread rule and `verdict()`

What legitimately differs is only the **submit** step: in-process it is
`submit_command` on a host; out-of-process it is a request over the data path.
That is one function, and it is the seam.

If a future reader finds the driver generating its own keys, this design has
failed and every cross-tier delta it produced is a comparison of two workloads.

## The data path

A request surface distinct from `chaos_node`'s control plane (Requirement 2.4),
carried over the same three HTTP transports the Tier B matrix sweeps, encoded
with the existing serializer registry (Requirement 2.3).

Three operations — put, get, delete — plus a read-kind selector, because
`read_state`, a `GET` through the log and a local stale read differ by three
orders of magnitude and a single default would make the read rows meaningless
(Requirement 2.5).

**Not-leader is returned, never forwarded** (Requirement 2.2). Forwarding would
hide the routing cost inside the cluster, and routing cost is a thing this
project measures deliberately: `.kiro/specs/multi-raft-performance/` Requirement
8.3 exists to price it, and four runs across three machines have bounded it at
≤22.1 µs. A cluster that silently forwards would make that measurement
impossible to repeat at Tier C.

## Configuration

Command line or file, never a rebuild (Requirement 1.2). The axes the
performance spec sweeps must all be settable: transport, wire serializer, tick
cadence, group count, node count, persistence engine, and the shard split.

Shards are **pre-split into N ranges** (Requirement 4.4), answering Appendix B's
question 4 the reproducible way and matching what `kv_cluster` already does, so
Tier C and Tier B tile the key space identically. Growth by automatic split is
what a real deployment does and is a different measurement; the row records
which it got.

## Lifecycle

The ordering `tests/multi_raft_transport_harness.hpp` documents is not optional
and is easy to get wrong in a `main()`: stop the groups, drain the transport,
then destroy. A host destroyed with a group still running terminates the
process, because `~group_state` destroys unstopped nodes through a deferred
closure's reference. `kv_cluster::shutdown()` is the reference implementation
and its comments explain each step; the binary should follow it rather than
rediscover it.

## Tier E, and what it adds

Tier C is N host processes on one machine. Tier E is the same processes on N
machines or containers, which adds:

- **Discovery** (Requirement 5.2), reusing what this project has rather than
  adding a mechanism.
- **`CLAUDE.md`'s container rules** (Requirement 5.3): no static IPs in compose
  files — rootless Podman ignores `ipam.config.ipv4_address` silently — no
  hardcoded `docker`, no privileged networking.
- **An RTT and bandwidth measurement before the measured window**
  (Requirement 5.4). Before, not after, and reported with the placement: a
  cluster number without the network between its nodes is not reproducible, and
  `.kiro/specs/multi-raft-performance/` task 11 established that the
  inter-round interval tracks the RPC round trip, so this is the axis most
  likely to explain a Tier E result.

## What this unblocks, and what it does not

Unblocks Tier C outright, and Tier E once the placement work lands. It does
**not** unblock Tier D: a Tier C host with a file-backed log is still not
durable, because the barrier is in the wrong place. That is
`.kiro/specs/durable-append-barrier/`, and Tier D needs both specs.

## Testing strategy

- **The driver and the in-process harness must agree.** Run a row both ways at
  Tier B — the driver against an in-process host over loopback — and assert the
  ratios match. Requirement 4.5 makes disagreement a defect rather than a tier
  effect, and this is the test that enforces it.
- **A lifecycle test** that starts and stops the host repeatedly, since the
  shutdown ordering is the thing most likely to be got wrong and its failure
  mode is process termination rather than a failed assertion.
- **A not-leader test**, asserting the response identifies the leader and that
  nothing was forwarded.
- **The container rules**, verified by running the Tier E compose under both
  Docker and rootless Podman, as `CLAUDE.md` requires of every compose file.

## Cost

This is the largest piece of new code the multi-raft-performance spec has
called for, and most of it is the data path rather than the host. Worth stating
plainly so the estimate is not made from the host alone, which is the small
half.

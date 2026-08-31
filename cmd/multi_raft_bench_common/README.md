# `multi_raft_bench_common`

Shared by `cmd/multi_raft_node` (which serves the data path),
`cmd/multi_raft_bench` (which offers load to it) and
`tests/multi_raft_driver_agreement_test.cpp` (which proves the two agree).

Written for `.kiro/specs/multi-raft-host-binary/`.

## Why any of this exists

Every performance row this project has produced is Tier A or Tier B: every host
inside one process, and the load driver inside that same process.
`.kiro/specs/multi-raft-performance/` Requirement 3.3 forbids a like-for-like
comparison against an external number from any tier below C, so its comparison
document's like-for-like table is empty and says so.

Tiers C, D and E are all blocked on the same missing thing — a process that
hosts `multi_raft` and accepts client traffic. That is what these two binaries
are.

## The rule that decides whether it was worth building

**One workload, two submit steps.** The key sampler, value construction, the
command mix, the read-kind taxonomy, both load modes with the
intended-start-time correction, `latency_sample_set`, `repeated_result`, the
spread rule and `verdict()` all live in `tests/multi_raft_kv_workload.hpp` and
`tests/multi_raft_transport_harness.hpp` and are shared unchanged. What differs
between an in-process row and an out-of-process one is `data_path_target`, and
nothing else may.

If you find either binary sampling a key, building a value or computing a
percentile, this design has failed and every cross-tier delta it produced is a
comparison of two workloads rather than of two tiers.
`tests/multi_raft_driver_agreement_test.cpp` asserts the identity directly:
the same seed through both targets must offer the same sequence of commands.

## What is not here, and why

- **No authentication, authorisation, multi-tenancy or rate limiting.** Each
  would add cost to the measured path that every number would then have to be
  corrected for, and none of them is what is being measured
  (`.kiro/specs/multi-raft-host-binary/` Requirement 6.2).
- **Neither binary is in any install target.** The surest way to keep a
  measurement host from being deployed is for `cmake --install` never to put it
  anywhere (Requirement 6.3).
- **Not-leader is returned, never forwarded.** Forwarding would move the
  routing cost inside the cluster, where no client-side measurement can see it,
  and this project prices routing deliberately (Requirement 2.2).
- **The host counts nothing about itself.** `.kiro/specs/multi-raft-performance/`
  Requirement 8.2 keeps measurement counters out of the measured process, so a
  Tier C row's replication and durability columns are **absent** rather than
  zero. A zero there would read as "no replication happened".

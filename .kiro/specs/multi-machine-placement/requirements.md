# Requirements Document

## Introduction

`.kiro/specs/multi-raft-host-binary/` delivered Tier C — one host process per
node, a driver in its own process — and its first row is stable: **1530.2
ops/sec at a 6.1% spread, zero elections in five windows**. It also produced
Tier E rows, and those rows are the reason this spec exists.

They were taken on three rootless-Podman containers on one machine. The
inter-node probe measured a **0.7–0.9 ms** round trip between them — which is
the same number loopback gives, because it *is* loopback. Containers on one host
share a kernel, a scheduler, a memory bus and a network stack. The row was
labelled Tier E because the hosts were in containers, and
`.kiro/specs/multi-raft-host-binary/` task 10 says plainly that this is not N
machines and that a reader should discount it.

So the one thing Tier E exists to introduce — **a real network between the
nodes** — is still absent from every row this project has ever produced.

That matters more than a missing row. `.kiro/specs/multi-raft-performance/` task
11 established that the per-stream inter-round interval tracks the RPC round
trip, and measured it at 0.75–0.91 ms on one cloud machine and 1.89–2.26 ms on
the development machine. Every one of those is a loopback number. A real network
is 10–100x larger, and response-driven pacing predicts the round interval follows
it. That prediction has never been tested, and it is the mechanism behind two of
this project's open questions.

It also blocks the comparison the whole performance spec is pointed at. Every
external number in `doc/data/multi_raft_external_comparison_register.json` was
taken on a multi-machine cluster. Tier C cleared Requirement 3.3's tier bar, but
a Tier C row is one machine's loopback; the closest thing this project has to
what etcd and Dragonboat published is a Tier E row on real machines.

**Scope**: standing up N `multi_raft_node` processes on N cloud instances with a
driver on its own, discovering each other without a hand-edited file, measuring
the network before the window, and tearing the whole thing down without leaking.

**Out of scope**: durability (that is Tier D, and needs only a volume and a
run), any change to the host or driver binaries beyond configuration, any change
to Raft or multi-Raft, and a second cloud provider for this shape — AWS first,
and the design says what generalises.

**Explicitly out of scope: making the number good.** A real network will make
every latency figure worse than the loopback ones this project has been
quoting. That is the point. A Tier E row that looked like a Tier C row would
mean the network was not real.

## Glossary

- **Shape 1** — the existing single-instance cloud row.
  `scripts/perf-cloud/run-aws-shape-1.sh`.
- **Shape 2** — N instances, one host per instance, driver on its own. What this
  spec builds. The name is `.kiro/specs/multi-raft-performance/` task 20's.
- **Placement** — where the instances are relative to each other: one
  availability zone, several, or a cluster placement group. An axis, not a
  detail.
- **Run tag** — the run-scoped tag every resource carries so the audit can find
  it. `perf-local-<epoch>-<pid>` today.
- **Leak** — a resource that outlives the run that created it.

## Requirement 1 — N instances, one host each, and the driver on its own

**User Story:** As a maintainer, I want the load driver's CPU on a machine that
hosts no replica, so that a Tier E throughput number is the cluster's and not an
average of the cluster and its client.

### Acceptance Criteria

1. WHEN Shape 2 runs THEN the system SHALL launch **N + 1** instances: N hosts
   and one driver, with N configurable and defaulting to 3
2. WHEN the driver instance is chosen THEN the system SHALL NOT place it on an
   instance that runs a host, and SHALL record its instance type separately from
   the hosts', since a driver that cannot offer the load makes the cluster look
   fast
3. WHEN the binaries are deployed THEN the system SHALL **ship the prebuilt
   binaries** rather than building on the instances, for the reason doctrine 134
   states: a rebuild folds a different compiler and a different dependency
   resolution into the delta the tier exists to isolate. IF the target
   architecture has no build host THEN the system SHALL record
   `binary_origin` on the row as Shape 1 already does
4. WHEN a host is configured THEN the system SHALL pass it the same options a
   Tier C host takes, so that the only difference between a Tier C row and a
   Tier E row is where the processes are
5. WHEN the run finishes THEN the system SHALL pull back every host's log and
   the driver's artifacts, because a Tier E failure is diagnosed from the logs
   of the machine that failed and there is no second chance to collect them

## Requirement 2 — Placement is chosen, recorded, and swept

**User Story:** As a reader, I want to know whether the nodes were metres or
kilometres apart, because that is the difference between the numbers this row
exists to produce.

### Acceptance Criteria

1. WHEN instances are launched THEN the system SHALL support at least: **one
   availability zone**, **several availability zones**, and **a cluster
   placement group**, chosen at run time
2. WHEN a row is emitted THEN the system SHALL record the placement, the region,
   and every instance's availability zone **verbatim on the row**, not in a
   commit message — `.kiro/specs/multi-raft-host-binary/` Requirement 3.5
   already makes placement a row field and this populates it
3. WHEN a cluster placement group is used THEN the system SHALL delete it during
   teardown. **The audit already checks for one** — `audit-aws-leaks.sh` has a
   placement-group class with a comment saying Tier E's shape uses one and that
   a leaked group holds a name and will collide with the next run. Shape 1 has
   never created one, so that check has never fired; this is the change that
   makes it load-bearing rather than anticipatory
4. IF two placements are compared THEN the system SHALL hold every other
   configuration value identical, per `.kiro/specs/multi-raft-performance/`
   Requirement 11 — a placement comparison that also moved the instance type is
   two changes and no finding

## Requirement 3 — The nodes find each other without a hand-edited file

**User Story:** As an operator standing up more than three nodes, I want to
launch instances and have them form a cluster, rather than collecting IP
addresses and editing a command line N times.

### Acceptance Criteria

1. WHEN Tier E starts THEN the system SHALL support discovery that does not
   require every host to know every address in advance —
   `.kiro/specs/multi-raft-host-binary/` Requirement 5.2, which that spec
   delivered as a static peer list only and recorded as not delivered
2. WHEN discovery is implemented THEN the system SHALL **reuse a mechanism this
   project already has** rather than adding one. `include/raft/peer_discovery.hpp`
   defines the concept — `register_node` and `find_peers` — and the tree already
   holds `rfc1035_peer_discovery`, `rfc6763_peer_discovery`,
   `rfc6763_ldns_peer_discovery`, `rfc2136_dns_sd_discovery` and
   `poco_peer_discovery`
3. WHEN the cloud's own inventory is used instead THEN the system SHALL say so
   and SHALL note that `aws_ec2_quorum_manager` already models instance
   discovery by tag, so a tag-scan back-end is a small implementation of an
   existing concept rather than a new subsystem
4. WHEN discovery is used THEN the **static peer list SHALL remain supported and
   SHALL remain the default for a measured row**. Discovery that runs inside a
   measured window is a cost in that window, and a row that cannot say whether
   it paid it is not reproducible
5. IF discovery fails to converge within a stated budget THEN the system SHALL
   abort the run and say which hosts were never seen, rather than measuring a
   cluster with a missing replica and reporting a number for it

## Requirement 4 — The network is measured before the window, and it is the point

**User Story:** As a reader of a Tier E number, I want the network it was taken
over stated alongside it, because on this tier the network is the independent
variable rather than the environment.

### Acceptance Criteria

1. WHEN a measured window opens THEN the system SHALL already have measured
   inter-node round-trip time and bandwidth **between every ordered pair of
   hosts** and recorded them with the row —
   `.kiro/specs/multi-raft-performance/` Requirement 18.8, and the host's
   `/probe` endpoint from `.kiro/specs/multi-raft-host-binary/` task 9 already
   produces exactly this
2. WHEN round-trip time is reported THEN the system SHALL report the **median**
   of at least eleven samples rather than the mean, for the reason the existing
   probe states: one scheduling hiccup moves a mean and a network figure a
   single outlier can move is not one to size a cluster from
3. WHEN a Tier E row is published THEN the system SHALL state the measured RTT
   **next to the latency figure**, so a reader can see what fraction of a
   committed operation was time on the wire
4. WHEN the row is read against a Tier C row THEN the system SHALL make the
   comparison available as its own artifact: the same workload, the same
   binaries, the same cluster shape, differing in placement alone. That delta
   **is the deliverable**, more than either number on its own
5. IF the measured inter-node RTT is within an order of magnitude of loopback
   THEN the system SHALL treat the row as a container-shaped row rather than a
   multi-machine one and SHALL say so, because that is the failure the
   Podman rows already demonstrated and it is silent

## Requirement 5 — It costs money, and nothing is left running

**User Story:** As the person paying for it, I want a pre-registered estimate, a
hard ceiling, and evidence that nothing survived the run.

### Acceptance Criteria

1. WHEN a run is planned THEN the system SHALL state its expected cost
   **before** it runs, as Shape 1 does — its two AWS rows were pre-registered at
   $0.11–$0.13 and came in at $0.09 and $0.34
2. WHEN a run executes THEN the system SHALL enforce a hard ceiling on the
   measured phase, and the ceiling SHALL be sized for **N + 1 instances for the
   whole run**, not for one instance for one repetition. Doctrine 135: a
   dead-man switch sized for the fast path is not a safety net
3. WHEN each instance boots THEN the system SHALL arm an **instance-local**
   dead-man switch — the cloud-init `shutdown -h +N` plus
   `instance-initiated-shutdown-behavior=terminate` Shape 1 already uses — since
   it is the only ceiling that survives the controlling machine dying, and
   Shape 2 has N+1 instances that could be orphaned instead of one
4. WHEN the run ends by any path THEN teardown SHALL fire from an **EXIT trap**
   rather than a trailing block, because the failure paths are where instances
   are left running and Shape 1 learned that from a run killed by hand
5. WHEN teardown completes THEN the system SHALL run a leak audit over **every**
   resource class the run could have created. The six classes `audit-aws-leaks.sh`
   already covers — instances, volumes, elastic IPs, security groups, placement
   groups, key pairs — are the right set; what changes is that every one of them
   must be checked for **N+1 instances' worth** of resources rather than one's,
   and that the placement-group class stops being hypothetical
6. IF the audit cannot enumerate a resource class THEN it SHALL exit non-zero
   rather than reporting clean, because `.kiro/specs/multi-raft-performance/`
   task 21 found `gcloud compute list` exiting 0 with an authentication failure
   and four of five queries "succeeding" blind

## Requirement 6 — What this row may and may not be compared to

**User Story:** As a reader, I want to know whether this is finally the number
that can sit beside etcd's, and I want the answer to be honest.

### Acceptance Criteria

1. WHEN a Tier E row is emitted THEN the system SHALL label it Tier E and state
   at the point of the number whether that tier admits a like-for-like external
   comparison
2. WHEN a like-for-like claim is made THEN the system SHALL match **durability
   as well as tier**. A Tier E row on `memory` persistence is comparable to an
   external no-fsync number and to nothing else; the durable comparison needs
   Tier D, which is a volume and a run away and is not this spec
3. WHEN the comparison document is updated THEN the system SHALL fill the
   like-for-like table **only for the metrics the row actually supports**,
   discharging Requirement 9.8 metric by metric as that document already does
   rather than once in a preamble
4. WHEN concurrency differs from an external source's THEN the system SHALL say
   so beside the number. The standing example is etcd's 1,000 clients against
   this suite's 16, which `doc/multi_raft_performance_comparison.md` already
   names as probably dominating that ratio
5. IF a row is unstable by the spread rule THEN it SHALL NOT enter a comparison
   table however expensive it was to take. Requirement 6.3 does not have a
   cost exemption, and a Tier E row costs real money — which is a reason to
   budget enough repetitions, not a reason to quote a bad one

# Implementation Plan

**All ten tasks are complete.** Five Tier E rows were taken on real machines
across three placements, plus a same-hardware Tier C row for the delta. Two of
the spec's own assumptions did not survive contact with the measurement, and
both are recorded against the task that found them: Requirement 4.5's
order-of-magnitude threshold calls three availability zones "container-shaped"
(task 4), and Requirement 2.4's "hold everything else identical" is not
achievable on EC2, because `c5.2xlarge` is not one machine (task 7).

Ordered so that nothing bills until the thing that stops it billing has been
tested, and so that the cheapest task that could invalidate the design — the
security-group rule that lets nodes reach each other at all — happens before
anything is measured.

**Task 1 must fail the audit on purpose.** An audit that has only ever been seen
to pass is not evidence, and `.kiro/specs/multi-raft-performance/` task 21 found
one reporting clean while blind.

- [x] 1. The leak audit, widened and tested in both directions
  - **Do not add a placement-group class: `audit-aws-leaks.sh` already has one**,
    with a comment saying Tier E's shape uses it and that a leaked group holds a
    name and collides with the next run. Shape 1 never created one, so that
    check has never fired. This is the change that makes it load-bearing
  - Widen all six existing classes — instances, volumes, elastic IPs, security
    groups, placement groups, key pairs — from "the instance" to "every resource
    carrying this run tag", since Shape 2 creates N+1 instances' worth
  - **Deliberately leak one resource of each class and confirm the audit exits
    non-zero.** Do this before anything else, because every later task depends
    on teardown being checkable
  - Keep the rule that an audit which cannot enumerate a class fails rather than
    reporting clean
  - **DONE.** The audit needed no widening: it queries by tag, never by id, so
    it was already N+1-wide by construction. What it needed was a test, and
    that is `scripts/perf-cloud/test-audit-aws-leaks.sh` — stub mode (14/14,
    no credentials, each of the six classes leaked ON ITS OWN so a class
    dropped from the loop cannot hide behind the others) and `--live` (real
    resources, only the three AWS does not bill for).
  - **The IAM policy had the same anticipatory gap the audit check had.**
    `perf-cloud.json` granted `ec2:DescribePlacementGroups` — the audit's read
    — and not `ec2:CreatePlacementGroup`. Shape 1 never created one, so the
    write was never needed and never noticed. Both actions added, and the
    account's `kythira-dev-perf-cloud` policy updated to match (v3).
  - With that granted, the live test passes 19/19 and **the placement-group
    class fired for the first time** — created, detected, deleted, re-audited
    clean. It is load-bearing now rather than anticipatory.
  - _Requirements: 5.5, 5.6, 2.3_

- [x] 2. Provision N+1 instances, and open the port between them
  - One security group, one key pair, N+1 instances, all run-tagged
  - **The security group needs a rule Shape 1 never needed**: the Raft port,
    between the instances. Shape 1 allows SSH from the controller and nothing
    else, so this is the first thing that will silently produce a cluster that
    never elects
  - Instance-local dead-man switch on **every** instance, sized for the whole
    run rather than one repetition (doctrine 135)
  - Teardown from an EXIT trap, so the failure paths are covered
  - `--dry-run` that provisions nothing and prints what it would do
  - **DONE.** `scripts/perf-cloud/run-aws-shape-2.sh`. The security-group rule
    Shape 1 never needed is three ingress rules whose source is THE GROUP
    ITSELF, which is exactly this run's N+1 and needs no addresses that do not
    exist yet. Dead-man switch on every instance sized for the whole run;
    teardown from an EXIT trap; `--dry-run` provisions nothing and prints both
    generated command lines.
  - Verified across seven live provisionings: every one torn down, every audit
    clean, and an independent account-wide sweep afterwards found nothing.
    Two of those runs were killed by hand mid-flight and the EXIT trap still
    tore them down.
  - _Requirements: 1.1, 5.2, 5.3, 5.4_

- [x] 3. Deploy and start, without the two mistakes Shape 1 already made
  - **Ship the prebuilt binaries** (doctrine 134); record `binary_origin`
  - Capture provenance **before** anything expensive, not after (doctrine 145)
  - Start hosts under `systemd-run`, never `ssh host "cmd &"` — a backgrounded
    process holds the ssh channel open regardless of its own redirections
    (doctrine 136), and the controlling side polls a state artifact instead
  - Wait on each host's `/ready`, which already means "every group has a
    leader" rather than "the process is up"
  - **DONE.** Binaries shipped, never rebuilt; provenance captured on every
    instance before any binary moves; hosts started under `systemd-run`;
    readiness polled on `/ready`.
  - **Stripping the binaries first was not cosmetic** — 86 MB and 42 MB became
    35 MB and 10 MB, and the deploy is the longest step of the whole run.
  - _Requirements: 1.3, 1.4_

- [x] 4. Probe the network before the window, every ordered pair
  - `/probe` already measures RTT and bandwidth to each peer and already reports
    the median of eleven samples; this calls it on every host and records the
    **matrix**, not an average — a cross-AZ cluster is not symmetric and an
    average hides which link is slow
  - **Assert the sanity check Requirement 4.5 asks for**: if the measured RTT is
    within an order of magnitude of loopback, this is a container-shaped row and
    must say so. That is the exact failure the Podman rows demonstrated, and it
    is silent
  - **DONE, and it refuted Requirement 4.5's threshold.** `/probe` gives the
    matrix; a curl-measured loopback baseline on the same host gives 4.5 its
    reference, because /probe deliberately skips self and changing that would
    be a host-binary change this spec puts out of scope.
  - Measured loopback is **51–69 µs** across six provisionings — a stable
    instrument. Inter-node is 120–222 µs same-AZ and **469 µs across three
    availability zones**, which is 9.20x loopback and therefore still
    "container-shaped" by the requirement's order-of-magnitude rule. Three
    machines in three zones is not container-shaped; **the constant is wrong,
    not the ratio.** ~5x separates same-AZ from cross-AZ on this evidence.
  - The premise the threshold rests on is also wrong: the Podman rows'
    0.7–0.9 ms was never a network measurement. This project's own loopback on
    a c5.2xlarge is 51–69 µs, so that figure was a slow machine and
    cpp-httplib's per-request overhead. Comparing absolute RTTs across machines
    conflates machine speed with distance.
  - _Requirements: 4.1, 4.2, 4.5_

- [x] 5. The first Tier E row on real machines
  - One availability zone, three hosts, driver on its own instance, five
    repetitions, the same workload a Tier C row uses
  - Record placement, region, every zone, the RTT matrix and the driver's own
    instance type on the row
  - **Expect it to be slower than Tier C, and say so before running it.** A
    Tier E row that resembled the Tier C row would mean the network was not real
  - **DONE.** Tier E, one availability zone, three hosts and a driver on four
    `c5.2xlarge`: **1037.9 ops/sec, 6.5% spread, stable over 11 windows, zero
    elections**, p50 3656.2 µs, inter-node RTT 171 µs.
  - It was predicted before running that Tier E would be slower than Tier C,
    and it is: 1037.9 against 1175.4 on the same instance type, an 11.7% cost
    for putting a network between the nodes.
  - _Requirements: 1.2, 1.5, 2.2, 4.3_

- [x] 6. The delta that is the actual deliverable
  - The same workload and the same binaries at Tier C and Tier E, differing in
    placement alone, published as one artifact
  - Read the round interval against the measured RTT. Task 11's
    response-driven-pacing hypothesis predicts the interval tracks the round
    trip; on loopback that was untestable because the round trip barely varied.
    **This is the first chance to falsify it rather than fit it**
  - Report what the delta says about the hypothesis, including if it says the
    hypothesis is wrong
  - **DONE, and the hypothesis survived a real test.** `--also-tier-c` takes
    the Tier C row ON THE DRIVER INSTANCE of the same run, so the two rows
    differ in placement and not in hardware, binaries or workload. A Tier C row
    from a development machine would have differed in the CPU too and the delta
    would have said nothing about placement.
  - Reading p50 against measured RTT, with the Tier C row as origin: one AZ
    gives **4.39** round trips per operation, three AZs gives **4.18** — two
    independent placements agreeing across a 3.5x difference in RTT, on a
    prediction made before either was measured. That is a mechanism, not a fit,
    and it is a plausible count: client to leader, leader to a follower
    majority, and the redirect a driver pays when it does not hold the leader.
  - **The cluster placement group cannot be read this way and the report does
    not try.** Its observations sit 66–168 µs from loopback and over that short
    a lever the same arithmetic returns 19.98 and 2.49 — noise. Within the
    120–222 µs band the placement effect is below this apparatus's resolution.
  - _Requirements: 4.4_

- [x] 7. Placement as a swept axis
  - The same row across one AZ, several AZs, and a cluster placement group,
    holding everything else identical (Requirement 11 of the performance spec)
  - Three rows, three RTT matrices, one table
  - Cost check against the pre-registered estimate, as Shape 1 does
  - **DONE for the measurements; the comparison is weaker than the spec
    assumed, and the report says so.** Five Tier E rows: cluster placement
    group three times (891.5 / 989.6 / 1049.9 ops/sec at 120 / 222 / 126 µs),
    one AZ (1037.9 at 171 µs), three AZs (775.0 at 469 µs).
  - **Requirement 2.4 could not be satisfied, and provenance is what caught
    it.** `c5.2xlarge` is not one machine: the hosts drew a mix of Xeon
    Platinum 8124M and 8275CL, varying between runs AND within a run. A Raft
    commit waits on a majority, so a mixed cluster is gated by its slowest
    member and the throughput column carries a hardware term the sweep did not
    choose.
  - **Provisioning is itself a variable.** The cluster group was run three
    times with everything identical and returned 120, 222 and 126 µs and
    891.5, 989.6 and 1049.9 ops/sec. The spread rule governs windows within a
    run; nothing repeats a run, and on this evidence it should. One of the
    three was UNSTABLE at 10.8% and is excluded from every claim.
  - So: **the cross-AZ row is a finding** — far outside that noise on both
    axes — and **cluster-group versus single-AZ is not established** by these
    data. Cost came in around $1.40 against a $2 pre-registration for three
    placements, with seven provisionings rather than three.
  - _Requirements: 2.1, 2.4, 5.1_

- [x] 8. Discovery without a static list
  - A `peer_discovery` implementation that scans the cloud's inventory by run
    tag — `register_node` / `find_peers`, the concept the tree already has, and
    the shape `aws_ec2_quorum_manager` already models
  - **Tested functionally, not through a measured row**: bring N hosts up with
    no static list, assert every group elects, tear down. Discovery inside a
    measured window is a cost in that window
  - The static list stays supported and stays the default for a row
  - Abort and name the missing hosts if it does not converge, rather than
    measuring a cluster with a replica missing
  - **DONE, end to end.** `include/raft/aws_ec2_peer_discovery.hpp` implements
    the existing `peer_discovery` concept over an EC2 tag scan, and
    `multi_raft_node` gained `--discovery ec2-tag` so a host can form a cluster
    with no peer list. Discovery resolves the map ONCE, before the transport is
    built, so the map a measured window uses is fixed either way and
    Requirement 3.4 stays true.
  - Six LocalStack cases pass, including the role filter that keeps the driver
    out of the peer set — without it the cluster believes it has N+1 voters,
    which presents as elections that will not settle — and `await_peers`
    naming what it never saw rather than proceeding.
  - **Demonstrated with real node processes and no static list anywhere:**
    three hosts registered, discovered all three peers, and every one reported
    `{"ready":true,"groups_without_leader":0}`. A workload then ran through the
    discovered cluster at 488.2 ops/sec over 5 windows, stable, zero elections.
  - The static list remains the default and every measured row above used it.
  - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5_

- [x] 9. The comparison, filled in only as far as it goes
  - Update `doc/multi_raft_performance_comparison.md`'s like-for-like table
    **metric by metric**, and only for metrics this row supports
  - A Tier E row on `memory` persistence is comparable to an external
    **no-fsync** number and to nothing else. The durable comparison is Tier D
  - State the concurrency mismatch beside every external pairing — etcd's 1,000
    clients against this suite's 16 is the standing example, and this document
    already says that probably dominates the ratio
  - **An unstable row does not enter the table however much it cost.**
    Requirement 6.3 has no cost exemption
  - **DONE, and it found that the table cannot be opened from our side alone.**
    Tier E removes the tier blocker completely. What remains is durability, and
    it is now blocked on **both** sides: ours is `memory`, and Requirement 6.2
    admits only an external no-fsync number — of which the register contains
    **none**. Four records state fsync honoured, the read records mark it not
    applicable, and every other write record says "not stated", which
    Requirement 9.2 forbids us from resolving.
  - So a Tier D row would pair only with Dragonboat and TiKV, whose 48 shards
    against 4 and 66 cores against 24 are mismatched enough that Requirement
    9.6 forbids the ratio anyway. The table stays empty for a reason no further
    work of ours removes.
  - **The table is no longer empty.** The one lawful pair was identified and
    then taken: a Tier E `read-local` row at 1 group, 256 B values and **1
    operation in flight** — **1261.2 ops/sec, p50 765.4 µs, p95 972.1, p99
    1150.9, spread 3.4%, stable over 11 windows, zero elections** — against
    etcd 3.2.0's serializable read at **2,909 QPS and 0.3 ms mean**, matched on
    tier, durability (not applicable to a read, and *stated* so on both sides),
    Raft group count, payload size and client concurrency.
  - **43.4% of etcd's rate at 2.55x their latency, and those are one fact.** At
    one operation in flight both systems are latency-bound: 1/765 µs = 1307
    against our measured 1261, 1/300 µs = 3333 against their published 2909.
  - It also produced this project's **first non-`n/a` p99**, because 2000
    operations per window finally supply the samples one needs.
  - The differences that remain are stated and not quantified, because they
    cannot be from these data: their 16-vCPU client against our 8 (immaterial
    at 1 in flight, but unmeasured), 2017 GCE against 2026 EC2 — the largest
    uncontrolled term, and it plausibly favours us — a mean against a median,
    and which replica serves the read.
  - _Requirements: 6.1, 6.2, 6.3, 6.4, 6.5_

- [x] 10. Hand Tier E back, and say what is left
  - `.kiro/specs/multi-raft-performance/` task 20 can stop recording Tier E as
    undelivered; its tier table can stop saying "containers only"
  - **Tier D remains unrun** and this task must not claim otherwise — it needs a
    volume and a run, and nothing else
  - Record what a second provider would cost, since GCP's Shape 1 audit is a
    re-implementation rather than a shared abstraction and Shape 2 inherits that
  - **DONE.** Tier E is delivered on real machines and
    `.kiro/specs/multi-raft-performance/` task 20 can stop recording it as
    undelivered; the tier table can stop saying "containers only".
  - **Tier D remains unrun** and nothing here claims otherwise. It needs a
    provisioned volume and a run: the barrier lands 100% of appends, the host
    takes `--persistence file-barrier`, and `run-aws-shape-2.sh` passes it
    through, so the work is a run and not a spec.
  - A second provider for this shape is its own task and inherits GCP Shape 1's
    re-implemented audit: GCE has no key pairs, its boot disk is a zonal child
    of the instance, and its firewall rules cannot carry labels. Shape 2 adds a
    placement-group analogue to that list, which GCE does not have at all.
  - _Requirements: 6.1_

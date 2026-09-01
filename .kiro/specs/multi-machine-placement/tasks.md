# Implementation Plan

Ordered so that nothing bills until the thing that stops it billing has been
tested, and so that the cheapest task that could invalidate the design — the
security-group rule that lets nodes reach each other at all — happens before
anything is measured.

**Task 1 must fail the audit on purpose.** An audit that has only ever been seen
to pass is not evidence, and `.kiro/specs/multi-raft-performance/` task 21 found
one reporting clean while blind.

- [ ] 1. The leak audit, widened and tested in both directions
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
  - _Requirements: 5.5, 5.6, 2.3_

- [ ] 2. Provision N+1 instances, and open the port between them
  - One security group, one key pair, N+1 instances, all run-tagged
  - **The security group needs a rule Shape 1 never needed**: the Raft port,
    between the instances. Shape 1 allows SSH from the controller and nothing
    else, so this is the first thing that will silently produce a cluster that
    never elects
  - Instance-local dead-man switch on **every** instance, sized for the whole
    run rather than one repetition (doctrine 135)
  - Teardown from an EXIT trap, so the failure paths are covered
  - `--dry-run` that provisions nothing and prints what it would do
  - _Requirements: 1.1, 5.2, 5.3, 5.4_

- [ ] 3. Deploy and start, without the two mistakes Shape 1 already made
  - **Ship the prebuilt binaries** (doctrine 134); record `binary_origin`
  - Capture provenance **before** anything expensive, not after (doctrine 145)
  - Start hosts under `systemd-run`, never `ssh host "cmd &"` — a backgrounded
    process holds the ssh channel open regardless of its own redirections
    (doctrine 136), and the controlling side polls a state artifact instead
  - Wait on each host's `/ready`, which already means "every group has a
    leader" rather than "the process is up"
  - _Requirements: 1.3, 1.4_

- [ ] 4. Probe the network before the window, every ordered pair
  - `/probe` already measures RTT and bandwidth to each peer and already reports
    the median of eleven samples; this calls it on every host and records the
    **matrix**, not an average — a cross-AZ cluster is not symmetric and an
    average hides which link is slow
  - **Assert the sanity check Requirement 4.5 asks for**: if the measured RTT is
    within an order of magnitude of loopback, this is a container-shaped row and
    must say so. That is the exact failure the Podman rows demonstrated, and it
    is silent
  - _Requirements: 4.1, 4.2, 4.5_

- [ ] 5. The first Tier E row on real machines
  - One availability zone, three hosts, driver on its own instance, five
    repetitions, the same workload a Tier C row uses
  - Record placement, region, every zone, the RTT matrix and the driver's own
    instance type on the row
  - **Expect it to be slower than Tier C, and say so before running it.** A
    Tier E row that resembled the Tier C row would mean the network was not real
  - _Requirements: 1.2, 1.5, 2.2, 4.3_

- [ ] 6. The delta that is the actual deliverable
  - The same workload and the same binaries at Tier C and Tier E, differing in
    placement alone, published as one artifact
  - Read the round interval against the measured RTT. Task 11's
    response-driven-pacing hypothesis predicts the interval tracks the round
    trip; on loopback that was untestable because the round trip barely varied.
    **This is the first chance to falsify it rather than fit it**
  - Report what the delta says about the hypothesis, including if it says the
    hypothesis is wrong
  - _Requirements: 4.4_

- [ ] 7. Placement as a swept axis
  - The same row across one AZ, several AZs, and a cluster placement group,
    holding everything else identical (Requirement 11 of the performance spec)
  - Three rows, three RTT matrices, one table
  - Cost check against the pre-registered estimate, as Shape 1 does
  - _Requirements: 2.1, 2.4, 5.1_

- [ ] 8. Discovery without a static list
  - A `peer_discovery` implementation that scans the cloud's inventory by run
    tag — `register_node` / `find_peers`, the concept the tree already has, and
    the shape `aws_ec2_quorum_manager` already models
  - **Tested functionally, not through a measured row**: bring N hosts up with
    no static list, assert every group elects, tear down. Discovery inside a
    measured window is a cost in that window
  - The static list stays supported and stays the default for a row
  - Abort and name the missing hosts if it does not converge, rather than
    measuring a cluster with a replica missing
  - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5_

- [ ] 9. The comparison, filled in only as far as it goes
  - Update `doc/multi_raft_performance_comparison.md`'s like-for-like table
    **metric by metric**, and only for metrics this row supports
  - A Tier E row on `memory` persistence is comparable to an external
    **no-fsync** number and to nothing else. The durable comparison is Tier D
  - State the concurrency mismatch beside every external pairing — etcd's 1,000
    clients against this suite's 16 is the standing example, and this document
    already says that probably dominates the ratio
  - **An unstable row does not enter the table however much it cost.**
    Requirement 6.3 has no cost exemption
  - _Requirements: 6.1, 6.2, 6.3, 6.4, 6.5_

- [ ] 10. Hand Tier E back, and say what is left
  - `.kiro/specs/multi-raft-performance/` task 20 can stop recording Tier E as
    undelivered; its tier table can stop saying "containers only"
  - **Tier D remains unrun** and this task must not claim otherwise — it needs a
    volume and a run, and nothing else
  - Record what a second provider would cost, since GCP's Shape 1 audit is a
    re-implementation rather than a shared abstraction and Shape 2 inherits that
  - _Requirements: 6.1_

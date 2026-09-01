# Design Document

## Overview

`scripts/perf-cloud/run-aws-shape-2.sh`, and the three things it needs that
Shape 1 does not: N+1 instances instead of one, a way for the hosts to find each
other, and a leak audit that knows about placement groups.

Almost nothing else is new. The host and driver binaries exist and take every
axis at run time; `/probe` already measures inter-node RTT and bandwidth;
`/ready` already refuses to let a driver start before every group has a leader;
and Shape 1's run-scoped tag, EXIT-trap teardown, dead-man switch and provenance
capture all generalise unchanged. **The expensive part of Tier E was the host
binary, and that already landed.**

## Why one machine could never have answered this

The Podman rows measured **0.7–0.9 ms** between "nodes" — the same figure
loopback gives, because containers on one host share a network stack. Every
inter-round-interval number this project has ever published is a loopback
number: 0.75–0.91 ms on a cloud instance, 1.89–2.26 ms on the development
machine.

`.kiro/specs/multi-raft-performance/` task 11's response-driven-pacing
hypothesis predicts the round interval tracks the RPC round trip. On loopback
that prediction is untestable, because the round trip barely varies. A real
network moves it by one to two orders of magnitude, which is the first
opportunity this project has had to falsify the hypothesis rather than fit it.

That is the design's actual purpose. The throughput number is a by-product.

## Shape 2, step by step

Shape 1's structure, widened:

1. **Provision.** One security group, one key pair, optionally one cluster
   placement group, then N+1 instances — all carrying the run tag. Shape 1's
   security group allows SSH only; Shape 2 must additionally allow the Raft
   port **between the instances**, which is a new rule and a new way to fail.
2. **Wait and verify.** SSH to each, capture provenance *before* anything
   expensive. Doctrine 145: provenance costs two seconds and refuses on its own
   terms, and running it after a fifty-five-minute build once cost an hour.
3. **Deploy.** Copy the prebuilt `multi_raft_node` to each host and
   `multi_raft_bench` to the driver. One `scp` each, no build.
4. **Start hosts.** Under `systemd-run --unit=...`, not `ssh host "cmd &"`.
   Doctrine 136: a backgrounded process holds the ssh session channel open
   however its own streams are redirected, so the controlling side polls a state
   artifact the work writes itself.
5. **Wait for readiness** on each host's control port, and **probe the network**
   from every host to every peer. Both before the window, both recorded.
6. **Measure.** Run the driver, five repetitions, the same workload a Tier C row
   uses.
7. **Collect.** Pull every host's log and the driver's CSV/JSON.
8. **Tear down from an EXIT trap**, then audit.

## Discovery, and why the default stays static

Requirement 3 asks for discovery that does not need every address up front.
There are three candidates already in the tree, and the design's recommendation
is to implement the third and default to none of them.

- **DNS-SD** (`rfc6763_peer_discovery`, `rfc2136_dns_sd_discovery`). Real
  implementations of the `peer_discovery` concept, used by
  `cmd/dns_sd_discovery_node`. They need a DNS server that accepts dynamic
  updates, which on AWS means standing up BIND or using Route 53 — a second
  moving part inside a measurement.
- **mDNS** (`poco_peer_discovery`). Works on a flat L2 segment. AWS VPC
  subnets are not one; multicast does not cross them.
- **Cloud inventory by tag.** Ask EC2 for the instances carrying this run's
  tag. This is what a real deployment does, and `aws_ec2_quorum_manager` already
  models exactly this shape — the concept is `register_node` / `find_peers`, and
  a tag scan is a small implementation of it rather than a new subsystem.

**The recommendation is the tag scan, and the default is the static list**
(Requirement 3.4). Discovery inside a measured window is a cost in that window;
the static list is what a *row* uses, and discovery is what proves the cluster
can form without one. Both are exercised; only one is measured through.

The distinction matters because it is easy to lose: a Tier E row whose numbers
include a control-plane API call per tick is measuring EC2.

## Placement as an axis, not a setting

Three placements, and the reason each is worth a row:

| placement | what it isolates |
|---|---|
| one AZ | the floor: real NICs, real switches, no cross-AZ hop |
| several AZs | what a fault-tolerant deployment actually pays |
| cluster placement group | the ceiling: the tightest network AWS sells |

A cluster placement group is also the one resource class this shape creates that
Shape 1 never has — and **the audit is already waiting for it.**
`audit-aws-leaks.sh` has a placement-group check with a comment saying Tier E's
shape uses one, that it costs nothing by itself, and that a leaked group holds a
name and will collide with the next run. Whoever wrote Shape 1 anticipated this
shape and left the check in place unfired.

That is worth stating plainly because the obvious assumption is the opposite
one. Reading "Shape 1 has never created a placement group" it is natural to
conclude the audit cannot see one, write a task to add it, and ship a spec whose
central claim is false. The audit covers six classes: instances, volumes,
elastic IPs, security groups, placement groups and key pairs. What this shape
owes it is not a seventh class but the widening of all six from one instance's
resources to N+1's.

## What the row must carry that a Tier C row need not

- Every instance's **availability zone**, verbatim.
- The **placement** choice.
- The full **RTT and bandwidth matrix** between ordered pairs, not an average —
  a cross-AZ cluster is not symmetric, and an average hides which link is slow.
- The **driver's** instance type and zone, separately from the hosts'.

Requirement 3.5 of `.kiro/specs/multi-raft-host-binary/` already made
`_placement` a row field and the driver already writes it verbatim. This
populates it with something worth reading.

## Cost, stated in advance

Shape 1's two AWS rows cost **$0.09** and **$0.34**, the second because
fifty-five minutes of it was an on-instance build. Shape 2 ships binaries, so
the build is not in the bill.

Four `c5.2xlarge` (three hosts, one driver) at roughly $0.34/hr each is about
$1.36/hr, and a run that boots, probes, measures five repetitions and tears down
should fit inside thirty minutes: **on the order of $0.70 per placement**, and
three placements is around **$2**. A cluster placement group costs nothing
extra.

That is an order of magnitude more than Shape 1 and still small. The number
worth watching is not the hourly rate but the **orphan**: four instances left
running is $32 a day, which is why Requirement 5.3 puts a dead-man switch on
each one rather than trusting the controlling process.

## Testing strategy

- **A dry run that provisions nothing**, as Shape 1 has, so the argument
  handling and the generated command lines can be read before anything is
  billed.
- **The audit, tested in both directions.** Task 21 of the performance spec
  found `gcloud compute list` exiting 0 under an authentication failure, so an
  audit is only trustworthy once it has been shown to fail when it should.
  Deliberately leak one resource of each class and confirm the audit catches it.
- **Discovery convergence, tested without a measurement.** Bring N hosts up with
  no static list and assert every group elects, then tear down. That is a
  functional test and does not need to be a billed measurement run.
- **The Tier C ↔ Tier E delta**, which is the deliverable: the same workload and
  binaries at both tiers, differing in placement alone.

## What this does not deliver

- **Tier D.** Still a volume and a run away, and still nothing but a run.
- **A second provider for this shape.** GCP's Shape 1 exists and its audit is a
  re-implementation rather than a shared abstraction, for reasons that spec
  records — GCE has no key pairs, its boot disk is a zonal child of the
  instance, and its firewall rules cannot carry labels. Shape 2 on GCP inherits
  all of that and is its own task.
- **Any claim about a WAN.** Cross-AZ is tens of kilometres. Cross-region is the
  regime where published numbers stop resembling each other at all, and it is
  not in scope.

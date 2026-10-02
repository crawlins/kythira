# AWS ASG Quorum Manager — Real-Cloud Validation Design Document

## Overview

One new test binary, `tests/aws_asg_quorum_manager_real_test.cpp`, modelled on
`tests/aws_quorum_manager_real_ec2_test.cpp`, which exercises
`aws_asg_quorum_manager` against real AWS Auto Scaling. One new IAM policy
fragment, one new bundle in the `aws` job that costs no `workflow_dispatch`
input, and an audit/sweep extension that can see Auto Scaling groups rather than
only instances.

The design decisions worth arguing about are all in §2 (self-provisioning), §4
(the bundle without an input) and §5 (the health-check case). Everything else is
mirroring an existing file.

## 1. What the manager actually does, and therefore what must be observed

`aws_asg_quorum_manager` calls seven AWS operations. The test's value is
proportional to how many of them it drives:

| Operation | Called from | Observed by |
|---|---|---|
| `DescribeAutoScalingGroups` | constructor (health check), provision, decommission | every case |
| `SetDesiredCapacity` / `UpdateAutoScalingGroup` | `provision_node` | 3.1, 3.4 |
| `DescribeInstances` | `provision_node` (private IP) | 3.1 |
| `CreateTags` | `provision_node` (NodeId tag) | 3.1 |
| `DescribeInstanceStatus` | `assess_quorum` | 3.2, 3.3 |
| `TerminateInstanceInAutoScalingGroup` | `decommission_node` | 3.4, 3.5 |

Note `DescribeInstanceStatus` with `SetIncludeAllInstances(true)`: liveness is
"instance state == running", read from EC2, **not** from ASG membership or ASG
health. Case 3.3 exists to prove that distinction holds against the real API,
because the two can disagree and the manager's correctness depends on which one
it believes.

## 2. The fixture provisions everything, and this is a deliberate departure

### What it creates

```
VPC (10.0.0.0/16, tagged kythira:cluster=<run id>)
├── one subnet per AZ (10.0.N.0/24)
├── internet gateway + route table      (instances need to reach nothing, but
│                                        the AMI's boot needs metadata only —
│                                        verify before adding an IGW at all)
├── security group (no ingress required; see §2.3)
├── launch template (NOT a launch configuration — Requirement 2.3)
└── one Auto Scaling group per AZ
    ├── HealthCheckType = EC2           (the manager requires it)
    ├── MinSize 0, MaxSize 3, Desired 0
    └── VPCZoneIdentifier = that AZ's subnet
```

### Why not operator-provisioned fixtures

`azure-cloud-services` took the other road: two hand-made scale sets, a PPG, an
availability set and five repository variables. The measured cost of that choice
was five test cases that skipped for months because one variable was never
passed to the job, plus two more in the VM half found only when the first five
were fixed. Nothing in that arrangement could fail loudly, because the thing
that was missing was an input, not a resource.

A self-provisioning fixture has no such failure mode: if it cannot create an
ASG, the case fails with the API's own error. It also keeps the
`workflow_dispatch` input count fixed, which §4 shows is a hard constraint
rather than a preference.

The precedent is already split both ways in this repo —
`aws_quorum_manager_real_ec2_test` creates its own VPC, and the (unusable)
LocalStack ASG fixture creates its own ASGs — so this is choosing the existing
AWS convention over the existing Azure one, not inventing a third.

### 2.3 On ingress and user data

The manager returns `private_ip:node_port` and never connects to it; nothing in
this suite runs a Raft node. So the security group needs **no ingress rules**,
and the launch template's user data can be a no-op. Resist the temptation to
copy `aws_quorum_manager_real_ec2_test`'s SSH setup: that file needs SSH because
one of its cases kills a process over it (`process_crash_via_ssh_kill`), and
this suite has no equivalent case. Every unused piece of that fixture is
something that can fail and confuse.

Verify whether an internet gateway is needed at all before adding one. If the
AMI boots to `running` without egress — which is what `DescribeInstanceStatus`
liveness actually depends on — the IGW and route table are pure cost and
teardown surface.

## 3. Teardown order, which is not negotiable

AWS refuses to delete a VPC that still has dependencies, and an ASG that still
has instances. Teardown therefore runs in reverse dependency order, and each
step waits for completion rather than assuming it:

```
1. For each ASG: UpdateAutoScalingGroup MinSize=0 DesiredCapacity=0
2. For each ASG: DeleteAutoScalingGroup ForceDelete=true
                 → wait until DescribeAutoScalingGroups no longer returns it
3. DeleteLaunchTemplate
4. Terminate any stray instances tagged with this run's cluster tag
                 → wait for terminated
5. DeleteSecurityGroup, DeleteSubnet ×N, (DeleteRouteTable, DetachInternetGateway,
   DeleteInternetGateway if created), DeleteVpc
```

Step 2's `ForceDelete` matters: without it, deleting an ASG with instances
fails, and a teardown that treats that failure as nothing leaves both the ASG
and its instances running. Step 4 is the backstop for an instance the ASG
launched but had not yet registered when the delete landed.

Each wait is bounded and, on expiry, **logs loudly and continues to the next
step** rather than aborting teardown — a teardown that stops at its first
failure strands everything after it. The loud log is what the job's audit then
confirms or contradicts.

## 4. Adding a bundle without adding an input

`real-cloud-tests.yml` is at exactly 25 `workflow_dispatch` inputs, GitHub's
cap. A 26th does not fail the PR that adds it; it invalidates the entire
workflow file, so every scheduled and dispatched run silently stops. That has
happened (PR #257, 24 → 27), and `ci.yml`'s `workflow-input-limits` job exists
because of it.

**Chosen approach: fold ASG into the existing `aws_bundle_ec2_quorum` input,
with its own repository variable.**

```yaml
# One input, two suites. The input count is at GitHub's hard cap of 25, and a
# 26th silently invalidates the whole file (PR #257 did it), so a new bundle
# cannot have its own input. Scheduled runs still control the two suites
# independently through their repository variables, which are not capped.
BUNDLE_EC2_QUORUM: ${{ ... aws_bundle_ec2_quorum ... vars.REAL_CLOUD_TESTS_AWS_EC2_QUORUM_ENABLED ... }}
BUNDLE_ASG_QUORUM: ${{ (github.event.inputs.aws_bundle_ec2_quorum == 'true')
                       || (github.event.inputs.aws_bundle_ec2_quorum == null
                           && vars.REAL_CLOUD_TESTS_AWS_ASG_QUORUM_ENABLED == 'true') }}
```

The asymmetry is deliberate and worth the comment: an explicit dispatch of
`aws_bundle_ec2_quorum=true` runs both suites, while the Monday cron runs
whichever the two repository variables enable. The alternative — a single
comma-separated `aws_bundles` string replacing four booleans — is strictly
better for input budget and strictly worse for discoverability in the dispatch
UI; it is the fallback if a second new bundle ever needs adding.

Note the trap this inherits: **omitting an input on `workflow_dispatch` makes it
inherit the repository variable**, so a dispatch that passes only
`aws_bundle_ec2_quorum` can still start other providers' billable jobs. Pass
explicit `false` for every other provider and bundle, then confirm from the job
list that the others are `skipped`.

## 5. The health-check case, and the honest uncertainty in it

The constructor's `HealthCheckType != "EC2"` rejection is the AWS analogue of
`azure_vmss_quorum_manager`'s `upgradePolicy.mode=Automatic` rejection, and the
Azure one was worth testing: proving a refusal needs a real resource in the
refused state, and that resource had to be created by an operator.

Here the open question is whether `CreateAutoScalingGroup` accepts
`HealthCheckType = "ELB"` with no load balancer or target group attached. AWS
documents ELB health checks in terms of an attached load balancer, but
documentation about what is *sensible* is not the same as what the API
*accepts* — and this project has just spent a session learning that an
`az vm create --validate` that accepts a configuration says nothing about
whether placement will.

**So: probe it first.** One `CreateAutoScalingGroup` call with
`HealthCheckType=ELB` and no load balancer, against the real API, answers it in
seconds and costs nothing if the ASG has `MinSize=0`.

- **If accepted**: case 3.8 creates that ASG, asserts the constructor throws
  `std::invalid_argument`, and deletes it. Cheap and complete.
- **If refused**: the cheapest sufficient attachment is a target group, which —
  unlike an ALB — has no hourly charge. Verify whether a target group alone
  satisfies `HealthCheckType=ELB`; if it does, create and delete one in the
  case. Only if an actual load balancer is required does this become a cost
  decision, and then the choice is an ALB for the ~30 seconds the case needs
  (cents) versus omitting the case.
- **If omitted**: record the omission in both this spec and a comment in the
  test file naming what is not covered and why. It SHALL NOT be a silent skip.

## 6. Audit and sweep

The existing AWS audit looks for instances. An ASG suite needs more, because an
ASG with desired capacity above zero and no instances yet is a leak that has not
started billing and will:

```sh
# Auto Scaling groups tagged with this suite's marker
aws autoscaling describe-auto-scaling-groups --query \
  "AutoScalingGroups[?Tags[?Key=='kythira:managed-by' && Value=='kythira-aws-asg-quorum-manager']]"
# Launch templates, which cost nothing but accumulate and confuse
aws ec2 describe-launch-templates --filters Name=tag:kythira:cluster,Values=...
```

Report the ASG's name, desired capacity and instance count; a non-zero desired
capacity is the finding that an instance-only audit misses. Sweep with
`delete-auto-scaling-group --force-delete`, then the launch template, then the
VPC chain.

The audit runs **before** the sweep, and fails the job; the sweep then stops the
billing. Sweeping first would turn every leak back into silence, which is the
failure mode the Azure and Alibaba jobs' comments both record paying for.

Mind the bounded-wait lesson from the Azure VM audit: AWS terminates
asynchronously, so a single sample of "instances still present" manufactures
false leaks. Wait, bounded, and report only what outlives the wait — and say in
the message that it outlived a wait, so the next reader knows it was not a
snapshot.

## 7. What this design does not do

It does not change `aws_asg_quorum_manager`. The expected outcome of running it
for the first time is that defects surface — the Azure sibling produced five —
and each is then its own fix with its own evidence. Writing those fixes into
this design in advance would be guessing, and this project has a measured record
of guesses: five wrong hypotheses on one Azure delete, three on one flake.

It also does not reuse `aws_quorum_manager_real_ec2_test`'s VPC by running in
the same binary. Separate binaries keep the ctest `TIMEOUT` derivation honest
and let the ASG suite be enabled independently, which §4's repository variable
depends on.

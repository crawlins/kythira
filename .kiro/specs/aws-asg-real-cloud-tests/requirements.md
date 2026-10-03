# AWS ASG Quorum Manager — Real-Cloud Validation Requirements Document

## Introduction

`aws_asg_quorum_manager` has **no coverage against any real or emulated AWS
Auto Scaling API**. Measured 2026-10-02:

- `tests/aws_quorum_manager_unit_test.cpp` covers configuration validation
  (`empty_asg_by_group_throws`, `missing_asg_for_group_throws`) and
  fault-injection points. It never reaches an Auto Scaling endpoint.
- `tests/aws_quorum_manager_localstack_test.cpp` **can never run**. Its fixture
  calls `CreateLaunchConfiguration`, and autoscaling is a Pro-only service in
  LocalStack community: every case fails at setup with
  `InternalFailure: Sorry, the autoscaling service is not included within your
  LocalStack license`. `f0a4775` made the suite skip with that reason instead of
  failing three cases, which removed the confusion but not the gap.
- The real-cloud AWS binaries are `tests/aws_quorum_manager_real_ec2_test.cpp`
  (EC2 only) and `tests/aws_s3_object_persistence_real_test.cpp`. Neither
  touches Auto Scaling: `grep -l 'asg_quorum_manager\|AutoScaling'
  tests/aws_*real*.cpp` returns nothing.

So every line of `aws_asg_quorum_manager` that talks to AWS —
`SetDesiredCapacity`, `UpdateAutoScalingGroup`, `DescribeAutoScalingGroups`,
`TerminateInstanceInAutoScalingGroup`, `DescribeInstanceStatus`,
`DescribeInstances`, `CreateTags` — is unexecuted code.

### Why this is not a theoretical risk

`azure_vmss_quorum_manager` was in exactly this position: a sibling manager,
written to the same design, whose real tests had never run because a workflow
never passed one environment variable. When they were made to run
(2026-09-30 → 2026-10-01) it turned out to be wrong in **five independent
ways**, each only reachable after the previous was fixed:

1. It tagged instances with `PATCH` on a resource that answers `405` to that
   method.
2. It targeted an orchestration mode that **cannot store** the per-instance tag
   its node identity depends on — and whose tag writes return success while
   applying nothing.
3. It passed an ARM resource id where a URL was required.
4. Its decommission wait gave up after 30s and returned **success**.
5. It treated `PowerState/running` as provisioned while the instance was still
   `Creating`, so deleting it made ARM unwind the create (9+ minutes versus
   24-37s).

Not one of those was detectable by the unit tests, which passed throughout. Four
of the five are in the class "the API does not behave the way the code assumes",
which is precisely what a hand-written mock cannot catch, because the mock
encodes the same assumption.

**Requirement 0 (framing).** This spec exists to execute
`aws_asg_quorum_manager` against real AWS. A mock or test double is explicitly
**not** an acceptable substitute, and the sibling LocalStack test's own comment
already states the reason: a hand-written mock "would get wrong in the same
direction as the code under test".

## Requirements

### Requirement 1: a real-AWS ASG test binary

1.1 A new test binary `tests/aws_asg_quorum_manager_real_test.cpp`, guarded by
`#ifdef KYTHIRA_AWS_REAL_TESTS` and `KYTHIRA_HAS_AWS_SDK`, falling back to a
compile-only stub case when either is absent, exactly as
`aws_quorum_manager_real_ec2_test.cpp` does.

1.2 It SHALL be registered with CTest only when `KYTHIRA_AWS_REAL_TESTS` is on,
with `SKIP_RETURN_CODE 77`, and labels matching the existing real-AWS tests.

1.3 Its CTest `TIMEOUT` SHALL be derived from the sum of its cases' own
`boost::unit_test::timeout` values plus fixture overhead, and the derivation
SHALL be recorded in a comment — the convention
`aws_quorum_manager_real_ec2_test` and `azure_quorum_manager_real_test` both
follow. A `TIMEOUT` below that sum is not a conservative choice: ctest kills on
timeout with `SIGKILL`, no destructor runs, and every instance the in-flight
case created is stranded and billing. That has already happened once on Azure
(run `36426191450`, three instances, 25.5 hours, $10.70).

### Requirement 2: the harness is self-provisioning

2.1 The fixture SHALL create its own VPC, subnets (one per Availability Zone),
security group, launch template and Auto Scaling groups, and SHALL destroy all
of them on teardown. It SHALL NOT depend on operator-provisioned fixtures or on
new repository variables.

2.2 Rationale, and the reason this differs from `azure-cloud-services`: the
Azure VMSS suite requires five repository variables and two hand-made scale
sets, and the cost of that was the defect this spec's introduction describes —
a variable nobody passed, five dead test cases, and no self-correcting failure.
`aws_quorum_manager_real_ec2_test` already creates its own VPC, and the
LocalStack ASG fixture already creates its own ASGs, so precedent exists on both
axes. Self-provisioning also keeps the `workflow_dispatch` input count fixed
(Requirement 6).

2.3 The launch template SHALL be an EC2 **launch template**, not a launch
configuration. AWS no longer supports launch configurations for new accounts,
and the unusable LocalStack test's `CreateLaunchConfiguration` is the one piece
of its fixture that SHALL NOT be mirrored.

2.4 Each ASG SHALL be created with `HealthCheckType = "EC2"`, which the manager
requires (Requirement 5.1), `MinSize = 0`, and a desired capacity of 0 at rest,
so an idle ASG costs nothing between cases.

### Requirement 3: the cases

Each case SHALL assert at least one property. A case that reports success while
asserting nothing is the failure mode `run-real-cloud-suite.sh`'s
`did not check any assertions` gate exists to catch, and seven such cases hid in
the Azure suite for months.

3.1 `provision_node_increases_desired_capacity` — provisioning a node into a
group raises that ASG's desired capacity by one, a new `InService` instance
appears, the manager tags it with the assigned NodeId, and the returned
`peer_info` address is the instance's private IP and the configured port.

3.2 `assess_quorum_reports_live_nodes` — a provisioned, running node is reported
live, with `live_node_count` matching.

3.3 `assess_detects_stopped_instance` — an instance stopped **outside** the
manager (a direct `StopInstances`) is reported not-live. This is the case that
proves liveness is read from `DescribeInstanceStatus` rather than inferred from
ASG membership.

3.4 `decommission_removes_instance` — `decommission_node` terminates the
instance through `TerminateInstanceInAutoScalingGroup` with
`ShouldDecrementDesiredCapacity = true`, the instance leaves the ASG, and
desired capacity returns to its previous value. The wait SHALL confirm removal
rather than returning after a fixed interval; see Requirement 7.

3.5 `decommission_is_idempotent` — decommissioning an unknown NodeId, and
decommissioning the same node twice, both succeed without throwing.

3.6 `maintain_quorum_restores_full_cluster` — after an instance is terminated
outside the manager, `maintain_quorum` returns the cluster to its desired
topology.

3.7 `multi_az_topology` — a cluster with one group per Availability Zone
provisions one node into each, and each instance lands in the AZ its group
names.

3.8 `rejects_non_ec2_health_check` — constructing the manager against an ASG
whose `HealthCheckType` is not `EC2` throws `std::invalid_argument`. See
Requirement 5 for why this case needs separate thought.

### Requirement 4: cost reporting, cleanup and leak detection

4.1 Every case that provisions a billable resource SHALL file a
`TestCostReport`, and the binary SHALL print the per-case and total estimate,
reusing the shared helpers rather than reimplementing them. A run that reports
no cost while having provisioned instances is a broken instrument.

4.2 The binary SHALL install the shared signal-cleanup handler so a `SIGINT` /
`SIGTERM` / `SIGHUP` / `SIGQUIT` tears down the VPC, ASGs, launch template and
instances. `SIGKILL` is uncatchable, which is what Requirement 1.3 and
Requirement 4.3 exist for.

4.3 The AWS job SHALL audit for leaked resources after the bundle, with
`if: always()`, and SHALL sweep them. The audit SHALL fail the job and the sweep
SHALL run **after** it, so detection stays loud and remediation does not hide
it — the ordering the Azure and Alibaba jobs already use.

4.4 The audit SHALL cover **Auto Scaling groups and launch templates as well as
instances.** An ASG with a non-zero desired capacity and no instances yet is a
leak that has not started billing but will, and an instance-only audit cannot
see it. This is the AWS analogue of the capacity check the Azure VMSS sweep
performs.

4.5 A leaked ASG SHALL be deleted with its instances (`ForceDelete`), because
deleting an ASG that still has instances otherwise fails and leaves both.

### Requirement 5: the health-check validation case needs a decision

5.1 `aws_asg_quorum_manager`'s constructor requires every configured ASG to have
`HealthCheckType == "EC2"` and throws `std::invalid_argument` otherwise, so that
the ASG and kythira agree on liveness and the ASG does not replace instances
kythira still considers live. This is the direct analogue of
`azure_vmss_quorum_manager`'s refusal of `upgradePolicy.mode=Automatic`, and
like it, **it has never executed against a real ASG.**

5.2 Requirement 3.8 therefore needs an ASG with a non-EC2 health check. **This
is an open question the implementer must resolve by measurement, not
assumption**: AWS documents ELB health checks as being for ASGs with an attached
load balancer or target group, and it is unverified whether
`CreateAutoScalingGroup` accepts `HealthCheckType = "ELB"` without one. Probe it
with a real API call before designing around either answer.

5.3 If an attached load balancer turns out to be required, the case SHALL
either create the cheapest sufficient target group / ALB for the few seconds the
constructor needs and delete it in the same case, **or** be omitted with its
absence recorded in the spec and the test file. It SHALL NOT be written to skip
silently: that is the Azure failure this project spent two sessions undoing.

### Requirement 6: the workflow must gain a bundle without gaining an input

6.1 `real-cloud-tests.yml` currently has **exactly 25 `workflow_dispatch`
inputs, which is GitHub's hard cap.** Exceeding it does not produce an error on
the offending PR: it makes the **whole workflow file invalid**, so the file
stops being a workflow at all and every scheduled and dispatched run silently
stops happening. PR #257 did this once, taking the file from 24 inputs to 27.
`ci.yml`'s `workflow-input-limits` job exists to catch it.

6.2 The ASG bundle SHALL therefore be added **without adding a 26th input.**
Acceptable approaches, in preference order:
  a. Fold ASG into the existing `aws_bundle_ec2_quorum` input, so that input
     selects both quorum-manager suites, with separate repository variables
     (`REAL_CLOUD_TESTS_AWS_ASG_QUORUM_ENABLED`) for independent scheduled
     control. Repository variables are not capped.
  b. Replace one or more boolean inputs with a single comma-separated
     `aws_bundles` string input, which reduces the count rather than raising it.

6.3 Whichever is chosen, `ci.yml`'s `workflow-input-limits` guard SHALL still
pass, and the chosen approach SHALL be recorded in the workflow next to the
input, because the next person to add a bundle will hit the same wall.

6.4 The job SHALL fail closed if the ASG bundle is enabled but its prerequisites
are missing, in the same shape as the existing `Fail closed if …` guards. A
bundle that is enabled and silently runs nothing is the fault this whole area
keeps relearning.

### Requirement 7: waits must confirm, not elapse

7.1 Every wait in the test and in any manager change this spec motivates SHALL
assert the condition it is waiting for, and SHALL fail with a message naming
what it was waiting for and for how long. A wait that expires and reports
success is worse than no wait: `azure_vmss_quorum_manager::decommission_node`
did exactly that for 30 seconds, and the resulting failure was unattributable
until the wait was made to throw.

7.2 Waits SHALL be bounded by values derived from measurement, recorded in a
comment. ASG instance launch to `InService` is minutes, not seconds; the
existing `provision_timeout` default of 120s is a starting point to verify, not
a fact.

### Requirement 8: IAM

8.1 A new policy fragment `scripts/ci-cloud-credentials/aws/policies/asg-quorum-manager.json`
SHALL grant exactly the Auto Scaling and EC2 actions the suite needs. **No
existing fragment grants any `autoscaling:*` action**, so this cannot be folded
into `ec2-quorum-manager.json` without widening that bundle's blast radius.

8.2 The fragment SHALL be scoped as narrowly as the API permits, and where
Auto Scaling does not support resource-level permissions for an action, the
comment SHALL say so rather than leaving a bare wildcard unexplained.

8.3 `provision-federated-identity.sh` SHALL accept `asg-quorum-manager` as a
bundle name, and the README SHALL document it.

## Out of scope

- Changing `aws_asg_quorum_manager`'s behaviour. This spec executes it. If
  execution reveals defects — which, per the introduction, is the expected
  outcome rather than a surprise — each one is its own fix with its own
  evidence, and this spec's job is to have made them visible.
- Mixed-instances policies, spot ASGs, warm pools and scaling policies. The
  manager deliberately does not touch an ASG's launch configuration or scaling
  policy; it changes desired capacity and tags instances. The test SHALL respect
  that boundary.
- Making `aws_quorum_manager_localstack_test` runnable. Autoscaling remains
  Pro-only; `f0a4775` made it skip honestly, and it SHALL be left in place
  rather than deleted, since it becomes useful if a Pro licence ever appears.

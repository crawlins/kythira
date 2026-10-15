# AWS ASG Quorum Manager — Real-Cloud Validation Implementation Plan

Ordered so that each task is verifiable on its own, and so the two unknowns
(§5's health-check probe, §2.3's egress question) are settled by cheap API calls
**before** any code depends on the answer.

Cost note: every task from 4 onward spends real money. `ecs`-equivalent here is
`t3.micro`/`t4g.micro` at well under a cent per instance-minute, and every ASG
sits at desired capacity 0 between cases, so the suite's steady-state cost is
zero and a full run is cents. That is only true if teardown works, which is why
tasks 3 and 9 come before the cases that rely on them.

**Status (re-checked October 15, 2026): all 12 tasks done.** PR #381
(merged October 3) carries tasks 2-12; the first real run (task 12, all 9
cases) passed after one fix to `decommission_node`'s idempotency, and the
CI `aws-asg` job passed 9/9 on x64 (job 111374315611, October 5, ~$0.003,
audit and sweep clean).

Task 1 answers, recorded October 2, 2026 from a throwaway probe under the
admin profile:
- 1a. `CreateAutoScalingGroup` accepts `HealthCheckType=ELB` with no load
  balancer or target group attached, and stores ELB, so case 3.8 creates
  such a group directly and checks that the manager's constructor refuses
  it.
- 1b. Not needed, since 1a was accepted.
- 1c. Yes: with no public IP and no internet gateway the instance reached
  `running` in about 14 s and `InService` in about 16 s, with status `ok`
  after about 2.5 minutes. The fixture builds no IGW.

Where the implementation departs from the text below: the tag and IAM
condition key is `kythira:suite=aws-asg-quorum-manager` (the manager
overwrites `kythira:managed-by` on instances); ASG names start
`kythira-asgtest-` for the IAM ARN scope; the bundle is listed in
`provision-oidc-role.sh`, not Azure's `provision-federated-identity.sh`;
and the suite runs in its own `aws-asg` job, because the `aws` job was
already at 345 of its 360 minutes.

---

- [x] **1. Settle the two open questions with throwaway API calls.**
  No repo changes. Use the admin credentials locally, record the answers in this
  file as dated observations, and stop if either answer changes the design.
  - [x] 1a. Does `CreateAutoScalingGroup` accept `HealthCheckType=ELB` with no
        load balancer or target group attached? (Requirement 5.2.) Create with
        `MinSize=0 MaxSize=0 DesiredCapacity=0`, record accept/refuse and the
        exact error if refused, delete it.
  - [x] 1b. If refused, does attaching only a **target group** (no load
        balancer, no hourly charge) satisfy it?
  - [x] 1c. Does an instance from a launch template with no public IP and no
        internet gateway reach `running` and report
        `DescribeInstanceStatus` state `running`? (Design §2.3.) This decides
        whether the fixture needs an IGW and route table at all.
  - [x] Record all three answers here with the date. An unrecorded probe gets
        re-run by the next person.

- [x] **2. IAM policy fragment.** (Requirement 8.)
  - [x] Create `scripts/ci-cloud-credentials/aws/policies/asg-quorum-manager.json`
        granting the seven operations from design §1 plus the fixture's
        `CreateVpc`/`CreateSubnet`/`CreateSecurityGroup`/`CreateLaunchTemplate`/
        `CreateAutoScalingGroup` and their deletes.
  - [x] Scope each statement as narrowly as the API allows. Where Auto Scaling
        does not support resource-level permissions for an action, say so in a
        comment next to the wildcard — an unexplained `"Resource": "*"` reads as
        laziness and gets copied.
  - [x] Teach `provision-federated-identity.sh` the `asg-quorum-manager` bundle
        name; document it in `scripts/ci-cloud-credentials/aws/README.md`.
  - [x] **Verify**: run the script with `--dry-run` and read the emitted
        statements; then attach it and confirm with one `DescribeAutoScalingGroups`
        call made as the CI principal, not as the admin user. A local probe with
        admin credentials proves nothing about the CI role — the OCI spec learned
        this the expensive way.

- [x] **3. The fixture, and its teardown, before any test case.** (Requirement 2,
      design §2-3.)
  - [x] `tests/aws_asg_quorum_manager_real_test.cpp` with the standard global
        fixtures: preflight skip (exit 77), Folly init, AWS SDK init, cost
        summary, signal cleanup.
  - [x] Fixture creates VPC, per-AZ subnets, security group (no ingress), launch
        template, and one `HealthCheckType=EC2` ASG per AZ at `MinSize 0`,
        desired 0. IGW only if task 1c says it is needed.
  - [x] Teardown in the reverse order design §3 specifies, each step bounded,
        each expiry logged loudly and **not** aborting the remaining steps.
  - [x] **Verify before writing a single case**: run the binary with an empty
        test case, then confirm by hand that the VPC, ASGs, launch template and
        every instance are gone. Then run it again and kill it with `SIGINT`
        mid-setup, and confirm the same. A fixture whose teardown is unproven is
        how this project accrued a $37 Alibaba bill and a $10.70 Azure one.

- [x] **4. CTest registration and the timeout derivation.** (Requirement 1.)
  - [x] Register under `KYTHIRA_AWS_REAL_TESTS` with `SKIP_RETURN_CODE 77` and
        labels matching the sibling real-AWS tests.
  - [x] Set `TIMEOUT` to the sum of the cases' own
        `boost::unit_test::timeout` values plus fixture overhead, and write the
        arithmetic into the comment. Re-derive it whenever a case is added.

- [x] **5. Cases 3.1-3.2: provision and assess.** (Requirement 3.1, 3.2.)
  - [x] `provision_node_increases_desired_capacity`: assert desired capacity
        rose by one, a new `InService` instance exists, it carries the NodeId
        tag, and the returned address is `private_ip:node_port`.
  - [x] `assess_quorum_reports_live_nodes`.
  - [x] File `TestCostReport` entries. **Verify** the printed cost summary is
        non-zero and names the instance type actually launched, not a hardcoded
        one — the Azure suite quoted a `Standard_D2s_v5` rate for a `D2s_v7`
        instance for months.

- [x] **6. Case 3.3: liveness comes from EC2, not from the ASG.**
  (Requirement 3.3.)
  - [x] Stop the instance with a direct `StopInstances`, outside the manager,
        and assert `assess_quorum` reports it not-live.
  - [x] **Wait for the condition, not an interval** (Requirement 7): poll until
        `DescribeInstanceStatus` stops reporting `running`, with a bounded wait
        whose expiry message names the instance and says the *stop*, not
        `assess_quorum`, is what failed. The Azure analogue of this case failed
        its first real run for exactly this reason — it asserted immediately
        after an asynchronous deallocate.

- [x] **7. Cases 3.4-3.5: decommission.** (Requirement 3.4, 3.5.)
  - [x] `decommission_removes_instance`: assert the instance leaves the ASG and
        desired capacity returns to its prior value.
  - [x] `decommission_is_idempotent`: unknown NodeId, and the same node twice.
  - [x] If the manager's removal wait expires, it must **throw**, not return
        success. If it currently returns success on expiry, that is a defect to
        fix with its own commit, not to work around in the test — see
        Requirement 7.1 and the Azure precedent it cites.

- [x] **8. Cases 3.6-3.7: maintain_quorum and multi-AZ.**
  (Requirement 3.6, 3.7.)
  - [x] `maintain_quorum_restores_full_cluster` after an out-of-band terminate.
  - [x] `multi_az_topology`: assert each instance's AZ matches its group's.

- [x] **9. Audit and sweep that can see Auto Scaling groups.** (Requirement 4,
      design §6.)
  - [x] Audit step, `if: always()`, reporting leaked ASGs **with their desired
        capacity**, launch templates, and instances. A non-zero desired capacity
        with no instances is the finding an instance-only audit misses.
  - [x] Bounded wait before declaring a leak, because AWS terminates
        asynchronously and one sample manufactures false alarms. Say in the
        message that the finding outlived a wait.
  - [x] Sweep step **after** the audit, `--force-delete` on ASGs, then launch
        template, then the VPC chain.
  - [x] **Verify by deliberately leaking**: run with teardown disabled, confirm
        the audit fails the job and names the ASG and its capacity, and the
        sweep then removes it. An audit that has never seen a real leak is an
        untested audit — this repo has shipped two of those.

- [x] **10. The bundle, without a 26th input.** (Requirement 6, design §4.)
  - [x] Add `BUNDLE_ASG_QUORUM` driven by the existing `aws_bundle_ec2_quorum`
        input plus a new `REAL_CLOUD_TESTS_AWS_ASG_QUORUM_ENABLED` repository
        variable. **Do not add an input.**
  - [x] Comment the reason at the input, with the PR #257 reference, so the next
        person to add a bundle does not have to rediscover the cap.
  - [x] Extend the job's "every bundle disabled" fail-closed guard to include
        the new bundle.
  - [x] **Verify**: `python3 -c "import yaml; ..."` counts inputs as exactly 25,
        and `ci.yml`'s `workflow-input-limits` job passes.

- [x] **11. Case 3.8: the health-check rejection, per task 1's answer.**
  (Requirement 5.)
  - [x] Implement whichever branch task 1a/1b established.
  - [x] If omitted, say so in this file **and** in a comment in the test file
        naming what is uncovered and why. Not a silent skip.

- [x] **12. First real run, and expect defects.**
  - [x] Dispatch AWS-only with explicit `false` for every other provider and
        bundle, then **confirm from the job list** that the others are
        `skipped` — omitting an input inherits the repository variable and can
        start four providers' billable jobs.
  - [x] Read the log for `did not check any assertions` and `Skipping:`. Zero of
        both is the bar; a green bundle with either is the fault this spec's
        introduction is about.
  - [x] Confirm the cost summary is present and plausible, and that the audit
        reports clean.
  - [x] **Treat each failure as a finding, not an obstacle.** The Azure sibling
        produced five real defects on first execution. Fix them one at a time,
        each with its own commit and its own measured evidence, and record any
        hypothesis that measurement refutes so the next person does not retry it.

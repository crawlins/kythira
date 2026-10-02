# AWS ASG Quorum Manager — Real-Cloud Validation Implementation Plan

Ordered so that each task is verifiable on its own, and so the two unknowns
(§5's health-check probe, §2.3's egress question) are settled by cheap API calls
**before** any code depends on the answer.

Cost note: every task from 4 onward spends real money. `ecs`-equivalent here is
`t3.micro`/`t4g.micro` at well under a cent per instance-minute, and every ASG
sits at desired capacity 0 between cases, so the suite's steady-state cost is
zero and a full run is cents. That is only true if teardown works, which is why
tasks 3 and 9 come before the cases that rely on them.

---

- [ ] **1. Settle the two open questions with throwaway API calls.**
  No repo changes. Use the admin credentials locally, record the answers in this
  file as dated observations, and stop if either answer changes the design.
  - [ ] 1a. Does `CreateAutoScalingGroup` accept `HealthCheckType=ELB` with no
        load balancer or target group attached? (Requirement 5.2.) Create with
        `MinSize=0 MaxSize=0 DesiredCapacity=0`, record accept/refuse and the
        exact error if refused, delete it.
  - [ ] 1b. If refused, does attaching only a **target group** (no load
        balancer, no hourly charge) satisfy it?
  - [ ] 1c. Does an instance from a launch template with no public IP and no
        internet gateway reach `running` and report
        `DescribeInstanceStatus` state `running`? (Design §2.3.) This decides
        whether the fixture needs an IGW and route table at all.
  - [ ] Record all three answers here with the date. An unrecorded probe gets
        re-run by the next person.

- [ ] **2. IAM policy fragment.** (Requirement 8.)
  - [ ] Create `scripts/ci-cloud-credentials/aws/policies/asg-quorum-manager.json`
        granting the seven operations from design §1 plus the fixture's
        `CreateVpc`/`CreateSubnet`/`CreateSecurityGroup`/`CreateLaunchTemplate`/
        `CreateAutoScalingGroup` and their deletes.
  - [ ] Scope each statement as narrowly as the API allows. Where Auto Scaling
        does not support resource-level permissions for an action, say so in a
        comment next to the wildcard — an unexplained `"Resource": "*"` reads as
        laziness and gets copied.
  - [ ] Teach `provision-federated-identity.sh` the `asg-quorum-manager` bundle
        name; document it in `scripts/ci-cloud-credentials/aws/README.md`.
  - [ ] **Verify**: run the script with `--dry-run` and read the emitted
        statements; then attach it and confirm with one `DescribeAutoScalingGroups`
        call made as the CI principal, not as the admin user. A local probe with
        admin credentials proves nothing about the CI role — the OCI spec learned
        this the expensive way.

- [ ] **3. The fixture, and its teardown, before any test case.** (Requirement 2,
      design §2-3.)
  - [ ] `tests/aws_asg_quorum_manager_real_test.cpp` with the standard global
        fixtures: preflight skip (exit 77), Folly init, AWS SDK init, cost
        summary, signal cleanup.
  - [ ] Fixture creates VPC, per-AZ subnets, security group (no ingress), launch
        template, and one `HealthCheckType=EC2` ASG per AZ at `MinSize 0`,
        desired 0. IGW only if task 1c says it is needed.
  - [ ] Teardown in the reverse order design §3 specifies, each step bounded,
        each expiry logged loudly and **not** aborting the remaining steps.
  - [ ] **Verify before writing a single case**: run the binary with an empty
        test case, then confirm by hand that the VPC, ASGs, launch template and
        every instance are gone. Then run it again and kill it with `SIGINT`
        mid-setup, and confirm the same. A fixture whose teardown is unproven is
        how this project accrued a $37 Alibaba bill and a $10.70 Azure one.

- [ ] **4. CTest registration and the timeout derivation.** (Requirement 1.)
  - [ ] Register under `KYTHIRA_AWS_REAL_TESTS` with `SKIP_RETURN_CODE 77` and
        labels matching the sibling real-AWS tests.
  - [ ] Set `TIMEOUT` to the sum of the cases' own
        `boost::unit_test::timeout` values plus fixture overhead, and write the
        arithmetic into the comment. Re-derive it whenever a case is added.

- [ ] **5. Cases 3.1-3.2: provision and assess.** (Requirement 3.1, 3.2.)
  - [ ] `provision_node_increases_desired_capacity`: assert desired capacity
        rose by one, a new `InService` instance exists, it carries the NodeId
        tag, and the returned address is `private_ip:node_port`.
  - [ ] `assess_quorum_reports_live_nodes`.
  - [ ] File `TestCostReport` entries. **Verify** the printed cost summary is
        non-zero and names the instance type actually launched, not a hardcoded
        one — the Azure suite quoted a `Standard_D2s_v5` rate for a `D2s_v7`
        instance for months.

- [ ] **6. Case 3.3: liveness comes from EC2, not from the ASG.**
  (Requirement 3.3.)
  - [ ] Stop the instance with a direct `StopInstances`, outside the manager,
        and assert `assess_quorum` reports it not-live.
  - [ ] **Wait for the condition, not an interval** (Requirement 7): poll until
        `DescribeInstanceStatus` stops reporting `running`, with a bounded wait
        whose expiry message names the instance and says the *stop*, not
        `assess_quorum`, is what failed. The Azure analogue of this case failed
        its first real run for exactly this reason — it asserted immediately
        after an asynchronous deallocate.

- [ ] **7. Cases 3.4-3.5: decommission.** (Requirement 3.4, 3.5.)
  - [ ] `decommission_removes_instance`: assert the instance leaves the ASG and
        desired capacity returns to its prior value.
  - [ ] `decommission_is_idempotent`: unknown NodeId, and the same node twice.
  - [ ] If the manager's removal wait expires, it must **throw**, not return
        success. If it currently returns success on expiry, that is a defect to
        fix with its own commit, not to work around in the test — see
        Requirement 7.1 and the Azure precedent it cites.

- [ ] **8. Cases 3.6-3.7: maintain_quorum and multi-AZ.**
  (Requirement 3.6, 3.7.)
  - [ ] `maintain_quorum_restores_full_cluster` after an out-of-band terminate.
  - [ ] `multi_az_topology`: assert each instance's AZ matches its group's.

- [ ] **9. Audit and sweep that can see Auto Scaling groups.** (Requirement 4,
      design §6.)
  - [ ] Audit step, `if: always()`, reporting leaked ASGs **with their desired
        capacity**, launch templates, and instances. A non-zero desired capacity
        with no instances is the finding an instance-only audit misses.
  - [ ] Bounded wait before declaring a leak, because AWS terminates
        asynchronously and one sample manufactures false alarms. Say in the
        message that the finding outlived a wait.
  - [ ] Sweep step **after** the audit, `--force-delete` on ASGs, then launch
        template, then the VPC chain.
  - [ ] **Verify by deliberately leaking**: run with teardown disabled, confirm
        the audit fails the job and names the ASG and its capacity, and the
        sweep then removes it. An audit that has never seen a real leak is an
        untested audit — this repo has shipped two of those.

- [ ] **10. The bundle, without a 26th input.** (Requirement 6, design §4.)
  - [ ] Add `BUNDLE_ASG_QUORUM` driven by the existing `aws_bundle_ec2_quorum`
        input plus a new `REAL_CLOUD_TESTS_AWS_ASG_QUORUM_ENABLED` repository
        variable. **Do not add an input.**
  - [ ] Comment the reason at the input, with the PR #257 reference, so the next
        person to add a bundle does not have to rediscover the cap.
  - [ ] Extend the job's "every bundle disabled" fail-closed guard to include
        the new bundle.
  - [ ] **Verify**: `python3 -c "import yaml; ..."` counts inputs as exactly 25,
        and `ci.yml`'s `workflow-input-limits` job passes.

- [ ] **11. Case 3.8: the health-check rejection, per task 1's answer.**
  (Requirement 5.)
  - [ ] Implement whichever branch task 1a/1b established.
  - [ ] If omitted, say so in this file **and** in a comment in the test file
        naming what is uncovered and why. Not a silent skip.

- [ ] **12. First real run, and expect defects.**
  - [ ] Dispatch AWS-only with explicit `false` for every other provider and
        bundle, then **confirm from the job list** that the others are
        `skipped` — omitting an input inherits the repository variable and can
        start four providers' billable jobs.
  - [ ] Read the log for `did not check any assertions` and `Skipping:`. Zero of
        both is the bar; a green bundle with either is the fault this spec's
        introduction is about.
  - [ ] Confirm the cost summary is present and plausible, and that the audit
        reports clean.
  - [ ] **Treat each failure as a finding, not an obstacle.** The Azure sibling
        produced five real defects on first execution. Fix them one at a time,
        each with its own commit and its own measured evidence, and record any
        hypothesis that measurement refutes so the next person does not retry it.

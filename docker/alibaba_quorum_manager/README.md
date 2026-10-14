# Alibaba Cloud components — operator guide

Two components, both hand-rolling their own request signing (no vendor SDK —
see `.kiro/specs/alibaba-cloud-services/design.md` for why):

- **`alibaba_ess_quorum_manager`** (`include/raft/alibaba_ess_quorum_manager.hpp`)
  — a `kythira::quorum_manager` over an Auto Scaling (ESS) scaling group.
- **`alibaba_oss_persistence_engine`** (`include/raft/alibaba_oss_persistence.hpp`)
  — a `kythira::persistence_engine` storing Raft state as OSS objects.

Configuration: copy `alibaba_quorum_manager.env.example` and fill it in.

## Prerequisite resources (the code deliberately does not create these)

Like every other cloud provider in this tree, these components operate
existing infrastructure rather than provisioning it. You need:

| Resource | Why |
|---|---|
| A VPC with **vSwitches in ≥2 zones** | The manager's per-zone topology is meaningless in one zone |
| A security group | Attached by the scaling configuration |
| A **scaling configuration** (image, instance type, security group) | ESS launches from this |
| A **scaling group** with MinSize 0, spanning those vSwitches | What the manager resizes |
| An **OSS bucket** in the same region | Persistence objects |

MinSize 0 matters: it means the group costs nothing at rest, and lets the
manager scale to zero between tests.

## Two behaviours to know before you deploy

**ESS chooses the zone, not the caller.** `provision_node(target_group, …)`
raises DesiredCapacity by one; ESS then places the instance according to the
group's own multi-zone policy. When the instance lands in a different zone
than `target_group` asked for, the manager **proceeds and reports the actual
zone** rather than failing — capacity is worth more than exact placement, the
same trade `aws_asg_quorum_manager` documents. If you need strict per-zone
placement, run **one scaling group per zone** and one manager per group;
that is a topology choice, not a code change.

**Persistence writes cost a network round trip, on the election hot path.**
The engine's durability contract is that `save_current_term` and
`save_voted_for` return only once OSS has acknowledged the write — that is
the whole point, and it is stronger than `file_persistence_engine`, which
does not even fsync. But it means those calls are now WAN-latency operations.

Two measurements, both to `ap-southeast-1` and both from outside the region:

| Measured from | Per write | Source |
|---|---|---|
| A developer machine | ~2–3 s per object round trip | `spike-notes.md` Finding 7 |
| A GitHub-hosted CI runner | `save_current_term` p50 **1.10 s**, p99 1.21 s; `append_log_entry` p50 1.08 s | `spike-notes.md` Finding 13 (scheduled run 36426191450) |

Both are upper bounds dominated by geography — an in-region node will see
far less, and nobody has yet measured from inside the region — but **size
election timeouts against a measurement from where your nodes actually run**,
not against these numbers and not against local-disk intuition. If your election
timeout is shorter than a round trip, the node cannot persist its vote before
the election it is voting in has already timed out.

## Timeout rollback and scale-in protection

When `provision_timeout` expires before the new instance is adoptable, the
manager undoes the scale-up without letting the group's `RemovalPolicies`
pick a victim (`.kiro/specs/group-scale-up-rollback/`). It removes every
instance that was not in the group before the increment, by id, with
`RemoveInstances(DecreaseDesiredCapacity=true)`, first waiting (within
`provision_timeout`) for any scaling activity in progress to finish, since
ESS refuses removals during one. Only when the group lists no new instance
does it write DesiredCapacity back to its old value, and it then re-lists
the group and names any existing member that left. The timeout error ends
with what the rollback did.

Every instance the manager adopts is put in ESS's `Protected` lifecycle
state with `SetInstancesProtection`, and the constructor protects any
adopted member of the cluster that is not, so no capacity change, from this
manager or anything else, makes ESS choose a voter. ESS also skips health
checks on protected members, which suits a manager that judges liveness
itself; the manager counts `Protected` as in service. `decommission_node`
still removes a protected node: `RemoveInstances` is ESS's documented way
to remove one. Instances the manager has not tagged are never protected. To
scale the group in by hand, clear protection on the instances you want
removed first:

```sh
aliyun ess SetInstancesProtection --RegionId ap-southeast-1 \
    --ScalingGroupId <asg> --InstanceId.1 <i-xxx> --ProtectedFromScaleIn false
```

Run one manager per scaling group, and do not scale it with other tooling
while the manager runs. An instance something else launches during a
provision looks new to the manager and is removed if that provision times
out.

The RAM policy needs, besides read access to ESS and ECS,
`ess:ModifyScalingGroup`, `ess:RemoveInstances`, `ess:SetInstancesProtection`
and `ecs:TagResources`. Without `ess:SetInstancesProtection` the
constructor throws as soon as the group holds an adopted member that is not
protected. `scripts/ci-cloud-credentials/alibaba/policies/ess-quorum-manager.json`
is CI's full list.

## Credentials

Three modes, all through `alibaba_client_config`:

1. **AccessKey pair** — a RAM user's long-lived key. Simplest; least good.
2. **STS temporary credentials** — set `security_token` alongside the
   temporary key pair. Preferred for anything long-lived.
3. **CI** — RAM `AssumeRoleWithOIDC`, no stored key at all. See
   `scripts/ci-cloud-credentials/alibaba/README.md`.

The region must match the bucket's region: it is folded into the OSS V4
signing scope, so a mismatch surfaces as `SignatureDoesNotMatch` rather than
as a helpful error.

## Worked example

```sh
# 1. Prerequisites (once, by an operator). See the credentials README for the
#    CI identity; these are the test resources themselves.
aliyun vpc CreateVpc        --RegionId ap-southeast-1 --CidrBlock 10.20.0.0/16
aliyun vpc CreateVSwitch    --RegionId ap-southeast-1 --VpcId <vpc> \
                            --ZoneId ap-southeast-1a --CidrBlock 10.20.1.0/24
aliyun vpc CreateVSwitch    --RegionId ap-southeast-1 --VpcId <vpc> \
                            --ZoneId ap-southeast-1b --CidrBlock 10.20.2.0/24
aliyun ecs CreateSecurityGroup --RegionId ap-southeast-1 --VpcId <vpc>
aliyun ess CreateScalingGroup  --RegionId ap-southeast-1 \
    --ScalingGroupName kythira --MinSize 0 --MaxSize 6 \
    --VSwitchIds.1 <vsw-a> --VSwitchIds.2 <vsw-b> --MultiAZPolicy BALANCE
aliyun ess CreateScalingConfiguration --RegionId ap-southeast-1 \
    --ScalingGroupId <asg> --ImageId <ubuntu-image> \
    --InstanceTypes.1 ecs.e-c1m1.large --SecurityGroupId <sg>
aliyun ess EnableScalingGroup --RegionId ap-southeast-1 \
    --ScalingGroupId <asg> --ActiveScalingConfigurationId <asc>
aliyun oss mb oss://<bucket>

# 2. Point the components at them.
cp alibaba_quorum_manager.env.example alibaba.env && $EDITOR alibaba.env
set -a && . ./alibaba.env && set +a

# 3. Exercise against the real services (opt-in; the quorum suite launches a
#    real instance and costs money, the persistence suite is ~free).
./build/tests/alibaba_oss_persistence_real_test --log_level=test_suite
./build/tests/alibaba_quorum_manager_real_test  --log_level=test_suite
```

Both suites exit **77** when configuration is missing, printing exactly which
variables are absent — a skip, not a failure, so an unconfigured checkout
does not go red.

## Verification status

- **OSS persistence: verified against the live service.** All four real cases
  pass, including a term and vote written by one engine and read back by a
  fresh one.
- **ESS quorum manager: built and mock-verified, not yet run live.** Its
  cheap cases (construct, assess, absent-node decommission) cost nothing and
  are what will first exercise the ESS/ECS response shapes, which are so far
  documentation-derived. Run those before trusting the manager in a
  deployment.

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

Measured from a developer machine to `ap-southeast-1`: **~2–3 s per object
round trip** (`spike-notes.md` Finding 7). That is an upper bound dominated
by geography — an in-region node will see far less — but **size election
timeouts against a measurement from where your nodes actually run**, not
against this number and not against local-disk intuition. If your election
timeout is shorter than a round trip, the node cannot persist its vote before
the election it is voting in has already timed out.

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

# Running kythira's AWS quorum managers

Setup for `kythira::aws_ec2_quorum_manager`
(`include/raft/aws_ec2_quorum_manager.hpp`) and
`kythira::aws_asg_quorum_manager` (`include/raft/aws_asg_quorum_manager.hpp`).
The companion [`aws_quorum_manager.env.example`](aws_quorum_manager.env.example)
lists every configuration field; this file is about the AWS resources those
fields point at, which the library deliberately does **not** create.

Both are built when the AWS SDK is found (`KYTHIRA_HAS_AWS_SDK`;
`aws-sdk-cpp` features `ec2`, `autoscaling`, `sts`).

## Which manager

| | `aws_ec2_quorum_manager` | `aws_asg_quorum_manager` |
|---|---|---|
| Provisions by | `RunInstances` into the group's subnet | Raising the group's ASG desired capacity |
| Launch settings live in | the config (AMI, type, subnets, SGs, user data, Spot, placement groups) | the ASG's launch template |
| Decommissions by | `TerminateInstances` | `TerminateInstanceInAutoScalingGroup`, decrementing desired capacity |
| Best for | Dev, staging, simple deployments | Production: launch templates, mixed instances policies, one place to change the image |

Both identify a node by its EC2 instance id (the hex digits after `i-`, read
as a 64-bit integer), so mapping a node to its instance needs no lookup, and
both judge liveness by `DescribeInstanceStatus` returning `running`. A
running instance whose kythira process has crashed still looks live to the
manager; the Raft leader that owns it catches that case instead, counting a
voter that answers no RPC for `quorum_peer_dead_after` as unreachable.

## What kythira does and does not create

| Resource | Created by |
|---|---|
| VPC, one subnet per AZ, security group | **you** |
| AMI with the kythira node installed | **you** (`packer/ca_cluster_node/` is a scripted example) |
| IAM instance profile for the nodes (optional) | **you** |
| EC2 placement groups (optional, EC2 manager) | **you** |
| Launch template and one ASG per AZ (ASG manager) | **you** |
| The instances, and their `kythira:*` tags | kythira |

## 1. Network

One subnet per Availability Zone you want a voter in, and a security group
that admits `node_port` (default 7000) from the other nodes. Nodes talk on
private IPs; the managers return `<private-ip>:<node_port>`.

```sh
aws ec2 create-security-group --group-name kythira-nodes \
    --description "kythira raft nodes" --vpc-id vpc-…
aws ec2 authorize-security-group-ingress --group-id sg-… \
    --protocol tcp --port 7000 --source-group sg-…
```

## 2. For the ASG manager: one ASG per AZ

`asg_by_group` maps each topology group to an ASG. Give each ASG subnets in
**one** AZ: the ASG, not kythira, picks the subnet for a new instance, so an
ASG spanning AZs makes `provision_node(target_group)` a request rather than
a placement. Each ASG must also:

- use `HealthCheckType=EC2`. The constructor throws on ELB health checks:
  two systems replacing instances on different health signals will
  eventually each replace a node the other still counts.
- have `MaxSize` above the target count, so a replacement can launch before
  the dead node is removed. `MinSize 0` costs nothing at rest.

```sh
aws autoscaling create-auto-scaling-group \
    --auto-scaling-group-name kythira-prod-a \
    --launch-template LaunchTemplateName=kythira-node,Version='$Latest' \
    --vpc-zone-identifier subnet-0aaaaaaaaaaaaaaaa \
    --min-size 0 --max-size 3 --desired-capacity 0 \
    --health-check-type EC2
# …and likewise kythira-prod-b, kythira-prod-c in their own AZs.
```

## 3. IAM

The principal the manager runs as needs only these actions. Scope the
mutating ones by tag or ARN where your account allows it;
`scripts/ci-cloud-credentials/aws/policies/` holds the CI policies, which
are broader because they also build and tear down test fixtures.

| Manager | Actions |
|---|---|
| EC2 | `ec2:RunInstances`, `ec2:CreateTags`, `ec2:DescribeInstances`, `ec2:DescribeInstanceStatus`, `ec2:TerminateInstances`; plus `iam:PassRole` on the node role when `iam_instance_profile` is set |
| ASG | `autoscaling:DescribeAutoScalingGroups`, `autoscaling:DescribeAutoScalingInstances`, `autoscaling:UpdateAutoScalingGroup`, `autoscaling:TerminateInstanceInAutoScalingGroup`, `autoscaling:DescribeLifecycleHooks`, `autoscaling:CompleteLifecycleAction`, `ec2:CreateTags`, `ec2:DescribeInstances`, `ec2:DescribeInstanceStatus` |

Credentials come from `aws_client_config::credentials_provider`, or, left
null, the SDK's default chain: environment variables, `~/.aws`, then the
instance profile. On EC2, use the instance profile and store no keys.

## 4. Worked example: three voters in three AZs

```sh
cp aws_quorum_manager.env.example /etc/default/aws_quorum_manager
$EDITOR /etc/default/aws_quorum_manager   # region, AMI or ASGs, subnets, SGs
chmod 0600 /etc/default/aws_quorum_manager
```

Then, in the loader that builds the config struct (the EC2 manager shown;
the ASG manager takes `asg_by_group` instead of the launch fields):

```cpp
#include <raft/aws_ec2_quorum_manager.hpp>

kythira::aws_ec2_quorum_manager_config cfg{
    .cluster_name = "kythira-prod",
    .image_id = "ami-0123456789abcdef0",
    .instance_type = "t3.micro",
    .node_port = 7000,
    .topology = {.groups = {
        {.group_id = "us-east-1a", .target_count = 1},
        {.group_id = "us-east-1b", .target_count = 1},
        {.group_id = "us-east-1c", .target_count = 1},
    }},
    .subnet_by_group = {
        {"us-east-1a", "subnet-0aaaaaaaaaaaaaaaa"},
        {"us-east-1b", "subnet-0bbbbbbbbbbbbbbbb"},
        {"us-east-1c", "subnet-0cccccccccccccccc"},
    },
    .security_group_ids = {"sg-0123456789abcdef0"},
    .aws = {.region = "us-east-1"},
};
kythira::aws_ec2_quorum_manager<std::uint64_t, std::string> mgr{cfg};
```

## 5. Verifying

- **Without an AWS account**: `docker compose -f docker/aws-localstack-compose.yml up -d`,
  then `build/tests/aws_quorum_manager_localstack_test`. The EC2 cases run;
  the ASG cases skip because LocalStack's community edition has no Auto
  Scaling.
- **Against real AWS**: `tests/aws_quorum_manager_real_ec2_test.cpp` and
  `tests/aws_asg_quorum_manager_real_test.cpp`, built whenever the AWS SDK
  is found (the EC2 suite also needs libssh2). They launch real instances
  and cost money. They read their own test variables (`KYTHIRA_TEST_AMI_ID`,
  `KYTHIRA_TEST_INSTANCE_TYPE`, …, see each file's header and
  `.github/workflows/real-cloud-tests.yml`), not this file's, and exit 77,
  a skip, when unconfigured.

## Known limitations

- **`{NODE_ID}` is not substituted in the EC2 manager's user data.** The
  node id comes from the instance id, which does not exist until
  `RunInstances` returns. Have the node read its own id from instance
  metadata or its `kythira:node-id` tag.
- **The ASG manager accepts a `replacing` hint and ignores it.** A
  replacement always gets a fresh instance and so a fresh node id.

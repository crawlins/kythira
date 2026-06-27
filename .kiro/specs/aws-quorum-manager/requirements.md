# Requirements Document

## Introduction

This document specifies the requirements for two AWS-based `quorum_manager`
implementations for Kythira: `aws_ec2_quorum_manager` and `aws_asg_quorum_manager`.

`aws_ec2_quorum_manager` directly manages EC2 instances through the EC2 API —
provisioning by calling `RunInstances`, assessing launch success by calling
`DescribeInstanceStatus`, and decommissioning by calling `TerminateInstances`.
The kythira `NodeId` is derived directly from the EC2 instance ID: the hex
suffix of `i-XXXXXXXXXXXXXXXXX` is interpreted as a `uint64_t`. This
eliminates `DescribeInstances`-based ID lookups and makes the manager
stateless. This is the closest AWS analogue to `docker_quorum_manager` and is
the primary implementation.

`aws_asg_quorum_manager` manages one Auto Scaling Group per placement group. It
drives cluster size by adjusting ASG desired capacity rather than calling
`RunInstances` directly. This is the production-grade option: it inherits
launch template versioning, spot instance support, and AWS-managed instance
replacement from the ASG, while the quorum manager retains control over *when*
capacity changes occur and *which* instance is decommissioned.

Both classes satisfy `quorum_manager<Q, NodeId, Address, GroupId>` from
`include/raft/quorum_management.hpp`. The kythira-level `NodeId` is derived
from the EC2 instance ID (hex suffix → `uint64_t` numeric value), so no
separate tag-based counter is maintained. The `kythira:node-id` EC2 tag stores
this derived value for operator visibility only.

The implementations live in:
- `include/raft/aws_ec2_quorum_manager.hpp`
- `include/raft/aws_asg_quorum_manager.hpp`

They are compiled behind `#ifdef KYTHIRA_HAS_AWS_SDK`, set by the build
system when `aws-sdk-cpp` with the `ec2` and `autoscaling` components is
detected.

## Glossary

- **`aws_ec2_quorum_manager`**: A `quorum_manager` implementation that manages EC2
  instances directly via the EC2 API.
- **`aws_asg_quorum_manager`**: A `quorum_manager` implementation that drives one
  Auto Scaling Group per placement group.
- **placement group**: In this spec, a logical failure domain — typically an
  AWS Availability Zone (`"us-east-1a"`). Not to be confused with the AWS EC2
  Placement Group resource (see below).
- **EC2 Placement Group**: An AWS resource that controls the physical placement
  of EC2 instances. Has a strategy: `none` (no group), `cluster` (low-latency,
  same rack), `spread` (distinct hardware, max 7 per AZ), or `partition`
  (separate hardware partitions). Configured via `ec2_placement_group_config`
  and applied per kythira placement group (AZ) in `placement_by_group`.
- **kythira node ID**: The `NodeId` used at the Raft layer. Derived
  deterministically from the EC2 instance ID: `stoull(ec2_id.substr(2),
  nullptr, 16)` → `uint64_t`. Stored in the `kythira:node-id` tag for
  operator visibility. The reverse mapping (`node_id_to_ec2_id`) reconstructs
  the EC2 instance ID without any AWS API call.
- **EC2 instance ID**: The `i-0XXXXXXXXXXXXXXXX` (17 hex digit) identifier
  assigned by AWS. The numeric portion is the kythira `NodeId`; there is no
  separate sequential counter.
- **user data script**: A shell script supplied to EC2 at launch time that
  starts the kythira node process. The manager interpolates `{NODE_ID}`,
  `{NODE_PORT}`, `{CLUSTER}`, and `{AZ}` placeholders.
- **`aws-sdk-cpp`**: The AWS SDK for C++; provides `Aws::EC2::EC2Client` and
  `Aws::AutoScaling::AutoScalingClient`.
- **`desired_topology`**: The `desired_topology<GroupId>` struct from
  `quorum_management.hpp`, expressing the target node count per AZ.

---

## Requirements

### Requirement 1: Build System Detection

**User Story:** As a developer building Kythira, I want the AWS implementations
to be compiled only when `aws-sdk-cpp` is available so that the project builds
cleanly on machines without the SDK.

#### Acceptance Criteria

1. `CMakeLists.txt` SHALL call `find_package(AWSSDK COMPONENTS ec2 autoscaling)`
   and set `KYTHIRA_HAS_AWS_SDK` when both components are found.
2. Both headers SHALL be wrapped in `#ifdef KYTHIRA_HAS_AWS_SDK` / `#endif`
   so they compile to nothing when the SDK is absent.
3. The `KYTHIRA_HAS_AWS_SDK` definition SHALL be propagated to consuming
   targets via `target_compile_definitions`.
4. `DEPENDENCIES.md` SHALL gain an entry: `aws-sdk-cpp ≥ 1.11 (ec2 +
   autoscaling components) — AWS EC2 and Auto Scaling quorum managers`.

---

### Requirement 2: Shared AWS Configuration

**User Story:** As a library user, I want a single credential/region
configuration struct I can fill once and pass to either AWS quorum manager,
rather than duplicating fields across both configuration structs.

#### Acceptance Criteria

1. An `aws_client_config` struct SHALL be defined in
   `include/raft/aws_client_config.hpp` (compiled unconditionally) with:

   | Field | Type | Default | Purpose |
   |---|---|---|---|
   | `region` | `std::string` | `""` | AWS region (empty = SDK default / `$AWS_DEFAULT_REGION`) |
   | `endpoint_override` | `std::string` | `""` | Override endpoint URL for LocalStack or VPC endpoints |
   | `api_timeout` | `std::chrono::seconds` | `30s` | Per-call timeout |
   | `credentials_provider` | `std::shared_ptr<Aws::Auth::AWSCredentialsProviderChain>` | `nullptr` | Credentials provider chain (only available when `KYTHIRA_HAS_AWS_SDK` is defined) |

2. When `credentials_provider` is `nullptr`, the implementations SHALL use
   `DefaultAWSCredentialsProviderChain` (env vars `$AWS_ACCESS_KEY_ID` /
   `$AWS_SECRET_ACCESS_KEY` / `$AWS_SESSION_TOKEN`, `~/.aws/credentials`
   profile via `$AWS_PROFILE`, IAM instance profile, ECS task role, etc.).
   When non-null, the provided chain is passed to every AWS service client
   constructor instead.
   There are no `access_key_id`, `secret_access_key`, or `session_token` fields
   on `aws_client_config`.
3. `aws_client_config` SHALL be an aggregate (no user-declared constructors).

---

### Requirement 3: `aws_ec2_quorum_manager` Configuration

**User Story:** As a library user deploying a multi-AZ Kythira cluster on EC2,
I want a configuration struct that captures all required instance launch
parameters so that I can construct the manager with a single designated
initializer and have the constructor validate the configuration.

#### Acceptance Criteria

1. An `aws_ec2_quorum_manager_config` struct SHALL be defined with:

   | Field | Type | Default | Purpose |
   |---|---|---|---|
   | `aws` | `aws_client_config` | `{}` | Credentials and region |
   | `cluster_name` | `std::string` | *(required)* | Scope for EC2 tag filters and `Name` tag |
   | `image_id` | `std::string` | *(required)* | AMI ID to launch for new nodes |
   | `instance_type` | `std::string` | `"t3.medium"` | EC2 instance type |
   | `key_name` | `std::string` | `""` | EC2 key pair name (empty = no key) |
   | `iam_instance_profile` | `std::string` | `""` | IAM instance profile ARN or name |
   | `security_group_ids` | `std::vector<std::string>` | `{}` | Security groups to attach |
   | `subnet_by_group` | `std::map<std::string, std::string>` | `{}` | GroupId (AZ) → subnet ID |
   | `node_port` | `std::uint16_t` | `7000` | Port the kythira process listens on |
   | `user_data_template` | `std::string` | `""` | Shell script; `{NODE_ID}`, `{NODE_PORT}`, `{CLUSTER}`, `{AZ}` are substituted |
   | `topology` | `desired_topology<std::string>` | `{}` | Target counts per AZ |
   | `provision_timeout` | `std::chrono::seconds` | `300s` | Max time to wait for instance `running` state |
   | `poll_interval` | `std::chrono::milliseconds` | `5000ms` | Interval between `DescribeInstances` polls during provision |
   | `placement_by_group` | `std::map<std::string, ec2_placement_group_config>` | `{}` | GroupId (AZ) → EC2 Placement Group config; absent = no placement group for that AZ |
   | `spot_options` | `std::optional<ec2_spot_options>` | `std::nullopt` | When set, instances are requested as one-time spot; when absent, on-demand is used |
   | `extra_tags` | `std::map<std::string, std::string>` | `{}` | Additional EC2 tags on every managed instance |

2. `aws_ec2_quorum_manager` SHALL validate at construction time that `cluster_name`
   and `image_id` are non-empty, `node_port` is non-zero, and every group in
   `topology.groups` has a corresponding entry in `subnet_by_group`. Violations
   SHALL throw `std::invalid_argument`.

---

### Requirement 4: `aws_ec2_quorum_manager` Class Interface

**User Story:** As a library user, I want `aws_ec2_quorum_manager` to satisfy
`quorum_manager<aws_ec2_quorum_manager<NodeId, Address>, NodeId, Address,
std::string>` so that I can wire it into a Raft node without any glue code.

#### Acceptance Criteria

1. `aws_ec2_quorum_manager<NodeId, Address>` SHALL satisfy
   `quorum_manager<aws_ec2_quorum_manager<NodeId, Address>, NodeId, Address,
   std::string>` with `placement_group_id_type = std::string`, verified by a
   `static_assert` in its header.
2. The class SHALL be defined in `include/raft/aws_ec2_quorum_manager.hpp`.
3. The class SHALL be move-constructible and move-assignable; copy is deleted
   (it holds an `Aws::EC2::EC2Client` which is non-copyable).
4. The class SHALL initialize the `Aws::EC2::EC2Client` with the region and
   endpoint from `aws_client_config`, applying explicit credentials when
   provided.
5. The class SHALL provide `maintain_quorum(cluster)` returning
   `kythira::Future<quorum_health<NodeId, std::string>>` per Requirement 19.

---

### Requirement 5: `aws_ec2_quorum_manager` Node Tagging Scheme

**User Story:** As an operator, I want every managed EC2 instance to carry
well-known tags so that I can audit which instances belong to a cluster, and so
that the quorum manager can reconstruct state after a process restart without
keeping in-memory records.

#### Acceptance Criteria

1. Every instance created by `provision_node` SHALL carry these tags:

   | Tag key | Value |
   |---|---|
   | `Name` | `kythira-{cluster_name}-{node_id}` |
   | `kythira:cluster` | `{cluster_name}` |
   | `kythira:node-id` | decimal string of the `NodeId` (= the numeric value of the EC2 instance ID hex suffix) |
   | `kythira:group` | `{target_group}` |
   | `kythira:managed-by` | `kythira-ec2-quorum-manager` |
   | `kythira:market` | `"spot"` when `config.spot_options` is set; `"on-demand"` otherwise |

2. Any tags in `config.extra_tags` SHALL be merged in; they SHALL NOT override
   the six tags above.
3. Because `NodeId = ec2_id_to_node_id(ec2_id)` and the reverse
   `node_id_to_ec2_id(nid)` is a pure computation, the manager needs no
   `DescribeInstances` call to look up the EC2 instance ID from a node ID.
   These tags are for operator visibility and filtering only.

---

### Requirement 6: `aws_ec2_quorum_manager::assess_quorum`

**User Story:** As an orchestrator, I need `assess_quorum` to report which
nodes are live at the EC2 infrastructure layer so that stopped or terminated
instances are detected without relying on an application-level heartbeat.

#### Acceptance Criteria

1. `assess_quorum` SHALL accept the caller-supplied `cluster` vector (a list
   of `node_placement<NodeId, GroupId>`). When the vector is empty, it SHALL
   return a healthy result immediately without making any AWS API call.
2. `assess_quorum` SHALL call `DescribeInstanceStatus` with
   `SetIncludeAllInstances(true)` and the list of EC2 instance IDs derived from
   the cluster vector via `node_id_to_ec2_id(np.node_id)` for each entry.
3. A node is **live** when `DescribeInstanceStatus` returns an entry for its
   EC2 instance ID with `InstanceState.Name = running`.
4. A node is **unreachable** when:
   - Its EC2 instance ID is absent from the `DescribeInstanceStatus` response, OR
   - `InstanceState.Name` is any value other than `running`
     (`stopped`, `shutting-down`, `terminated`, `pending`, etc.).
5. `quorum_status` SHALL be derived from the ratio of live to total nodes in
   the cluster vector, using the standard four-level mapping from
   `quorum_management.hpp`, applied globally.
6. Per-group health SHALL be computed from the `group_id` field of each
   `node_placement` in the cluster vector. The `target_count` for each group
   SHALL come from `config.topology`.
7. WHEN the `DescribeInstanceStatus` call fails THEN `assess_quorum` SHALL
   return an exceptional Future.
8. `assess_quorum` SHALL NOT modify any EC2 resources.
9. `assess_quorum` SHALL check the fault injection point
   `"raft/aws/ec2/describe_instance_status"` before calling
   `DescribeInstanceStatus`.

---

### Requirement 7: `aws_ec2_quorum_manager::provision_node`

**User Story:** As an orchestrator that has detected a degraded placement group,
I need `provision_node` to launch a new EC2 instance in the target AZ, wait
for it to reach `running` state, and return its kythira node ID and address so
that the node can join the cluster via the normal `ClusterJoin` flow.

#### Acceptance Criteria

1. `provision_node(target_group, replacing)` SHALL derive the new `NodeId`
   from the EC2 instance ID returned by `RunInstances` via
   `ec2_id_to_node_id(ec2_id)` (= `stoull(ec2_id.substr(2), nullptr, 16)`).
   No pre-launch `DescribeInstances` call is made to determine the next ID.
2. The new instance SHALL be launched in the subnet given by
   `config.subnet_by_group.at(target_group)`. When `target_group` is absent
   from `subnet_by_group` the Future SHALL be rejected with
   `std::invalid_argument`.
3. `provision_node` SHALL call `RunInstances` (min/max count = 1) with:
   - `ImageId`: `config.image_id`
   - `InstanceType`: `config.instance_type`
   - `KeyName`: `config.key_name` (omitted when empty)
   - `IamInstanceProfile`: `config.iam_instance_profile` (omitted when empty)
   - `SecurityGroupIds`: `config.security_group_ids`
   - `SubnetId`: the subnet for `target_group`
   - `UserData`: base64-encoded `config.user_data_template` after placeholder
     substitution (`{NODE_ID}`, `{NODE_PORT}`, `{CLUSTER}`, `{AZ}`)
   - `Placement.GroupName`: the `name` from `config.placement_by_group[target_group]`
     when the entry exists and `strategy ≠ none`; omitted otherwise
   - `Placement.PartitionNumber`: the `partition_number` from the above config
     when `strategy = partition` and `partition_number > 0`; omitted otherwise
4. After `RunInstances` succeeds, `provision_node` SHALL apply the tags
   from Requirement 5.1 to the new instance via `CreateTags`.
5. `provision_node` SHALL then poll `DescribeInstances` for the new instance at
   `config.poll_interval` intervals until `instance-state-name = running` or
   `config.provision_timeout` elapses. Polling SHALL use the EC2 instance ID
   returned by `RunInstances`, not a tag filter.
6. Once the instance is `running`, `provision_node` SHALL read
   `PrivateIpAddress` from the `DescribeInstances` response and return
   `peer_info{new_node_id, "{private_ip}:{node_port}"}`.
7. WHEN `RunInstances` fails THEN the Future SHALL be rejected with the AWS
   error; no cleanup is needed (no instance was created).
8. WHEN `provision_timeout` elapses before `running` state is reached THEN
   `provision_node` SHALL call `TerminateInstances` on the new EC2 instance
   (best-effort cleanup) and return an exceptional Future.
9. The `replacing` hint, when non-null, SHALL be logged for diagnostic
   purposes. This implementation does not copy instance attributes from the
   replaced node.

---

### Requirement 8: `aws_ec2_quorum_manager::decommission_node`

**User Story:** As an orchestrator removing a broken node, I need
`decommission_node` to terminate the EC2 instance so that it cannot rejoin
and the account is not billed for a permanently broken node.

#### Acceptance Criteria

1. `decommission_node(node_id)` SHALL derive the EC2 instance ID via
   `node_id_to_ec2_id(node_id)` (a pure computation; no AWS API call needed)
   and call `TerminateInstances` with that ID.
2. WHEN `TerminateInstances` returns an `InvalidInstanceID.NotFound` error
   (instance never existed or was already purged from AWS's tracking)
   THEN `decommission_node` SHALL return a successfully-resolved Future
   (idempotent). AWS also accepts `TerminateInstances` on an already-terminated
   instance without error, so recently terminated instances are handled
   transparently.
3. WHEN `TerminateInstances` fails for any other reason THEN
   `decommission_node` SHALL return an exceptional Future with the AWS error
   message.
4. After a successful `TerminateInstances` call, `decommission_node` SHALL
   poll `DescribeInstanceStatus` (with `IncludeAllInstances=true`) until the
   instance state is no longer `running` (i.e., transitions to `shutting-down`,
   `terminated`, or disappears from the response), or 30 seconds elapse.
   This ensures that a subsequent `assess_quorum` call reliably sees the
   instance as unreachable rather than observing a brief window of eventual
   consistency where `TerminateInstances` has been acknowledged but the EC2
   state has not yet been updated.
5. `decommission_node` SHALL NOT remove the node from the Raft cluster
   configuration — that is done by the `remove_server()` / `ClusterLeave`
   path.

---

### Requirement 9: `aws_ec2_quorum_manager::topology`

**User Story:** As an orchestrator, I need `topology()` to return the desired
node count per AZ so that I can compute per-group deficits.

#### Acceptance Criteria

1. `topology()` SHALL return `config.topology` unmodified.
2. `topology()` SHALL be synchronous and make no AWS API calls.

---

### Requirement 10: `aws_asg_quorum_manager` Configuration

**User Story:** As a library user running a production cluster, I want an ASG-
backed quorum manager so that I can leverage launch templates, spot capacity,
and ASG placement spread, while the quorum manager retains authority over
*when* new nodes are added or removed.

#### Acceptance Criteria

1. An `aws_asg_quorum_manager_config` struct SHALL be defined with:

   | Field | Type | Default | Purpose |
   |---|---|---|---|
   | `aws` | `aws_client_config` | `{}` | Credentials and region |
   | `cluster_name` | `std::string` | *(required)* | Scope for tag filters; also identifies which instances belong to this cluster |
   | `asg_by_group` | `std::map<std::string, std::string>` | *(required, ≥1 entry)* | GroupId (AZ) → Auto Scaling Group name |
   | `node_port` | `std::uint16_t` | `7000` | Port the kythira process listens on |
   | `provision_timeout` | `std::chrono::seconds` | `300s` | Max time to wait for a new instance to become `InService` |
   | `poll_interval` | `std::chrono::milliseconds` | `5000ms` | Interval between `DescribeAutoScalingGroups` polls during provision |
   | `topology` | `desired_topology<std::string>` | `{}` | Desired counts per group (used by `topology()` and for validation only; not used to set ASG desired capacity at construction) |

   There are no `heartbeat_timeout` or `heartbeat_grace_period` fields.
   Liveness is determined solely by EC2 instance state via `DescribeInstanceStatus`.

2. `aws_asg_quorum_manager` SHALL validate at construction that `cluster_name` is
   non-empty, `asg_by_group` is non-empty, `node_port` is non-zero, and every
   group in `topology.groups` has a corresponding entry in `asg_by_group`.
   Violations SHALL throw `std::invalid_argument`.
3. At construction, the manager SHALL call `DescribeAutoScalingGroups` for every
   ASG in `asg_by_group` and verify that each one has `HealthCheckType == "EC2"`.
   If any ASG uses a different health check type (e.g. `"ELB"`), the constructor
   SHALL throw `std::invalid_argument`.

   **Rationale:** kythira assesses node liveness via `DescribeInstanceStatus`
   (instance state == `running`). AWS Auto Scaling EC2 health checks use the
   same signal. If the ASG used a different health check type (e.g. ELB), the
   ASG could terminate and replace an instance that kythira still considers live,
   causing a split-brain. Aligning health check types keeps both systems
   consistent.

---

### Requirement 11: `aws_asg_quorum_manager` Class Interface

**User Story:** As a library user, I want `aws_asg_quorum_manager` to satisfy the
`quorum_manager` concept with the same NodeId/Address/GroupId type parameters
as `aws_ec2_quorum_manager` so that the two classes are interchangeable at the type
level.

#### Acceptance Criteria

1. `aws_asg_quorum_manager<NodeId, Address>` SHALL satisfy
   `quorum_manager<aws_asg_quorum_manager<NodeId, Address>, NodeId, Address,
   std::string>` with `placement_group_id_type = std::string`, verified by a
   `static_assert` in its header.
2. The class SHALL be defined in `include/raft/aws_asg_quorum_manager.hpp`.
3. The class SHALL be move-constructible; copy is deleted.
4. The class SHALL hold one `Aws::AutoScaling::AutoScalingClient` and one
   `Aws::EC2::EC2Client` (needed for `DescribeInstances` in `assess_quorum`
   and `decommission_node`).
5. The class SHALL provide `maintain_quorum(cluster)` returning
   `kythira::Future<quorum_health<NodeId, std::string>>` per Requirement 19.

---

### Requirement 12: `aws_asg_quorum_manager::assess_quorum`

**User Story:** As an orchestrator, I need `assess_quorum` to determine which
kythira nodes are live at the EC2 layer, using `DescribeInstanceStatus` to
reflect actual EC2 state rather than application-level heartbeats.

#### Acceptance Criteria

1. `assess_quorum` SHALL delegate liveness checking to
   `DescribeInstanceStatus` using the same algorithm as
   `aws_ec2_quorum_manager::assess_quorum` (Requirement 6): the EC2 instance
   IDs are derived from the cluster vector via `ec2_mgr_t::node_id_to_ec2_id`,
   and a node is live iff its `InstanceState.Name = running`.
2. When the cluster vector is empty `assess_quorum` SHALL return a healthy
   result immediately without making any AWS API call.
3. `quorum_status` and per-group health SHALL be computed using the same rules
   as `aws_ec2_quorum_manager::assess_quorum` (Requirements 6.5–6.6).
4. WHEN the `DescribeInstanceStatus` call fails THEN `assess_quorum` SHALL
   return an exceptional Future.
5. `assess_quorum` SHALL check the fault injection point
   `"raft/aws/asg/describe_instance_status"` before calling
   `DescribeInstanceStatus`.

---

### Requirement 13: `aws_asg_quorum_manager::provision_node`

**User Story:** As an orchestrator, I need `provision_node` to increase the
target ASG's desired capacity by one and wait for a new `InService` instance
to appear, then return that instance's kythira node ID and address.

#### Acceptance Criteria

1. `provision_node(target_group, replacing)` SHALL identify the ASG name from
   `config.asg_by_group.at(target_group)`. When `target_group` is absent the
   Future SHALL be rejected with `std::invalid_argument`.
2. `provision_node` SHALL call `DescribeAutoScalingGroups` to read the current
   `DesiredCapacity`, then call `UpdateAutoScalingGroup` to set
   `DesiredCapacity = current + 1`.
3. After increasing desired capacity, `provision_node` SHALL poll
   `DescribeAutoScalingGroups` at `config.poll_interval` intervals until a
   new instance with `LifecycleState = InService` appears that does NOT yet
   have a `kythira:node-id` tag, or until `config.provision_timeout` elapses.
4. Once the new instance is found, `provision_node` SHALL derive the new
   kythira node ID via `ec2_mgr_t::ec2_id_to_node_id(new_ec2_id)` (no
   separate `DescribeInstances` call to determine the next sequential ID)
   and tag the instance with the tags from Requirement 5.1.
5. `provision_node` SHALL then call `DescribeInstances` with the EC2 instance
   ID to read `PrivateIpAddress` and return
   `peer_info{new_node_id, "{private_ip}:{node_port}"}`.
6. WHEN `provision_timeout` elapses THEN `provision_node` SHALL call
   `UpdateAutoScalingGroup` to restore `DesiredCapacity` to its original value
   (best-effort rollback) and return an exceptional Future.

---

### Requirement 14: `aws_asg_quorum_manager::decommission_node`

**User Story:** As an orchestrator, I need `decommission_node` to remove a
specific instance from its ASG and terminate it, reducing the ASG's desired
capacity so that the ASG does not immediately replace it.

#### Acceptance Criteria

1. `decommission_node(node_id)` SHALL derive the EC2 instance ID via
   `ec2_mgr_t::node_id_to_ec2_id(node_id)` (a pure computation; no AWS API
   call) and call `TerminateInstanceInAutoScalingGroup` with
   `ShouldDecrementDesiredCapacity = true`, which both terminates the instance
   and decrements the ASG desired capacity atomically.
2. WHEN `TerminateInstanceInAutoScalingGroup` fails with a "not found" or
   `ValidationError` THEN `decommission_node` SHALL return a
   successfully-resolved Future (idempotent).
3. WHEN the call fails for any other reason THEN `decommission_node` SHALL
   return an exceptional Future.
4. After a successful `TerminateInstanceInAutoScalingGroup`, `decommission_node`
   SHALL apply the same 30-second consistency poll as
   `aws_ec2_quorum_manager::decommission_node` (AC 4 of Requirement 8).

---

### Requirement 15: Fault Injection

**User Story:** As a developer writing chaos tests, I need fault injection
points in both managers so that I can simulate AWS API failures without
actually interacting with AWS.

#### Acceptance Criteria

1. `aws_ec2_quorum_manager::assess_quorum` SHALL check the fault injection point
   `"raft/aws/ec2/describe_instance_status"` (throws the injected exception
   when enabled) before calling `DescribeInstanceStatus`.
2. `aws_ec2_quorum_manager::provision_node` SHALL check
   `"raft/aws/ec2/run_instances"` before calling `RunInstances`.
3. `aws_ec2_quorum_manager::decommission_node` SHALL check
   `"raft/aws/ec2/terminate_instances"` before calling `TerminateInstances`.
4. `aws_asg_quorum_manager::assess_quorum` SHALL check
   `"raft/aws/asg/describe_instance_status"` before calling
   `DescribeInstanceStatus`.
5. `aws_asg_quorum_manager::provision_node` SHALL check
   `"raft/aws/asg/update_asg"` before calling `UpdateAutoScalingGroup`.
6. `aws_asg_quorum_manager::decommission_node` SHALL check
   `"raft/aws/asg/terminate_instance"` before calling
   `TerminateInstanceInAutoScalingGroup`.
7. `aws_ec2_quorum_manager::maintain_quorum` SHALL check
   `"raft/aws/ec2/maintain_quorum"` before executing the assessment step.
8. `aws_asg_quorum_manager::maintain_quorum` SHALL check
   `"raft/aws/asg/maintain_quorum"` before executing the assessment step.
9. All fault points SHALL compile to no-ops when `FIU_ENABLE` is not defined,
   using the `fiu_do_on()` macro from `include/raft/fault_injection.hpp`.

---

### Requirement 17: EC2 Placement Group Support

**User Story:** As a library user running a latency-sensitive or HA Kythira
cluster on EC2, I want to specify an EC2 Placement Group strategy per AZ so
that the quorum manager places new nodes on optimal hardware — tightly packed
for throughput (cluster), on distinct racks for resilience (spread), or in
named hardware partitions for topology-aware fault isolation (partition).

#### Acceptance Criteria

1. An `ec2_placement_group_strategy` enum SHALL be defined in
   `include/raft/aws_ec2_quorum_manager.hpp` (inside `KYTHIRA_HAS_AWS_SDK`
   guard) with four values:

   | Value | EC2 Placement Group strategy | Notes |
   |---|---|---|
   | `none` | No placement group | Default; `Placement.GroupName` omitted from `RunInstances` |
   | `cluster` | `cluster` | Packs instances on low-latency hardware in one AZ |
   | `spread` | `spread` | Each instance on distinct underlying hardware; max 7 per AZ |
   | `partition` | `partition` | Instances divided into named partitions with separate hardware |

2. An `ec2_placement_group_config` struct SHALL be defined with:

   | Field | Type | Default | Purpose |
   |---|---|---|---|
   | `name` | `std::string` | `""` | EC2 Placement Group resource name; empty means `strategy::none` |
   | `strategy` | `ec2_placement_group_strategy` | `none` | Strategy type (informational; EC2 enforces via the group resource) |
   | `partition_number` | `std::uint32_t` | `0` | For `partition` strategy: target partition (0 = EC2 auto-assigns) |

3. `aws_ec2_quorum_manager_config` SHALL contain a field:
   `std::map<std::string, ec2_placement_group_config> placement_by_group{}`
   mapping each kythira `GroupId` (AZ name) to its placement group config.
   Absent entries mean no placement group for that AZ.

4. WHEN `placement_by_group[target_group].name` is non-empty THEN
   `provision_node` SHALL include `Placement.GroupName` in the `RunInstances`
   request (per Requirement 7.3).

5. WHEN `strategy = partition` AND `partition_number > 0` THEN `provision_node`
   SHALL additionally include `Placement.PartitionNumber` in the `RunInstances`
   request.

6. WHEN `placement_by_group` has no entry for `target_group` OR
   `placement_by_group[target_group].name` is empty THEN `provision_node`
   SHALL omit the `Placement` block from `RunInstances` entirely (AWS default
   behaviour: no placement constraint).

7. The placement group strategy SHALL be recorded in the instance tag
   `kythira:placement-strategy` (value: `"none"`, `"cluster"`, `"spread"`, or
   `"partition"`). When no placement group is used, the tag value is `"none"`.

8. A unit test SHALL verify that constructing `aws_ec2_quorum_manager_config`
   with `strategy = partition`, `partition_number = 2`, and a non-empty `name`
   correctly populates the struct (no validation error).

---

### Requirement 16: Tests

**User Story:** As a developer, I need automated tests for both AWS quorum
managers that can run without an AWS account, plus optional integration tests
that run against real AWS or LocalStack.

#### Acceptance Criteria

##### Concept satisfaction

1. `static_assert`s in both headers SHALL verify the `quorum_manager` concept
   is satisfied for `aws_ec2_quorum_manager<std::uint64_t, std::string>` and
   `aws_asg_quorum_manager<std::uint64_t, std::string>`.

##### Unit tests (no AWS dependency)

2. A unit test file `tests/aws_quorum_manager_unit_test.cpp` SHALL be added
   and registered as CTest target `aws-quorum-manager-unit-tests` with labels
   `unit;aws;quorum_manager`.
3. A unit test SHALL verify that constructing `aws_ec2_quorum_manager` with an
   empty `cluster_name` throws `std::invalid_argument`.
4. A unit test SHALL verify that constructing `aws_ec2_quorum_manager` with a
   `topology` group that has no corresponding entry in `subnet_by_group`
   throws `std::invalid_argument`.
5. A unit test SHALL verify that constructing `aws_asg_quorum_manager` with an
   empty `asg_by_group` throws `std::invalid_argument`.
6. A unit test SHALL verify that `provision_node` with a `target_group` not
   in `subnet_by_group` (for `aws_ec2_quorum_manager`) or `asg_by_group` (for
   `aws_asg_quorum_manager`) returns an exceptional Future.
7. Chaos (fault injection) unit tests SHALL verify that enabling
   `"raft/aws/ec2/describe_instance_status"` causes
   `aws_ec2_quorum_manager::assess_quorum` to return an exceptional Future,
   and similarly for the other fault points (Requirement 15).
8. A unit test SHALL verify that the `ec2_id_to_node_id` / `node_id_to_ec2_id`
   round-trip is correct: `node_id_to_ec2_id(ec2_id_to_node_id(id)) == id`
   for a representative EC2 instance ID.

##### Integration test fixture infrastructure

Both the LocalStack and real-AWS integration test fixtures SHALL manage
AWS/LocalStack resource lifecycles, following these rules:

8. Each fixture SHALL generate a unique UUID per test run (e.g.
   `kythira-test-{uuid}`) and tag every AWS resource it creates with
   `kythira:test-run = {uuid}`. Resources provided via env vars are used
   as-is and are never deleted by the fixture.

9. As the first action in setup, before any resource creation, each fixture
   SHALL call `sts:GetCallerIdentity` using the configured credentials and
   endpoint. If the call fails for any reason (missing credentials, expired
   token, insufficient permissions, or network error to the endpoint) the
   entire test suite SHALL be skipped (not failed). This check ensures that
   a missing IAM role, an expired CI token, or a LocalStack instance that is
   not running causes a clean skip rather than a cascade of confusing AWS API
   errors. For LocalStack fixtures the call is directed at `endpoint_override`
   so an unreachable LocalStack also causes a skip.

10. Each fixture SHALL, in its setup phase, verify or create the following
    resources before constructing any `aws_ec2_quorum_manager`:

   | Resource | Env-var override | Created when absent |
   |---|---|---|
   | VPC | `AWS_TEST_VPC_ID` | `CreateVpc(CidrBlock="10.77.0.0/16")` |
   | Internet Gateway | *(auto-detected)* | `CreateInternetGateway` + `AttachInternetGateway(vpc)`; route `0.0.0.0/0 → igw` on the public route table |
   | Public subnet (bastion) | `AWS_TEST_BASTION_SUBNET_ID` | `CreateSubnet(vpc, "10.77.3.0/28", az1)` with `MapPublicIpOnLaunch=true` |
   | NAT Gateway | *(always created)* | `AllocateAddress` (EIP) + `CreateNatGateway(SubnetId=bastion_subnet, AllocationId=eip)`; add route `0.0.0.0/0 → nat` on each private route table; needed for EC2/STS API calls from private-subnet nodes |
   | Private subnet AZ1 (cluster) | `AWS_TEST_SUBNET_ID_AZ1` | `CreateSubnet(vpc, "10.77.0.0/24", az1)` |
   | Private subnet AZ2 (cluster) | `AWS_TEST_SUBNET_ID_AZ2` | `CreateSubnet(vpc, "10.77.1.0/24", az2)` |
   | Private subnet AZ3 (cluster) | `AWS_TEST_SUBNET_ID_AZ3` | `CreateSubnet(vpc, "10.77.2.0/24", az3)` |
   | S3 VPC gateway endpoint | *(auto-detected)* | `CreateVpcEndpoint(ServiceName=com.amazonaws.{region}.s3, VpcId)` when none exists; associated with all private route tables |
   | Cluster node security group | `AWS_TEST_SG_ID` | `CreateSecurityGroup(vpc)`; ingress: port 7000 from `10.77.0.0/16`; SSH (22) from bastion SG |
   | Quarantine security group | *(always created)* | `CreateSecurityGroup(vpc, "kythira-test-quarantine-{uuid}")`; **no inbound rules, no outbound rules** (all traffic blocked); used by tests to simulate complete network isolation |
   | Deny-all Network ACL | *(always created)* | `CreateNetworkAcl(vpc)`; add deny-all inbound rule (rule 1, all traffic, DENY) and deny-all outbound rule (rule 1, all traffic, DENY); used to simulate subnet-level network partition for `az_outage_instances_launch_but_cannot_join` |
   | Bastion security group | *(always created)* | `CreateSecurityGroup(vpc)`; ingress: SSH (22) from `AWS_TEST_ALLOWED_CIDR` (default `0.0.0.0/0`) |
   | SSH key pair | `AWS_TEST_KEY_NAME` | `CreateKeyPair(KeyName=kythira-test-{uuid})`; private key material held in memory for the test run only and NOT written to disk |
   | Bastion EC2 instance | *(always created)* | `RunInstances(ImageId=AWS_TEST_AMI_ID, InstanceType=t3.nano, SubnetId=bastion_subnet, SG=bastion_sg, KeyName=key, MarketType=spot, SpotInstanceType=one-time)`; tagged `kythira:test-run = {uuid}` |
   | IAM instance profile | `AWS_TEST_IAM_PROFILE` | `CreateRole` (EC2 trust) with inline policy: `ec2:DescribeInstances`, `ec2:CreateTags`, `s3:GetObject` on `arn:aws:s3:::{AWS_TEST_S3_BUCKET}/kythira-test/*`; wrap in `CreateInstanceProfile` + `AddRoleToInstanceProfile` |
   | kythira-node S3 object | *(always uploaded)* | Upload `KYTHIRA_NODE_BINARY` to `s3://{AWS_TEST_S3_BUCKET}/kythira-test/{uuid}/kythira-node` |
   | PG — cluster strategy | `AWS_TEST_PG_CLUSTER_NAME` | `CreatePlacementGroup(Strategy=cluster)` |
   | PG — spread strategy | `AWS_TEST_PG_SPREAD_NAME` | `CreatePlacementGroup(Strategy=spread)` |
   | PG — partition strategy | `AWS_TEST_PG_PARTITION_NAME` | `CreatePlacementGroup(Strategy=partition, PartitionCount=2)` |

   Cluster nodes are in private subnets. The NAT Gateway provides egress for EC2
   API and STS API calls (heartbeat, peer discovery, status tags). The S3 VPC
   gateway endpoint handles S3 traffic without passing through the NAT. The bastion
   is the sole public-facing access point for SSH. Placement groups are created on
   demand per test, not unconditionally.

10. Each fixture SHALL, in its teardown phase, destroy resources in the
    following order (reverse dependency order, cost-bearing items first):

    a. Terminate all cluster node EC2 instances provisioned during the test
       AND the bastion instance; poll `DescribeInstances` until all reach
       `terminated` state (or 120 s elapses).
    b. Delete the kythira-node S3 object uploaded during setup.
    c. Delete any EC2 Placement Groups created during setup.
    d. Restore any subnet that had its NACL association replaced to the VPC's
       default NACL, then delete the deny-all NACL (always created).
    e. Delete the quarantine security group (always created).
    f. Delete the bastion security group (always created).
    g. Delete any cluster node security group created during setup.
    h. Delete any subnets created during setup (bastion subnet and all
       private cluster subnets: AZ1, AZ2, AZ3).
    i. Remove the S3 VPC gateway endpoint (if fixture created it).
    j. Remove the IAM role from the instance profile, delete the instance
       profile, then delete the IAM role (all three, in order) when the
       fixture created them.
    k. Delete the NAT Gateway (always created); wait for state `deleted`
       (or 60 s); then release the associated Elastic IP address.
    l. Detach and delete the Internet Gateway (if fixture created it).
    m. Delete the SSH key pair (if fixture created it).
    n. Delete any VPC created during setup.

    Each teardown step SHALL be executed unconditionally regardless of whether
    previous steps failed. All teardown errors SHALL be collected and written to
    `std::cerr` after all steps complete. Teardown failures SHALL NOT cause the
    test to report failure — resource leak warnings are surfaced to the operator
    but do not indicate a bug in the quorum manager.

    This teardown sequence applies equally when setup fails mid-way: if any
    resource creation step throws, the fixture destructor runs immediately (via
    RAII), applies best-effort cleanup to every resource that was successfully
    created before the failure, and the test is reported as skipped (not
    failed). The fixture SHALL record the setup failure reason and surface it as
    the skip message so operators can distinguish a skipped-due-to-missing-infra
    run from a skipped-due-to-missing-credentials run.

11. The real-AWS fixture's `user_data_template` SHALL bootstrap kythira using
    the following sequence, which runs on the EC2 instance at first boot:

    ```bash
    #!/bin/bash
    set -e
    # Download binary from S3 via the VPC gateway endpoint
    aws s3 cp s3://{S3_BUCKET}/{S3_PREFIX}/kythira-node /usr/local/bin/kythira-node
    chmod +x /usr/local/bin/kythira-node

    REGION=$(curl -s http://169.254.169.254/latest/meta-data/placement/region)
    INSTANCE_ID=$(curl -s http://169.254.169.254/latest/meta-data/instance-id)

    # Start kythira. kythira-node is responsible for writing kythira:last-heartbeat
    # to EC2 tags at its configured heartbeat interval. No external heartbeat loop.
    /usr/local/bin/kythira-node \
        --node-id={NODE_ID} --port={NODE_PORT} --cluster={CLUSTER} \
        --ec2-heartbeat-tag=kythira:last-heartbeat \
        --ec2-instance-id="$INSTANCE_ID" \
        --ec2-region="$REGION" &
    KYTHIRA_PID=$!

    # Wait until the node is accepting connections on its port
    until nc -z localhost {NODE_PORT} 2>/dev/null; do sleep 1; done

    # Discover peers via EC2 tags and call ClusterJoin on each
    PEERS=$(aws ec2 describe-instances \
        --region "$REGION" \
        --filters "Name=tag:kythira:cluster,Values={CLUSTER}" \
                  "Name=instance-state-name,Values=running" \
        --query 'Reservations[].Instances[].[PrivateIpAddress,Tags[?Key==`kythira:node-id`]|[0].Value]' \
        --output text)
    while IFS=$'\t' read -r peer_ip peer_id; do
        [ "$peer_id" = "{NODE_ID}" ] && continue   # skip self
        kythira-join --host "$peer_ip" --port {NODE_PORT} || true
    done <<< "$PEERS"

    # Signal readiness via EC2 tag (written by the user_data script, not kythira)
    aws ec2 create-tags --region "$REGION" \
        --resources "$INSTANCE_ID" \
        --tags Key=kythira:status,Value=ready
    ```

    The `kythira:last-heartbeat` tag is written exclusively by the kythira
    process using its built-in EC2 heartbeat mechanism (configured via the
    `--ec2-heartbeat-tag` / `--ec2-instance-id` / `--ec2-region` flags or
    equivalent configuration). No shell-level heartbeat loop is used; the
    quorum manager relies solely on the kythira-native heartbeat. When kythira
    exits (crash, signal, OOM) or is unable to reach the EC2 API (network
    quarantine), heartbeat updates stop and `assess_quorum` detects the failure
    after `heartbeat_timeout`.

    The `{S3_BUCKET}` and `{S3_PREFIX}` tokens are substituted by the fixture
    when constructing `user_data_template` (before passing it to
    `aws_ec2_quorum_manager_config`); they are constants for the run.
    `{NODE_ID}`, `{NODE_PORT}`, `{CLUSTER}` are the existing per-node
    placeholders substituted by `provision_node`.

    The fixture SHALL provision nodes one at a time (sequential, not
    concurrent), waiting after each `provision_node` call for the
    `kythira:status = ready` tag to appear on the new instance before
    provisioning the next. This ensures each joining node finds its
    predecessor already running and tagged.

    The fixture SHALL poll `DescribeInstances` for `kythira:status = ready`
    on each newly provisioned instance, with a timeout of 120 s.

    LocalStack integration tests SHALL use `user_data_template = "#!/bin/bash\n"`
    (a no-op) because LocalStack does not execute user_data scripts. LocalStack
    tests verify API call sequences and tag behaviour only; they do not test
    kythira node startup or cluster formation.

##### Integration tests (LocalStack)

12. LocalStack integration tests SHALL be placed in
    `tests/aws_quorum_manager_localstack_test.cpp` and guarded by
    `KYTHIRA_AWS_LOCALSTACK_TESTS=1`. They SHALL use the fixture infrastructure
    from ACs 8–11 with `endpoint_override = "http://localhost:4566"` and dummy
    credentials. LocalStack creates all resources synchronously without real
    billing, so teardown order still applies for correctness (e.g. deleting a
    subnet before its SG).
13. The LocalStack fixture SHALL construct `aws_ec2_quorum_manager` with
    `spot_options = ec2_spot_options{}`. LocalStack accepts
    `InstanceMarketOptions` without error. EC2 test cases:
    a. `ec2_provision_three_nodes`: provision 3 nodes; verify correct tags,
       sequential IDs, and `kythira:market == "spot"`.
    b. `ec2_assess_detects_stopped_node`: stop one instance; verify
       `quorum_status::degraded`.
    c. `ec2_decommission_all_nodes`: decommission each; verify `terminated`.
    d. `ec2_decommission_idempotent`: double-decommission; verify resolved Future.
14. A LocalStack integration test SHALL exercise `aws_asg_quorum_manager` with
    an ASG created during fixture setup (via `CreateAutoScalingGroup` against
    LocalStack). Test cases:
    `asg_provision_increments_desired_capacity`,
    `asg_assess_detects_not_inservice`,
    `asg_decommission_decrements_desired_capacity`,
    `asg_decommission_idempotent`. The ASG SHALL be deleted in fixture teardown.

##### Integration tests (real AWS EC2)

15. Real-AWS integration tests SHALL be placed in
    `tests/aws_quorum_manager_real_ec2_test.cpp` and guarded by
    `KYTHIRA_AWS_REAL_EC2_TESTS=1`. They SHALL be excluded from the default
    CTest run and tagged `integration;aws;real-ec2`.
16. Real-AWS tests SHALL read the following env vars. Only `AWS_REGION` is
    strictly required; the test SHALL skip (not fail) when it is absent. All
    other resources are created or auto-detected by the fixture when the
    corresponding env var is absent (per AC 9).

    | Variable | Required | Purpose |
    |---|---|---|
    | `AWS_REGION` | Yes | AWS region (e.g. `us-east-1`) |
    | `AWS_TEST_AMI_ID` | No | AMI ID override; when absent the fixture queries `DescribeImages` for the latest Amazon Linux 2023 HVM AMI matching the build-host architecture (`x86_64` or `arm64`) from owner `amazon` |
    | `AWS_TEST_S3_BUCKET` | Yes | S3 bucket for kythira-node binary upload; bucket must already exist |
    | `KYTHIRA_NODE_BINARY` | Yes | Local path to the kythira-node binary to upload |
    | `AWS_TEST_INSTANCE_TYPE` | No | Cluster node instance type (default `t3.micro` on x86_64, `t4g.micro` on aarch64) |
    | `KYTHIRA_TEST_BASTION_INSTANCE_TYPE` | No | Bastion instance type (default `t3.micro` on x86_64, `t4g.micro` on aarch64) |
    | `AWS_TEST_VPC_ID` | No | Pre-existing VPC; created if absent |
    | `AWS_TEST_SUBNET_ID_AZ1` | No | Pre-existing private subnet AZ1; created if absent |
    | `AWS_TEST_SUBNET_ID_AZ2` | No | Pre-existing private subnet AZ2; created if absent |
    | `AWS_TEST_SUBNET_ID_AZ3` | No | Pre-existing private subnet AZ3; created if absent |
    | `AWS_TEST_BASTION_SUBNET_ID` | No | Pre-existing public subnet for bastion; created if absent |
    | `AWS_TEST_SG_ID` | No | Pre-existing cluster node security group; created if absent |
    | `AWS_TEST_KEY_NAME` | No | Pre-existing EC2 key pair name; created if absent |
    | `AWS_TEST_ALLOWED_CIDR` | No | CIDR allowed to SSH to bastion (default `0.0.0.0/0`; restrict to test runner IP for tighter security) |
    | `AWS_TEST_IAM_PROFILE` | No | Pre-existing IAM instance profile; created if absent |
    | `AWS_TEST_PG_CLUSTER_NAME` | No | Pre-existing cluster PG; created if absent |
    | `AWS_TEST_PG_SPREAD_NAME` | No | Pre-existing spread PG; created if absent |
    | `AWS_TEST_PG_PARTITION_NAME` | No | Pre-existing partition PG; created if absent |

17. Each real-AWS test SHALL use a unique `cluster_name` suffixed with a
    random UUID so that concurrent runs do not interfere.
18. The shared `RealEc2Fixture` SHALL construct `aws_ec2_quorum_manager` with
    `spot_options = ec2_spot_options{}`. Individual test cases that need
    on-demand behaviour construct their own manager with
    `spot_options = std::nullopt` explicitly.
19. Real-AWS `aws_ec2_quorum_manager` test cases:
    a. **`provision_and_assess_single_az`**: provision 3 nodes in `AZ1`; verify
       `quorum_status::healthy`, `live_node_count = 3`,
       `InstanceLifecycle == "spot"`, and `kythira:market == "spot"` (Req 18.10).
    b. **`provision_multi_az_topology`**: provision 2 nodes in `AZ1` and 1 in
       `AZ2`; verify per-group health with correct AZ labels.
    c. **`terminate_one_node_degraded`**: provision 3 nodes; stop one via
       `StopInstances`; verify `quorum_status::degraded` and that node in
       `unreachable_nodes`.
    d. **`decommission_idempotent`**: call `decommission_node` twice on the same
       node ID; verify both return a resolved Future.
    e. **`placement_group_cluster_strategy`**: provision 2 nodes with cluster PG;
       verify `Placement.GroupName` in `DescribeInstances`.
    f. **`placement_group_spread_strategy`**: same with spread PG across AZ1+AZ2.
    g. **`placement_group_partition_strategy`**: provision 2 nodes with
       `partition_number = 1`; verify both land in partition 1.
    h. **`provision_timeout_cleanup`**: set `provision_timeout = 1s`; verify
       exceptional Future and that the partially-created instance terminates.
    i. **`on_demand_provision_and_decommission`**: construct fresh manager with
       `spot_options = std::nullopt`; verify `InstanceLifecycle` absent/`"normal"`
       and `kythira:market == "on-demand"`; decommission and verify `terminated`.
    j. **`heartbeat_timeout_triggers_replacement`**: per Requirement 19.6.
    k. **`network_isolation_triggers_replacement`**: assign the quarantine SG
       to one node (via `ModifyNetworkInterfaceAttribute`); wait for
       `heartbeat_timeout` to elapse; call `assess_quorum` and verify the
       node is `unreachable`; call `maintain_quorum`; verify the quarantined
       instance is terminated and a replacement in the same AZ reaches
       `kythira:status = ready`. This test exercises the network-issue
       failure mode: the EC2 instance is alive but completely isolated.
    l. **`host_termination_triggers_replacement`**: call `TerminateInstances`
       on one node directly (bypassing the quorum manager); call
       `assess_quorum` and verify the node is `unreachable`; call
       `maintain_quorum`; verify a replacement in the same AZ reaches
       `kythira:status = ready`. This test exercises the hardware-failure
       mode: the instance is destroyed at the infrastructure layer.
    m. **`az_outage_during_rolling_deployment`**: Uses a 3-AZ × 3-node
       topology (9 nodes total: 3 in each of AZ1, AZ2, AZ3). Simulates an
       entire AZ going offline while a simultaneous single-host failure occurs
       in another AZ, as would happen during a rolling software deployment. Test
       sequence:
       i.   Provision all 9 nodes (sequential with readiness wait).
       ii.  Simulate complete AZ3 failure: call `decommission_node` on all 3
            AZ3 nodes (TerminateInstances → EC2 state = shutting-down/terminated).
       iii. Simulate a single-host failure in AZ2: call `decommission_node` on
            1 AZ2 node.
       iv.  Call `assess_quorum`; verify `quorum_status::critical` (5 of 9
            live: 3 in AZ1 + 2 in AZ2), AZ1=3/3 live, AZ2=2/3 live,
            AZ3=0/3 live, 4 nodes in `unreachable_nodes`.
       v.   Call `maintain_quorum`; verify:
            - All 4 unreachable nodes are decommissioned (idempotent
              TerminateInstances on already-terminated instances).
            - 3 replacements provisioned in AZ3, 1 in AZ2.
            - All 4 replacements reach `kythira:status = ready`.
       vi.  Call `assess_quorum` again; verify `quorum_status::healthy`,
            `live_node_count = 9`.
    n. **`az_outage_provision_fails_in_broken_az`**: Uses a 3-AZ × 3-node
       topology. Tests the scenario where `maintain_quorum` attempts to
       provision replacement nodes into a broken AZ but the instances fail to
       launch. Test sequence:
       i.   Provision all 9 nodes (sequential with readiness wait).
       ii.  Call `decommission_node` on all 3 AZ3 nodes and 1 AZ2 node.
       iii. Construct a second `aws_ec2_quorum_manager` with the same config
            except `subnet_by_group["AZ3"]` replaced with
            `"subnet-00000000000000000"` (a syntactically valid but
            non-existent subnet ID). This simulates the AZ's subnets being
            unavailable in the control plane.
       iv.  Call `maintain_quorum` on the broken-config manager.
       v.   Verify: `maintain_quorum` returns the pre-remediation
            `quorum_health` without throwing (Req 19.5). Decommissions of
            the 4 unreachable nodes succeed. AZ3 `provision_node` calls fail
            (AWS error: invalid subnet); failures are written to `std::cerr`.
            AZ2 `provision_node` succeeds and the replacement reaches
            `kythira:status = ready`.
       vi.  Call `assess_quorum` using the original (valid) manager; verify
            `quorum_status::degraded` (6/9 live: AZ1=3, AZ2=3, AZ3=0).

    o. **`az_outage_instances_launch_but_cannot_join`**: Uses a 3-AZ × 3-node
       topology. Tests the scenario where `maintain_quorum` successfully
       provisions replacement nodes into the broken AZ at the EC2 layer via
       the control plane, but those instances cannot communicate at the data
       plane because a subnet-level NACL blocks all traffic. Test sequence:
       i.   Provision all 9 nodes (sequential with readiness wait).
       ii.  Apply the deny-all NACL to the AZ3 subnet
            (`ReplaceNetworkAclAssociation`). All data-plane traffic into and
            out of AZ3 is now blocked. New instances launched into this subnet
            will reach EC2 `running` state (the control plane is unaffected by
            the data-plane NACL) but cannot communicate with the cluster.
       iii. Call `decommission_node` on all 3 AZ3 nodes (TerminateInstances
            via control plane succeeds despite NACL).
       iv.  Call `assess_quorum`; verify AZ3=0/3 live.
       v.   Call `maintain_quorum` (using the valid manager with correct
            subnets). Verify: 3 new AZ3 instances launch and reach EC2
            `running` (so `provision_node` returns `peer_info` for each);
            AZ2 replacement decommissions (if any) and provisions succeed.
       vi.  `assess_quorum` immediately after: new AZ3 instances are
            `running` → live. Verify AZ3=3/3 live.
       vii. Restore AZ3 subnet to the default NACL
            (`ReplaceNetworkAclAssociation`).

20. All existing tests SHALL pass without modification after adding these files.
21. The `RealEc2Fixture` SHALL expose the following helpers:
    - `get_console_output(ec2_instance_id) → std::string` per Req 19.7;
      returns empty string on failure (never throws).
    - `stop_instance(ec2_instance_id)`: calls `StopInstances` then polls
      `DescribeInstances` until `instance-state-name = stopped` or a 3-minute
      deadline elapses.
    - `start_instance(ec2_instance_id)`: calls `StartInstances` then polls
      until `instance-state-name = running`.
    These helpers are called from test cases; the fixture handles cleanup of
    stopped or running instances via `terminate_cluster_instances()` in teardown.

---

### Requirement 18: EC2 Spot Instance Support

**User Story:** As a cost-conscious operator running a Kythira cluster on EC2,
I want `aws_ec2_quorum_manager` to optionally launch nodes as spot instances so
that I can reduce instance cost, while relying on the existing quorum assessment
loop to detect and re-provision any instances that are interrupted.

#### Acceptance Criteria

1. An `ec2_spot_interruption_behavior` enum SHALL be defined in
   `include/raft/aws_ec2_quorum_manager.hpp` (inside `KYTHIRA_HAS_AWS_SDK`
   guard) with three values:

   | Value | `InstanceInterruptionBehavior` sent to EC2 |
   |---|---|
   | `terminate` | `terminate` (default) |
   | `stop` | `stop` (EBS-backed instances only) |
   | `hibernate` | `hibernate` (requires hibernation prerequisites) |

2. An `ec2_spot_options` struct SHALL be defined with:

   | Field | Type | Default | Purpose |
   |---|---|---|---|
   | `max_price` | `std::string` | `""` | Maximum hourly price; empty = AWS on-demand price cap |
   | `interruption_behavior` | `ec2_spot_interruption_behavior` | `terminate` | Action taken on interruption |

3. `aws_ec2_quorum_manager_config` SHALL contain a field
   `std::optional<ec2_spot_options> spot_options{}` (see Requirement 3.1).
   When `std::nullopt`, `provision_node` requests on-demand capacity.

4. WHEN `spot_options` is set THEN `provision_node` SHALL include
   `InstanceMarketOptions` in the `RunInstances` request with:
   - `MarketType = "spot"`
   - `SpotOptions.SpotInstanceType = "one-time"` (persistent spot is never used;
     the quorum manager owns the instance lifecycle)
   - `SpotOptions.MaxPrice` set to `spot_options.max_price` when non-empty;
     omitted otherwise
   - `SpotOptions.InstanceInterruptionBehavior` mapped from
     `spot_options.interruption_behavior`

5. WHEN `spot_options` is absent THEN `provision_node` SHALL NOT include
   `InstanceMarketOptions` in `RunInstances` (on-demand is the EC2 default).

6. The instance tag `kythira:market` SHALL be set to `"spot"` when
   `spot_options` is set and `"on-demand"` when it is absent (see Requirement
   5.1).

7. `assess_quorum` SHALL NOT require any changes for spot support. When a
   spot instance is interrupted, EC2 transitions its state to `shutting-down`
   within the 2-minute notice window. The existing `assess_quorum` filter
   (`instance-state-name in [pending, running]`) already causes interrupted
   instances to appear unreachable on the next assessment cycle.

8. `aws_asg_quorum_manager` SHALL NOT expose any spot-related configuration.
   Spot and mixed on-demand/spot capacity are configured in the ASG's launch
   template or mixed-instances policy, which is the caller's responsibility.

9. A unit test SHALL verify that constructing `aws_ec2_quorum_manager_config`
   with `spot_options = ec2_spot_options{.max_price = "0.05",
   .interruption_behavior = ec2_spot_interruption_behavior::terminate}` populates
   the struct correctly (no exception).

10. The `provision_and_assess_single_az` test (Req 16.16a) SHALL additionally
    call `DescribeInstances` on each provisioned instance and verify that
    `InstanceLifecycle == "spot"` and the `kythira:market` tag is `"spot"`,
    confirming that the fixture's default spot config is propagated end-to-end.

---

### Requirement 19: `maintain_quorum` — Topology-Aware Auto-Remediation

**User Story:** As a Raft leader, I need a single call that assesses quorum
health and automatically terminates unhealthy nodes and provisions replacements
in the correct placement groups, so that the cluster self-heals without
requiring a separate orchestration loop that must understand AWS infrastructure.

#### Acceptance Criteria

1. The `quorum_manager` concept in `include/raft/quorum_management.hpp` SHALL
   be extended to require `maintain_quorum`:
   ```cpp
   { mgr.maintain_quorum(cluster) }
       -> std::same_as<kythira::Future<quorum_health<NodeId, GroupId>>>;
   ```
   The `no_op_quorum_manager` SHALL implement `maintain_quorum` by calling its
   own `assess_quorum` and returning the result unchanged (no decommission, no
   provisioning). The `docker_quorum_manager` SHALL implement the full
   assess → decommission → provision loop.

2. `aws_ec2_quorum_manager::maintain_quorum(cluster)` SHALL:
   a. Check fault point `"raft/aws/ec2/maintain_quorum"` (Req 15.7).
   b. Call `assess_quorum(cluster)` internally. On exceptional Future, propagate
      immediately without any remediation.
   c. For each node in `quorum_health.unreachable_nodes`, call
      `decommission_node(node_id)`. Each decommission is fire-and-wait (the
      Future is awaited before proceeding to the next). Decommission failures
      SHALL be collected and logged but SHALL NOT abort the remaining steps.
   d. Compute per-group deficits: for each group `g` in `config.topology`,
      `deficit[g] = target_count[g] − live_count[g]`. Groups not in the
      topology are ignored.
   e. For each group `g` with `deficit[g] > 0`, call `provision_node(g,
      replacing_hint)` `deficit[g]` times. The `replacing_hint` is the node ID
      of a decommissioned node from that group, if one exists; otherwise
      `std::nullopt`. Provision failures SHALL be collected and returned in an
      aggregate error after all groups are attempted; they SHALL NOT abort
      provisioning in other groups.
   f. Return the `quorum_health` computed in step (b) — the state **before**
      remediation. This lets the caller log what triggered the repair. The
      updated health is visible on the next `maintain_quorum` call after new
      nodes have started and written their first heartbeats.

3. `maintain_quorum` SHALL honor the node topology declared in
   `config.topology`: it NEVER provisions a replacement in a group other than
   the one with a deficit. A cluster configured as 2 nodes in AZ1 + 1 node in
   AZ2 that loses both AZ1 nodes will receive 2 new nodes in AZ1, not 2 nodes
   anywhere.

4. `aws_asg_quorum_manager::maintain_quorum(cluster)` SHALL follow the same
   sequence as AC 2, substituting `TerminateInstanceInAutoScalingGroup` for the
   decommission step and `UpdateAutoScalingGroup` for the provision step per the
   existing `decommission_node` and `provision_node` semantics.

5. Provision failures in step (e) SHALL be written to `std::cerr` and SHALL
   NOT cause `maintain_quorum` to return an exceptional Future. `maintain_quorum`
   SHALL always return the pre-remediation `quorum_health` when `assess_quorum`
   succeeded, regardless of how many provision calls failed. The caller detects
   persistent failures on the next `maintain_quorum` or `assess_quorum` call
   when the newly launched instances fail to produce heartbeats.

6. WHEN `assess_quorum` returns an exceptional Future THEN `maintain_quorum`
   SHALL propagate that exception immediately. No decommission or provision
   calls are made.

6. The integration test suite SHALL include a test
   `heartbeat_timeout_triggers_replacement` (real-AWS only) that:
   a. Provisions a 3-node cluster in AZ1 using the default
      `RealEc2Fixture` (spot, sequential with readiness wait).
   b. SSHes through the bastion host to one cluster node and kills the kythira
      process (`kill $(pgrep kythira-node)`).
   c. Waits for `heartbeat_timeout` (configured to 30 s for this test).
   d. Calls `assess_quorum` and verifies the dead node appears in
      `unreachable_nodes`.
   e. Calls `maintain_quorum` and verifies:
      - The dead instance is terminated.
      - A new instance is provisioned in AZ1 (same group as the dead node).
      - The new instance reaches `kythira:status = ready` (120 s timeout).
   f. Calls `GetConsoleOutput` on both the dead instance (before termination)
      and the new instance (after ready) and includes the output in the test
      log for post-mortem analysis.

7. The integration test fixture SHALL expose a helper
   `get_console_output(ec2_instance_id)` that calls `GetConsoleOutput` and
   returns the decoded output as `std::string`. The fixture SHALL call this
   helper automatically when a readiness timeout expires and include the output
   in the skip/failure message.

---

### Requirement 20: Test Cost Estimation and Reporting

**User Story:** As a developer running the real-EC2 integration suite, I want
each test to report the AWS costs it incurred and a grand total at the end, so
that I can understand the financial impact of the test run and set appropriate
billing alerts.

Every real-EC2 integration test case MUST emit an estimate of the AWS costs it
incurred, and the test module MUST print a consolidated cost summary after all
test cases complete.

#### Acceptance Criteria

1. A `BilledResource` struct SHALL track a named AWS resource with an hourly
   rate and a wall-clock start/end interval.  `cost_usd()` returns
   `(end − start).hours() * hourly_rate`.  Resources with no explicit end time
   SHALL use the moment `cost_usd()` is called.

2. A `TestCostReport` SHALL aggregate all `BilledResource` entries for one test
   case and expose:
   - `total_usd() -> double` — sum of all resource costs.
   - `format() -> std::string` — a multi-line human-readable breakdown with one
     row per resource (label, duration in minutes, cost in USD) plus a TOTAL
     row.

3. A module-level `CostAccumulator` (file-scope variable, mutex-protected)
   SHALL collect one `TestCostReport` per test case.  A `CostSummaryFixture`
   registered with `BOOST_GLOBAL_FIXTURE` SHALL print the consolidated table
   (one row per test + grand total + disclaimer) via `BOOST_TEST_MESSAGE` in
   its destructor after all test cases have run.

4. `RealEc2Fixture` SHALL automatically open billing timers for the following
   fixture-level resources:
   - **EIP**: from `AllocateAddress` call to `ReleaseAddress` in teardown.
   - **NAT gateway**: from `CreateNatGateway` call to `DeleteNatGateway` in
     teardown.
   - **Bastion instance** (`bastion_instance_type × 1`, spot): from `RunInstances`
     call to `TerminateInstances` in teardown.

5. `RealEc2Fixture` SHALL expose a public method:
   ```cpp
   void track_instances(std::size_t count);
   ```
   that opens a billing timer for `count` cluster instances of `instance_type`
   beginning at the moment of the call.  Test cases MUST call this immediately
   after provisioning their initial cluster set.  Instances provisioned
   internally by `maintain_quorum` are outside the caller's view and are
   therefore excluded from the estimate; the report SHALL note this.

6. `RealEc2Fixture::teardown()` SHALL, after all AWS resource deletions:
   a. Call `finalize()` on every open `BilledResource` (setting end = now).
   b. Emit the per-test cost report via `BOOST_TEST_MESSAGE`.
   c. Push the completed `TestCostReport` into the global `CostAccumulator`.

7. The pricing table SHALL use published on-demand us-east-1 Linux prices for
   known instance types; unknown types SHALL fall back to the t3.micro rate
   ($0.0104/hr).  Fixed rates applied:
   - NAT Gateway: $0.045/hr (data-processing charges are not tracked).
   - EIP: $0.005/hr (all EIPs billed regardless of association since Feb 2024).

8. The module-level summary table format SHALL be:
   ```
   ================================================================
    AWS Real-EC2 Test Cost Estimate Summary
   ================================================================
     <test_name>                               $0.001234
     ...
   ----------------------------------------------------------------
     GRAND TOTAL                               $0.012345
   ================================================================
    Pricing: on-demand us-east-1 Linux (approximate).
    Actual costs vary by region, savings plans, and data transfer.
    Use AWS Cost Explorer for authoritative billing data.
   ================================================================
   ```

9. Unit tests and LocalStack integration tests SHALL NOT be subject to this
   requirement; those test binaries incur no real AWS spend.

---

### Requirement 21: Signal-Driven Test Cleanup

**User Story:** As a developer running the real-EC2 integration suite, I want any
trappable signal that would prematurely terminate the test process to trigger the
same AWS resource cleanup that a normal test teardown performs, so that killing or
interrupting the test never leaves VPCs, NAT gateways, EIPs, key pairs, IAM roles,
or EC2 instances orphaned in the account.

#### Acceptance Criteria

1. The real-EC2 test binary SHALL install handlers for all trappable signals that
   would otherwise cause premature process exit: at minimum SIGTERM, SIGINT, SIGHUP,
   SIGQUIT, and SIGPIPE.  SIGKILL and SIGSTOP are untrappable and are explicitly
   excluded.  SIGALRM SHALL NOT be intercepted because Boost.Test uses it for
   per-test-case timeouts.

2. A single global atomic pointer SHALL track the currently-active `RealEc2Fixture`
   instance.  The fixture SHALL register itself in this pointer before allocating any
   AWS resource, and SHALL clear it as the first act of its destructor.

3. When a handled signal fires, the handler SHALL:
   a. Atomically exchange the global pointer to `nullptr` so re-entrant signals are
      no-ops.
   b. Call `teardown()` on the previously-active fixture (if any).
   c. Reset the signal disposition to `SIG_DFL` (via `SA_RESETHAND` or explicit
      `sigaction`) and re-raise the signal, so the process exits with the correct
      status and coredump behaviour is preserved.

4. Signal handlers SHALL be installed by a `BOOST_GLOBAL_FIXTURE` named
   `SignalHandlerFixture` so that coverage begins before the first test case
   fixture constructor runs.

5. The `teardown()` method SHALL be idempotent (guarded by a `torn_down_` flag) so
   that a signal firing concurrently with normal teardown does not double-free
   resources or double-print cost reports.

6. Making AWS SDK calls from a signal handler is not async-signal-safe; this
   trade-off is accepted because the only alternative (losing all orphaned resources)
   is worse for a long-running integration test suite.  This caveat SHALL be
   documented in a comment at the signal handler definition.

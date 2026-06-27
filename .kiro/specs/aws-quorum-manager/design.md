# Design Document

## Overview

This document describes the design for two AWS-based `quorum_manager`
implementations. Both satisfy the `quorum_manager<Q, NodeId, Address, GroupId>`
concept from `include/raft/quorum_management.hpp` with `GroupId = std::string`
(AWS Availability Zone name).

| Class | Mechanism | Best for |
|---|---|---|
| `aws_ec2_quorum_manager` | Direct `RunInstances` / `TerminateInstances` | Dev / staging, simple deployments |
| `aws_asg_quorum_manager` | `UpdateAutoScalingGroup` desired-capacity | Production (spot, launch templates, AZ rebalance) |

Both classes are header-only, compiled behind `#ifdef KYTHIRA_HAS_AWS_SDK`, and
use `aws-sdk-cpp` ≥ 1.11 (`ec2` + `autoscaling` components). Node identity at
the Raft layer is a sequential integer counter stored in the `kythira:node-id`
EC2 tag, not the EC2 instance ID.

## Architecture

```
quorum_manager concept (quorum_management.hpp)
  │
  ├── aws_ec2_quorum_manager                    (include/raft/aws_ec2_quorum_manager.hpp)
  │     ├── Aws::EC2::EC2Client
  │     ├── assess_quorum  → DescribeInstances (tag filter)
  │     ├── provision_node → RunInstances + CreateTags + poll DescribeInstances
  │     └── decommission_node → DescribeInstances + TerminateInstances
  │
  └── aws_asg_quorum_manager                   (include/raft/aws_asg_quorum_manager.hpp)
        ├── Aws::AutoScaling::AutoScalingClient
        ├── Aws::EC2::EC2Client             (for tag reads and decommission)
        ├── assess_quorum  → DescribeAutoScalingGroups + DescribeInstances
        ├── provision_node → UpdateAutoScalingGroup (capacity+1) + poll + CreateTags
        └── decommission_node → DescribeInstances + TerminateInstanceInAutoScalingGroup

Shared:
  aws_client_config (include/raft/aws_client_config.hpp)  — credentials / region / timeout
```

## Data Models

### EC2 Tag Schema

Every EC2 instance managed by either class carries five tags:

```
Name                        = kythira-{cluster_name}-{node_id}
kythira:cluster             = {cluster_name}
kythira:node-id             = {node_id}              ; decimal string for integer NodeId
kythira:group               = {az_name}              ; e.g. "us-east-1a"
kythira:managed-by          = kythira-ec2-quorum-manager
kythira:placement-strategy  = {strategy}             ; "none", "cluster", "spread", or "partition"
kythira:market              = {market}               ; "on-demand" or "spot"
kythira:last-heartbeat      = {unix_timestamp}       ; seconds since epoch; written by kythira process, not by provision_node
```

The first seven tags are written by `provision_node` via `CreateTags`. The
`kythira:last-heartbeat` tag is written and updated by the kythira node
process itself (via the heartbeat loop in user_data or a future built-in
mechanism) and is read by `assess_quorum` and `maintain_quorum`. Its absence
on a recently-launched instance is normal; its absence on an older instance
indicates the kythira process has not started or has crashed.

These tags make both managers stateless across restarts: the mapping from
kythira `NodeId` to EC2 instance ID is reconstructed from tag filters on every
API call.

## Components and Interfaces

### 1. `include/raft/aws_client_config.hpp`

A plain aggregate, compiled unconditionally (no SDK guard needed; the fields
are all standard library types):

```cpp
#pragma once

#include <chrono>
#include <string>

namespace kythira {

struct aws_client_config {
    std::string region;
    std::string endpoint_override;
    std::string access_key_id;
    std::string secret_access_key;
    std::string session_token;
    std::chrono::seconds api_timeout{30};
};

}  // namespace kythira
```

### 2. `include/raft/aws_ec2_quorum_manager.hpp`

#### Spot instance types

```cpp
enum class ec2_spot_interruption_behavior : std::uint8_t {
    terminate,  // AWS terminates the instance on interruption (default)
    stop,       // AWS stops the instance (EBS-backed only)
    hibernate,  // AWS hibernates the instance (requires hibernation prereqs)
};

struct ec2_spot_options {
    // Empty string means no price cap (AWS caps at the on-demand price).
    std::string max_price;
    ec2_spot_interruption_behavior interruption_behavior{
        ec2_spot_interruption_behavior::terminate};
};
```

`spot_options` is always applied as a one-time spot request (`SpotInstanceType =
one-time`). Persistent requests are not used because the quorum manager owns the
instance lifecycle: `decommission_node` terminates the instance directly and the
orchestration loop handles re-provisioning. Using persistent requests would create a
second instance automatically after interruption, racing with the manager's own
`provision_node` call.

When `spot_options` is absent, `provision_node` requests on-demand capacity (the
default `RunInstances` behaviour). The `kythira:market` tag records which market
type was used so operators and `assess_quorum` can distinguish the two in the
console.

For workloads that need multi-instance-type spot selection or mixed on-demand/spot
capacity, use `aws_asg_quorum_manager` with a mixed-instances-policy ASG instead:
the ASG launch template handles instance-type diversification and spot capacity
pools natively, without any changes to the quorum manager.

#### Placement group types

```cpp
enum class ec2_placement_group_strategy : std::uint8_t {
    none,       // No placement group (default)
    cluster,    // Low-latency, tightly packed in one AZ
    spread,     // Distinct underlying hardware per instance (max 7 per AZ)
    partition,  // Separate hardware partitions within one or more AZs
};

struct ec2_placement_group_config {
    std::string name;                                          // EC2 Placement Group resource name
    ec2_placement_group_strategy strategy{                     // Informational; EC2 enforces via the group resource
        ec2_placement_group_strategy::none};
    std::uint32_t partition_number{0};                         // partition strategy: 0 = EC2 auto-assigns
};
```

#### Configuration struct

```cpp
struct aws_ec2_quorum_manager_config {
    aws_client_config aws{};
    std::string cluster_name;                                          // required
    std::string image_id;                                              // required
    std::string instance_type{"t3.medium"};
    std::string key_name;
    std::string iam_instance_profile;
    std::vector<std::string> security_group_ids;
    std::map<std::string, std::string> subnet_by_group;                // AZ → subnet ID
    std::map<std::string, ec2_placement_group_config> placement_by_group{};  // AZ → PG config
    std::optional<ec2_spot_options> spot_options{};                          // nullopt = on-demand
    std::chrono::seconds heartbeat_timeout{30};    // 0 = EC2 state only
    std::chrono::seconds heartbeat_grace_period{120};
    std::uint16_t node_port{7000};
    std::string user_data_template;
    desired_topology<std::string> topology{};
    std::chrono::seconds provision_timeout{300};
    std::chrono::milliseconds poll_interval{5000};
    std::map<std::string, std::string> extra_tags{};
};
```

#### Class sketch

```cpp
#ifdef KYTHIRA_HAS_AWS_SDK

template<typename NodeId = std::uint64_t, typename Address = std::string>
requires kythira::node_id<NodeId>
class aws_ec2_quorum_manager {
public:
    using node_id_type            = NodeId;
    using address_type            = Address;
    using placement_group_id_type = std::string;

    explicit aws_ec2_quorum_manager(aws_ec2_quorum_manager_config cfg);

    aws_ec2_quorum_manager(const aws_ec2_quorum_manager&)            = delete;
    aws_ec2_quorum_manager& operator=(const aws_ec2_quorum_manager&) = delete;
    aws_ec2_quorum_manager(aws_ec2_quorum_manager&&)                 = default;
    aws_ec2_quorum_manager& operator=(aws_ec2_quorum_manager&&)      = default;

    auto assess_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::Future<quorum_health<NodeId, std::string>>;

    auto maintain_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::Future<quorum_health<NodeId, std::string>>;

    auto provision_node(std::string target_group, std::optional<NodeId> replacing)
        -> kythira::Future<peer_info<NodeId, Address>>;

    auto decommission_node(const NodeId& node)
        -> kythira::Future<void>;

    [[nodiscard]] auto topology() const
        -> kythira::desired_topology<std::string>;

private:
    aws_ec2_quorum_manager_config _cfg;
    Aws::EC2::EC2Client       _ec2;

    // Tag key constants
    static constexpr const char* tag_cluster    = "kythira:cluster";
    static constexpr const char* tag_node_id    = "kythira:node-id";
    static constexpr const char* tag_group      = "kythira:group";
    static constexpr const char* tag_managed_by = "kythira:managed-by";

    auto make_client_config() const -> Aws::Client::ClientConfiguration;
    auto node_id_str(const NodeId&) const -> std::string;
    auto next_node_id() const -> NodeId;
    auto find_ec2_id(const NodeId&) const -> std::optional<std::string>;
    auto apply_tags(const std::string& ec2_instance_id,
                    const NodeId& node_id,
                    const std::string& group,
                    ec2_placement_group_strategy strategy,
                    std::string_view market) const -> void;
    auto render_user_data(const NodeId&, const std::string& az) const -> std::string;
    static auto compute_quorum_status(std::size_t live, std::size_t total)
        -> quorum_status;
};

static_assert(quorum_manager<aws_ec2_quorum_manager<std::uint64_t, std::string>,
                             std::uint64_t, std::string, std::string>);

#endif  // KYTHIRA_HAS_AWS_SDK
```

#### `assess_quorum` sequence

1. Check fault point `"raft/aws/ec2/describe_instances"`.
2. Build `DescribeInstancesRequest` with one filter only:
   - `tag:kythira:cluster = {cluster_name}`
   (No state filter — all states returned so grace-period checks apply to
   new instances that are still `pending`.)
3. Call `_ec2.DescribeInstances(req)`. On SDK error, return exceptional Future.
4. Walk the response's reservation/instance list. For each instance read:
   - `kythira:node-id` tag → key in `instance_map`
   - `instance-state-name`
   - `kythira:group` tag
   - `kythira:last-heartbeat` tag (may be missing → empty string)
   - `LaunchTime` (as Unix timestamp)
5. Classify each instance as live or unreachable:
   ```
   now = current Unix time (seconds)
   for each instance:
     if state != "running":           → unreachable
     elif heartbeat_timeout == 0:     → live (legacy mode)
     elif last_heartbeat tag present:
       if now - last_heartbeat <= heartbeat_timeout: → live
       else:                          → unreachable (stale heartbeat)
     else:  // no heartbeat tag yet
       if now - LaunchTime <= heartbeat_grace_period: → live (starting up)
       else:                          → unreachable (grace period expired)
   ```
   Build:
   ```cpp
   std::unordered_map<std::string, bool> live_map;  // node_id_str → live
   std::unordered_map<std::string, std::size_t> group_live;
   ```
6. Iterate the supplied `cluster` vector. For each `node_placement`:
   - Live if `live_map[node_id_str(np.node_id)]` is true.
   - Otherwise push to `unreachable_nodes`.
7. Build `placement_group_health` entries: for each distinct `group_id` in
   the cluster vector, `live_count` from `group_live`, `target_count` from
   `_cfg.topology`.
8. Compute `quorum_status` via `compute_quorum_status(live, total)`.
9. Return `quorum_health` wrapped in an immediately-resolved Future.

#### `maintain_quorum` sequence

```
1. Check fault point "raft/aws/ec2/maintain_quorum".
2. health = await assess_quorum(cluster)  → exceptional propagates immediately
3. For each node_id in health.unreachable_nodes:
     group = group of that node (look up in cluster vector)
     await decommission_node(node_id)     → log failures, continue
     record (group → replaced_node_id) for provisioning hint
4. Compute per-group deficit:
     for each group g in _cfg.topology.groups:
       deficit[g] = g.target_count − health.groups[g].live_count
5. For each group g with deficit[g] > 0:
     for i in 0 .. deficit[g]-1:
       replacing = replaced_node_id[g] if available else nullopt
       await provision_node(g, replacing)  → log failures, continue
6. Return health  // pre-remediation snapshot
```

The caller receives the pre-remediation `quorum_health`. On the next
`maintain_quorum` call after new nodes have started and written their first
heartbeats, the health will reflect the repaired state. This separation keeps
the remediation action and the observation distinct and auditable.

#### `provision_node` sequence

```
1. Check fault point "raft/aws/ec2/run_instances".
2. Validate target_group is in subnet_by_group → exceptional Future if absent.
3. NodeId new_id = next_node_id()          // DescribeInstances + max tag + 1
4. Substitute user_data_template: {NODE_ID}, {NODE_PORT}, {CLUSTER}, {AZ}
5. Base64-encode user data.
6. Lookup placement: pg = placement_by_group[target_group] (or default-constructed)
7. If cfg.spot_options.has_value():
     Build InstanceMarketOptionsRequest:
       MarketType               = "spot"
       SpotOptions.SpotInstanceType = "one-time"
       SpotOptions.MaxPrice     = spot_options.max_price (when non-empty)
       SpotOptions.InstanceInterruptionBehavior = terminate|stop|hibernate
8. RunInstances(ImageId, InstanceType, KeyName?, IamInstanceProfile?,
               SecurityGroupIds, SubnetId, UserData, MinCount=1, MaxCount=1,
               Placement.GroupName = pg.name (when non-empty),
               Placement.PartitionNumber = pg.partition_number
                 (when strategy=partition and partition_number > 0),
               InstanceMarketOptions (step 7, when spot_options set))
   → on failure: return exceptional Future (no cleanup needed).
9. ec2_instance_id = response.GetInstances()[0].GetInstanceId()
10. market_tag = spot_options.has_value() ? "spot" : "on-demand"
    apply_tags(ec2_instance_id, new_id, target_group, pg.strategy, market_tag)
11. Poll DescribeInstances(ec2_instance_id) every poll_interval:
   - state == "running" → read PrivateIpAddress, break
   - state in {"shutting-down", "terminated"} → break with error
     (covers spot interruption during the pending phase)
   - elapsed > provision_timeout → TerminateInstances (best-effort), return exceptional Future
12. Return peer_info{new_id, "{private_ip}:{node_port}"}
```

The address uses the private IP because:
- Other cluster nodes are typically in the same VPC/VPN.
- DNS propagation of the private hostname may lag; the IP is available
  immediately once `running` state is reached.

#### `decommission_node` sequence

```
1. Check fault point "raft/aws/ec2/terminate_instances".
2. optional<string> ec2_id = find_ec2_id(node_id)
   → DescribeInstances(tag:kythira:cluster, tag:kythira:node-id)
3. if not found → return resolved Future   // idempotent: already gone
4. state = instance.GetState().GetName()
5. if state in {"terminated", "shutting-down"} → return resolved Future
6. TerminateInstances({ec2_id})
7. on error → return exceptional Future
8. return resolved Future
```

#### `next_node_id` helper

```cpp
auto aws_ec2_quorum_manager::next_node_id() const -> NodeId {
    // DescribeInstances with tag:kythira:cluster filter (all states)
    // Scan kythira:node-id tags, parse to numeric type, take max + 1
    // If empty: return NodeId{1}
}
```

For `std::string` NodeId the tag is treated as a decimal integer string for
ordering purposes (max-of-parsed + 1, serialized back to string).

#### User data placeholder substitution

`render_user_data` performs a simple linear scan and replacement:
- `{NODE_ID}` → `node_id_str(new_id)`
- `{NODE_PORT}` → `std::to_string(node_port)`
- `{CLUSTER}` → `cluster_name`
- `{AZ}` → `target_group`

No external templating library is needed. A typical user data template (integration tests use the full bootstrap form
from Req 16 AC 11):

```sh
#!/bin/bash
REGION=$(curl -s http://169.254.169.254/latest/meta-data/placement/region)
INSTANCE_ID=$(curl -s http://169.254.169.254/latest/meta-data/instance-id)

# kythira-node is responsible for updating kythira:last-heartbeat via EC2 tags.
# No external heartbeat loop — the quorum manager relies solely on the heartbeat
# mechanism built into the kythira node binary.
exec /usr/local/bin/kythira-node \
  --node-id={NODE_ID} --port={NODE_PORT} --cluster={CLUSTER} \
  --ec2-heartbeat-tag=kythira:last-heartbeat \
  --ec2-instance-id="$INSTANCE_ID" \
  --ec2-region="$REGION"
```

---

### 3. `include/raft/aws_asg_quorum_manager.hpp`

#### Configuration struct

```cpp
struct aws_asg_quorum_manager_config {
    aws_client_config aws{};
    std::string cluster_name;                         // required
    std::map<std::string, std::string> asg_by_group;  // required, ≥ 1 entry
    std::uint16_t node_port{7000};
    std::chrono::seconds provision_timeout{300};
    std::chrono::milliseconds poll_interval{5000};
    desired_topology<std::string> topology{};
};
```

Spot and on-demand mix is configured entirely in the ASG's launch template or
mixed-instances policy — not in this config struct. The quorum manager does not
need to know which market type the ASG uses: `provision_node` increments desired
capacity and waits for an instance to reach `InService`, regardless of whether
that instance is spot or on-demand. `assess_quorum` detects interruption when the
ASG transitions the instance out of `InService`, which it does within seconds of
the 2-minute spot-interruption notice.

#### Class sketch

```cpp
#ifdef KYTHIRA_HAS_AWS_SDK

template<typename NodeId = std::uint64_t, typename Address = std::string>
requires kythira::node_id<NodeId>
class aws_asg_quorum_manager {
public:
    using node_id_type            = NodeId;
    using address_type            = Address;
    using placement_group_id_type = std::string;

    explicit aws_asg_quorum_manager(aws_asg_quorum_manager_config cfg);

    aws_asg_quorum_manager(const aws_asg_quorum_manager&)            = delete;
    aws_asg_quorum_manager& operator=(const aws_asg_quorum_manager&) = delete;
    aws_asg_quorum_manager(aws_asg_quorum_manager&&)                 = default;
    aws_asg_quorum_manager& operator=(aws_asg_quorum_manager&&)      = default;

    auto assess_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::Future<quorum_health<NodeId, std::string>>;

    auto maintain_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::Future<quorum_health<NodeId, std::string>>;

    auto provision_node(std::string target_group, std::optional<NodeId> replacing)
        -> kythira::Future<peer_info<NodeId, Address>>;

    auto decommission_node(const NodeId& node)
        -> kythira::Future<void>;

    [[nodiscard]] auto topology() const
        -> kythira::desired_topology<std::string>;

private:
    aws_asg_quorum_manager_config              _cfg;
    Aws::AutoScaling::AutoScalingClient    _asg;
    Aws::EC2::EC2Client                    _ec2;

    static constexpr const char* tag_cluster    = "kythira:cluster";
    static constexpr const char* tag_node_id    = "kythira:node-id";
    static constexpr const char* tag_group      = "kythira:group";
    static constexpr const char* tag_managed_by = "kythira:managed-by";

    auto node_id_str(const NodeId&) const -> std::string;
    auto next_node_id() const -> NodeId;
    auto find_ec2_id(const NodeId&) const -> std::optional<std::string>;
    auto apply_tags(const std::string& ec2_instance_id,
                    const NodeId& node_id,
                    const std::string& group,
                    ec2_placement_group_strategy strategy,
                    std::string_view market) const -> void;
    static auto compute_quorum_status(std::size_t live, std::size_t total)
        -> quorum_status;
};

static_assert(quorum_manager<aws_asg_quorum_manager<std::uint64_t, std::string>,
                             std::uint64_t, std::string, std::string>);

#endif  // KYTHIRA_HAS_AWS_SDK
```

#### `assess_quorum` sequence

1. Check fault point `"raft/aws/asg/describe_asgs"`.
2. Collect all ASG names from `_cfg.asg_by_group`.
3. `DescribeAutoScalingGroups({all ASG names})` — returns all instances with
   their lifecycle states.
4. Collect EC2 instance IDs of all `InService` instances across all ASGs.
5. Issue one `DescribeInstances` with all collected EC2 IDs. For each returned
   instance, read: `kythira:node-id`, `kythira:cluster`, `kythira:group`,
   `kythira:last-heartbeat`, `LaunchTime`.
6. Apply the same heartbeat liveness rule as `aws_ec2_quorum_manager`
   (Req 6.3b–c): an `InService` instance is **live** only if its heartbeat
   is current (or it is within the grace period).
7. For each `node_placement` in the supplied cluster vector, classify as live
   or unreachable based on the live map.
8. Build per-group health from `kythira:group` tag of live instances.
9. Return `quorum_health` in an immediately-resolved Future.

#### `maintain_quorum` sequence

Identical to `aws_ec2_quorum_manager::maintain_quorum` (same six steps) with
fault point `"raft/aws/asg/maintain_quorum"` and using `decommission_node` /
`provision_node` from `aws_asg_quorum_manager`.

#### `provision_node` sequence

```
1. Check fault point "raft/aws/asg/update_asg".
2. Validate target_group is in asg_by_group → exceptional Future if absent.
3. asg_name = asg_by_group[target_group]
4. DescribeAutoScalingGroups({asg_name}) → read DesiredCapacity as orig_capacity
5. UpdateAutoScalingGroup(asg_name, DesiredCapacity = orig_capacity + 1)
   → on failure: return exceptional Future
6. Record start_time = now()
7. Poll DescribeAutoScalingGroups(asg_name) every poll_interval:
   For each InService instance whose EC2 tags do NOT yet contain kythira:node-id:
     → This is the newly launched instance; break
   If elapsed > provision_timeout:
     → UpdateAutoScalingGroup(asg_name, DesiredCapacity = orig_capacity)  // best-effort rollback
     → return exceptional Future
8. new_ec2_id = the newly found instance's EC2 instance ID
9. new_node_id = next_node_id()  // same max-tag strategy as aws_ec2_quorum_manager
10. apply_tags(new_ec2_id, new_node_id, target_group)
11. DescribeInstances(new_ec2_id) → read PrivateIpAddress
12. Return peer_info{new_node_id, "{private_ip}:{node_port}"}
```

The "no kythira:node-id tag" heuristic in step 7 identifies new instances: the
quorum manager tags instances immediately after detecting them. An untagged
`InService` instance is therefore one that was just launched by this provision
call.

#### `decommission_node` sequence

```
1. Check fault point "raft/aws/asg/terminate_instance".
2. optional<string> ec2_id = find_ec2_id(node_id)
3. if not found → return resolved Future   // idempotent
4. Check instance state via DescribeInstances
5. if state in {"terminated", "shutting-down"} → return resolved Future
6. TerminateInstanceInAutoScalingGroup(ec2_id, ShouldDecrementDesiredCapacity=true)
7. on error → return exceptional Future
8. return resolved Future
```

Using `ShouldDecrementDesiredCapacity=true` ensures the ASG does not
automatically replace the terminated instance. The orchestrator decides when to
provision again, not the ASG health check policy.

---

## Shared Private Helpers

`node_id_str`, `next_node_id`, `find_ec2_id`, `apply_tags`, and
`compute_quorum_status` have identical signatures and semantics in both classes.
They are NOT refactored into a shared base class or free-function header to
avoid coupling the two implementations; the duplication is intentional and
small. If a third AWS manager is added in the future, a
`detail/aws_quorum_helpers.hpp` private header can absorb them.

---

## Client Initialization

Both classes construct their SDK clients in the constructor body after
`Aws::InitAPI()` has been called by the application. The library does NOT call
`Aws::InitAPI()` or `Aws::ShutdownAPI()` itself — that is the caller's
responsibility. Typically:

```cpp
int main() {
    Aws::SDKOptions opts;
    Aws::InitAPI(opts);
    // ... construct managers and run cluster ...
    Aws::ShutdownAPI(opts);
}
```

The `make_client_config()` helper sets:
- `region` from `config.aws.region` (empty → SDK picks up `AWS_DEFAULT_REGION`)
- `endpointOverride` from `config.aws.endpoint_override` (empty → omitted)
- `connectTimeoutMs` and `requestTimeoutMs` from `config.aws.api_timeout`

Explicit credentials (when `access_key_id` non-empty) are passed via
`Aws::Auth::AWSCredentials` to a `Aws::Auth::SimpleAWSCredentialsProvider`
wrapped in the client config. When empty, the default provider chain is used.

---

## Fault Injection

```cpp
// In assess_quorum:
fiu_do_on("raft/aws/ec2/describe_instances", throw std::runtime_error("injected"););

// In provision_node:
fiu_do_on("raft/aws/ec2/run_instances", throw std::runtime_error("injected"););

// In decommission_node:
fiu_do_on("raft/aws/ec2/terminate_instances", throw std::runtime_error("injected"););
```

All six fault points (three for each class) are guarded by `#ifdef FIU_ENABLE`
via the `fiu_do_on` macro from `include/raft/fault_injection.hpp`.

---

## Correctness Properties

### Property 1: Tag-based statelesness
**Validates: Requirements 5.3, 8.2**

Both managers reconstruct the node-ID-to-EC2-instance mapping from EC2 tags on
every API call. No in-memory state is required for `assess_quorum` or
`decommission_node` to function correctly after a process restart or leader
failover. The seven mandatory tags (`kythira:cluster`, `kythira:node-id`,
`kythira:group`, `kythira:managed-by`, `kythira:placement-strategy`,
`kythira:market`, `Name`) are written atomically via
`CreateTags` before the manager polls for `running` state, so a crash between
`RunInstances` and `CreateTags` leaves an untagged orphan that is invisible to
tag-filtered `DescribeInstances` calls and will eventually be discovered and
terminated manually or by billing alerts.

### Property 2: Idempotency of decommission
**Validates: Requirements 8.2, 8.3, 14.2**

`decommission_node` returns a resolved Future (no error) when:
- No instance with the matching `kythira:node-id` tag is found.
- An instance is found but its state is `terminated` or `shutting-down`.

`TerminateInstances` itself is idempotent in the EC2 API (calling it on an
already-terminated instance succeeds). The manager's pre-check covers the
no-instance-found case that the EC2 API would otherwise reject.

### Property 3: Monotonically increasing NodeId assignment
**Validates: Requirements 7.1, 13.4**

`next_node_id()` reads all existing `kythira:node-id` tags under the cluster
and returns `max + 1`. Because it scans all states (including `shutting-down`
and `terminated`), a recently decommissioned node's ID is never reused. This
prevents a race where a newly joined node receives the same ID as one that is
mid-decommission and still visible to some Raft peers.

### Property 4: No autonomous replacement by the ASG
**Validates: Requirements 14.3**

`aws_asg_quorum_manager::decommission_node` passes
`ShouldDecrementDesiredCapacity=true` to `TerminateInstanceInAutoScalingGroup`.
This simultaneously terminates the instance and decrements the ASG desired
capacity, preventing the ASG from automatically launching a replacement. Only
the quorum manager (driven by the Raft leader's assessment loop) decides when
to provision a replacement and in which group to place it.

### Property 5: Application-level health detection via heartbeat timeout
**Validates: Requirements 6.3, 12.3, 19.2**

A kythira node running on an EC2 instance in `running` state that has crashed
(OOM, segfault, SIGKILL) stops updating `kythira:last-heartbeat`. After
`heartbeat_timeout` seconds without an update, `assess_quorum` classifies the
node as unreachable regardless of the EC2 instance state. This gives the quorum
manager accurate application-level health visibility without requiring a direct
network path from the quorum manager to the instance (which may not exist for
private-subnet deployments). The subsequent `maintain_quorum` call then
terminates the stale instance and provisions a replacement.

### Property 6: Spot interruption is detected without special handling
**Validates: Requirements 18.3**

When EC2 interrupts a spot instance it transitions the instance's state to
`shutting-down` within the 2-minute notice window. This state change causes
`assess_quorum` to classify the node as unreachable (EC2 state ≠ `running`
takes precedence over the heartbeat check). The subsequent `maintain_quorum`
call decommissions the node (idempotent since it is already terminating) and
provisions a replacement. No spot-specific code path is required in
`assess_quorum` or `maintain_quorum`.

### Property 7: Topology-invariant replacement
**Validates: Requirements 19.3**

`maintain_quorum` computes per-group deficits from `config.topology` and
provisions replacements into each deficit group independently. A cluster
configured as 2-in-AZ1 + 1-in-AZ2 that loses both AZ1 nodes will receive
2 new nodes in AZ1. The AZ2 group is unaffected. This ensures the operator's
intended fault-domain layout is preserved through any remediation cycle.

---

## Error Handling

- **AWS SDK error outcomes**: The C++ SDK represents errors as `Aws::Client::AWSError`
  returned in the outcome type. When `!outcome.IsSuccess()`, the implementations
  extract `outcome.GetError().GetMessage()` and wrap it in `std::runtime_error`
  before rejecting the Future.
- **Transient throttling**: `DescribeInstances` and `DescribeAutoScalingGroups`
  may return `ThrottlingException`. The SDK's built-in retry policy handles
  standard retries; the quorum manager does not add additional retry logic.
  If throttling persists past the SDK retry budget, the Future is rejected and
  the orchestrator's next assessment cycle will retry.
- **Polling timeout**: When `provision_timeout` elapses during provision polling,
  a best-effort cleanup call is made before the Future is rejected, to avoid
  leaving orphaned instances running indefinitely.
- **Constructor validation**: Required-but-missing config fields throw
  `std::invalid_argument` synchronously at construction, before any AWS API
  calls are attempted.

---

## Testing Strategy

### Unit tests (no AWS)

Unit tests mock-free: they exercise constructor validation, config error paths,
and fault injection points. Since the AWS SDK client cannot be easily mocked
without a dedicated dependency injection seam, the fault injection tests use
`fiu_enable` to inject errors at the points identified in Requirement 15,
verifying the Future is rejected with the expected message without any actual
EC2 client instantiation.

Constructor validation tests do not need a live client at all (the client
object is constructed in the ctor body but never called in error-path tests).

### Integration test fixture: `IntegrationFixture`

Both the LocalStack and real-AWS integration test suites share a common fixture
pattern called `IntegrationFixture` that owns the full lifecycle of every AWS
(or LocalStack) resource used during a test run.

#### Resource creation (setup)

The fixture constructor wraps the entire setup sequence in a try/catch. If any
step throws, the destructor runs immediately (RAII), executes the best-effort
teardown sequence against all resources that were already recorded, and the
fixture stores the failure reason. Each test case begins with:

```cpp
if (!fixture.setup_ok()) BOOST_TEST_SKIP(fixture.skip_reason());
```

so the test is reported as skipped, not failed. This applies to every error
after the credential check: a `CreateVpc` that hits a service quota, a
`PutObject` that hits an S3 permission error, a timeout waiting for the bastion
to reach `running` state, etc.

The fixture generates a UUID at construction and uses it for all created
resource names and the `kythira:test-run` tag. For each required resource, the
fixture checks the corresponding env-var override first; if absent it creates
the resource and **records it immediately** so that the destructor can clean it
up even if a later step fails:

```
// Step 0: credential check (skip entire suite on failure)
outcome = StsClient(endpoint_override, credentials).GetCallerIdentity()
if (!outcome.IsSuccess()) BOOST_TEST_SKIP("AWS credentials unavailable: " +
                                          outcome.GetError().GetMessage())

uuid              = random_uuid_v4()
test_run          = "kythira-test-" + uuid
cluster           = "kythira-realtest-" + uuid
s3_prefix         = "kythira-test/" + uuid

// Network
VPC:              use AWS_TEST_VPC_ID if set, else
                      CreateVpc(CidrBlock="10.77.0.0/16") +
                      ModifyVpcAttribute(EnableDnsHostnames=true)
IGW:              if fixture created VPC, CreateInternetGateway +
                      AttachInternetGateway(vpc) +
                      add route 0.0.0.0/0 → igw to the public route table
Bastion subnet:   use AWS_TEST_BASTION_SUBNET_ID if set, else
                      CreateSubnet(vpc, "10.77.3.0/28", az1,
                                   MapPublicIpOnLaunch=true)
NAT Gateway:      AllocateAddress() → eip_alloc_id
                  CreateNatGateway(SubnetId=bastion_subnet, AllocationId=eip_alloc_id)
                  poll until NatGateway.State = "available" (max 90 s)
                  for each private route table: CreateRoute(0.0.0.0/0 → nat_gateway_id)
                  (required: ec2:CreateTags, ec2:DescribeInstances etc. from
                  private subnets go through NAT; S3 traffic still uses the
                  gateway endpoint)
Cluster subnet1:  use AWS_TEST_SUBNET_ID_AZ1 if set, else
                      CreateSubnet(vpc, "10.77.0.0/24", az1)
Cluster subnet2:  use AWS_TEST_SUBNET_ID_AZ2 if set, else
                      CreateSubnet(vpc, "10.77.1.0/24", az2)
Cluster subnet3:  use AWS_TEST_SUBNET_ID_AZ3 if set, else
                      CreateSubnet(vpc, "10.77.2.0/24", az3)
S3 endpoint:      check DescribeVpcEndpoints for existing S3 gateway on the VPC;
                      if absent, CreateVpcEndpoint(
                          ServiceName=com.amazonaws.{region}.s3,
                          VpcId, RouteTableIds=[all three private route tables])

// Security
Bastion SG:       CreateSecurityGroup(vpc, "kythira-test-bastion-{uuid}");
                      ingress SSH(22) from AWS_TEST_ALLOWED_CIDR
Node SG:          use AWS_TEST_SG_ID if set, else
                      CreateSecurityGroup(vpc, "kythira-test-node-{uuid}");
                      ingress port 7000 from 10.77.0.0/16;
                      ingress SSH(22) from bastion_sg (source SG reference)
Quarantine SG:    CreateSecurityGroup(vpc, "kythira-test-quarantine-{uuid}");
                      no inbound rules; no outbound rules
                      (used to completely isolate an instance during tests)
Key pair:         use AWS_TEST_KEY_NAME if set, else
                      CreateKeyPair(KeyName="kythira-test-{uuid}");
                      private key material held in memory only

// Compute
Bastion:          RunInstances(ImageId=AWS_TEST_AMI_ID, InstanceType=t3.nano,
                               SubnetId=bastion_subnet, SG=bastion_sg,
                               KeyName=key_name,
                               InstanceMarketOptions={MarketType=spot,
                                 SpotOptions={SpotInstanceType=one-time}},
                               TagSpecifications=[kythira:test-run={uuid},
                                                  kythira:role=bastion]);
                      wait for bastion EC2 state = running

// Identity
IAM role:         use AWS_TEST_IAM_PROFILE if set, else
                      CreateRole(EC2 trust) + inline policy:
                          ec2:DescribeInstances
                          ec2:CreateTags
                          s3:GetObject on arn:aws:s3:::{S3_BUCKET}/kythira-test/*
                      CreateInstanceProfile + AddRoleToInstanceProfile

// Binary
S3 binary:        PutObject(Bucket=AWS_TEST_S3_BUCKET,
                             Key=kythira-test/{uuid}/kythira-node,
                             Body=read(KYTHIRA_NODE_BINARY))

// Placement groups (on demand per test case, not unconditional)
PGs:              created when needed by a placement-group test case
```

All resources created by the fixture are tagged with `kythira:test-run = {uuid}`.
Cluster nodes use the NAT Gateway for EC2/STS API egress (heartbeat updates,
peer discovery); the S3 VPC gateway endpoint handles S3 traffic without going
through the NAT.

#### Resource teardown (destructor)

Teardown runs unconditionally in the destructor, in reverse dependency order.
Cost-bearing resources (EC2 instances) are always destroyed first. All steps are
attempted even when earlier steps fail; errors are collected and written to
`std::cerr` after all steps complete. Teardown failures do not cause the test
to report failure.

```
Step 1:  TerminateInstances(all cluster node EC2 IDs + bastion EC2 ID)
         Poll DescribeInstances until all reach "terminated" (max 120 s)

Step 2:  DeleteObject(Bucket=AWS_TEST_S3_BUCKET,
                      Key=kythira-test/{uuid}/kythira-node)

Step 3:  DeletePlacementGroup for each PG created during this run

Step 4:  DeleteSecurityGroup(quarantine_sg)  (always created by fixture)
Step 5:  DeleteSecurityGroup(bastion_sg)     (always created by fixture)
Step 6:  DeleteSecurityGroup(node_sg)        (only if fixture created it)

Step 7:  DeleteSubnet(bastion_subnet)        (only if fixture created it)
         DeleteSubnet(cluster_subnet1)       (only if fixture created it)
         DeleteSubnet(cluster_subnet2)       (only if fixture created it)
         DeleteSubnet(cluster_subnet3)       (only if fixture created it)

Step 8:  DeleteVpcEndpoints({s3_endpoint_id})  (only if fixture created it)

Step 9:  RemoveRoleFromInstanceProfile
         DeleteInstanceProfile
         DeleteRole            (all three, only if fixture created them)

Step 10: DeleteNatGateway(nat_gateway_id)    (always created by fixture)
         Poll DescribeNatGateways until state = "deleted" (max 60 s)
         ReleaseAddress(eip_alloc_id)

Step 11: DetachInternetGateway(igw, vpc)    (only if fixture created it)
         DeleteInternetGateway(igw)

Step 12: DeleteKeyPair(key_name)            (only if fixture created it)

Step 13: DeleteVpc                          (only if fixture created it)
```

#### Node bootstrap and cluster formation (real-AWS only)

The fixture sets `user_data_template` to a script that:
1. Downloads kythira-node from `s3://{S3_BUCKET}/{S3_PREFIX}/kythira-node`
   via the S3 VPC gateway endpoint.
2. Starts kythira-node with `--node-id={NODE_ID} --port={NODE_PORT}
   --cluster={CLUSTER} --ec2-heartbeat-tag=kythira:last-heartbeat
   --ec2-instance-id={INSTANCE_ID} --ec2-region={REGION}`.
   kythira-node updates `kythira:last-heartbeat` via the EC2 API at its
   configured heartbeat interval. No external shell loop is used.
3. Polls `localhost:{NODE_PORT}` until kythira accepts connections.
4. Queries `DescribeInstances` for instances with
   `kythira:cluster = {CLUSTER}` and `instance-state-name = running`;
   skips self; calls `kythira-join --host {peer_ip} --port {NODE_PORT}`
   for each peer found.
5. Tags the instance `kythira:status = ready` via `ec2 create-tags`
   (this is the bootstrap script's responsibility, not kythira's).

`{S3_BUCKET}` and `{S3_PREFIX}` are substituted by the fixture at construction
time (constants for the run). `{NODE_ID}`, `{NODE_PORT}`, `{CLUSTER}` are
substituted per-node by `provision_node` using the existing template mechanism.

The fixture provisions nodes **sequentially**: after each `provision_node`
call it polls `DescribeInstances` until the new instance has
`kythira:status = ready` (timeout 120 s) before calling `provision_node` for
the next node. This guarantees each joining node finds its predecessor already
running, making peer discovery deterministic.

#### LocalStack fixture behaviour

LocalStack does not execute user_data scripts and does not simulate inter-instance
networking or S3 VPC endpoints. The LocalStack fixture therefore uses
`user_data_template = "#!/bin/bash\n"` (no-op) and skips the S3 binary upload,
the VPC endpoint creation, and the readiness-tag polling. LocalStack tests verify
the API call sequences, tag filters, and idempotency properties only; they do
not test kythira node startup or cluster formation.

### Integration tests (LocalStack)

LocalStack tests set `endpoint_override = "http://localhost:4566"` and use dummy
credentials. LocalStack state transitions are instantaneous (no simulated boot
delay), so the provision polling loop completes on the first poll. The teardown
sequence is followed in the correct dependency order even though no real resources
were created with real costs.

### Integration tests (real AWS EC2)

The real-AWS fixture provisions instances that boot, download kythira from S3,
form a real cluster via `ClusterJoin`, and tag themselves `ready`. The fixture
waits for readiness before making assertions. Only `AWS_REGION`,
`AWS_TEST_AMI_ID`, `AWS_TEST_S3_BUCKET`, and `KYTHIRA_NODE_BINARY` are required;
all other resources are created and torn down automatically.

---

## Dependencies

```
aws-sdk-cpp ≥ 1.11   Components: ec2 autoscaling iam s3 sts
                      Link: AWS::aws-cpp-sdk-ec2
                            AWS::aws-cpp-sdk-autoscaling
                            AWS::aws-cpp-sdk-iam
                            AWS::aws-cpp-sdk-s3
                            AWS::aws-cpp-sdk-sts
                      find_package: find_package(AWSSDK COMPONENTS ec2 autoscaling iam s3 sts)
```

The SDK components required are:
- `aws-cpp-sdk-core` (transitive dependency of all)
- `aws-cpp-sdk-ec2` — quorum managers + integration test fixture
- `aws-cpp-sdk-autoscaling` — `aws_asg_quorum_manager`
- `aws-cpp-sdk-iam` — integration test fixture only (IAM role + instance profile
  creation/deletion); not used by either production manager class
- `aws-cpp-sdk-s3` — integration test fixture only (kythira-node binary upload
  and teardown deletion); not used by either production manager class
- `aws-cpp-sdk-sts` — integration test fixture only (credential verification
  via `GetCallerIdentity`); not used by either production manager class

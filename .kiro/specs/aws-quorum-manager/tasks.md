# Implementation Plan — AWS Quorum Managers

## Status: Complete — all 7 tasks

**Last Updated**: July 18, 2026 (tracking doc corrected; implementation itself
landed earlier — see `doc/TODO.md`'s Cloud Provider Support "AWS" entry and
`doc/CHANGELOG.md`'s July 7–8, 2026 entry, commit
`feat(raft): add AWS EC2 and ASG quorum manager implementations`. This
tracking document was simply never updated to reflect that, the same issue
`doc/CHANGELOG.md`'s July 9–10, 2026 entry found and fixed for
`membership-change`.)

Verified directly against the real implementation: `include/raft/aws_ec2_quorum_manager.hpp`,
`include/raft/aws_asg_quorum_manager.hpp`, and `include/raft/aws_client_config.hpp` all
exist with the config structs and manager classes this plan calls for, and
`tests/aws_quorum_manager_unit_test.cpp`/`aws_quorum_manager_localstack_test.cpp`/
`aws_quorum_manager_real_ec2_test.cpp` implement the three test tiers below,
including task 7's AZ-outage/heartbeat-replacement/quarantine scenarios.
(This plan's task text below previously used the bare `ec2_quorum_manager`/
`asg_quorum_manager` names instead of the `aws_`-prefixed names the
implementation actually uses — matching `requirements.md`/`design.md`, which
already used the `aws_`-prefixed names throughout. Updated in place so the
plan matches reality and the rest of this spec directory consistently.)

## Overview

Implement two AWS-based `quorum_manager` classes:
- `aws_ec2_quorum_manager` — direct EC2 instance management (primary)
- `aws_asg_quorum_manager` — Auto Scaling Group management (production-grade)

Both satisfy the `quorum_manager` concept from `include/raft/quorum_management.hpp`.
The implementations are header-only, gated behind `KYTHIRA_HAS_AWS_SDK`, and use
the `aws-sdk-cpp` C++ SDK.

Reference implementations to study before starting:
- `include/raft/docker_quorum_manager.hpp` — closest structural analogue
- `include/raft/quorum_management.hpp` — concept definition and shared types
- `include/raft/fault_injection.hpp` — `fiu_do_on` macro usage

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [1, 2],
      "description": "Build system and shared config — prerequisites for both managers"
    },
    {
      "wave": 2,
      "tasks": [3],
      "description": "aws_ec2_quorum_manager — no dependency on aws_asg_quorum_manager"
    },
    {
      "wave": 3,
      "tasks": [4],
      "description": "aws_asg_quorum_manager — shares helpers with ec2 but is independent"
    },
    {
      "wave": 4,
      "tasks": [5, 6],
      "description": "Unit tests and LocalStack integration tests"
    },
    {
      "wave": 5,
      "tasks": [7],
      "description": "Real-AWS EC2 integration tests — independent of LocalStack"
    }
  ]
}
```

## Tasks

- [x] 1. Add `aws-sdk-cpp` CMake detection and `aws_client_config`
  - In `CMakeLists.txt` (root), add:
    ```cmake
    find_package(AWSSDK COMPONENTS ec2 autoscaling iam s3 sts QUIET)
    if (AWSSDK_FOUND)
        set(KYTHIRA_HAS_AWS_SDK TRUE)
        target_compile_definitions(kythira INTERFACE KYTHIRA_HAS_AWS_SDK)
        target_link_libraries(kythira INTERFACE AWS::aws-cpp-sdk-ec2
                                                AWS::aws-cpp-sdk-autoscaling)
        # iam is used only by integration test targets (linked separately)
    endif()
    ```
  - Create `include/raft/aws_client_config.hpp` (no SDK guard needed):
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
  - Add `DEPENDENCIES.md` entry:
    `aws-sdk-cpp ≥ 1.11 (ec2 + autoscaling) — AWS EC2 and Auto Scaling quorum managers`
  - Verify: `cmake --build build` succeeds with and without `aws-sdk-cpp` present
  - _Requirements: 1.1–1.4, 2.1–2.3_

- [x] 2. Define config structs and placement group types
  - Add `ec2_spot_interruption_behavior` enum, `ec2_spot_options` struct,
    `ec2_placement_group_strategy` enum, and `ec2_placement_group_config` struct
    to `include/raft/aws_ec2_quorum_manager.hpp` (inside `#ifdef KYTHIRA_HAS_AWS_SDK`):
    ```cpp
    enum class ec2_spot_interruption_behavior : std::uint8_t {
        terminate, stop, hibernate
    };
    struct ec2_spot_options {
        std::string max_price;
        ec2_spot_interruption_behavior interruption_behavior{
            ec2_spot_interruption_behavior::terminate};
    };
    enum class ec2_placement_group_strategy : std::uint8_t {
        none, cluster, spread, partition
    };
    struct ec2_placement_group_config {
        std::string name;
        ec2_placement_group_strategy strategy{ec2_placement_group_strategy::none};
        std::uint32_t partition_number{0};
    };
    ```
  - Add `aws_ec2_quorum_manager_config` struct with all fields per Requirement
    3.1 including `placement_by_group`, `spot_options`, and heartbeat fields:
    ```cpp
    std::optional<ec2_spot_options> spot_options{};
    std::chrono::seconds heartbeat_timeout{30};
    std::chrono::seconds heartbeat_grace_period{120};
    ```
  - Add `aws_asg_quorum_manager_config` struct to
    `include/raft/aws_asg_quorum_manager.hpp` (inside `#ifdef KYTHIRA_HAS_AWS_SDK`)
    with fields per Requirement 10.1 (no spot fields — ASG handles market type
    via launch template)
  - Both config structs SHALL be aggregates (no user-declared constructors)
  - Verify: headers compile cleanly with and without SDK present
  - _Requirements: 3.1, 3.2, 10.1, 10.2, 17.1–17.3, 18.1–18.3_

- [x] 3. Implement `aws_ec2_quorum_manager`
  - Create `include/raft/aws_ec2_quorum_manager.hpp` with the full class body inside
    `#ifdef KYTHIRA_HAS_AWS_SDK`:

  **Constructor** (Req 3.2, 4.4):
  - Validate `cluster_name` non-empty, `image_id` non-empty, `node_port` non-zero,
    every `topology.groups[i].group_id` has an entry in `subnet_by_group` →
    throw `std::invalid_argument` on violation
  - Build `Aws::Client::ClientConfiguration` from `aws_client_config`
    (region, endpoint, timeout); if `access_key_id` non-empty construct
    `Aws::Auth::AWSCredentials` and pass to client
  - Construct `_ec2` client

  **Private helpers** (design.md "Shared Private Helpers"):
  - `node_id_str(id)`: `std::to_string(id)` for integer types; identity for
    `std::string`
  - `next_node_id()`: `DescribeInstances` with `tag:kythira:cluster` filter
    (all states); scan `kythira:node-id` tags; return max-parsed + 1 (or 1 if
    empty)
  - `find_ec2_id(node_id)`: `DescribeInstances` with `tag:kythira:cluster` and
    `tag:kythira:node-id` filters; return first matching instance ID or
    `std::nullopt`
  - `apply_tags(ec2_id, node_id, group, strategy, market)`: `CreateTags` with
    seven tags per Req 5.1 + Req 17.7 (`kythira:placement-strategy`) + Req 18.6
    (`kythira:market = "spot"|"on-demand"`), merged with `extra_tags`
  - `render_user_data(node_id, az)`: linear string replacement of `{NODE_ID}`,
    `{NODE_PORT}`, `{CLUSTER}`, `{AZ}` in `user_data_template`; base64-encode
    result
  - `compute_quorum_status(live, total)`: same logic as `docker_quorum_manager`
    (majority threshold, four-level enum)

  **`assess_quorum`** (Req 6.1–6.8, design.md sequence):
  - `fiu_do_on("raft/aws/ec2/describe_instances", throw ...;)`
  - `DescribeInstances` with `tag:kythira:cluster` only (no state filter)
  - For each returned instance read: `kythira:node-id`, `instance-state-name`,
    `kythira:group`, `kythira:last-heartbeat` tag, `LaunchTime`
  - Apply heartbeat liveness rule (design.md assess_quorum step 5):
    running AND (recent heartbeat OR within grace period OR timeout==0)
  - Build `live_map` (node_id_str → bool) and `group_live` (group → count)
  - Iterate cluster vector; classify each node; build `placement_group_health`
    entries with `target_count` from `_cfg.topology`
  - Return exceptional Future on AWS error; otherwise resolved Future with
    `quorum_health`

  **`maintain_quorum`** (Req 19.2–19.3, design.md sequence):
  - `fiu_do_on("raft/aws/ec2/maintain_quorum", throw ...;)`
  - Call `assess_quorum(cluster)` internally; propagate exceptional Future
  - For each `unreachable_node`: `decommission_node` (log failures, continue);
    track which group each decommissioned node belonged to
  - Compute per-group deficits from `_cfg.topology` vs health live counts
  - For each group with deficit > 0: `provision_node(group, replacing_hint)`
    the required number of times (log failures, continue)
  - Return the pre-remediation `quorum_health`

  **`provision_node`** (Req 7.1–7.9, 17.4–17.6, 18.4–18.6, design.md sequence):
  - `fiu_do_on("raft/aws/ec2/run_instances", throw ...;)`
  - Validate `target_group` in `subnet_by_group`
  - `next_node_id()` → `new_id`
  - Look up `placement_by_group[target_group]` (default = `{name="", strategy=none}`)
  - Build `InstanceMarketOptions` when `spot_options` is set:
    `MarketType="spot"`, `SpotInstanceType="one-time"`,
    `MaxPrice=spot_options.max_price` (when non-empty),
    `InstanceInterruptionBehavior` from `spot_options.interruption_behavior`
  - Include `Placement.GroupName` in `RunInstances` when `name` non-empty;
    include `Placement.PartitionNumber` when `strategy=partition` and
    `partition_number > 0`
  - `RunInstances` with full parameters including market options; on failure
    return exceptional Future
  - `market_tag = spot_options ? "spot" : "on-demand"`
  - `apply_tags(ec2_id, new_id, target_group, pg.strategy, market_tag)`
  - Poll `DescribeInstances(ec2_id)` every `poll_interval` until `running` or
    timeout/terminal state; on timeout call `TerminateInstances` best-effort
  - Read `PrivateIpAddress`; return `peer_info{new_id, ip+":"+port}`

  **`decommission_node`** (Req 8.1–8.6, design.md sequence):
  - `fiu_do_on("raft/aws/ec2/terminate_instances", throw ...;)`
  - `find_ec2_id(node_id)` → optional; return resolved if empty
  - Check state; return resolved if already terminating/terminated
  - `TerminateInstances`; return exceptional on error, resolved on success

  **`topology()`** (Req 9.1–9.2):
  - Return `_cfg.topology` — synchronous, no API calls

  **`static_assert`** (Req 4.1):
  - Add at bottom of file (inside SDK guard):
    ```cpp
    static_assert(quorum_manager<aws_ec2_quorum_manager<std::uint64_t, std::string>,
                                 std::uint64_t, std::string, std::string>);
    ```

  - Verify: `cmake --build build` succeeds with SDK present; headers included in
    coverage build do not regress coverage floor
  - _Requirements: 3.2, 4.1–4.5, 5.1–5.4, 6.1–6.8, 7.1–7.9, 8.1–8.6, 9.1–9.2,
    15.1–15.3, 15.7, 17.4–17.7, 18.4–18.6, 19.2–19.3_

- [x] 4. Implement `aws_asg_quorum_manager`
  - Create `include/raft/aws_asg_quorum_manager.hpp` with full class body inside
    `#ifdef KYTHIRA_HAS_AWS_SDK`:

  **Constructor** (Req 10.2, 11.3–11.4):
  - Validate `cluster_name` non-empty, `asg_by_group` non-empty, `node_port`
    non-zero, every `topology.groups[i].group_id` in `asg_by_group`
  - Construct `_asg` (`AutoScalingClient`) and `_ec2` (`EC2Client`) from
    `aws_client_config`

  **Private helpers**:
  - Copy `node_id_str`, `next_node_id` (uses `_ec2`), `find_ec2_id` (uses
    `_ec2`), `apply_tags` (uses `_ec2`), `compute_quorum_status` — same
    implementations as in `aws_ec2_quorum_manager`

  **`assess_quorum`** (Req 12.1–12.6, design.md sequence):
  - `fiu_do_on("raft/aws/asg/describe_asgs", throw ...;)`
  - `DescribeAutoScalingGroups` for all ASG names in `asg_by_group`
  - Collect EC2 instance IDs of all `InService` instances
  - Single `DescribeInstances` batch call; read `kythira:node-id`,
    `kythira:cluster`, `kythira:last-heartbeat`, `LaunchTime` for each
  - Apply same heartbeat liveness rule as EC2 assess_quorum
  - Build `live_map`; classify cluster vector nodes; build health report; return

  **`maintain_quorum`** (Req 19.4, design.md sequence):
  - `fiu_do_on("raft/aws/asg/maintain_quorum", throw ...;)`
  - Same six-step pattern as `aws_ec2_quorum_manager::maintain_quorum`
    using ASG decommission and provision machinery

  **`provision_node`** (Req 13.1–13.6, design.md sequence):
  - `fiu_do_on("raft/aws/asg/update_asg", throw ...;)`
  - Validate `target_group` in `asg_by_group`
  - `DescribeAutoScalingGroups(asg_name)` → `orig_capacity`
  - `UpdateAutoScalingGroup(asg_name, DesiredCapacity = orig_capacity + 1)`
  - Poll `DescribeAutoScalingGroups(asg_name)` every `poll_interval`:
    find first `InService` instance lacking `kythira:node-id` tag
  - On timeout: restore capacity (best-effort), return exceptional Future
  - `next_node_id()`, `apply_tags`, read `PrivateIpAddress`, return `peer_info`

  **`decommission_node`** (Req 14.1–14.4, design.md sequence):
  - `fiu_do_on("raft/aws/asg/terminate_instance", throw ...;)`
  - `find_ec2_id(node_id)` → check state → idempotent if gone/terminating
  - `TerminateInstanceInAutoScalingGroup(ec2_id, ShouldDecrementDesiredCapacity=true)`

  **`topology()`**: Return `_cfg.topology`

  **`static_assert`**:
  ```cpp
  static_assert(quorum_manager<aws_asg_quorum_manager<std::uint64_t, std::string>,
                               std::uint64_t, std::string, std::string>);
  ```

  - Verify: `cmake --build build` succeeds with SDK present
  - _Requirements: 10.1–10.2, 11.1–11.5, 12.1–12.6, 13.1–13.6, 14.1–14.4, 15.4–15.6,
    15.8, 19.4_

- [x] 5. Unit tests
  - Create `tests/aws_quorum_manager_unit_test.cpp`
  - Register in `tests/CMakeLists.txt` as `aws-quorum-manager-unit-tests`,
    guarded by `if (KYTHIRA_HAS_AWS_SDK)`, with labels `unit;aws;quorum_manager`
    and a 30-second timeout per test case
  - Test cases (all using two-argument `BOOST_AUTO_TEST_CASE` per coding standards):

    **Concept satisfaction** (Req 16.1):
    - `concept_ec2_satisfied`: static_assert already in header; this test is a
      compile-time check that passes trivially at runtime (just instantiate the type)
    - `concept_asg_satisfied`: same for `aws_asg_quorum_manager`

    **`aws_ec2_quorum_manager` construction validation** (Req 16.3–16.4):
    - `ec2_empty_cluster_name_throws`: construct with `cluster_name = ""` → verify
      `std::invalid_argument`
    - `ec2_empty_image_id_throws`: construct with `image_id = ""` → verify
      `std::invalid_argument`
    - `ec2_zero_node_port_throws`: `node_port = 0` → verify `std::invalid_argument`
    - `ec2_missing_subnet_for_topology_group_throws`: topology with group `"us-east-1a"`
      but empty `subnet_by_group` → verify `std::invalid_argument`

    **`aws_asg_quorum_manager` construction validation** (Req 16.5):
    - `asg_empty_cluster_name_throws`
    - `asg_empty_asg_by_group_throws`
    - `asg_missing_asg_for_topology_group_throws`

    **Unknown-group provision futures** (Req 16.6):
    - `ec2_provision_unknown_group_returns_exceptional_future`: construct valid
      `aws_ec2_quorum_manager` (no SDK calls in ctor), call
      `provision_node("unknown-az", nullopt)`, verify the returned Future is
      exceptional with an `std::invalid_argument`
    - `asg_provision_unknown_group_returns_exceptional_future`: same for
      `aws_asg_quorum_manager`

    **Placement group config** (Req 17.8):
    - `placement_group_partition_config_valid`: construct
      `ec2_placement_group_config{.name="pg-1", .strategy=partition,
      .partition_number=2}` and verify the fields are set correctly (no exception)
    - `placement_group_none_omits_group_name`: construct manager with
      `placement_by_group` absent for the target group; call
      `provision_node` and verify no `Placement.GroupName` in the injected
      run_instances fault path (structural check only — no real EC2 needed)

    **Spot instance config** (Req 18.9):
    - `spot_options_default_is_on_demand`: construct `aws_ec2_quorum_manager_config`
      with no `spot_options` field set; verify `spot_options == std::nullopt`
    - `spot_options_terminate_behavior_populates_correctly`: construct
      `ec2_spot_options{.max_price="0.05",
      .interruption_behavior=ec2_spot_interruption_behavior::terminate}` and
      verify both fields are set as expected (no exception, aggregate init check)

    **Fault injection** (Req 16.7):
    - `ec2_assess_quorum_fault`: `fiu_enable("raft/aws/ec2/describe_instances")`,
      call `assess_quorum`, verify exceptional Future, `fiu_disable`
    - `ec2_provision_node_fault`: `fiu_enable("raft/aws/ec2/run_instances")`,
      call `provision_node("us-east-1a", nullopt)`, verify exceptional Future
    - `ec2_decommission_node_fault`: `fiu_enable("raft/aws/ec2/terminate_instances")`,
      call `decommission_node(NodeId{1})`, verify exceptional Future
    - `asg_assess_quorum_fault`, `asg_provision_node_fault`,
      `asg_decommission_node_fault`: same patterns for ASG fault points
    - `ec2_maintain_quorum_fault`: `fiu_enable("raft/aws/ec2/maintain_quorum")`,
      call `maintain_quorum({})`, verify exceptional Future
    - `asg_maintain_quorum_fault`: same for `"raft/aws/asg/maintain_quorum"`

    Note: fault injection tests for `provision_node` require `target_group` to
    be present in `subnet_by_group` / `asg_by_group` so that the fault point
    is reached (the group validation runs before the fault check in
    `decommission_node` but after the run-instances check in `provision_node` —
    align test configs accordingly).

  - Verify: `ctest -R aws-quorum-manager-unit-tests` passes; all existing
    tests pass without modification
  - _Requirements: 16.1–16.7, 15.7–15.8, 17.8, 18.9_

- [x] 6. LocalStack integration tests
  - Create `tests/aws_quorum_manager_localstack_test.cpp` guarded by
    `#ifdef KYTHIRA_AWS_LOCALSTACK_TESTS`
  - Register in `tests/CMakeLists.txt` as `aws-quorum-manager-localstack-tests`
    guarded by `KYTHIRA_HAS_AWS_SDK`; labels `integration;aws;localstack`;
    per-test timeout 120 s

  **Implement `LocalstackFixture`** (design.md § Testing Strategy, LocalStack variant):
  - Call `sts:GetCallerIdentity` against `http://localhost:4566` with dummy
    credentials; if it fails (LocalStack not running, connection refused, etc.)
    call `BOOST_TEST_SKIP` to skip the entire suite — do not fail
  - Generate UUID; derive `test_run` and `cluster_name` from it
  - Set `endpoint_override = "http://localhost:4566"`, dummy credentials
  - `user_data_template = "#!/bin/bash\n"` (LocalStack does not execute
    user_data; no S3 upload, no bastion, no readiness polling)
  - Setup (all calls go to LocalStack EC2/IAM/AutoScaling):
    - Create VPC (`10.77.0.0/16`), two private subnets in two AZs, node SG
      (inbound port 7000 from VPC CIDR)
    - Create IAM instance profile (LocalStack IAM endpoint)
    - Create Launch Template and AutoScalingGroup for ASG tests
  - Teardown (destructor, unconditional, best-effort, errors → `std::cerr`):
    1. `TerminateInstances` (all provisioned node IDs); poll until `terminated` or 60 s
    2. Delete any placement groups created during the run
    3. Delete ASG (for ASG test cases)
    4. Delete node security group
    5. Delete subnets
    6. `RemoveRoleFromInstanceProfile` → `DeleteInstanceProfile` → `DeleteRole`
    7. Delete VPC

  **`aws_ec2_quorum_manager` LocalStack tests** (Req 16.13):
  Fixture constructs manager with `spot_options = ec2_spot_options{}`:
  - `ec2_provision_three_nodes`: provision 3 nodes; verify correct tags,
    sequential IDs, and `kythira:market == "spot"`
  - `ec2_assess_detects_stopped_node`: stop one instance; verify
    `quorum_status::degraded`
  - `ec2_decommission_all_nodes`: decommission each; verify `terminated`
  - `ec2_decommission_idempotent`: double-decommission; verify resolved Future

  **`aws_asg_quorum_manager` LocalStack tests** (Req 16.14):
  - `asg_provision_increments_desired_capacity`
  - `asg_assess_detects_not_inservice`
  - `asg_decommission_decrements_desired_capacity`
  - `asg_decommission_idempotent`

  - Verify: when `KYTHIRA_AWS_LOCALSTACK_TESTS` is unset, the binary compiles
    but is not registered in CTest
  - _Requirements: 16.8–16.14_

- [x] 7. Real-AWS EC2 integration tests
  - Create `tests/aws_quorum_manager_real_ec2_test.cpp` guarded by
    `#ifdef KYTHIRA_AWS_REAL_EC2_TESTS`
  - Register in `tests/CMakeLists.txt` as `aws-quorum-manager-real-ec2-tests`
    guarded by `KYTHIRA_HAS_AWS_SDK`; labels `integration;aws;real-ec2`;
    per-test timeout 600 s

  **Implement `RealEc2Fixture`** (design.md § Testing Strategy, real-AWS variant):
  - Skip the entire suite when `AWS_REGION`, `AWS_TEST_AMI_ID`,
    `AWS_TEST_S3_BUCKET`, or `KYTHIRA_NODE_BINARY` is absent
  - Add fixture helpers (Req 16.21):
    - `get_console_output(ec2_id) → std::string`: `GetConsoleOutput` + base64-decode; empty on error
    - `quarantine_instance(ec2_id)`: `DescribeNetworkInterfaces` (filter by `attachment.instance-id`)
      to get ENI ID + original SG list; `ModifyNetworkInterfaceAttribute` to set SGs to
      `{quarantine_sg_id}` only; save original SG list per instance
    - `restore_instance_sg(ec2_id)`: `ModifyNetworkInterfaceAttribute` to restore original SGs
  - Call `sts:GetCallerIdentity` using the default credential provider chain
    (honours `AWS_ACCESS_KEY_ID` / `AWS_PROFILE` / instance profile / etc.);
    if it fails for any reason call `BOOST_TEST_SKIP` — do not fail
  - Generate UUID; derive `test_run`, `cluster_name`, `s3_prefix` from it
  - Execute setup sequence from design.md (VPC → IGW → subnets → S3 endpoint
    → bastion SG → node SG → key pair → bastion instance → IAM role →
    S3 binary upload); record every created resource for teardown
  - Build `user_data_template` with `{S3_BUCKET}`, `{S3_PREFIX}`, `{INSTANCE_ID}`,
    and `{REGION}` already substituted (constants for the run); keep
    `{NODE_ID}`, `{NODE_PORT}`, `{CLUSTER}` as per-node placeholders for the
    quorum manager; heartbeat is written by kythira-node itself (no shell loop)
  - Construct `aws_ec2_quorum_manager` with `spot_options = ec2_spot_options{}`
    as the shared default
  - After each `provision_node` call, poll `DescribeInstances` until the new
    instance has `kythira:status = ready` (timeout 120 s) before proceeding;
    this ensures peer-discovery and ClusterJoin have completed on the instance
  - Teardown (destructor, unconditional, best-effort, errors → `std::cerr`):
    Follow Req 16.10 teardown steps a–n: cluster instances + bastion →
    S3 object → PGs → restore any modified NACL association → delete
    deny-all NACL → quarantine SG → bastion SG → node SG → subnets →
    S3 endpoint → IAM → NAT GW + EIP → IGW → key pair → VPC

  **Test cases per Req 16.19:**
  - `provision_and_assess_single_az`: provision 3 nodes (sequential, with
    readiness wait between each); verify `healthy`, `live_node_count = 3`,
    `InstanceLifecycle == "spot"`, `kythira:market == "spot"` (Req 18.10)
  - `provision_multi_az_topology`: 2 nodes in AZ1, 1 in AZ2; verify per-group
    health labels
  - `terminate_one_node_degraded`: provision 3 nodes; `StopInstances` on one;
    verify `assess_quorum` → `degraded` with that node in `unreachable_nodes`
  - `decommission_idempotent`: decommission same node ID twice; both resolved
  - `placement_group_cluster_strategy`: fixture creates cluster PG if
    `AWS_TEST_PG_CLUSTER_NAME` absent; PG deleted in teardown step 3
  - `placement_group_spread_strategy`: same with spread PG across AZ1+AZ2
  - `placement_group_partition_strategy`: partition PG, `partition_number=1`;
    verify `DescribeInstances[].Placement.PartitionNumber == 1`
  - `provision_timeout_cleanup`: `provision_timeout = 1s`; verify exceptional
    Future; verify partially-created instance is terminated
  - `on_demand_provision_and_decommission`: fresh manager with
    `spot_options = std::nullopt`; verify `InstanceLifecycle` absent/`"normal"`
    and `kythira:market == "on-demand"`; decommission; verify `terminated`
  - `heartbeat_timeout_triggers_replacement` (Req 19.6):
    - Provision 3 nodes (AZ1) with `heartbeat_timeout = 30s`
    - SSH through bastion: `kill $(pgrep kythira-node)` on one node
      (kythira stops; heartbeat loop inside kythira stops with it)
    - Wait 35 s; call `assess_quorum`; verify dead node is `unreachable`
    - Call `maintain_quorum`; verify termination + AZ1 replacement + ready
    - Call `get_console_output` on both instances; include in test log
  - `network_isolation_triggers_replacement` (Req 16.19k):
    - Provision 3 nodes (AZ1)
    - Call `quarantine_instance` on one node (replaces SG with quarantine SG;
      no outbound traffic → kythira can't reach EC2 API → heartbeat stops)
    - Wait for `heartbeat_timeout`; call `assess_quorum`; verify `unreachable`
    - Call `maintain_quorum`; verify termination + AZ1 replacement + ready
  - `host_termination_triggers_replacement` (Req 16.19l):
    - Provision 3 nodes (AZ1)
    - Call `TerminateInstances` directly on one node (hardware failure sim)
    - Verify `assess_quorum` reports it `unreachable` (state = terminated)
    - Call `maintain_quorum`; verify replacement in AZ1 reaches ready
  - `az_outage_during_rolling_deployment` (Req 16.19m):
    - Use 3-AZ topology: `{AZ1: 3, AZ2: 3, AZ3: 3}` with the fixture's
      three private subnets; topology passed to `aws_ec2_quorum_manager_config`
    - Provision 9 nodes sequentially (readiness wait between each)
    - AZ3 network failure (quarantine all 3 AZ3 nodes via SG):
        `quarantine_instance` on all 3 AZ3 nodes
        (no outbound → heartbeat stops on all three)
    - AZ2 single-host termination (EC2 API):
        `TerminateInstances` on 1 AZ2 node
    - Wait for `heartbeat_timeout`
    - Call `assess_quorum`; verify:
        `quorum_status::critical` (5/9 live),
        AZ1=3/3, AZ2=2/3, AZ3=0/3,
        `unreachable_nodes` has 4 entries
    - Call `maintain_quorum`; verify:
        4 decommissions (3 AZ3 + 1 AZ2, topology-aware)
        3 new nodes provisioned in AZ3
        1 new node provisioned in AZ2
        All 4 replacements reach `kythira:status = ready`
    - Call `assess_quorum` again; verify `healthy`, `live_node_count = 9`

  - `az_outage_provision_fails_in_broken_az` (Req 16.19n):
    - Use same 3-AZ × 3-node setup as above; provision 9 nodes
    - `quarantine_instance` all 3 AZ3 nodes; `TerminateInstances` 1 AZ2 node
    - Wait for `heartbeat_timeout`
    - Construct a SECOND `aws_ec2_quorum_manager` (same config except
      `subnet_by_group["AZ3"]` = `"subnet-00000000000000000"`)
    - Call `maintain_quorum` on the broken-config manager
    - Verify: no exception thrown; 4 decommissions succeed; AZ3 provision
      calls fail (AWS error logged to stderr); AZ2 provision succeeds and
      replacement reaches `kythira:status = ready`
    - Call `assess_quorum` via the ORIGINAL manager; verify
      `quorum_status::degraded` (6/9 live: AZ1=3/3, AZ2=3/3, AZ3=0/3)

  - `az_outage_instances_launch_but_cannot_join` (Req 16.19o):
    - Use same 3-AZ × 3-node setup; provision 9 nodes
    - `quarantine_instance` all 3 AZ3 nodes; `TerminateInstances` 1 AZ2 node
    - Wait for `heartbeat_timeout`
    - Call `DescribeNetworkAcls` to find the AZ3 subnet's current association
      ID; save it for restore
    - Call `ReplaceNetworkAclAssociation` to associate AZ3 subnet with the
      deny-all NACL (created by the fixture; blocks all inbound and outbound)
    - Call `maintain_quorum` (valid subnets); verify:
        4 decommissions succeed
        3 AZ3 instances reach EC2 `running` (provision_node returns peer_info)
        1 AZ2 replacement succeeds and reaches `kythira:status = ready`
    - Wait for `heartbeat_grace_period` to elapse
    - Call `assess_quorum`; verify AZ3=0/3 (grace expired, no heartbeat),
      `quorum_status::degraded` (6/9 live)
    - Call `ReplaceNetworkAclAssociation` to restore AZ3 subnet to the saved
      original association
    - Poll `assess_quorum` (up to `heartbeat_grace_period`, every 15 s) until
      AZ3 live count = 3
    - Call `assess_quorum`; verify `quorum_status::healthy`, `live_node_count = 9`

  Add `get_console_output(ec2_instance_id) → std::string` helper to
  `RealEc2Fixture` (Req 16.21, 19.7):
  - Calls `ec2:GetConsoleOutput(InstanceId, Latest=true)`
  - Base64-decodes the `Output` field
  - Returns empty string on any error (never throws)
  - Called automatically when `kythira:status = ready` poll times out;
    output appended to the skip/failure message

  - Verify: when `KYTHIRA_AWS_REAL_EC2_TESTS` is unset, the binary compiles
    but is not registered in CTest; all other tests pass without modification
  - _Requirements: 16.8–16.11, 16.15–16.21, 17.4–17.7, 18.10, 19.6–19.7_

## Notes

- The AWS SDK requires `Aws::InitAPI()` to be called before any client is
  constructed. Unit tests that instantiate a manager must call
  `Aws::InitAPI(opts)` in a fixture setup and `Aws::ShutdownAPI(opts)` in
  teardown. A shared `AwsSdkFixture` struct in the test file handles this.

- The `next_node_id()` helper has a TOCTOU race if two leaders simultaneously
  call `provision_node`. The quorum management spec (Requirements 14.3–14.4)
  prevents this: the leader tracks pending provisions and does not call
  `provision_node` again for a slot while a prior call is in-flight. A single
  `aws_ec2_quorum_manager` instance is therefore never called concurrently for the
  same slot. No locking is required.

- `user_data_template` is the primary mechanism for starting kythira on a new
  instance. In real-AWS tests the fixture constructs this dynamically with S3
  coordinates embedded; the quorum manager only substitutes `{NODE_ID}`,
  `{NODE_PORT}`, `{CLUSTER}`, and `{AZ}`.

- When using LocalStack for integration tests, `DescribeInstances` state
  transitions happen synchronously (LocalStack does not simulate boot delays),
  so the provision polling loop completes on the first poll. This makes
  `provision_timeout` irrelevant for LocalStack tests but exercises the happy
  path correctly.

- The IAM caller running the real-AWS integration tests needs the following
  permissions for the fixture's resource creation and teardown to succeed:
  ```
  ec2:CreateVpc, ec2:DeleteVpc, ec2:ModifyVpcAttribute
  ec2:CreateInternetGateway, ec2:DeleteInternetGateway,
  ec2:AttachInternetGateway, ec2:DetachInternetGateway
  ec2:AllocateAddress, ec2:ReleaseAddress
  ec2:CreateNatGateway, ec2:DeleteNatGateway, ec2:DescribeNatGateways
  ec2:CreateRouteTable, ec2:CreateRoute, ec2:AssociateRouteTable
  ec2:CreateSubnet, ec2:DeleteSubnet, ec2:ModifySubnetAttribute
  ec2:CreateVpcEndpoint, ec2:DeleteVpcEndpoints, ec2:DescribeVpcEndpoints
  ec2:CreateSecurityGroup, ec2:DeleteSecurityGroup,
  ec2:AuthorizeSecurityGroupIngress, ec2:RevokeSecurityGroupEgress
  ec2:CreateNetworkAcl, ec2:DeleteNetworkAcl, ec2:DescribeNetworkAcls,
  ec2:CreateNetworkAclEntry, ec2:ReplaceNetworkAclAssociation
  ec2:DescribeNetworkInterfaces, ec2:ModifyNetworkInterfaceAttribute
  ec2:CreateKeyPair, ec2:DeleteKeyPair
  ec2:RunInstances, ec2:TerminateInstances, ec2:DescribeInstances,
  ec2:StopInstances, ec2:CreatePlacementGroup, ec2:DeletePlacementGroup,
  ec2:CreateTags, ec2:DescribeAvailabilityZones
  iam:CreateRole, iam:DeleteRole, iam:PutRolePolicy, iam:DeleteRolePolicy,
  iam:CreateInstanceProfile, iam:DeleteInstanceProfile,
  iam:AddRoleToInstanceProfile, iam:RemoveRoleFromInstanceProfile,
  iam:PassRole
  s3:PutObject, s3:DeleteObject on arn:aws:s3:::{AWS_TEST_S3_BUCKET}/kythira-test/*
  autoscaling:* (for ASG LocalStack tests only)
  ```
  A sample IAM policy for CI should be added to `doc/aws-test-iam-policy.json`.

- Teardown uses best-effort deletion with accumulated error reporting. The
  implementation pattern:
  ```cpp
  std::vector<std::string> teardown_errors;
  auto try_delete = [&](auto fn) {
      try { fn(); } catch (const std::exception& e) {
          teardown_errors.push_back(e.what());
      }
  };
  // ... call try_delete for each step ...
  if (!teardown_errors.empty()) {
      for (const auto& err : teardown_errors)
          std::cerr << "[IntegrationFixture teardown] " << err << "\n";
  }
  ```

- If `clang-tidy` is enabled, add suppressions for any `aws-sdk-cpp` headers
  that trigger warnings (use `// NOLINT` on the includes or add a `.clang-tidy`
  suppression glob for `aws/` paths). The existing `.clang-tidy` config
  suppresses third-party includes via `HeaderFilterRegex`.

### Task: Implement test cost estimation and reporting (Req 20)

Add to `tests/aws_quorum_manager_real_ec2_test.cpp`:

1. **`BilledResource` struct** — label, hourly_rate, start time,
   optional end time; `finalize()` sets end = now; `cost_usd()` and
   `minutes()` computed from the interval.

2. **`TestCostReport` struct** — `std::string test_name`,
   `std::vector<BilledResource> resources`; `total_usd()` sums all
   entries; `format()` returns a `BOOST_TEST_MESSAGE`-ready string with
   one row per resource (label, minutes, $cost) plus a TOTAL row.

3. **`ec2_hourly_rate(type)` free function** — `std::map` of known
   instance types to on-demand us-east-1 Linux prices; unknown types
   fall back to `0.0104` (t3.micro).  Constants `kNatGwHourly = 0.045`
   and `kEipHourly = 0.005`.

4. **`CostAccumulator` file-scope variable** — `std::mutex` + `std::vector<TestCostReport>`;
   `add(TestCostReport)` is the only mutating method.

5. **`CostSummaryFixture` global fixture** — registered with
   `BOOST_GLOBAL_FIXTURE`; destructor locks `CostAccumulator`, formats
   and emits the summary table (per-test rows + grand total +
   disclaimer) via `BOOST_TEST_MESSAGE`.

6. **`RealEc2Fixture` additions**:
   - `TestCostReport cost_report` member; `test_name` set from
     `boost::unit_test::framework::current_test_case()->p_name` in
     constructor.
   - Open EIP timer immediately after `AllocateAddress`.
   - Open NAT gateway timer immediately after `CreateNatGateway`.
   - Open bastion timer immediately after `RunInstances` for the
     bastion.
   - Public `track_instances(std::size_t count)` method that appends a
     cluster-instance `BilledResource` for `count × instance_type`.
   - In `teardown()`, after all deletions: call `r.finalize()` on every
     resource, `BOOST_TEST_MESSAGE(cost_report.format())`, then
     `g_cost_accumulator.add(std::move(cost_report))`.

7. **Test-case call sites** — every test case in the real-EC2 suite
   MUST call `this->track_instances(n)` immediately after it finishes
   provisioning its initial cluster nodes, where `n` is the number of
   instances provisioned.  Tests that let `maintain_quorum` provision
   additional nodes SHOULD note in the report comment that the estimate
   is a lower bound.

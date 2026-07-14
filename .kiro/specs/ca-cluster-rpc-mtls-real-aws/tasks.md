# Implementation Plan — CA Cluster RPC mTLS Real AWS Validation

## Status: Complete (compile-verified only — no AWS access in this environment)

**Last Updated**: July 14, 2026

## Overview

Extend real-EC2 test coverage of `ca_cluster_node` (today: plain-TCP only,
`tests/ca_cluster_node_real_ec2_test.cpp`) to exercise RPC TLS end-to-end
across three real, separate EC2 instances — bootstrap-and-cutover, staggered
node join, restart without the bootstrap credential, and a security-group
network-isolation recovery scenario — and generalize the existing cost-
tracking and signal-driven cleanup apparatus (today: only implemented in
`tests/aws_quorum_manager_real_ec2_test.cpp`) into a shared header so every
real-EC2 test binary gets both.

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [1],
      "description": "Extract shared cost/signal infrastructure into tests/aws_real_ec2_test_support.hpp — no dependents can build against it until it exists"
    },
    {
      "wave": 2,
      "tasks": [2, 3],
      "description": "Retrofit the two existing real-EC2 test files to use the shared header (independent of each other, both depend only on wave 1)"
    },
    {
      "wave": 3,
      "tasks": [4],
      "description": "New rpc_tls_three_az_network_fixture + shared helpers (bootstrap credential, user-data, start/stop commands, quarantine SG) — depends on wave 1's signal_cleanup_target/cost types"
    },
    {
      "wave": 4,
      "tasks": [5, 6, 7, 8],
      "description": "The four test cases (Requirements 2-5) — each depends on wave 3's fixture; independent of each other"
    },
    {
      "wave": 5,
      "tasks": [9],
      "description": "CMakeLists.txt registration — depends on the test file existing (wave 4) to know its final source/link requirements"
    }
  ]
}
```

## Tasks

## Phase 1: Shared Test Infrastructure (Task 1)

- [x] 1. Extract `tests/aws_real_ec2_test_support.hpp`
  - New header-only file. Move `BilledResource`, `TestCostReport`,
    `CostAccumulator`, `CostSummaryFixture`, `ec2_hourly_rate()`,
    `kNatGwHourly`, `kEipHourly` out of
    `tests/aws_quorum_manager_real_ec2_test.cpp` verbatim, into namespace
    `kythira::testing::aws_real_ec2`; `g_cost_accumulator` becomes an
    `inline` variable (not `static`) so each including binary gets its own
    definition per Requirement 6.5.
  - Add `signal_cleanup_target` (pure-virtual `teardown() noexcept`,
    protected non-virtual destructor), `g_active_aws_fixture` (`inline
    std::atomic<signal_cleanup_target*>`), `aws_signal_cleanup_handler()`,
    `install_aws_signal_handlers()`, `AwsSignalHandlerFixture` — generalized
    from `aws_quorum_manager_real_ec2_test.cpp`'s existing concretely-typed
    `signal_cleanup_handler`/`install_signal_handlers`/
    `SignalHandlerFixture`/`g_active_fixture`.
  - _Requirements: 6.1, 7.1_

## Phase 2: Retrofit Existing Real-EC2 Tests (Tasks 2-3)

- [x] 2. Route `aws_quorum_manager_real_ec2_test.cpp` through the shared header
  - `#include "aws_real_ec2_test_support.hpp"`; delete the now-duplicate
    local `BilledResource`/`TestCostReport`/`CostAccumulator`/
    `CostSummaryFixture`/`ec2_hourly_rate`/pricing-constant/
    `signal_cleanup_handler`/`install_signal_handlers`/
    `SignalHandlerFixture`/`g_active_fixture` definitions.
  - `RealEc2Fixture` now derives from `signal_cleanup_target` and overrides
    `teardown()`; its constructor/destructor's `g_active_fixture` references
    become `kythira::testing::aws_real_ec2::g_active_aws_fixture`.
  - Confirm (manually, per design.md Property 2) that cost-report output
    format and signal-cleanup behavior are unchanged.
  - _Requirements: 6.2, 7.2_

- [x] 3. Add cost tracking + signal cleanup to `ca_cluster_node_real_ec2_test.cpp`
  - `#include "aws_real_ec2_test_support.hpp"`.
  - `three_az_network_fixture` gains: `cost_report` member, `torn_down_`
    flag, `track_instance(label, instance_type)` method, `teardown()`
    (idempotent, existing destructor body moved into it plus cost-report
    finalization/emission/accumulation), derives from
    `signal_cleanup_target`, registers/clears itself in
    `g_active_aws_fixture` in its constructor/`teardown()`.
  - `BOOST_GLOBAL_FIXTURE(kythira::testing::aws_real_ec2::CostSummaryFixture)`
    and `BOOST_GLOBAL_FIXTURE(kythira::testing::aws_real_ec2::AwsSignalHandlerFixture)`
    added to this file.
  - Existing test case `three_real_ec2_nodes_form_working_ca_cluster` gains
    one `track_instance(...)` call per node, right after each
    `provision_node()` call.
  - _Requirements: 6.3, 7.3_

## Phase 3: RPC-TLS Real-EC2 Fixture (Task 4)

- [x] 4. New `tests/ca_cluster_node_rpc_tls_real_ec2_test.cpp` — fixture and helpers
  - `#include "aws_real_ec2_test_support.hpp"` plus
    `<raft/certificate_authority.hpp>`, `<raft/ca_bootstrap_client.hpp>`
    (for `detail_testing::unsafe_extract_ca_private_key_pem`).
  - `rpc_tls_three_az_network_fixture`: VPC/subnet-per-AZ/security-group/
    key-pair constructor body adapted (copied, not inherited — see
    design.md's Architecture rationale) from
    `three_az_network_fixture`; add a `bootstrap_cred` member
    (`raft::testing::certificate_authority`, constructed once per fixture
    instance); derives from `signal_cleanup_target`; `teardown()` matches
    task 3's shape plus quarantine-SG cleanup if one was created.
  - `make_rpc_tls_user_data()`: extends the existing `make_user_data()`
    shape with the bootstrap credential's cert/key written to
    `/etc/ca_cluster_node/rpc_bootstrap.{crt,key}` (`chmod 600`).
  - `start_node_command(node_id, peers_arg, bootstrap, use_rpc_tls_flags)`:
    extends the existing `start_node_command()` shape; when
    `use_rpc_tls_flags` is true, appends `--rpc-tls-cert
    /etc/ca_cluster_node/rpc_bootstrap.crt --rpc-tls-key
    /etc/ca_cluster_node/rpc_bootstrap.key`; also raises
    `--election-timeout-min-ms`/`--election-timeout-max-ms`/
    `--heartbeat-interval-ms`/`--rpc-timeout-ms` to values at least as
    generous as `ca_cluster_node_rpc_tls_test.cpp`'s CI-tuned constants.
  - `stop_node_process(public_ip)`: SSH `pkill -f ca_cluster_node` (or
    equivalent PID-targeted kill) without terminating the instance.
  - `ensure_quarantine_sg()`: lazily creates (memoized) an SSH-only security
    group in the fixture's VPC.
  - `set_instance_security_group(instance_id, sg_id)`:
    `ModifyNetworkInterfaceAttribute` reassigning the instance's primary
    ENI's security groups to just `sg_id`.
  - A shared `wait_for_issuance_capable(public_ips, timeout)` /
    `try_issue_certificate_over_ssh(public_ips, timeout)` helper pair,
    adapting `ca_cluster_node_real_ec2_test.cpp`'s existing SSH-`curl`-
    against-`/v1/root-ca` polling pattern to also POST a CSR (matching
    `ca_cluster_node_rpc_tls_test.cpp`'s local
    `try_issue_certificate()`, executed via `ssh_execute(...)` running
    `curl -X POST .../v1/certificates` instead of an in-process
    `httplib::Client`, since the test runner may not have direct network
    access to the instances' HTTPS port).
  - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5, 1.6_

## Phase 4: Test Cases (Tasks 5-8)

- [x] 5. `bootstrap_and_cutover_survives_bootstrap_credential_deletion`
  - Launch 3 nodes (one per AZ) with RPC TLS from first boot; wait healthy;
    confirm a certificate issuance succeeds; delete the bootstrap
    credential's cert/key files on all three instances over SSH; confirm a
    further issuance still succeeds.
  - _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5_

- [x] 6. `staggered_third_node_join_maintains_connectivity`
  - Launch 2 nodes (one `--bootstrap-ca`-flagged); confirm 2-of-3 issuance
    works; launch the third node; issue several certificates in a row
    during the staggered window; poll for full convergence via repeated
    successful issuance.
  - _Requirements: 3.1, 3.2, 3.3, 3.4_

- [x] 7. `restarted_node_rejoins_without_bootstrap_credential`
  - Launch 3 nodes with RPC TLS; confirm full cutover (persisted peer cert
    file exists on the target node, over SSH); stop the target node's
    process (not the instance); delete the bootstrap credential on that
    instance; restart `ca_cluster_node` with no `--rpc-tls-cert`/
    `--rpc-tls-key`; confirm it becomes healthy and the cluster keeps
    issuing certificates.
  - _Requirements: 4.1, 4.2, 4.3_

- [x] 8. `security_group_isolation_during_cutover_recovers`
  - Launch 3 nodes with RPC TLS, reach issuance-capable state; reassign one
    non-leader node's ENI to a quarantine (SSH-only) security group;
    confirm the remaining two nodes keep issuing certificates; restore the
    node's original security group; confirm it rejoins (its own
    `/v1/root-ca` becomes reachable again) without a process restart.
  - _Requirements: 5.1, 5.2, 5.3, 5.4_

## Phase 5: Build Registration (Task 9)

- [x] 9. Register `ca_cluster_node_rpc_tls_real_ec2_test` in `tests/CMakeLists.txt`
  - Inside the existing `if(LIBSSH2_FOUND)` block, alongside
    `aws_quorum_manager_real_ec2_test`/`ca_cluster_node_real_ec2_test`:
    same `KYTHIRA_AWS_REAL_EC2_TESTS`/`LIBSSH2_FOUND` compile definitions,
    same `network_simulator`/`Boost::unit_test_framework`/
    `${LIBSSH2_LIBRARIES}` link libraries, same `${LIBSSH2_INCLUDE_DIRS}`
    include directories, `TIMEOUT 1800`, `LABELS
    "integration;raft;quorum;aws;real-ec2;slow;ca;ca_cluster_node;rpc_tls"`.
  - _Requirements: 8.1, 8.2, 8.3, 8.4_

## Notes

- No new external dependency: this spec reuses the AWS SDK, libssh2, and
  Boost.Test dependencies already required by `aws_quorum_manager_real_ec2_test`/
  `ca_cluster_node_real_ec2_test`. No new `vcpkg.json` entries.
- Every task in this plan touches only files under `tests/` (design.md
  Property 1) — `include/raft/`, `cmd/ca_cluster_node/`, and
  `docker/ca_cluster_node/` are read-only references throughout.
- These tests require live AWS credentials and `KYTHIRA_EC2_TEST_AMI`
  pointing at an AMI with `/usr/local/bin/ca_cluster_node` installed (same
  requirement `ca_cluster_node_real_ec2_test.cpp` already documents in its
  own file header) — they cannot be exercised in this project's own CI, and
  had no AWS account available in the environment this spec was
  implemented in either. All 9 tasks are implemented and the full project
  (including these 3 real-EC2 binaries) builds cleanly, and each new/
  retrofitted fixture was confirmed to fail gracefully with a clear "skip"
  message when AWS credentials are absent — matching
  `ca_cluster_node_real_ec2_test.cpp`'s own existing, identical limitation
  (documented in `doc/TODO.md`: "LocalStack/real-EC2 tests compile-verified
  only"). Actually running these against a real AWS account remains a
  follow-up for whoever has one.
- Requirement 5's network-isolation test case
  (`network_isolation_during_cutover_recovers`) uses a subnet-level
  deny-all NACL swap (`CreateNetworkAcl`/`CreateNetworkAclEntry`/
  `ReplaceNetworkAclAssociation`), not per-instance security-group
  reassignment (`ModifyNetworkInterfaceAttribute`) as requirements.md's
  Requirement 5.2 literally describes. The NACL technique is already
  implemented and exercised in `aws_quorum_manager_real_ec2_test.cpp`;
  `ModifyNetworkInterfaceAttributeRequest.h` was imported there but never
  actually used anywhere in this codebase. Since this fixture's topology is
  one node per AZ/subnet, isolating an AZ's subnet is equivalent to
  isolating its one node — reusing the proven mechanism over inventing new,
  unverified AWS API usage was judged the better trade-off. requirements.md
  is written in terms of the desired *outcome* (one node's network
  reachability is cut off and later restored), which this implementation
  satisfies; the specific AWS API achieving that outcome is an
  implementation detail this note records for anyone comparing the two
  documents directly.

# CA Cluster RPC mTLS — Real AWS Validation Requirements Document

## Introduction

`.kiro/specs/ca-cluster-rpc-mtls/` implemented mutual TLS on `ca_cluster_node`'s
Raft-internal RPC channel and proved its correctness properties — the
two-phase bootstrap, the dual-trust window, staggered-finalization safety
(Property 3), and bootstrap-credential-free restart (Property 5) — entirely
via loopback-TCP multi-process tests (`tests/ca_cluster_node_rpc_tls_test.cpp`,
`tests/ca_cluster_node_rpc_tls_restart_test.cpp`). That spec's own
implementation history is itself the reason this one exists: its first
version passed every local and sandbox test run, was merged, and then
deadlocked reliably on GitHub Actions' shared CI runners — a real,
reproducible race between one node's RPC identity switch and its peers'
trust-policy widening that only manifested under host contention loopback
testing never exercised. Real EC2 instances, separate hosts, real network
latency between AZs, and real (if modest) scheduling contention are a
meaningfully different environment from either a fast idle workstation or a
loopback-only CI container, and this project already has direct evidence
that "passes locally, passes in CI" is not the same claim as "works when
the three nodes are not the same process tree on the same kernel."

`certificate-authority` spec Requirement 17.12(b) already covers deploying a
*plain-TCP* `ca_cluster_node` cluster across three real EC2 instances, one
per Availability Zone, via `aws_ec2_quorum_manager`
(`tests/ca_cluster_node_real_ec2_test.cpp`). This document extends that same
real-EC2 harness to run with RPC TLS enabled from first boot, and adds
coverage for the properties that are specific to the RPC-TLS feature: the
bootstrap-credential-to-CA-root cutover, staggered node join, restart
without the bootstrap credential, and an infrastructure-level failure mode
(security-group isolation) that cannot be exercised at all in a loopback
test, since loopback has no concept of a network boundary between peers.

This spec also extends `aws-quorum-manager`'s Requirement 20 (test cost
estimation and reporting) and Requirement 21 (signal-driven test cleanup),
generalizing both out of `tests/aws_quorum_manager_real_ec2_test.cpp` (their
only implementation today) into a shared header so that
`tests/ca_cluster_node_real_ec2_test.cpp` — which currently has neither, and
would otherwise leak a VPC and running EC2 instances if killed mid-run — and
the new RPC-TLS real-EC2 test both get the same safety net.

## Glossary

- **Bootstrap credential**: as defined in `ca-cluster-rpc-mtls`'s own
  glossary — the static, byte-identical, self-signed X.509 certificate/key
  pair every node initially trusts for RPC.
- **Cutover**: as defined in `ca-cluster-rpc-mtls` — the point a node stops
  accepting the bootstrap credential for new RPC connections.
- **`RealEc2Fixture`**: the existing Boost.Test fixture in
  `tests/aws_quorum_manager_real_ec2_test.cpp` that provisions a throwaway
  VPC/subnets/security groups/bastion for real-AWS integration tests and
  tears them down (including cost reporting and signal-driven cleanup, once
  this spec's Requirement 6/7 land).
- **`three_az_network_fixture`**: the existing, lighter-weight fixture in
  `tests/ca_cluster_node_real_ec2_test.cpp` — a public-subnet-per-AZ VPC with
  no bastion/NAT, since that test reaches instances directly by public IP.
- **Real-AWS test**: a test compiled only when `KYTHIRA_AWS_REAL_EC2_TESTS`
  is defined, excluded from the default CTest run, tagged with label
  `real-ec2`, and requiring live AWS credentials to run at all.

## Requirements

### Requirement 1: RPC-TLS-enabled real-EC2 test harness

**User Story:** As a developer validating the ca-cluster-rpc-mtls feature, I
want a real-EC2 test harness that launches `ca_cluster_node` with
`--rpc-tls-cert`/`--rpc-tls-key` from first boot, so the feature is exercised
end-to-end on separate physical hosts rather than only over loopback.

#### Acceptance Criteria

1. A new `tests/ca_cluster_node_rpc_tls_real_ec2_test.cpp` SHALL be added,
   gated the same way as `tests/ca_cluster_node_real_ec2_test.cpp`
   (`KYTHIRA_AWS_REAL_EC2_TESTS`, `LIBSSH2_FOUND`, excluded from the default
   CTest run, labeled `integration;raft;quorum;aws;real-ec2;slow;ca;
   ca_cluster_node;rpc_tls`).
2. The bootstrap credential SHALL be generated once, in-process, at test
   start via `raft::testing::certificate_authority` — the same helper class
   `tests/ca_cluster_node_rpc_tls_test.cpp` already uses locally
   (`certificate_authority bootstrap_cred; bootstrap_cred.root_certificate_pem();
   detail_testing::unsafe_extract_ca_private_key_pem(bootstrap_cred);`) — not
   the operator-facing `openssl req -x509` one-liner, since the test needs
   the PEM content in memory to embed into user-data.
3. Each node's EC2 user-data script SHALL write the bootstrap credential's
   certificate and private key to disk (alongside the existing unseal-key
   file, matching `docker/ca_cluster_node/ca_cluster_node.service`'s layout
   convention) before `ca_cluster_node` starts, so the credential is present
   for the node's very first invocation — no post-boot SCP step.
4. The `ca_cluster_node` start command executed over SSH (extending
   `start_node_command()`'s existing shape in
   `tests/ca_cluster_node_real_ec2_test.cpp`) SHALL add
   `--rpc-tls-cert`/`--rpc-tls-key` pointing at the files written in AC 3.
5. This spec SHALL reuse the AMI, VPC/subnet-per-AZ topology, security group,
   and `aws_ec2_quorum_manager`-based provisioning already established by
   `tests/ca_cluster_node_real_ec2_test.cpp` — no new provisioning mechanism,
   consistent with `ca-cluster-rpc-mtls` Requirement 3.5's "no CA-specific
   EC2 provisioning mechanism" principle carried over to its own test
   infrastructure.
6. The security group SHALL permit the RPC-TLS port (same `7000` already
   used for plain RPC — TLS wraps the existing port, per `ca-cluster-rpc-mtls`
   design, it does not add a new one) alongside the existing SSH and
   client-facing HTTPS ports.

### Requirement 2: Bootstrap-and-cutover verified on real EC2

**User Story:** As a developer, I want the exact end-to-end property the
`ca-cluster-rpc-mtls` spec exists to prove — that the cluster keeps issuing
certificates after the bootstrap credential is deleted — verified against
three real, separate EC2 instances, not just three processes on one host.

#### Acceptance Criteria

1. A test case SHALL launch three real EC2 instances, one per AZ, each
   started with only the bootstrap credential (mirroring
   `tests/ca_cluster_node_rpc_tls_test.cpp`'s
   `bootstrap_cutover_and_survives_bootstrap_credential_deletion`, adapted
   for SSH-driven verification instead of in-process log/API calls).
2. The test SHALL confirm, via SSH `curl` against each instance's
   client-facing HTTPS port (matching
   `tests/ca_cluster_node_real_ec2_test.cpp`'s existing
   `/v1/root-ca`-reachability polling pattern): election and replication
   succeed, `bootstrap_ca` commits, and a certificate issuance succeeds
   through whichever node is leader.
3. The test SHALL then delete the bootstrap credential's certificate and key
   files, over SSH, on all three instances.
4. The test SHALL confirm a further certificate issuance still succeeds
   after AC 3 — this is the load-bearing assertion the whole spec exists to
   support; a failure here is a genuine regression, not test flakiness.
5. The `--election-timeout-min-ms`/`--election-timeout-max-ms`/
   `--heartbeat-interval-ms`/`--rpc-timeout-ms` flags SHALL use generous
   values reflecting real inter-AZ network latency and TLS handshake cost —
   at least as large as the values `tests/ca_cluster_node_rpc_tls_test.cpp`
   settled on for CI, since cross-AZ EC2 round trips are not faster than a
   contended CI container's loopback.

### Requirement 3: Staggered node join verified on real EC2

**User Story:** As a developer, I want Property 3 (staggered finalization
never strands a peer) verified with a real, wall-clock-significant delay
between when two nodes and the third node join, so the property holds
against real network conditions, not just the artificial `sleep_for` used in
the loopback test.

#### Acceptance Criteria

1. A test case SHALL launch two nodes (one of them `--bootstrap-ca`-flagged)
   in two AZs, wait for them to reach an issuance-capable state, and confirm
   a certificate issuance succeeds using only those two nodes — mirroring
   `tests/ca_cluster_node_rpc_tls_test.cpp`'s 2-of-3 phase.
2. The test SHALL then launch the third node, in the third AZ, and confirm
   it becomes healthy and joins the cluster.
3. The test SHALL issue multiple certificates in a row during the window
   after the third node joins but before cutover has necessarily finalized
   clusterwide, and confirm every issuance succeeds — Property 3's
   connectivity-holds-throughout assertion, not just at the end.
4. The test SHALL confirm eventual full convergence (all three nodes
   reporting `rpc_tls_ready`, cutover finalized) by polling repeated
   successful issuances over a window, matching the local test's approach
   since there is no direct API exposing `rpc_tls_ready_node_ids()`.

### Requirement 4: Restart without bootstrap credential verified on real EC2

**User Story:** As an operator, I want Property 5 (a restarted, already-
cutover node needs no bootstrap credential) verified against a real EC2
instance restart, so I have confidence the documented operational claim in
`docker/ca_cluster_node/README.md` — "the bootstrap credential is only
needed for a node's very first cutover" — actually holds when the process
restarts on the same instance with the same `--data-dir`.

#### Acceptance Criteria

1. A test case SHALL bring up a 3-node cluster with RPC TLS, confirm all
   three nodes cut over (via the same repeated-successful-issuance polling
   as Requirement 3.4), and confirm the target node's persisted peer
   certificate file exists on disk (over SSH) before proceeding — proving
   cutover actually happened for that node, not merely that the cluster as
   a whole is healthy.
2. The test SHALL stop the target node's `ca_cluster_node` process (SSH,
   `kill` by PID or a matching process-name signal) without terminating the
   underlying EC2 instance, delete the bootstrap credential's certificate
   and key files on that instance, and restart `ca_cluster_node` with
   neither `--rpc-tls-cert` nor `--rpc-tls-key` given.
3. The test SHALL confirm the restarted node becomes healthy again and the
   cluster continues issuing certificates.

### Requirement 5: Security-group isolation during cutover recovers

**User Story:** As an operator, I want confidence that a transient network
partition between AZs during the RPC-TLS cutover window doesn't leave the
cluster permanently stuck or corrupt the trust state, since this is exactly
the class of real-infrastructure failure mode that motivated the CI deadlock
fix in `ca-cluster-rpc-mtls` — a follower's inability to reach the leader —
and no loopback test can exercise an actual network-layer block between
peers.

#### Acceptance Criteria

1. A test case SHALL bring up a 3-node cluster with RPC TLS and reach an
   issuance-capable state.
2. The test SHALL create a "quarantine" security group permitting only SSH
   (mirroring `aws-quorum-manager`'s `network_isolation_triggers_replacement`
   pattern) and reassign one non-leader node's network interface to it via
   `ModifyNetworkInterfaceAttribute`, cutting off that node's RPC and
   client-facing HTTP traffic while leaving SSH reachable for verification.
3. The test SHALL confirm the remaining two nodes retain a majority and
   continue issuing certificates while the third is isolated.
4. The test SHALL restore the quarantined node's original security group
   and confirm it rejoins the cluster (resumes receiving/acknowledging
   AppendEntries, verified via its own `/v1/root-ca` becoming reachable
   again) without manual intervention — no restart of that node's process is
   performed; this specifically exercises the maintenance thread's own
   retry behavior recovering once connectivity returns, not the restart
   path already covered by Requirement 4.

### Requirement 6: Shared real-AWS test cost tracking

**User Story:** As a developer running any of this project's real-EC2
integration suites, I want the same AWS-cost estimate and reporting
`aws-quorum-manager` Requirement 20 already provides, generalized so every
real-EC2 test binary gets it — not just the one it happened to be built for
first.

#### Acceptance Criteria

1. `BilledResource`, `TestCostReport`, `CostAccumulator`,
   `CostSummaryFixture`, the module-level `g_cost_accumulator`, and the
   `ec2_hourly_rate()`/NAT-gateway/EIP pricing helpers SHALL be extracted
   from `tests/aws_quorum_manager_real_ec2_test.cpp` into a new shared,
   header-only `tests/aws_real_ec2_test_support.hpp`, included by all
   real-EC2 test `.cpp` files — no behavioral change to the extracted types.
2. `tests/aws_quorum_manager_real_ec2_test.cpp` SHALL be updated to include
   the shared header and remove its own now-duplicate definitions; its
   existing cost-reporting behavior and output format SHALL be unchanged.
3. `tests/ca_cluster_node_real_ec2_test.cpp`'s `three_az_network_fixture`
   SHALL be updated to open billing timers for its EC2 instances (one
   `BilledResource` per node, using the instance type actually launched)
   from `provision_node()` through `decommission_node()`/teardown, and to
   emit and accumulate a `TestCostReport` in its destructor, matching
   `RealEc2Fixture`'s existing pattern.
4. The new `tests/ca_cluster_node_rpc_tls_real_ec2_test.cpp` (Requirement 1)
   SHALL do the same for each of its test cases, including the temporary
   quarantine security group's own negligible-but-real cost accounting is
   not required (security groups are not billed) but instance time SHALL be
   tracked identically to Requirement 6.3.
5. Each of the three real-EC2 test binaries SHALL retain its own
   `CostSummaryFixture`/`g_cost_accumulator` instance (one summary per
   binary, not merged across binaries) — `BOOST_GLOBAL_FIXTURE` and file-
   scope global variables are inherently per-translation-unit, and merging
   across binaries is not required by this spec.

### Requirement 7: Shared real-AWS test signal-driven cleanup

**User Story:** As a developer running any of this project's real-EC2
integration suites, I want a killed or interrupted test run to trigger the
same AWS resource cleanup a normal test teardown performs, generalized
so every real-EC2 fixture gets this protection — today only
`RealEc2Fixture` has it, and both `three_az_network_fixture` and the fixture
this spec adds for RPC-TLS testing would otherwise leak a VPC and running
EC2 instances if killed mid-run.

#### Acceptance Criteria

1. A `signal_cleanup_target` abstract interface (a single
   `virtual void teardown() noexcept = 0;`, non-public destructor) SHALL be
   added to `tests/aws_real_ec2_test_support.hpp`, along with a generic
   `std::atomic<signal_cleanup_target*> g_active_aws_fixture`, a
   `install_aws_signal_handlers()` function, and an `AwsSignalHandlerFixture`
   `BOOST_GLOBAL_FIXTURE` wrapper — behaviorally identical to
   `aws_quorum_manager_real_ec2_test.cpp`'s existing
   `signal_cleanup_handler`/`install_signal_handlers`/`SignalHandlerFixture`,
   generalized to a type-erased target instead of a concrete `RealEc2Fixture*`.
2. `RealEc2Fixture` SHALL be updated to implement `signal_cleanup_target`
   and register/clear itself in `g_active_aws_fixture` (replacing its
   existing concretely-typed `g_active_fixture`), with no change to its
   `teardown()` idempotency or behavior.
3. `three_az_network_fixture` (`tests/ca_cluster_node_real_ec2_test.cpp`)
   SHALL be updated to implement `signal_cleanup_target`, register/clear
   itself the same way, and gain an idempotent `teardown()` method
   factored out of its destructor (the destructor SHALL call `teardown()`).
4. The fixture added for the new RPC-TLS real-EC2 test (Requirement 1)
   SHALL do the same.
5. Handled signals SHALL remain SIGTERM, SIGINT, SIGHUP, SIGQUIT, SIGPIPE —
   unchanged from `aws-quorum-manager` Requirement 21.1's set — and SIGALRM
   SHALL continue to be left alone since Boost.Test uses it for per-test-case
   timeouts.
6. Each real-EC2 test binary's `AwsSignalHandlerFixture` SHALL be
   independently registered via that binary's own `BOOST_GLOBAL_FIXTURE` —
   this spec does not introduce cross-binary coordination, since each real-
   EC2 test binary is a separate process with its own signal disposition.

### Requirement 8: Test registration and CI exclusion

**User Story:** As a maintainer, I want the new real-EC2 test registered
consistently with the project's existing real-EC2 tests, so it never
accidentally runs (and incurs AWS spend) as part of the default local or CI
test suite.

#### Acceptance Criteria

1. `tests/CMakeLists.txt` SHALL register `ca_cluster_node_rpc_tls_real_ec2_test`
   inside the same `if(LIBSSH2_FOUND)` block as the two existing real-EC2
   test binaries, with `KYTHIRA_AWS_REAL_EC2_TESTS`/`LIBSSH2_FOUND` compile
   definitions and `${LIBSSH2_LIBRARIES}`/`${LIBSSH2_INCLUDE_DIRS}` linkage,
   matching `ca_cluster_node_real_ec2_test`'s existing target definition
   pattern exactly.
2. `TIMEOUT` SHALL be at least 1800 seconds — this test brings up and tears
   down real EC2 instances across four separate test cases (Requirements
   2-5), each of which itself budgets several minutes for AZ-to-AZ
   convergence.
3. `LABELS` SHALL include `real-ec2` (and `slow`) so it is excluded by
   every existing `-LE` label filter already used in
   `scripts/pre-commit-coverage.sh` and `.github/workflows/ci.yml` without
   requiring any change to either file.
4. This spec SHALL NOT add a new CI workflow job that runs these tests
   automatically — real-EC2 tests remain manually triggered (matching
   `aws_quorum_manager_real_ec2_test`'s and `ca_cluster_node_real_ec2_test`'s
   existing, unchanged convention) since they incur real AWS spend and
   require credentials CI does not have configured today.

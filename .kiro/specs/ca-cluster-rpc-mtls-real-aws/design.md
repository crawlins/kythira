# CA Cluster RPC mTLS — Real AWS Validation Design Document

## Overview

This spec adds one new real-EC2 test binary
(`tests/ca_cluster_node_rpc_tls_real_ec2_test.cpp`) covering the
`ca-cluster-rpc-mtls` feature's properties against three real, separate EC2
instances, and extracts the cost-tracking and signal-cleanup apparatus
`aws_quorum_manager_real_ec2_test.cpp` already has into a shared header so
`ca_cluster_node_real_ec2_test.cpp` (which has neither today) and the new
test both get it.

Nothing in `include/raft/` changes. This is entirely test infrastructure:
one new `.cpp` test file, one new shared test-support header, and small,
behavior-preserving refactors to two existing test files to route through
that shared header instead of their own local copies.

## Architecture

```
tests/aws_real_ec2_test_support.hpp   (NEW — header-only, no .cpp)
  ├── BilledResource / TestCostReport / CostAccumulator / CostSummaryFixture
  ├── ec2_hourly_rate() / kNatGwHourly / kEipHourly
  └── signal_cleanup_target / g_active_aws_fixture /
      install_aws_signal_handlers() / AwsSignalHandlerFixture

tests/aws_quorum_manager_real_ec2_test.cpp   (MODIFIED)
  └── #include "aws_real_ec2_test_support.hpp"; RealEc2Fixture implements
      signal_cleanup_target; local BilledResource et al. definitions removed

tests/ca_cluster_node_real_ec2_test.cpp   (MODIFIED)
  └── #include "aws_real_ec2_test_support.hpp"; three_az_network_fixture
      gains cost tracking + signal_cleanup_target + idempotent teardown()

tests/ca_cluster_node_rpc_tls_real_ec2_test.cpp   (NEW)
  └── rpc_tls_three_az_network_fixture (extends three_az_network_fixture's
      shape: same VPC/subnet-per-AZ/SG/key-pair setup, plus bootstrap-
      credential generation and RPC-TLS-aware user-data/start-command
      helpers), 4 test cases (Requirements 2-5)
```

The new fixture is **not** implemented as a C++ base class of
`three_az_network_fixture`, even though the two share most of their setup.
`three_az_network_fixture` today is a private, anonymous-namespace type in
`ca_cluster_node_real_ec2_test.cpp` — promoting it to a shared, inheritable
type is more invasive than this spec needs, and the RPC-TLS variant's
differences (bootstrap-credential generation, per-node quarantine-SG
reassignment for Requirement 5, a `stop_node_process()`/`restart_node()`
pair for Requirement 4 that the base fixture has no reason to ever need)
are substantial enough that duplicating the ~150-line VPC/subnet/SG/key-pair
constructor is the lower-risk choice — it is copy-and-adapt, not a rewrite,
and it means a change to one fixture's real-AWS resource-provisioning shape
can never accidentally change the other's. If a third real-EC2 test needs
this exact same VPC/subnet-per-AZ shape in the future, extracting a shared
base at that point (three call sites, not two) is the point to reconsider
this trade-off — not before.

## Components and Interfaces

### 1. `tests/aws_real_ec2_test_support.hpp`

```cpp
#pragma once

// Shared real-EC2 integration test infrastructure: AWS cost estimation/
// reporting (originally aws_quorum_manager_real_ec2_test.cpp's Requirement
// 20 implementation) and signal-driven cleanup (originally that same
// file's Requirement 21 implementation), generalized so every real-EC2
// test binary in this project gets both, not just the first one that
// needed them.

#include <atomic>
#include <chrono>
#include <csignal>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

namespace kythira::testing::aws_real_ec2 {

// ── Cost estimation (Requirement 6 / aws-quorum-manager Requirement 20) ────

inline auto ec2_hourly_rate(const std::string& type) -> double {
    // ... identical table to today's aws_quorum_manager_real_ec2_test.cpp
    // ec2_hourly_rate() — moved verbatim, not reproduced here.
}

constexpr double kNatGwHourly = 0.045;
constexpr double kEipHourly = 0.005;

struct BilledResource { /* moved verbatim */ };
struct TestCostReport { /* moved verbatim */ };
struct CostAccumulator { /* moved verbatim */ };

// One CostAccumulator + CostSummaryFixture PER INCLUDING TRANSLATION UNIT is
// the intent (Requirement 6.5) — achieved by declaring these `inline`, not
// `static`, so ODR gives each real-EC2 test *binary* exactly one definition
// even though each is a separate executable including this same header (a
// header-only, multi-binary library, not a single shared translation unit).
inline CostAccumulator g_cost_accumulator;

struct CostSummaryFixture {
    ~CostSummaryFixture() { /* moved verbatim, reads g_cost_accumulator */ }
};

// ── Signal-driven cleanup (Requirement 7 / aws-quorum-manager Requirement 21) ─

// Any real-EC2 fixture that allocates AWS resources implements this so a
// trappable signal mid-run can still tear them down. Non-public destructor:
// this interface is never used to `delete` through a base pointer, only to
// call teardown() before the concrete fixture's own destructor runs
// normally (or, on the signal path, instead of it running at all — the
// process re-raises the signal and exits before the destructor would fire).
struct signal_cleanup_target {
    virtual void teardown() noexcept = 0;
protected:
    ~signal_cleanup_target() = default;
};

inline std::atomic<signal_cleanup_target*> g_active_aws_fixture{nullptr};

inline void aws_signal_cleanup_handler(int sig) {
    auto* f = g_active_aws_fixture.exchange(nullptr, std::memory_order_acq_rel);
    if (f != nullptr) {
        f->teardown();
    }
    struct sigaction sa{};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, nullptr);
    raise(sig);
}

inline void install_aws_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = aws_signal_cleanup_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    for (int sig : {SIGTERM, SIGINT, SIGHUP, SIGQUIT, SIGPIPE}) {
        sigaction(sig, &sa, nullptr);
    }
}

struct AwsSignalHandlerFixture {
    AwsSignalHandlerFixture() { install_aws_signal_handlers(); }
};

}  // namespace kythira::testing::aws_real_ec2
```

Each including `.cpp` still declares its own
`BOOST_GLOBAL_FIXTURE(kythira::testing::aws_real_ec2::CostSummaryFixture)`
and `BOOST_GLOBAL_FIXTURE(kythira::testing::aws_real_ec2::AwsSignalHandlerFixture)`
— Boost.Test global fixtures are registered per translation unit, so this is
required regardless of where the types live, and keeps each binary's
registration explicit and visible at its own call site rather than implicit
from an include.

### 2. `RealEc2Fixture` changes (`aws_quorum_manager_real_ec2_test.cpp`)

```cpp
struct RealEc2Fixture : kythira::testing::aws_real_ec2::signal_cleanup_target {
    // ... existing members unchanged ...

    void teardown() noexcept override {
        // existing teardown() body, unchanged, except the global-pointer
        // clear at its top now targets g_active_aws_fixture instead of the
        // old concretely-typed g_active_fixture (deleted by this spec).
    }
};
```

`RealEc2Fixture`'s own constructor already does
`g_active_fixture.store(this, ...)`; this becomes
`kythira::testing::aws_real_ec2::g_active_aws_fixture.store(this, ...)`
(implicit upcast to `signal_cleanup_target*`), and its destructor's existing
`g_active_fixture.store(nullptr, ...)` becomes the same against the shared
pointer. No other behavior change.

### 3. `three_az_network_fixture` changes (`ca_cluster_node_real_ec2_test.cpp`)

```cpp
struct three_az_network_fixture : kythira::testing::aws_real_ec2::signal_cleanup_target {
    // ... existing members unchanged ...
    kythira::testing::aws_real_ec2::TestCostReport cost_report{
        boost::unit_test::framework::current_test_case().p_name};
    bool torn_down_ = false;

    three_az_network_fixture() {
        // existing body unchanged, plus, at the very top (before any AWS
        // call, matching aws-quorum-manager Requirement 21.2):
        kythira::testing::aws_real_ec2::g_active_aws_fixture.store(
            this, std::memory_order_release);
    }

    // Existing destructor's body moves here verbatim; destructor becomes
    // `~three_az_network_fixture() override { teardown(); }`.
    void teardown() noexcept override {
        if (torn_down_) return;
        torn_down_ = true;
        kythira::testing::aws_real_ec2::g_active_aws_fixture.store(
            nullptr, std::memory_order_release);
        // existing DeleteKeyPair/DeleteSecurityGroup/DeleteSubnet/
        // DeleteRouteTable/DetachInternetGateway+DeleteInternetGateway/
        // DeleteVpc calls, unchanged;
        for (auto& r : cost_report.resources) r.finalize();
        BOOST_TEST_MESSAGE(cost_report.format());
        kythira::testing::aws_real_ec2::g_cost_accumulator.add(
            std::move(cost_report));
    }

    // New: called once per node right after RunInstances succeeds for it —
    // mirrors RealEc2Fixture::track_instances(), but per-instance since this
    // fixture provisions nodes one at a time via aws_ec2_quorum_manager
    // rather than in one batch RunInstances call.
    void track_instance(const std::string& label, const std::string& instance_type) {
        cost_report.resources.push_back({label, ec2_hourly_rate(instance_type)});
    }
};
```

The existing test case (`three_real_ec2_nodes_form_working_ca_cluster`)
gains one line per provisioned node calling `track_instance(...)` right
after `provision_node()` — no other change to that test case.

### 4. `rpc_tls_three_az_network_fixture` (new, `ca_cluster_node_rpc_tls_real_ec2_test.cpp`)

Structurally a sibling of `three_az_network_fixture`, not a subclass (see
Architecture's rationale). Same VPC/subnet-per-AZ/security-group/key-pair
constructor body, plus:

```cpp
struct rpc_tls_three_az_network_fixture
    : kythira::testing::aws_real_ec2::signal_cleanup_target {
    // ... VPC/subnet/SG/key-pair setup identical to
    //     three_az_network_fixture's constructor ...

    raft::testing::certificate_authority bootstrap_cred;  // Requirement 1.2
    std::string quarantine_sg_id;  // created lazily by the Requirement 5 test only

    // Requirement 1.3: writes both the unseal key AND the bootstrap
    // credential's cert/key to disk via user-data, before ca_cluster_node
    // ever starts.
    auto make_rpc_tls_user_data() const -> std::string;

    // Requirement 1.4: extends start_node_command()'s shape with
    // --rpc-tls-cert/--rpc-tls-key. `use_rpc_tls_flags` is false for the
    // Requirement 4 restart-without-bootstrap-credential test case's second
    // launch of the target node.
    auto start_node_command(std::uint64_t node_id, const std::string& peers_arg,
                            bool bootstrap, bool use_rpc_tls_flags) const -> std::string;

    // Requirement 4.2: stop just the process, not the instance.
    void stop_node_process(const std::string& public_ip);

    // Requirement 5.2/5.4: create-once, reused across whichever test case
    // needs it (only Requirement 5's test case does, in this spec).
    auto ensure_quarantine_sg() -> std::string;

    // Requirement 5.2/5.4: reassign/restore a node's ENI's security group.
    void set_instance_security_group(const std::string& instance_id,
                                     const std::string& sg_id);

    void teardown() noexcept override { /* same shape as
        three_az_network_fixture::teardown(), plus quarantine_sg_id cleanup
        if non-empty */ }
};
```

`bootstrap_cred` is a fixture member (constructed once, in the fixture
constructor) rather than a per-test-case local, since three of this spec's
four test cases (Requirements 2-4; Requirement 5 also needs a running
cluster but doesn't touch the credential itself after startup) all need the
identical PEM material for user-data — constructing it once per fixture
instance and reusing it across a `BOOST_FIXTURE_TEST_CASE`'s single test
case body is sufficient; Boost.Test constructs a fresh fixture per test
case, so there is no cross-test-case credential reuse to worry about (each
test case's cluster gets its own freshly generated bootstrap credential,
which is correct — Requirement 2.4's byte-identical constraint is *within*
one cluster, not across the whole test binary).

### `make_rpc_tls_user_data()`

Extends `tests/ca_cluster_node_real_ec2_test.cpp`'s existing
`make_user_data()` free function (which only writes the unseal key) with two
more `printf`/heredoc lines writing
`/etc/ca_cluster_node/rpc_bootstrap.crt`/`.key` from
`bootstrap_cred.root_certificate_pem()`/
`detail_testing::unsafe_extract_ca_private_key_pem(bootstrap_cred)`, `chmod
600` on both, matching the file-layout convention
`docker/ca_cluster_node/ca_cluster_node.service` already documents for a
real deployment's bootstrap credential.

## Correctness Properties

### Property 1: This spec changes no production code

Every file this spec adds or modifies lives under `tests/`. `include/raft/`,
`cmd/ca_cluster_node/`, and `docker/ca_cluster_node/` are all read-only
references — verified by `git diff --stat` over this spec's implementation
showing only `tests/*` paths touched.

### Property 2: Cost/signal extraction is behavior-preserving

`aws_quorum_manager_real_ec2_test.cpp`'s cost-report output format and
signal-cleanup behavior are byte-for-byte unchanged by the extraction
(Requirement 6.2, 7.2) — verified by running that binary's existing test
suite before and after the refactor and diffing `BOOST_TEST_MESSAGE` output
for the cost summary (module names/values will differ run-to-run since
they're real timings/prices, but the *format* — column widths, section
dividers, disclaimer text — must be identical).

### Property 3: A killed test run leaks no AWS resources

For each of the three real-EC2 fixtures (`RealEc2Fixture`,
`three_az_network_fixture`, `rpc_tls_three_az_network_fixture`), sending
SIGTERM to the test process mid-run SHALL result in the same set of
`Delete*`/`Terminate*` AWS API calls that a normal (non-signaled) test
completion's teardown makes — verified manually (not by an automated test,
since reliably synchronizing "signal arrives exactly mid-provisioning" is
itself a source of flakiness disproportionate to the value of automating
it) by starting a real-EC2 test case, sending SIGTERM partway through
`Requirement 2`'s test case, and confirming via `DescribeVpcs`/
`DescribeInstances` that nothing from that test run's `cluster_name`/VPC CIDR
remains within a few minutes.

### Property 4: Cutover survives real inter-AZ network conditions

Requirement 2's core assertion (certificate issuance succeeds after the
bootstrap credential is deleted) is the same property
`ca_cluster_node_rpc_tls_test.cpp` already proves locally — this spec does
not claim to prove a *different* property, only that the *same* property
holds when the network between peers is a real AWS VPC across three AZs
rather than loopback. A failure here without a corresponding local-test
failure indicates an environment-specific issue (timing margins, security
group rules, AZ-pair latency) rather than a logic bug in
`ca_cluster_node`/`tls_tcp_rpc.hpp` — the local tests remain the
first place to look for a genuine correctness regression; this spec's tests
are a second, environment-specific line of defense, added specifically
because `ca-cluster-rpc-mtls`'s own CI deadlock proved that line of defense
has already once caught something loopback testing could not.

## Error Handling

Test cases follow the existing real-EC2 test convention throughout this
project: `BOOST_REQUIRE`/`BOOST_REQUIRE_MESSAGE` for setup and
load-bearing assertions (a failure here aborts the test case, and the
fixture's destructor/`teardown()` still runs and cleans up AWS resources
regardless of *why* the test case ended). SSH command execution
(`ssh_execute`, reused unmodified from `ca_cluster_node_real_ec2_test.cpp`)
already retries the *connection* (not the command) since a freshly-launched
instance's sshd may not be immediately ready; this spec does not change that
helper.

`ensure_quarantine_sg()` and `set_instance_security_group()` (Requirement 5)
use plain `BOOST_REQUIRE(...IsSuccess())` on their AWS SDK calls — no retry
logic is added, since `CreateSecurityGroup`/`ModifyNetworkInterfaceAttribute`
do not have the eventual-consistency read-after-write gap that (for
example) newly-created IAM roles do elsewhere in this project's AWS test
code; a failure here is a real error, not a transient one worth retrying.

## Testing Strategy

This entire spec *is* a testing-strategy addition — there is no separate
"tests for the tests" layer beyond the properties above, which are verified
manually/by-inspection rather than automated, for the reasons Property 3
explains (signal-timing races are themselves flaky to automate) and
Property 2 explains (output-format diffing is a one-time manual check at
implementation time, not an ongoing automated gate — the existing
`aws_quorum_manager_real_ec2_test.cpp` test cases already exercise the cost-
report code path on every real-EC2 run; this spec doesn't need a second,
redundant check for the exact same code after it moves to a new file).

The four new `BOOST_FIXTURE_TEST_CASE`s (Requirements 2-5) are themselves
the deliverable, matching the "Real-AWS integration test" category already
established by `certificate-authority`/`aws-quorum-manager`'s own
requirements documents — see this spec's `requirements.md` Requirement 8 for
their registration/labeling/exclusion rules.

## Non-Goals

- **Certificate renewal on real EC2** (`ca-cluster-rpc-mtls` Requirement
  7.2/7.3) is explicitly out of scope. It is timing-sensitive (needs a
  certificate deliberately issued close to expiry), already thoroughly
  covered by `tests/ca_cluster_node_rpc_tls_restart_test.cpp` locally, and
  the incremental confidence a real-EC2 run would add is not proportional
  to the real AWS spend and wall-clock time an artificially-shortened-
  validity-window test would require. May be revisited as a follow-up if
  a renewal-specific bug is ever found that loopback testing alone cannot
  reproduce — mirroring exactly why this spec exists for cutover in the
  first place.
- **Multi-region or cross-region deployment** is out of scope —
  `ca_cluster_node`'s existing design (and `certificate-authority` spec
  Requirement 17.12) is single-region, 3-AZ; this spec tests that shape,
  not a hypothetical multi-region one.
- **ECS/Fargate deployment verification** (`docker/ca_cluster_node/
  ecs-task-definitions/`) is out of scope. That path is Cloud Map/EFS/
  Secrets-Manager-based and structurally different from the EC2+SSH harness
  this spec extends; validating it would need a materially different test
  harness (likely driving the AWS CLI/SDK against ECS APIs and reaching
  tasks via Cloud Map DNS rather than SSH+public IP), which is large enough
  to be its own spec if ever undertaken.
- **Automating Property 3 (signal-driven cleanup verification)** into a
  CI-run test case is out of scope, per this design's own Property 3
  discussion — it remains a documented manual verification step.

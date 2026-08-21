// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file oci_quorum_manager_mock_test.cpp
/// @brief `oci_instance_pool_quorum_manager` driven end to end against
///        `oci_mock_server` (`.kiro/specs/oci-cloud-provider/`,
///        Requirements 13.5-13.7).
///
/// This is the tier AWS gets from LocalStack and OCI has no vendor equivalent
/// for. What it buys over the unit file is *sequences*: provisioning is four
/// calls with a poll in the middle, decommission is a lookup then a detach then
/// a consistency poll, and `maintain_quorum` composes both on top of an
/// assessment. Each of those has failure modes that no single-call test can
/// reach.
///
/// One server per case, torn down at the end of it. Shared state across cases
/// would make the pool's size and the assigned node ids depend on test
/// execution order, which is the sort of coupling that turns one real failure
/// into five.

#define BOOST_TEST_MODULE oci_quorum_manager_mock_test
#include <boost/test/unit_test.hpp>

#include <raft/oci_instance_pool_quorum_manager.hpp>

#include "oci_mock_server.hpp"
#include "test_timeout_scale.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace {

using kythira::node_placement;
using kythira::oci_instance_pool_quorum_manager;
using kythira::oci_instance_pool_quorum_manager_config;
using kythira::quorum_status;

constexpr std::uint16_t test_port = 18322;

auto generate_rsa_key_pem() -> std::string {
    EVP_PKEY* raw = nullptr;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    EVP_PKEY_keygen_init(ctx);
    EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048);
    EVP_PKEY_keygen(ctx, &raw);
    EVP_PKEY_CTX_free(ctx);
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(bio, raw, nullptr, nullptr, 0, nullptr, nullptr);
    char* data = nullptr;
    const long len = BIO_get_mem_data(bio, &data);  // NOLINT(google-runtime-int) — OpenSSL's type
    std::string pem(data, static_cast<std::size_t>(len));
    BIO_free_all(bio);
    EVP_PKEY_free(raw);
    return pem;
}

auto shared_key_pem() -> const std::string& {
    static const std::string pem = generate_rsa_key_pem();
    return pem;
}

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = boost::unit_test::framework::master_test_suite().argc;
        char** argv = boost::unit_test::framework::master_test_suite().argv;
        folly::init(&argc, &argv, false);
    }
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

/// A fresh mock control plane plus a config pointed at it.
struct MockFixture {
    MockFixture() : server(test_port) {
        // Before start(): from here on the server verifies every signature
        // against the key the client uses, so a request whose sent headers
        // disagree with its signed ones fails here rather than at Oracle.
        server.set_signing_key_pem(shared_key_pem());
        server.start();
    }
    ~MockFixture() { server.stop(); }

    MockFixture(const MockFixture&) = delete;
    auto operator=(const MockFixture&) -> MockFixture& = delete;
    MockFixture(MockFixture&&) = delete;
    auto operator=(MockFixture&&) -> MockFixture& = delete;

    [[nodiscard]] auto config(std::size_t target_count = 3) const
        -> oci_instance_pool_quorum_manager_config {
        oci_instance_pool_quorum_manager_config cfg;
        cfg.oci.region = "us-phoenix-1";
        cfg.oci.tenancy_id = "ocid1.tenancy.oc1..aaaa";
        cfg.oci.user_id = "ocid1.user.oc1..bbbb";
        cfg.oci.fingerprint = "aa:bb:cc:dd";
        cfg.oci.private_key_pem = shared_key_pem();
        cfg.oci.endpoint_override = server.origin();
        cfg.oci.api_timeout = std::chrono::seconds{5};
        cfg.compartment_id = "ocid1.compartment.oc1..mock";
        cfg.cluster_name = "mock-cluster";
        cfg.instance_pool_id = server.pool_id();
        cfg.node_port = 7000;
        cfg.poll_interval = std::chrono::milliseconds{50};
        cfg.provision_timeout = std::chrono::seconds{5};
        cfg.topology.groups.push_back(
            {.group_id = server.availability_domain(), .target_count = target_count});
        return cfg;
    }

    /// Seed an already-managed node without going through `provision_node`.
    ///
    /// Used by the assessment and decommission cases so they start from a known
    /// cluster instead of depending on provisioning having worked — a failure in
    /// one operation should not be able to fail the tests for another.
    auto seed_node(std::uint64_t node_id, const std::string& lifecycle_state = "RUNNING")
        -> std::string {
        return server.add_instance(
            {
                {"kythira-cluster", "mock-cluster"},
                {"kythira-node-id", std::to_string(node_id)},
                {"kythira-group", server.availability_domain()},
                {"kythira-managed-by", "kythira-oci-instance-pool-quorum-manager"},
            },
            lifecycle_state);
    }

    kythira::testing::oci_mock_server server;
};

auto cluster_of(const MockFixture& fixture, std::initializer_list<std::uint64_t> ids)
    -> std::vector<node_placement<std::uint64_t, std::string>> {
    std::vector<node_placement<std::uint64_t, std::string>> out;
    for (const auto id : ids) {
        out.push_back({.node_id = id, .group_id = fixture.server.availability_domain()});
    }
    return out;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(oci_quorum_manager_mock_tests)

/// Requirement 13.7's "provision N nodes and verify tags/sequential IDs".
///
/// All three claims are checked, and they are genuinely different: the pool grew
/// once per call (not three times, and not zero), the ids came out 1/2/3 rather
/// than repeating, and every managed instance carries all four well-known tags.
/// A manager that assigned the same id three times would still return three
/// addresses.
BOOST_AUTO_TEST_CASE(oci_provision_three_nodes,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(90))) {
    MockFixture fixture;
    oci_instance_pool_quorum_manager<> mgr{fixture.config()};

    std::vector<std::uint64_t> assigned;
    std::vector<std::string> addresses;
    for (int i = 0; i < 3; ++i) {
        auto peer =
            std::move(mgr.provision_node(fixture.server.availability_domain(), std::nullopt)).get();
        assigned.push_back(peer.node_id);
        addresses.push_back(peer.address);
    }

    BOOST_REQUIRE_EQUAL(assigned.size(), 3U);
    BOOST_CHECK_EQUAL(assigned[0], 1U);
    BOOST_CHECK_EQUAL(assigned[1], 2U);
    BOOST_CHECK_EQUAL(assigned[2], 3U);
    BOOST_CHECK_EQUAL(fixture.server.pool_size(), 3);

    // The address is the VNIC's private IP plus the configured node port —
    // proving the ListVnicAttachments/GetVnic pair ran, not just that a string
    // was assembled.
    for (const auto& address : addresses) {
        BOOST_CHECK_MESSAGE(address.ends_with(":7000"), "unexpected address: " << address);
        BOOST_CHECK_MESSAGE(address.starts_with("10.0.0."), "unexpected address: " << address);
    }
    BOOST_CHECK(addresses[0] != addresses[1]);

    const auto instances = fixture.server.pool_instances();
    BOOST_REQUIRE_EQUAL(instances.size(), 3U);
    for (const auto& inst : instances) {
        BOOST_CHECK_EQUAL(inst.freeform_tags.at("kythira-cluster"), "mock-cluster");
        BOOST_CHECK_EQUAL(inst.freeform_tags.at("kythira-group"),
                          fixture.server.availability_domain());
        BOOST_CHECK_EQUAL(inst.freeform_tags.at("kythira-managed-by"),
                          "kythira-oci-instance-pool-quorum-manager");
        BOOST_CHECK(inst.freeform_tags.contains("kythira-node-id"));
    }
}

/// A newly launched instance is not taggable the instant it appears.
///
/// The regression test for the failure the *first* real `provision_node`
/// against OCI hit. The instance shows up in `ListInstancePoolInstances` while
/// the pool is still building it, and `UpdateInstance` against one in that
/// state is refused: `409 Conflict: instance ... is currently being modified,
/// try again later`. Since tagging is the very next thing `provision_node`
/// does, taking the candidate too early fails the whole provision.
///
/// `set_provisioning_reads(3)` is what makes this expressible at all — the
/// default mock materialises instances already `RUNNING`, which is precisely
/// why 31 green tests had nothing to say about it. What makes the case
/// non-vacuous is that the manager must *wait*: drop the `lifecycle_state ==
/// "RUNNING"` guard from the candidate scan and this fails with that 409.
BOOST_AUTO_TEST_CASE(provisioning_waits_for_the_instance_to_stop_being_modified,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(90))) {
    MockFixture fixture;
    fixture.server.set_provisioning_reads(3);
    oci_instance_pool_quorum_manager<> mgr{fixture.config()};

    auto peer =
        std::move(mgr.provision_node(fixture.server.availability_domain(), std::nullopt)).get();

    BOOST_CHECK_EQUAL(peer.node_id, 1U);
    const auto instances = fixture.server.pool_instances();
    BOOST_REQUIRE_EQUAL(instances.size(), 1U);
    BOOST_CHECK_EQUAL(instances[0].lifecycle_state, "RUNNING");
    BOOST_CHECK_EQUAL(instances[0].freeform_tags.at("kythira-node-id"), "1");
}

/// A decommission that follows a provision must wait for the pool to settle.
///
/// The regression test for the last defect the live lifecycle turned up, and
/// the least exotic of them: `UpdateInstancePool` leaves the pool `SCALING`,
/// and `DetachInstancePoolInstance` is refused for the duration with
/// `409 IncorrectState: instancepool ... Must be in State 'Running'`. So
/// provision-then-decommission — which is not a corner case but precisely what
/// `maintain_quorum` does to replace a node — failed against real OCI.
///
/// `set_scaling_reads(2)` is what makes it expressible; the default mock keeps
/// the pool `RUNNING` forever. Drop `await_pool_running()` from
/// `decommission_node` and this fails with that 409.
BOOST_AUTO_TEST_CASE(decommission_waits_for_the_pool_to_leave_scaling,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(90))) {
    MockFixture fixture;
    fixture.server.set_scaling_reads(2);
    oci_instance_pool_quorum_manager<> mgr{fixture.config()};

    auto peer =
        std::move(mgr.provision_node(fixture.server.availability_domain(), std::nullopt)).get();
    BOOST_CHECK_EQUAL(peer.node_id, 1U);

    // Straight into a decommission, with the pool still reporting SCALING.
    BOOST_CHECK_NO_THROW(std::move(mgr.decommission_node(peer.node_id)).get());
    BOOST_CHECK(fixture.server.pool_instances().empty());
    BOOST_CHECK_EQUAL(fixture.server.pool_size(), 0);
}

/// Requirement 4.3: `extra_tags` are merged, and cannot displace the four keys
/// the manager owns. Without the second half an operator could point a managed
/// instance at another cluster's node id by setting one config field.
BOOST_AUTO_TEST_CASE(extra_tags_are_merged_but_cannot_override_the_well_known_keys,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    MockFixture fixture;
    auto cfg = fixture.config();
    cfg.extra_tags["owner"] = "platform-team";
    cfg.extra_tags["kythira-node-id"] = "9999";
    oci_instance_pool_quorum_manager<> mgr{cfg};

    auto peer =
        std::move(mgr.provision_node(fixture.server.availability_domain(), std::nullopt)).get();

    const auto instances = fixture.server.pool_instances();
    BOOST_REQUIRE_EQUAL(instances.size(), 1U);
    BOOST_CHECK_EQUAL(instances[0].freeform_tags.at("owner"), "platform-team");
    BOOST_CHECK_EQUAL(instances[0].freeform_tags.at("kythira-node-id"), "1");
    BOOST_CHECK_EQUAL(peer.node_id, 1U);
}

/// Requirement 6.1: a `target_group` the pool has no placement configuration
/// for is rejected as `std::invalid_argument`, and — the part worth asserting —
/// without growing the pool. Growing first and failing after would leave a
/// billed instance nobody is tracking.
BOOST_AUTO_TEST_CASE(provisioning_into_an_unconfigured_availability_domain_is_rejected,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    MockFixture fixture;
    oci_instance_pool_quorum_manager<> mgr{fixture.config()};

    auto fut = mgr.provision_node("kIdk:PHX-AD-9", std::nullopt);
    BOOST_CHECK_THROW(std::move(fut).get(), std::invalid_argument);
    BOOST_CHECK_EQUAL(fixture.server.pool_size(), 0);
}

/// Requirement 6.7: on timeout the size goes back where it was.
///
/// `set_auto_launch(false)` is what makes this reachable — the pool accepts the
/// new size and never produces an instance, which is what a capacity shortfall
/// looks like from the client. Asserting the *restored* size is the whole test:
/// a manager that returned an exceptional Future and left the pool one larger
/// would look identical from the caller's side and would leak an instance on
/// every failed provision.
BOOST_AUTO_TEST_CASE(a_provision_timeout_rolls_the_pool_size_back,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    MockFixture fixture;
    fixture.seed_node(1);
    const auto original = fixture.server.pool_size();
    BOOST_REQUIRE_EQUAL(original, 1);

    fixture.server.set_auto_launch(false);
    auto cfg = fixture.config();
    cfg.provision_timeout = std::chrono::seconds{2};
    cfg.poll_interval = std::chrono::milliseconds{100};
    oci_instance_pool_quorum_manager<> mgr{cfg};

    auto fut = mgr.provision_node(fixture.server.availability_domain(), std::nullopt);
    BOOST_CHECK_THROW(std::move(fut).get(), std::runtime_error);
    BOOST_CHECK_EQUAL(fixture.server.pool_size(), original);
}

/// Requirement 13.7's "detect a stopped/terminated instance as degraded".
///
/// Five nodes rather than three so the expected status is `degraded` rather than
/// `critical` — with three, losing one puts live exactly at the majority
/// threshold, which the shared four-level mapping calls `critical`. Sizing the
/// cluster to make the interesting status observable is the point; asserting
/// "not healthy" would have passed either way.
BOOST_AUTO_TEST_CASE(oci_assess_detects_stopped_node,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    MockFixture fixture;
    std::vector<std::string> ids;
    for (std::uint64_t id = 1; id <= 5; ++id) {
        ids.push_back(fixture.seed_node(id));
    }
    oci_instance_pool_quorum_manager<> mgr{fixture.config(5)};

    auto healthy = std::move(mgr.assess_quorum(cluster_of(fixture, {1, 2, 3, 4, 5}))).get();
    BOOST_CHECK_EQUAL(healthy.status, quorum_status::healthy);
    BOOST_CHECK_EQUAL(healthy.live_node_count, 5U);
    BOOST_CHECK(healthy.unreachable_nodes.empty());

    fixture.server.set_lifecycle_state(ids[2], "STOPPED");

    auto degraded = std::move(mgr.assess_quorum(cluster_of(fixture, {1, 2, 3, 4, 5}))).get();
    BOOST_CHECK_EQUAL(degraded.status, quorum_status::degraded);
    BOOST_CHECK_EQUAL(degraded.live_node_count, 4U);
    BOOST_REQUIRE_EQUAL(degraded.unreachable_nodes.size(), 1U);
    BOOST_CHECK_EQUAL(degraded.unreachable_nodes.front(), 3U);
    BOOST_REQUIRE_EQUAL(degraded.groups.size(), 1U);
    BOOST_CHECK_EQUAL(degraded.groups.front().live_count, 4U);
    BOOST_CHECK_EQUAL(degraded.groups.front().target_count, 5U);
}

/// Requirement 5.3's heartbeat rule, which `lifecycleState` alone cannot express.
///
/// The instance stays `RUNNING` throughout — only the heartbeat tag moves. That
/// is the case the rule exists for: a node whose kythira process has wedged
/// while the VM underneath it is perfectly healthy. A test that stopped the
/// instance would have been green with the heartbeat check deleted.
BOOST_AUTO_TEST_CASE(a_stale_heartbeat_makes_a_running_instance_unreachable,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    MockFixture fixture;
    const auto fresh_id = fixture.seed_node(1);
    const auto stale_id = fixture.seed_node(2);
    const auto now = static_cast<long long>(std::time(nullptr));
    fixture.server.set_freeform_tag(fresh_id, "kythira-last-heartbeat", std::to_string(now));
    fixture.server.set_freeform_tag(stale_id, "kythira-last-heartbeat", std::to_string(now - 600));

    auto cfg = fixture.config(2);
    cfg.heartbeat_timeout = std::chrono::seconds{30};
    oci_instance_pool_quorum_manager<> mgr{cfg};

    auto health = std::move(mgr.assess_quorum(cluster_of(fixture, {1, 2}))).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 1U);
    BOOST_REQUIRE_EQUAL(health.unreachable_nodes.size(), 1U);
    BOOST_CHECK_EQUAL(health.unreachable_nodes.front(), 2U);
    BOOST_CHECK_EQUAL(fixture.server.instance(stale_id)->lifecycle_state, "RUNNING");

    // The control: with the heartbeat rule switched off the same cluster is
    // fully live. Without this the case above would also pass if `is_live`
    // returned false for every instance for some unrelated reason.
    auto legacy_cfg = fixture.config(2);
    legacy_cfg.heartbeat_timeout = std::chrono::seconds{0};
    oci_instance_pool_quorum_manager<> legacy{legacy_cfg};
    auto legacy_health = std::move(legacy.assess_quorum(cluster_of(fixture, {1, 2}))).get();
    BOOST_CHECK_EQUAL(legacy_health.live_node_count, 2U);
}

/// Requirement 13.7's decommission pair, in one case because the second claim
/// is only meaningful immediately after the first.
BOOST_AUTO_TEST_CASE(oci_decommission_all_nodes_and_repeat_is_idempotent,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(90))) {
    MockFixture fixture;
    for (std::uint64_t id = 1; id <= 3; ++id) {
        fixture.seed_node(id);
    }
    oci_instance_pool_quorum_manager<> mgr{fixture.config()};
    BOOST_REQUIRE_EQUAL(fixture.server.pool_size(), 3);

    for (std::uint64_t id = 1; id <= 3; ++id) {
        BOOST_CHECK_NO_THROW(std::move(mgr.decommission_node(id)).get());
    }
    BOOST_CHECK(fixture.server.pool_instances().empty());
    BOOST_CHECK_EQUAL(fixture.server.pool_size(), 0);

    // Requirement 7.2: a second pass finds nothing and must still succeed. The
    // mock answers 409 on a genuine double-detach, so a manager that re-issued
    // the call would fail here rather than quietly passing.
    for (std::uint64_t id = 1; id <= 3; ++id) {
        BOOST_CHECK_NO_THROW(std::move(mgr.decommission_node(id)).get());
    }

    // A node that never existed is the other half of idempotency.
    BOOST_CHECK_NO_THROW(std::move(mgr.decommission_node(std::uint64_t{99})).get());
}

/// Requirement 7.4: an already-terminating instance is left alone.
///
/// Distinguished from "not found" because the code paths differ — this one has
/// the instance in hand and chooses not to detach it. The assertion is the
/// absence of the detach call, since the observable state is the same either way.
BOOST_AUTO_TEST_CASE(decommissioning_an_already_terminating_instance_issues_no_detach,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    MockFixture fixture;
    fixture.seed_node(1, "TERMINATING");
    oci_instance_pool_quorum_manager<> mgr{fixture.config()};

    BOOST_CHECK_NO_THROW(std::move(mgr.decommission_node(std::uint64_t{1})).get());
    BOOST_CHECK_EQUAL(
        fixture.server.hits("POST /20160918/instancePools/{id}/actions/detachInstance"), 0U);
}

/// Requirements 9.1-9.2: assess, remove what is dead, refill the deficit — and
/// return the health from *before* any of that.
///
/// The returned report showing two live nodes while the pool ends with three is
/// the assertion that matters; a `maintain_quorum` that returned post-remediation
/// health would report a healthy cluster and hide the fault that triggered it.
///
/// The reassigned node id is pinned deliberately. Node 3 is decommissioned and
/// the replacement comes back as 3 again, because the id scan can only see
/// instances still in the pool. That is documented on `next_node_id`; this case
/// is what stops it from being rediscovered as a surprise.
BOOST_AUTO_TEST_CASE(maintain_quorum_replaces_a_dead_node_and_reports_pre_remediation_health,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(90))) {
    MockFixture fixture;
    fixture.seed_node(1);
    fixture.seed_node(2);
    const auto dead_id = fixture.seed_node(3);
    fixture.server.set_lifecycle_state(dead_id, "STOPPED");

    oci_instance_pool_quorum_manager<> mgr{fixture.config(3)};
    auto pre = std::move(mgr.maintain_quorum(cluster_of(fixture, {1, 2, 3}))).get();

    BOOST_CHECK_EQUAL(pre.live_node_count, 2U);
    BOOST_CHECK_EQUAL(pre.total_node_count, 3U);
    BOOST_REQUIRE_EQUAL(pre.unreachable_nodes.size(), 1U);
    BOOST_CHECK_EQUAL(pre.unreachable_nodes.front(), 3U);
    BOOST_CHECK_EQUAL(pre.status, quorum_status::critical);

    const auto after = fixture.server.pool_instances();
    BOOST_CHECK_EQUAL(after.size(), 3U);
    std::vector<std::string> node_ids;
    for (const auto& inst : after) {
        const auto tag = inst.freeform_tags.find("kythira-node-id");
        if (tag != inst.freeform_tags.end()) {
            node_ids.push_back(tag->second);
        }
    }
    std::ranges::sort(node_ids);
    const std::vector<std::string> expected{"1", "2", "3"};
    BOOST_CHECK_EQUAL_COLLECTIONS(node_ids.begin(), node_ids.end(), expected.begin(),
                                  expected.end());
}

/// Requirement 5.1: an empty cluster costs no API call at all.
///
/// Asserted as a hit count on the listing route, because "returned healthy" is
/// true whether or not the early exit exists.
BOOST_AUTO_TEST_CASE(assessing_an_empty_cluster_makes_no_api_call,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    MockFixture fixture;
    oci_instance_pool_quorum_manager<> mgr{fixture.config()};
    const auto before = fixture.server.hits("GET /20160918/instancePools/{id}/instances");

    auto health =
        std::move(mgr.assess_quorum(std::vector<node_placement<std::uint64_t, std::string>>{}))
            .get();

    BOOST_CHECK_EQUAL(health.status, quorum_status::healthy);
    BOOST_CHECK_EQUAL(health.total_node_count, 0U);
    BOOST_CHECK_EQUAL(fixture.server.hits("GET /20160918/instancePools/{id}/instances"), before);
}

BOOST_AUTO_TEST_SUITE_END()

// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file alibaba_quorum_manager_mock_test.cpp
/// @brief `alibaba_ess_quorum_manager` driven end to end against
///        `alibaba_mock_server` (`.kiro/specs/alibaba-cloud-services/`,
///        Requirements 16.6-16.7).
///
/// This is the tier AWS gets from LocalStack and Alibaba has no vendor
/// equivalent for. What it buys over `alibaba_quorum_manager_unit_test.cpp` is
/// two things the unit file cannot have.
///
/// **Sequences.** Provisioning is a snapshot, a capacity write, a poll loop and
/// a tag write; decommission is a tag resolve, a removal and a consistency
/// poll; `maintain_quorum` composes both on top of an assessment. Each of those
/// has failure modes no single-call test reaches.
///
/// **Live signature verification.** Every request in every case below is
/// checked by the mock against a canonical request rebuilt from the bytes that
/// arrived — so a `signed-versus-sent` divergence anywhere in
/// `alibaba_http_client` fails here rather than at Alibaba. `signature_failures()
/// == 0` is asserted alongside the functional result in the flow cases, because
/// otherwise a mock that had quietly stopped verifying would look identical to
/// one that was. The `alibaba_quorum_manager_mock_signature` suite closes the
/// other half of that argument by tampering with requests on purpose and
/// requiring the mock to refuse them: a mock that accepts everything is worse
/// than no mock at all, and this suite is what proves this one does not.
///
/// One server per case, torn down at the end of it. Shared state would make
/// assigned node ids depend on test execution order, which is the coupling that
/// turns one real failure into five.

#define BOOST_TEST_MODULE alibaba_quorum_manager_mock_test
#include <boost/test/unit_test.hpp>

#include <raft/alibaba_ess_quorum_manager.hpp>
#include <raft/alibaba_http_client.hpp>

#include "alibaba_mock_server.hpp"
#include "test_timeout_scale.hpp"

#include <httplib.h>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

using kythira::alibaba_ess_quorum_manager;
using kythira::alibaba_ess_quorum_manager_config;
using kythira::node_placement;
using kythira::quorum_status;
using kythira::testing::alibaba_mock_server;
using namespace std::chrono_literals;

namespace {

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

constexpr const char* k_cluster = "mock-cluster";
constexpr const char* k_zone = "cn-hangzhou-b";
constexpr const char* k_other_zone = "cn-hangzhou-h";

/// A started mock plus the config that points a manager at it.
struct MockFixture {
    alibaba_mock_server server{"asg-kythira-mock", k_zone};

    MockFixture() { server.start(); }
    ~MockFixture() { server.stop(); }

    MockFixture(const MockFixture&) = delete;
    auto operator=(const MockFixture&) -> MockFixture& = delete;
    MockFixture(MockFixture&&) = delete;
    auto operator=(MockFixture&&) -> MockFixture& = delete;

    [[nodiscard]] auto config(std::size_t target = 3) const -> alibaba_ess_quorum_manager_config {
        alibaba_ess_quorum_manager_config cfg;
        cfg.alibaba.region = "cn-hangzhou";
        cfg.alibaba.access_key_id = server.access_key_id();
        cfg.alibaba.access_key_secret = server.access_key_secret();
        cfg.alibaba.endpoint_override = server.origin();
        cfg.alibaba.api_timeout = 10s;
        cfg.cluster_name = k_cluster;
        cfg.scaling_group_id = server.scaling_group_id();
        cfg.node_port = 7000;
        cfg.poll_interval = 25ms;
        cfg.provision_timeout = 20s;
        cfg.topology.groups.push_back({.group_id = k_zone, .target_count = target});
        return cfg;
    }

    /// Seed one already-adopted node of this cluster. Returns its instance ID.
    auto seed_node(std::uint64_t node_id, std::string status = "Running") -> std::string {
        return server.add_instance({{"kythira-cluster", k_cluster},
                                    {"kythira-node-id", std::to_string(node_id)},
                                    {"owner", "platform-team"}},
                                   "InService", std::move(status), k_zone);
    }
};

auto cluster_of(const std::vector<std::uint64_t>& ids)
    -> std::vector<node_placement<std::uint64_t, std::string>> {
    std::vector<node_placement<std::uint64_t, std::string>> out;
    out.reserve(ids.size());
    for (const auto id : ids) {
        out.push_back({.node_id = id, .group_id = k_zone});
    }
    return out;
}

/// The `kythira-node-id` tag of every instance the group holds, sorted.
auto node_id_tags(const alibaba_mock_server& server) -> std::vector<std::string> {
    std::vector<std::string> out;
    for (const auto& inst : server.instances()) {
        const auto tag = inst.tags.find("kythira-node-id");
        if (tag != inst.tags.end()) {
            out.push_back(tag->second);
        }
    }
    std::ranges::sort(out);
    return out;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Signature verification is live
// ─────────────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(alibaba_quorum_manager_mock_signature)

/// The baseline: an unsigned request must not be served.
///
/// Without this case, every "flow works" assertion below is compatible with a
/// mock that never looked at the `Authorization` header — which is exactly the
/// state the OCI mock was in while two signed-versus-sent defects shipped past
/// it (see `alibaba_mock_server.hpp`'s header).
BOOST_AUTO_TEST_CASE(an_unsigned_control_plane_request_is_rejected,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    MockFixture fixture;
    httplib::Client client(fixture.server.origin());
    auto result = client.Post("/?ScalingGroupId.1=" + fixture.server.scaling_group_id(),
                              httplib::Headers{{"x-acs-action", "DescribeScalingGroups"},
                                               {"x-acs-version", "2014-08-28"}},
                              std::string{}, "application/json");

    BOOST_REQUIRE(result);
    BOOST_CHECK_EQUAL(result->status, 400);
    BOOST_CHECK(result->body.find("MissingSecurityToken") != std::string::npos);
    BOOST_CHECK_EQUAL(fixture.server.signature_failures(), 1U);
}

/// One flipped hex character in the signature is enough. If this case ever
/// passes with a 200, the mock has stopped verifying and every other case in
/// this file has quietly lost its meaning.
BOOST_AUTO_TEST_CASE(a_tampered_signature_is_rejected,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    MockFixture fixture;
    const kythira::alibaba_http_client http{fixture.config().alibaba};

    const std::map<std::string, std::string> params{
        {"ScalingGroupId.1", fixture.server.scaling_group_id()}};
    auto headers =
        http.prepared_headers(fixture.server.origin(), "DescribeScalingGroups", "2014-08-28",
                              params, {}, std::chrono::system_clock::now(), "nonce000000000000");

    // Sanity: the untampered form of this exact request is accepted, so the
    // rejection below is attributable to the tamper and nothing else.
    httplib::Headers good;
    for (const auto& [name, value] : headers) {
        good.emplace(name, value);
    }
    httplib::Client client(fixture.server.origin());
    auto accepted = client.Post("/?ScalingGroupId.1=" + fixture.server.scaling_group_id(), good,
                                std::string{}, "application/json");
    BOOST_REQUIRE(accepted);
    BOOST_CHECK_EQUAL(accepted->status, 200);
    BOOST_CHECK_EQUAL(fixture.server.signature_failures(), 0U);

    auto& authorization = headers["Authorization"];
    authorization.back() = authorization.back() == 'a' ? 'b' : 'a';
    httplib::Headers bad;
    for (const auto& [name, value] : headers) {
        bad.emplace(name, value);
    }
    auto rejected = client.Post("/?ScalingGroupId.1=" + fixture.server.scaling_group_id(), bad,
                                std::string{}, "application/json");

    BOOST_REQUIRE(rejected);
    BOOST_CHECK_EQUAL(rejected->status, 400);
    BOOST_CHECK(rejected->body.find("SignatureDoesNotMatch") != std::string::npos);
    BOOST_CHECK_EQUAL(fixture.server.signature_failures(), 1U);
    // The rejection echoes the canonical request the *server* built, which is
    // the diagnostic that turns a bare 400 into a five-minute fix.
    BOOST_CHECK(fixture.server.last_rejected_canonical_request().starts_with("POST\n/\n"));
}

/// **The OCI defect, in its ACS3 form**: sign one `Host` and send another.
///
/// The signature is computed over `ess.aliyuncs.com` and the request goes to
/// the mock, which canonicalises the `host` header it actually received. This
/// is the class of bug the whole tier exists for — golden-vector tests cannot
/// see it, because both sides of a golden vector agree by construction.
BOOST_AUTO_TEST_CASE(signing_one_host_and_sending_another_is_rejected,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    MockFixture fixture;
    auto cfg = fixture.config().alibaba;
    // No override: the signature is built for the real ESS endpoint...
    cfg.endpoint_override.clear();
    const kythira::alibaba_http_client signer{cfg};
    auto headers =
        signer.prepared_headers("ess.aliyuncs.com", "DescribeScalingGroups", "2014-08-28", {}, {},
                                std::chrono::system_clock::now(), "nonce000000000000");

    // ...and then the request is sent somewhere else, with the wire `Host`
    // matching where it actually went.
    httplib::Headers wire;
    for (const auto& [name, value] : headers) {
        if (name == "host" || name == "Host") {
            continue;
        }
        wire.emplace(name, value);
    }
    httplib::Client client(fixture.server.origin());
    auto result = client.Post("/", wire, std::string{}, "application/json");

    BOOST_REQUIRE(result);
    BOOST_CHECK_EQUAL(result->status, 400);
    BOOST_CHECK(result->body.find("SignatureDoesNotMatch") != std::string::npos);
    BOOST_CHECK_EQUAL(fixture.server.signature_failures(), 1U);
}

/// A header the request carries but does not declare in `SignedHeaders` is a
/// hole the same shape as the OSS `content-type` one: the recomputation would
/// agree with the client precisely because both ignore the header. The mock
/// derives the required set from what arrived and refuses.
BOOST_AUTO_TEST_CASE(an_undeclared_x_acs_header_is_rejected,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    MockFixture fixture;
    const kythira::alibaba_http_client http{fixture.config().alibaba};
    auto headers =
        http.prepared_headers(fixture.server.origin(), "DescribeScalingGroups", "2014-08-28", {},
                              {}, std::chrono::system_clock::now(), "nonce000000000000");

    httplib::Headers wire;
    for (const auto& [name, value] : headers) {
        wire.emplace(name, value);
    }
    // Signed nothing about it, but it is on the wire and it is an `x-acs-*`
    // header — so ACS3 requires it in the signed set.
    wire.emplace("x-acs-instance-id", "i-smuggled");

    httplib::Client client(fixture.server.origin());
    auto result = client.Post("/", wire, std::string{}, "application/json");

    BOOST_REQUIRE(result);
    BOOST_CHECK_EQUAL(result->status, 400);
    BOOST_CHECK(result->body.find("IncompleteSignature") != std::string::npos);
    BOOST_CHECK(result->body.find("x-acs-instance-id") != std::string::npos);
}

/// A well-formed signature made with the wrong secret is a different bug from a
/// well-formed signature over the wrong bytes, and the mock says so.
BOOST_AUTO_TEST_CASE(an_unknown_access_key_is_named_as_such,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    MockFixture fixture;
    auto cfg = fixture.config().alibaba;
    cfg.access_key_id = "not-the-mocks-key";
    const kythira::alibaba_http_client http{cfg};

    BOOST_CHECK_THROW(
        (void)http.rpc("ess", "DescribeScalingGroups", "2014-08-28", {{"ScalingGroupId.1", "x"}}),
        std::runtime_error);
    BOOST_CHECK_EQUAL(fixture.server.signature_failures(), 1U);
}

BOOST_AUTO_TEST_SUITE_END()

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(alibaba_quorum_manager_mock_construction)

BOOST_AUTO_TEST_CASE(construction_probes_the_group_over_a_signed_request,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    MockFixture fixture;
    const alibaba_ess_quorum_manager<> mgr{fixture.config()};

    BOOST_CHECK_EQUAL(mgr.topology().groups.size(), 1U);
    BOOST_CHECK_EQUAL(fixture.server.count_requests("RPC DescribeScalingGroups"), 1U);
    BOOST_CHECK_EQUAL(fixture.server.signature_failures(), 0U);
}

/// Requirement 3.2's fail-fast: a group the control plane does not have is a
/// configuration error, and the worst moment to find one is the first
/// remediation.
BOOST_AUTO_TEST_CASE(an_absent_scaling_group_fails_construction,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    MockFixture fixture;
    fixture.server.set_group_exists(false);
    BOOST_CHECK_THROW(alibaba_ess_quorum_manager<>{fixture.config()}, std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()

// ─────────────────────────────────────────────────────────────────────────────
// assess_quorum
// ─────────────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(alibaba_quorum_manager_mock_assess)

BOOST_AUTO_TEST_CASE(a_stopped_instance_reads_as_unreachable,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    MockFixture fixture;
    fixture.seed_node(1);
    fixture.seed_node(2);
    fixture.seed_node(3, "Stopped");

    alibaba_ess_quorum_manager<> mgr{fixture.config()};
    auto health = std::move(mgr.assess_quorum(cluster_of({1, 2, 3}))).get();

    BOOST_CHECK_EQUAL(health.live_node_count, 2U);
    BOOST_CHECK_EQUAL(health.total_node_count, 3U);
    BOOST_REQUIRE_EQUAL(health.unreachable_nodes.size(), 1U);
    BOOST_CHECK_EQUAL(health.unreachable_nodes.front(), 3U);
    BOOST_CHECK_EQUAL(health.status, quorum_status::critical);
    BOOST_REQUIRE_EQUAL(health.groups.size(), 1U);
    BOOST_CHECK_EQUAL(health.groups.front().group_id, k_zone);
    BOOST_CHECK_EQUAL(health.groups.front().live_count, 2U);
    BOOST_CHECK_EQUAL(fixture.server.signature_failures(), 0U);
}

/// Requirement 6.2's other half: `InService` is not enough on its own.
BOOST_AUTO_TEST_CASE(an_out_of_service_instance_reads_as_unreachable,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    MockFixture fixture;
    fixture.seed_node(1);
    const auto pending = fixture.seed_node(2);
    fixture.server.set_instance_lifecycle(pending, "Pending");

    alibaba_ess_quorum_manager<> mgr{fixture.config(2)};
    auto health = std::move(mgr.assess_quorum(cluster_of({1, 2}))).get();

    BOOST_CHECK_EQUAL(health.live_node_count, 1U);
    BOOST_REQUIRE_EQUAL(health.unreachable_nodes.size(), 1U);
    BOOST_CHECK_EQUAL(health.unreachable_nodes.front(), 2U);
}

/// Requirement 6.4 / design Property 4: another cluster's instances share the
/// group and are invisible here. Sharing a scaling group is a supported
/// configuration, not a hazard.
BOOST_AUTO_TEST_CASE(a_foreign_clusters_instance_is_not_counted,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    MockFixture fixture;
    fixture.seed_node(1);
    fixture.seed_node(2);
    fixture.seed_node(3);
    fixture.server.add_instance({{"kythira-cluster", "someone-elses"}, {"kythira-node-id", "99"}},
                                "InService", "Running", k_zone);
    // ...and an instance nobody has adopted at all (Requirement 5.4).
    fixture.server.add_instance({{"owner", "platform-team"}}, "InService", "Running", k_zone);

    alibaba_ess_quorum_manager<> mgr{fixture.config()};
    auto health = std::move(mgr.assess_quorum(cluster_of({1, 2, 3}))).get();

    BOOST_CHECK_EQUAL(health.live_node_count, 3U);
    BOOST_CHECK_EQUAL(health.total_node_count, 3U);
    BOOST_CHECK_EQUAL(health.status, quorum_status::healthy);
    // The foreign node id must not raise this cluster's next id either: 99 is
    // visible in the group and must still not become 100.
    BOOST_CHECK_EQUAL(mgr.next_node_id(), 4U);
}

/// Requirement 6.1: `DescribeScalingInstances` pages at 50, so a group larger
/// than one page only reads correctly if the walk continues. Seeded past the
/// boundary so a single-page implementation loses the highest node id.
BOOST_AUTO_TEST_CASE(the_scaling_instance_listing_is_paginated_to_exhaustion,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    MockFixture fixture;
    for (std::uint64_t id = 1; id <= 55; ++id) {
        fixture.seed_node(id);
    }

    alibaba_ess_quorum_manager<> mgr{fixture.config()};
    BOOST_CHECK_EQUAL(mgr.next_node_id(), 56U);
    // Two full pages plus the short third that ends the walk.
    BOOST_CHECK_GE(fixture.server.count_requests("RPC DescribeScalingInstances"), 2U);
    BOOST_CHECK_EQUAL(fixture.server.signature_failures(), 0U);
}

/// An empty cluster is healthy by definition and must cost no API call — a
/// caller polling an empty cluster should not need Alibaba reachable at all.
BOOST_AUTO_TEST_CASE(assessing_an_empty_cluster_makes_no_api_call,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    MockFixture fixture;
    alibaba_ess_quorum_manager<> mgr{fixture.config()};
    fixture.server.clear_request_log();

    auto health =
        std::move(mgr.assess_quorum(std::vector<node_placement<std::uint64_t, std::string>>{}))
            .get();

    BOOST_CHECK_EQUAL(health.status, quorum_status::healthy);
    BOOST_CHECK_EQUAL(health.total_node_count, 0U);
    BOOST_CHECK_EQUAL(fixture.server.request_log().size(), 0U);
}

BOOST_AUTO_TEST_SUITE_END()

// ─────────────────────────────────────────────────────────────────────────────
// provision_node
// ─────────────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(alibaba_quorum_manager_mock_provision)

/// The whole four-step sequence: snapshot, capacity+1, poll for the newcomer,
/// tag it. The address is the assertion that ties the ESS half (membership) to
/// the ECS half (private IP) — the join no single-call test performs.
BOOST_AUTO_TEST_CASE(provisioning_grows_the_group_adopts_the_newcomer_and_returns_its_address,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    MockFixture fixture;
    fixture.seed_node(1);
    fixture.seed_node(2);

    alibaba_ess_quorum_manager<> mgr{fixture.config()};
    auto peer = std::move(mgr.provision_node(k_zone, std::nullopt)).get();

    BOOST_CHECK_EQUAL(peer.node_id, 3U);
    BOOST_CHECK(peer.address.ends_with(":7000"));
    BOOST_CHECK_EQUAL(fixture.server.instance_count(), 3U);
    BOOST_CHECK_EQUAL(fixture.server.desired_capacity(), 3);

    const std::vector<std::string> expected{"1", "2", "3"};
    const auto observed = node_id_tags(fixture.server);
    BOOST_CHECK_EQUAL_COLLECTIONS(observed.begin(), observed.end(), expected.begin(),
                                  expected.end());
    BOOST_CHECK_EQUAL(fixture.server.signature_failures(), 0U);
}

/// Design Property 4 in its positive form: `TagResources` is additive, so the
/// operator's own tag on a launched instance survives adoption. A mock that
/// modelled replacement would let a read-merge-write regression pass.
BOOST_AUTO_TEST_CASE(adoption_preserves_tags_the_manager_did_not_write,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    MockFixture fixture;
    fixture.server.set_launch_tags({{"owner", "platform-team"}, {"cost-center", "infra-42"}});

    alibaba_ess_quorum_manager<> mgr{fixture.config(1)};
    auto peer = std::move(mgr.provision_node(k_zone, std::nullopt)).get();

    const auto instances = fixture.server.instances();
    BOOST_REQUIRE_EQUAL(instances.size(), 1U);
    const auto& tags = instances.front().tags;
    BOOST_CHECK_EQUAL(tags.at("owner"), "platform-team");
    BOOST_CHECK_EQUAL(tags.at("cost-center"), "infra-42");
    BOOST_CHECK_EQUAL(tags.at("kythira-cluster"), k_cluster);
    BOOST_CHECK_EQUAL(tags.at("kythira-node-id"), "1");
    BOOST_CHECK_EQUAL(peer.node_id, 1U);
}

/// Requirement 7.6: `extra_tags` are written, but they cannot redirect a
/// managed instance — the two well-known keys are applied last.
BOOST_AUTO_TEST_CASE(extra_tags_cannot_override_the_well_known_keys,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    MockFixture fixture;
    auto cfg = fixture.config(1);
    cfg.extra_tags = {{"env", "test"}, {"kythira-node-id", "999"}};

    alibaba_ess_quorum_manager<> mgr{cfg};
    (void)std::move(mgr.provision_node(k_zone, std::nullopt)).get();

    const auto instances = fixture.server.instances();
    BOOST_REQUIRE_EQUAL(instances.size(), 1U);
    BOOST_CHECK_EQUAL(instances.front().tags.at("env"), "test");
    BOOST_CHECK_EQUAL(instances.front().tags.at("kythira-node-id"), "1");
}

/// Requirement 7.3: ESS chooses the zone. A placement that disagrees with the
/// requested one proceeds and reports where the capacity actually landed —
/// capacity in the wrong failure domain still beats no capacity.
BOOST_AUTO_TEST_CASE(a_zone_mismatch_proceeds_rather_than_failing,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    MockFixture fixture;
    fixture.server.set_launch_zone(k_other_zone);

    alibaba_ess_quorum_manager<> mgr{fixture.config(1)};
    auto peer = std::move(mgr.provision_node(k_zone, std::nullopt)).get();

    BOOST_CHECK_EQUAL(peer.node_id, 1U);
    const auto instances = fixture.server.instances();
    BOOST_REQUIRE_EQUAL(instances.size(), 1U);
    BOOST_CHECK_EQUAL(instances.front().zone_id, k_other_zone);
    BOOST_CHECK_EQUAL(instances.front().tags.at("kythira-cluster"), k_cluster);
}

/// An instance ESS admits but that never reaches `Running` must not be adopted:
/// tagging a half-built instance is how the OCI sibling learned to wait.
BOOST_AUTO_TEST_CASE(an_instance_that_never_comes_up_times_out_and_rolls_capacity_back,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    MockFixture fixture;
    fixture.seed_node(1);
    auto cfg = fixture.config();
    cfg.provision_timeout = 1s;
    fixture.server.set_launch_status("Starting");

    alibaba_ess_quorum_manager<> mgr{cfg};
    BOOST_CHECK_THROW((void)std::move(mgr.provision_node(k_zone, std::nullopt)).get(),
                      std::runtime_error);

    // Requirement 7.4: the capacity bump is undone, or the group stays one
    // instance larger than the cluster believes forever.
    BOOST_CHECK_EQUAL(fixture.server.desired_capacity(), 1);
    // The unadoptable instance carries no cluster tag, so a later assessment
    // still ignores it.
    const auto instances = fixture.server.instances();
    BOOST_REQUIRE_EQUAL(instances.size(), 2U);
    BOOST_CHECK(!instances.back().tags.contains("kythira-cluster"));
}

/// The other timeout shape: capacity goes up and ESS launches nothing at all,
/// which is what a real capacity shortfall looks like from the client's side.
BOOST_AUTO_TEST_CASE(a_launch_that_never_happens_times_out_and_rolls_capacity_back,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    MockFixture fixture;
    fixture.seed_node(1);
    fixture.server.set_auto_launch(false);
    auto cfg = fixture.config();
    cfg.provision_timeout = 1s;

    alibaba_ess_quorum_manager<> mgr{cfg};
    BOOST_CHECK_THROW((void)std::move(mgr.provision_node(k_zone, std::nullopt)).get(),
                      std::runtime_error);

    BOOST_CHECK_EQUAL(fixture.server.instance_count(), 1U);
    BOOST_CHECK_EQUAL(fixture.server.desired_capacity(), 1);
    BOOST_CHECK_EQUAL(fixture.server.signature_failures(), 0U);
}

/// Requirement 7.1's idempotency argument, made observable: the manager writes
/// an *absolute* DesiredCapacity, so two provisions from a two-node group leave
/// four instances, not five — and the second call's capacity write is 4, not a
/// second "+1 relative to the original".
BOOST_AUTO_TEST_CASE(two_provisions_in_a_row_each_add_exactly_one,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(180))) {
    MockFixture fixture;
    fixture.seed_node(1);
    fixture.seed_node(2);

    alibaba_ess_quorum_manager<> mgr{fixture.config(4)};
    auto first = std::move(mgr.provision_node(k_zone, std::nullopt)).get();
    auto second = std::move(mgr.provision_node(k_zone, std::nullopt)).get();

    BOOST_CHECK_EQUAL(first.node_id, 3U);
    BOOST_CHECK_EQUAL(second.node_id, 4U);
    BOOST_CHECK_EQUAL(fixture.server.instance_count(), 4U);
    BOOST_CHECK_EQUAL(fixture.server.desired_capacity(), 4);
}

/// Requirement 7.5: a replacement gets a *fresh* id, not the retired one —
/// resurrecting an identity the Raft log may still be reasoning about is the
/// failure this rule prevents. `replacing` is diagnostic only.
BOOST_AUTO_TEST_CASE(a_replacement_does_not_inherit_the_retired_node_id,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    MockFixture fixture;
    fixture.seed_node(1);
    fixture.seed_node(2);

    alibaba_ess_quorum_manager<> mgr{fixture.config()};
    auto peer = std::move(mgr.provision_node(k_zone, std::uint64_t{1})).get();

    BOOST_CHECK_EQUAL(peer.node_id, 3U);
    const std::vector<std::string> expected{"1", "2", "3"};
    const auto observed = node_id_tags(fixture.server);
    BOOST_CHECK_EQUAL_COLLECTIONS(observed.begin(), observed.end(), expected.begin(),
                                  expected.end());
}

BOOST_AUTO_TEST_SUITE_END()

// ─────────────────────────────────────────────────────────────────────────────
// decommission_node
// ─────────────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(alibaba_quorum_manager_mock_decommission)

/// Requirement 8.1: `RemoveInstances` terminates *and* shrinks capacity in one
/// call, so there is no window in which ESS helpfully replaces the node that
/// was just retired.
BOOST_AUTO_TEST_CASE(decommission_removes_the_instance_and_shrinks_capacity,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    MockFixture fixture;
    fixture.seed_node(1);
    fixture.seed_node(2);
    fixture.seed_node(3);

    alibaba_ess_quorum_manager<> mgr{fixture.config()};
    std::move(mgr.decommission_node(2)).get();

    BOOST_CHECK_EQUAL(fixture.server.instance_count(), 2U);
    BOOST_CHECK_EQUAL(fixture.server.desired_capacity(), 2);
    const std::vector<std::string> expected{"1", "3"};
    const auto observed = node_id_tags(fixture.server);
    BOOST_CHECK_EQUAL_COLLECTIONS(observed.begin(), observed.end(), expected.begin(),
                                  expected.end());
    BOOST_CHECK_EQUAL(fixture.server.signature_failures(), 0U);
}

/// Requirement 8.3: the second delete of an already-gone node is the common
/// remediation race, not an error — and it must issue no mutating call.
BOOST_AUTO_TEST_CASE(decommissioning_an_absent_node_succeeds_without_mutating,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    MockFixture fixture;
    fixture.seed_node(1);

    alibaba_ess_quorum_manager<> mgr{fixture.config()};
    std::move(mgr.decommission_node(1)).get();
    fixture.server.clear_request_log();

    BOOST_CHECK_NO_THROW(std::move(mgr.decommission_node(1)).get());
    BOOST_CHECK_EQUAL(fixture.server.count_requests("RPC RemoveInstances"), 0U);
    BOOST_CHECK_EQUAL(fixture.server.instance_count(), 0U);
}

/// Requirement 6.4 again, from the mutating side: a NodeId that belongs to a
/// *different* cluster sharing the group resolves to nothing here. The blast
/// radius of every mutating call is bounded to this cluster's instances.
BOOST_AUTO_TEST_CASE(decommission_never_touches_a_foreign_clusters_instance,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    MockFixture fixture;
    fixture.seed_node(1);
    fixture.server.add_instance({{"kythira-cluster", "someone-elses"}, {"kythira-node-id", "7"}},
                                "InService", "Running", k_zone);

    alibaba_ess_quorum_manager<> mgr{fixture.config()};
    std::move(mgr.decommission_node(7)).get();

    BOOST_CHECK_EQUAL(fixture.server.instance_count(), 2U);
    BOOST_CHECK_EQUAL(fixture.server.count_requests("RPC RemoveInstances"), 0U);
}

/// Requirement 8.4: MinSize is ESS's business, not this class's. Removing the
/// last instance is *attempted*, and the group's own refusal surfaces rather
/// than being pre-empted by a local guess at the group's policy — a manager
/// that second-guessed MinSize would also have to keep its guess in sync with
/// an operator's console.
BOOST_AUTO_TEST_CASE(a_min_size_refusal_surfaces_as_a_failure,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    MockFixture fixture;
    fixture.seed_node(1);
    fixture.server.set_min_size(1);

    alibaba_ess_quorum_manager<> mgr{fixture.config()};
    BOOST_CHECK_THROW(std::move(mgr.decommission_node(1)).get(), std::runtime_error);

    // The instance is still there — a failed decommission must not look like a
    // successful one, or the orchestrator loses track of a billed instance.
    BOOST_CHECK_EQUAL(fixture.server.instance_count(), 1U);
    BOOST_CHECK_EQUAL(fixture.server.count_requests("RPC RemoveInstances"), 1U);
}

BOOST_AUTO_TEST_SUITE_END()

// ─────────────────────────────────────────────────────────────────────────────
// maintain_quorum
// ─────────────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(alibaba_quorum_manager_mock_maintain)

/// The composed flow, and the reason this tier exists: assess, decommission the
/// unreachable node, refill the zone's deficit — six control-plane operations
/// in one call, every one of them signature-verified.
///
/// The returned report is the **pre**-remediation health. A `maintain_quorum`
/// that answered with post-remediation health would report a healthy cluster
/// and hide the fault that triggered it.
///
/// The reassigned node id is pinned deliberately: node 3 is removed and the
/// replacement comes back as 3, because the tag scan can only see instances the
/// group still lists. That is documented on `next_node_id`; this case stops it
/// from being rediscovered as a surprise.
BOOST_AUTO_TEST_CASE(maintain_replaces_an_unreachable_node_and_reports_pre_remediation_health,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(180))) {
    MockFixture fixture;
    fixture.seed_node(1);
    fixture.seed_node(2);
    fixture.seed_node(3, "Stopped");

    alibaba_ess_quorum_manager<> mgr{fixture.config()};
    auto pre = std::move(mgr.maintain_quorum(cluster_of({1, 2, 3}))).get();

    BOOST_CHECK_EQUAL(pre.live_node_count, 2U);
    BOOST_CHECK_EQUAL(pre.total_node_count, 3U);
    BOOST_REQUIRE_EQUAL(pre.unreachable_nodes.size(), 1U);
    BOOST_CHECK_EQUAL(pre.unreachable_nodes.front(), 3U);
    BOOST_CHECK_EQUAL(pre.status, quorum_status::critical);

    BOOST_CHECK_EQUAL(fixture.server.instance_count(), 3U);
    BOOST_CHECK_EQUAL(fixture.server.desired_capacity(), 3);
    const std::vector<std::string> expected{"1", "2", "3"};
    const auto observed = node_id_tags(fixture.server);
    BOOST_CHECK_EQUAL_COLLECTIONS(observed.begin(), observed.end(), expected.begin(),
                                  expected.end());
    BOOST_CHECK_EQUAL(fixture.server.signature_failures(), 0U);
}

/// A cluster already at target is left alone: no capacity write, no removal.
/// "Changed nothing" is the assertion, and only the request log can make it.
BOOST_AUTO_TEST_CASE(maintaining_a_healthy_cluster_mutates_nothing,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    MockFixture fixture;
    fixture.seed_node(1);
    fixture.seed_node(2);
    fixture.seed_node(3);

    alibaba_ess_quorum_manager<> mgr{fixture.config()};
    fixture.server.clear_request_log();
    auto health = std::move(mgr.maintain_quorum(cluster_of({1, 2, 3}))).get();

    BOOST_CHECK_EQUAL(health.status, quorum_status::healthy);
    BOOST_CHECK_EQUAL(fixture.server.count_requests("RPC ModifyScalingGroup"), 0U);
    BOOST_CHECK_EQUAL(fixture.server.count_requests("RPC RemoveInstances"), 0U);
    BOOST_CHECK_EQUAL(fixture.server.count_requests("RPC TagResources"), 0U);
    BOOST_CHECK_EQUAL(fixture.server.instance_count(), 3U);
}

/// Requirement 9.1: a shortfall with nothing unreachable still refills. This is
/// the scale-out case rather than the repair case, and it exercises the same
/// provisioning sequence twice in one call.
BOOST_AUTO_TEST_CASE(maintain_fills_a_deficit_with_no_failures_present,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(180))) {
    MockFixture fixture;
    fixture.seed_node(1);

    alibaba_ess_quorum_manager<> mgr{fixture.config()};
    auto pre = std::move(mgr.maintain_quorum(cluster_of({1}))).get();

    BOOST_CHECK_EQUAL(pre.live_node_count, 1U);
    BOOST_CHECK_EQUAL(fixture.server.instance_count(), 3U);
    BOOST_CHECK_EQUAL(fixture.server.desired_capacity(), 3);
    const std::vector<std::string> expected{"1", "2", "3"};
    const auto observed = node_id_tags(fixture.server);
    BOOST_CHECK_EQUAL_COLLECTIONS(observed.begin(), observed.end(), expected.begin(),
                                  expected.end());
}

/// Requirement 9.2: one remediation that will not complete must not stop the
/// rest. With launches disabled, both refills fail and `maintain_quorum` still
/// returns the pre-remediation health rather than propagating.
BOOST_AUTO_TEST_CASE(a_failed_remediation_is_logged_and_stepped_over,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(180))) {
    MockFixture fixture;
    fixture.seed_node(1);
    fixture.server.set_auto_launch(false);
    auto cfg = fixture.config();
    cfg.provision_timeout = 1s;

    alibaba_ess_quorum_manager<> mgr{cfg};
    auto pre = std::move(mgr.maintain_quorum(cluster_of({1}))).get();

    // A one-node cluster with its one node live is `critical`, not `healthy`:
    // live == majority, i.e. one more failure loses quorum. That is a property
    // of the cluster's size, not of this assessment.
    BOOST_CHECK_EQUAL(pre.status, quorum_status::critical);
    BOOST_CHECK_EQUAL(pre.live_node_count, 1U);
    BOOST_CHECK_EQUAL(fixture.server.instance_count(), 1U);
    // Both attempted refills rolled their capacity bumps back.
    BOOST_CHECK_EQUAL(fixture.server.desired_capacity(), 1);
}

/// An *assessment* failure is different from a remediation failure and aborts
/// outright: provisioning against an unknown cluster state is how a transient
/// API error turns into a doubled cluster.
BOOST_AUTO_TEST_CASE(an_assessment_failure_aborts_before_any_remediation,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    MockFixture fixture;
    fixture.seed_node(1);

    alibaba_ess_quorum_manager<> mgr{fixture.config()};
    fixture.server.stop();
    fixture.server.clear_request_log();

    BOOST_CHECK_THROW((void)std::move(mgr.maintain_quorum(cluster_of({1}))).get(),
                      std::runtime_error);
    BOOST_CHECK_EQUAL(fixture.server.count_requests("RPC ModifyScalingGroup"), 0U);
}

BOOST_AUTO_TEST_SUITE_END()

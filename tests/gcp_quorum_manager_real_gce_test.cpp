#define BOOST_TEST_MODULE gcp_quorum_manager_real_gce_test
#include <boost/test/unit_test.hpp>

// Real-GCP integration tests for gcp_compute_quorum_manager and
// gcp_mig_quorum_manager. Guarded by KYTHIRA_GCP_REAL_TESTS=1 and excluded from
// the default ctest run (CTest label `integration;gcp;real-gce`). Skips — never
// fails — when credentials or the required env vars are absent (Requirement 23
// ACs 13-19).
//
// Required env vars: GCP_PROJECT_ID, GCP_REGION. All other resources
// (GCP_TEST_IMAGE, GCP_TEST_NETWORK, GCP_TEST_SUBNET_PREFIX,
// GCP_TEST_SERVICE_ACCOUNT, KYTHIRA_NODE_BINARY, and the MIGs named by
// GCP_TEST_MIG_A/B/C) are operator-supplied or default; see
// doc/gcp_quorum_manager_README.md and
// scripts/ci-cloud-credentials/gcp/README.md.

#if defined(KYTHIRA_HAS_GCP_SDK)

#include <raft/gcp_compute_quorum_manager.hpp>
#include <raft/gcp_mig_quorum_manager.hpp>

#include "gcp_real_gce_test_support.hpp"

#include <google/cloud/compute/instances/v1/instances_client.h>
#include <google/cloud/credentials.h>
#include <google/cloud/options.h>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

using namespace kythira;
using namespace kythira::testing::gcp_real_gce;

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

// Reports the whole suite as skipped (ctest "Not Run", return code 77) when the
// real-GCP prerequisites are absent — the GCP analogue of the AWS suite's
// PreflightSkipFixture. Its first action against GCP is a read-only instances
// list, mirroring the AWS sts:GetCallerIdentity pre-check (Requirement 23 AC 14).
struct PreflightSkipFixture {
    PreflightSkipFixture() {
        if (!real_tests_enabled()) {
            BOOST_TEST_MESSAGE(
                "KYTHIRA_GCP_REAL_TESTS!=1 or GCP_PROJECT_ID/GCP_REGION unset — "
                "skipping real-GCP quorum manager tests");
            std::exit(77);
        }
        const std::string project = env_or("GCP_PROJECT_ID", "");
        const std::string zone = env_or("GCP_REGION", "us-central1") + "-a";
        try {
            google::cloud::Options opts;
            opts.set<google::cloud::UnifiedCredentialsOption>(
                google::cloud::MakeGoogleDefaultCredentials());
            google::cloud::compute_instances_v1::InstancesClient client(
                google::cloud::compute_instances_v1::MakeInstancesConnectionRest(opts));
            google::cloud::cpp::compute::instances::v1::ListInstancesRequest req;
            req.set_project(project);
            req.set_zone(zone);
            for (auto const& maybe : client.ListInstances(req)) {
                if (!maybe) {
                    BOOST_TEST_MESSAGE("GCP pre-flight instances.list failed: " +
                                       maybe.status().message() + " — skipping");
                    std::exit(77);
                }
                break;  // One successful page is enough to confirm access.
            }
        } catch (const std::exception& ex) {
            BOOST_TEST_MESSAGE(std::string("GCP pre-flight failed: ") + ex.what() + " — skipping");
            std::exit(77);
        }
    }
};

BOOST_GLOBAL_FIXTURE(PreflightSkipFixture);
BOOST_TEST_GLOBAL_FIXTURE(CostSummaryFixture);
BOOST_TEST_GLOBAL_FIXTURE(GcpSignalHandlerFixture);

// Unique run id used to label every resource these tests create (Requirement
// 23 AC 16). steady_clock isn't a wall clock, but combined with the PID it is
// unique enough to disambiguate concurrent runs' resources.
auto run_id() -> const std::string& {
    static const std::string id = "run-" + std::to_string(::getpid());
    return id;
}

auto base_compute_config() -> gcp_compute_quorum_manager_config<std::string> {
    gcp_compute_quorum_manager_config<std::string> cfg;
    cfg.gcp.project_id = env_or("GCP_PROJECT_ID", "");
    cfg.cluster_name = "kythira-it";  // valid GCP label
    cfg.boot_disk_image =
        env_or("GCP_TEST_IMAGE", "projects/debian-cloud/global/images/family/debian-12");
    cfg.network = env_or("GCP_TEST_NETWORK", "default");
    cfg.machine_type = "e2-small";
    cfg.node_port = 7000;
    cfg.service_account_email = env_or("GCP_TEST_SERVICE_ACCOUNT", "");
    cfg.extra_labels["kythira-test-run"] = run_id();
    cfg.provision_timeout = std::chrono::seconds(300);
    const std::string region = env_or("GCP_REGION", "us-central1");
    const std::string subnet = env_or("GCP_TEST_SUBNET_PREFIX", "default");
    cfg.subnetwork_by_group[region + "-a"] = subnet;
    return cfg;
}

}  // namespace

// ── gcp_compute_quorum_manager real-GCE cases (Requirement 23 AC 18) ────────

BOOST_AUTO_TEST_SUITE(gcp_compute_real_gce)

BOOST_AUTO_TEST_CASE(provision_and_assess_single_zone) {
    auto cfg = base_compute_config();
    const std::string zone = env_or("GCP_REGION", "us-central1") + "-a";
    cfg.topology.groups.push_back({.group_id = zone, .target_count = 1});
    gcp_compute_quorum_manager<> mgr{cfg};

    auto peer = std::move(mgr.provision_node(zone, std::nullopt)).get();
    BOOST_TEST_MESSAGE("provisioned node " + std::to_string(peer.node_id) + " at " + peer.address);

    std::vector<node_placement<std::uint64_t, std::string>> cluster{
        {.node_id = peer.node_id, .group_id = zone}};
    auto health = std::move(mgr.assess_quorum(cluster)).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 1u);

    std::move(mgr.decommission_node(peer.node_id)).get();
}

BOOST_AUTO_TEST_CASE(provision_multi_zone_topology) {
    auto cfg = base_compute_config();
    const std::string region = env_or("GCP_REGION", "us-central1");
    const std::string subnet = env_or("GCP_TEST_SUBNET_PREFIX", "default");
    std::vector<std::string> zones{region + "-a", region + "-b", region + "-c"};
    for (const auto& z : zones) {
        cfg.subnetwork_by_group[z] = subnet;
        cfg.topology.groups.push_back({.group_id = z, .target_count = 1});
    }
    gcp_compute_quorum_manager<> mgr{cfg};

    std::vector<node_placement<std::uint64_t, std::string>> cluster;
    for (const auto& z : zones) {
        auto peer = std::move(mgr.provision_node(z, std::nullopt)).get();
        cluster.push_back({.node_id = peer.node_id, .group_id = z});
    }
    auto health = std::move(mgr.assess_quorum(cluster)).get();
    BOOST_CHECK_EQUAL(health.live_node_count, zones.size());
    for (const auto& np : cluster) {
        std::move(mgr.decommission_node(np.node_id)).get();
    }
}

BOOST_AUTO_TEST_CASE(stopped_instance_marked_unreachable) {
    auto cfg = base_compute_config();
    const std::string zone = env_or("GCP_REGION", "us-central1") + "-a";
    cfg.topology.groups.push_back({.group_id = zone, .target_count = 1});
    gcp_compute_quorum_manager<> mgr{cfg};

    auto peer = std::move(mgr.provision_node(zone, std::nullopt)).get();
    // Stop the instance directly via the SDK so assess sees a non-RUNNING status.
    google::cloud::Options opts;
    opts.set<google::cloud::UnifiedCredentialsOption>(
        google::cloud::MakeGoogleDefaultCredentials());
    google::cloud::compute_instances_v1::InstancesClient client(
        google::cloud::compute_instances_v1::MakeInstancesConnectionRest(opts));
    const std::string name =
        gcp_compute_quorum_manager<>::node_id_to_instance_name(cfg.cluster_name, peer.node_id);
    client.Stop(cfg.gcp.project_id, zone, name).get();

    std::vector<node_placement<std::uint64_t, std::string>> cluster{
        {.node_id = peer.node_id, .group_id = zone}};
    // Poll a few times for the state transition to STOPPING/TERMINATED.
    bool unreachable = false;
    for (int i = 0; i < 30 && !unreachable; ++i) {
        auto health = std::move(mgr.assess_quorum(cluster)).get();
        unreachable = !health.unreachable_nodes.empty();
        if (!unreachable) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
    BOOST_CHECK(unreachable);
    std::move(mgr.decommission_node(peer.node_id)).get();
}

BOOST_AUTO_TEST_CASE(decommission_idempotent) {
    auto cfg = base_compute_config();
    const std::string zone = env_or("GCP_REGION", "us-central1") + "-a";
    cfg.topology.groups.push_back({.group_id = zone, .target_count = 1});
    gcp_compute_quorum_manager<> mgr{cfg};
    // A node that was never provisioned decommissions to a resolved future.
    BOOST_CHECK_NO_THROW(std::move(mgr.decommission_node(std::uint64_t{123456789})).get());
}

BOOST_AUTO_TEST_CASE(spot_provision_and_decommission) {
    auto cfg = base_compute_config();
    cfg.spot = true;
    const std::string zone = env_or("GCP_REGION", "us-central1") + "-a";
    cfg.topology.groups.push_back({.group_id = zone, .target_count = 1});
    gcp_compute_quorum_manager<> mgr{cfg};

    auto peer = std::move(mgr.provision_node(zone, std::nullopt)).get();
    BOOST_CHECK_NO_THROW(std::move(mgr.decommission_node(peer.node_id)).get());
}

BOOST_AUTO_TEST_CASE(provision_timeout_cleanup) {
    auto cfg = base_compute_config();
    cfg.provision_timeout = std::chrono::seconds(1);  // too short to reach RUNNING
    const std::string zone = env_or("GCP_REGION", "us-central1") + "-a";
    cfg.topology.groups.push_back({.group_id = zone, .target_count = 1});
    gcp_compute_quorum_manager<> mgr{cfg};

    // The future is exceptional, and the partially-created instance is deleted
    // best-effort before it resolves.
    BOOST_CHECK_THROW(std::move(mgr.provision_node(zone, std::nullopt)).get(), std::exception);
}

BOOST_AUTO_TEST_SUITE_END()

// ── gcp_mig_quorum_manager real-GCE cases (Requirement 23 AC 19) ────────────
//
// These require operator-provisioned zonal MIGs (with autohealing unset) named
// by GCP_TEST_MIG_A/B/C; the suite skips individual cases when unset.

BOOST_AUTO_TEST_SUITE(gcp_mig_real_gce)

namespace {
auto base_mig_config() -> gcp_mig_quorum_manager_config<std::string> {
    gcp_mig_quorum_manager_config<std::string> cfg;
    cfg.gcp.project_id = env_or("GCP_PROJECT_ID", "");
    cfg.cluster_name = "kythira-it";
    cfg.node_port = 7000;
    const std::string region = env_or("GCP_REGION", "us-central1");
    const std::string mig_a = env_or("GCP_TEST_MIG_A", "");
    if (!mig_a.empty()) {
        cfg.mig_by_group[region + "-a"] = mig_a;
        cfg.topology.groups.push_back({.group_id = region + "-a", .target_count = 1});
    }
    return cfg;
}
}  // namespace

BOOST_AUTO_TEST_CASE(mig_provision_increments_target_size) {
    if (env_or("GCP_TEST_MIG_A", "").empty()) {
        BOOST_TEST_MESSAGE("GCP_TEST_MIG_A unset — skipping MIG provision test");
        return;
    }
    gcp_mig_quorum_manager<> mgr{base_mig_config()};
    const std::string zone = env_or("GCP_REGION", "us-central1") + "-a";
    auto peer = std::move(mgr.provision_node(zone, std::nullopt)).get();
    std::vector<node_placement<std::uint64_t, std::string>> cluster{
        {.node_id = peer.node_id, .group_id = zone}};
    auto health = std::move(mgr.assess_quorum(cluster)).get();
    BOOST_CHECK_GE(health.live_node_count, 1u);
    std::move(mgr.decommission_node(peer.node_id)).get();
}

BOOST_AUTO_TEST_CASE(mig_assess_detects_non_running_instance) {
    if (env_or("GCP_TEST_MIG_A", "").empty()) {
        BOOST_TEST_MESSAGE("GCP_TEST_MIG_A unset — skipping");
        return;
    }
    gcp_mig_quorum_manager<> mgr{base_mig_config()};
    // A node id that the MIG never labelled is reported unreachable.
    const std::string zone = env_or("GCP_REGION", "us-central1") + "-a";
    std::vector<node_placement<std::uint64_t, std::string>> cluster{
        {.node_id = std::uint64_t{999999999}, .group_id = zone}};
    auto health = std::move(mgr.assess_quorum(cluster)).get();
    BOOST_CHECK_EQUAL(health.unreachable_nodes.size(), 1u);
}

BOOST_AUTO_TEST_CASE(mig_decommission_decrements_target_size) {
    if (env_or("GCP_TEST_MIG_A", "").empty()) {
        BOOST_TEST_MESSAGE("GCP_TEST_MIG_A unset — skipping");
        return;
    }
    gcp_mig_quorum_manager<> mgr{base_mig_config()};
    const std::string zone = env_or("GCP_REGION", "us-central1") + "-a";
    auto peer = std::move(mgr.provision_node(zone, std::nullopt)).get();
    BOOST_CHECK_NO_THROW(std::move(mgr.decommission_node(peer.node_id)).get());
}

BOOST_AUTO_TEST_CASE(mig_decommission_idempotent) {
    if (env_or("GCP_TEST_MIG_A", "").empty()) {
        BOOST_TEST_MESSAGE("GCP_TEST_MIG_A unset — skipping");
        return;
    }
    gcp_mig_quorum_manager<> mgr{base_mig_config()};
    BOOST_CHECK_NO_THROW(std::move(mgr.decommission_node(std::uint64_t{888888888})).get());
}

BOOST_AUTO_TEST_CASE(mig_construction_rejects_autohealing_policy) {
    const std::string mig = env_or("GCP_TEST_MIG_AUTOHEAL", "");
    if (mig.empty()) {
        BOOST_TEST_MESSAGE(
            "GCP_TEST_MIG_AUTOHEAL unset — skipping (needs a MIG deliberately "
            "configured with an autohealing policy)");
        return;
    }
    gcp_mig_quorum_manager_config<std::string> cfg;
    cfg.gcp.project_id = env_or("GCP_PROJECT_ID", "");
    cfg.cluster_name = "kythira-it";
    cfg.node_port = 7000;
    cfg.mig_by_group[env_or("GCP_REGION", "us-central1") + "-a"] = mig;
    BOOST_CHECK_THROW((gcp_mig_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

#else  // !KYTHIRA_HAS_GCP_SDK

BOOST_AUTO_TEST_CASE(skipped_no_gcp_sdk) {
    BOOST_TEST_MESSAGE("KYTHIRA_HAS_GCP_SDK not defined — real-GCE tests not built");
}

#endif  // KYTHIRA_HAS_GCP_SDK

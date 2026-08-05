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
#if defined(KYTHIRA_HAS_GCP_MACHINE_TYPES)
#include <google/cloud/compute/machine_types/v1/machine_types_client.h>
#endif
#include <google/cloud/credentials.h>

#include <algorithm>
#include <sstream>
#include <google/cloud/options.h>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <chrono>
#include <cstdlib>
#include <stdexcept>
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

/// Names of the instances in @p zone carrying this suite's cluster label — the
/// same `labels.kythira-cluster` filter `assess_quorum` uses. Read directly via
/// the SDK rather than through the manager so a leak assertion cannot be masked
/// by the manager's own bookkeeping.
auto cluster_instance_names(const std::string& project, const std::string& zone,
                            const std::string& cluster) -> std::vector<std::string> {
    google::cloud::Options opts;
    opts.set<google::cloud::UnifiedCredentialsOption>(
        google::cloud::MakeGoogleDefaultCredentials());
    google::cloud::compute_instances_v1::InstancesClient client(
        google::cloud::compute_instances_v1::MakeInstancesConnectionRest(opts));

    google::cloud::cpp::compute::instances::v1::ListInstancesRequest req;
    req.set_project(project);
    req.set_zone(zone);
    req.set_filter("labels.kythira-cluster = \"" + cluster + "\"");

    std::vector<std::string> names;
    for (auto const& maybe_inst : client.ListInstances(req)) {
        if (!maybe_inst) {
            throw std::runtime_error("gcp instances.list (" + zone +
                                     "): " + maybe_inst.status().message());
        }
        names.push_back(maybe_inst->name());
    }
    return names;
}

/// Candidate zones for the launch ladder, preferred order irrelevant here (the
/// ranking applies the caller's preference). `GCP_TEST_ZONES` overrides;
/// otherwise the region's conventional four suffixes are tried and any that do
/// not exist simply contribute no machine types.
auto candidate_zones() -> std::vector<std::string> {
    const std::string region = env_or("GCP_REGION", "us-central1");
    const std::string configured = env_or("GCP_TEST_ZONES", "");
    std::vector<std::string> zones;
    if (!configured.empty()) {
        std::stringstream ss(configured);
        std::string item;
        while (std::getline(ss, item, ',')) {
            if (!item.empty()) {
                zones.push_back(item);
            }
        }
        return zones;
    }
    for (const auto* suffix : {"-a", "-b", "-c", "-f"}) {
        zones.push_back(region + suffix);
    }
    return zones;
}

/// Machine types actually offered in each candidate zone, via
/// `machineTypes.list`.
///
/// Discovered rather than assumed because substituting a zone that does not
/// offer the requested type would trade a retryable stockout for a fatal
/// "does not exist in zone" error, which escalation deliberately does not
/// retry — turning the fix into a different failure.
///
/// Stated precisely, because it would be easy to overclaim: in us-central1
/// today the eligible set is in fact *uniform* across zones a/b/c/f (16 types,
/// all four zones, checked against `gcloud compute machine-types list`), so
/// discovery changes nothing there right now. It earns its place as insurance
/// rather than as a fix for an observed gap — newer machine families do roll
/// out zone by zone, and this set is uniform only because it is restricted to
/// mature general-purpose families.
///
/// Cached process-wide: this costs one API call per zone and the answer cannot
/// meaningfully change mid-run. Returns empty (explaining itself on stderr) if
/// discovery fails, leaving the caller on its configured default.
auto available_machine_types() -> const std::vector<gcp_machine_type_info>& {
    static const std::vector<gcp_machine_type_info> discovered = [] {
        std::vector<gcp_machine_type_info> out;
#if !defined(KYTHIRA_HAS_GCP_MACHINE_TYPES)
        // Component absent from this build: the ladder degrades to the single
        // configured machine type, which is exactly the pre-escalation
        // behaviour, rather than failing to build.
        std::cerr << "[gcp-spot] compute_machine_types component unavailable; "
                     "falling back to the configured machine type\n";
        return out;
#else
        try {
            gcp_client_config gcp;
            gcp.project_id = env_or("GCP_PROJECT_ID", "");
            auto client = google::cloud::compute_machine_types_v1::MachineTypesClient(
                google::cloud::compute_machine_types_v1::MakeMachineTypesConnectionRest(
                    gcp_compute_detail::make_options(gcp)));
            for (const auto& zone : candidate_zones()) {
                for (const auto& mt : client.ListMachineTypes(gcp.project_id, zone)) {
                    if (!mt) {
                        break;  // zone absent or not listable; try the next one
                    }
                    out.push_back({.name = mt->name(),
                                   .zone = zone,
                                   .guest_cpus = static_cast<unsigned>(mt->guest_cpus()),
                                   .memory_mb = static_cast<unsigned>(mt->memory_mb())});
                }
            }
        } catch (const std::exception& ex) {
            std::cerr << "[gcp-spot] machine-type discovery failed (" << ex.what()
                      << "); falling back to the configured machine type\n";
            return std::vector<gcp_machine_type_info>{};
        }
        std::cerr << "[gcp-spot] discovered " << out.size() << " (machine type, zone) pairs across "
                  << candidate_zones().size() << " candidate zones\n";
        return out;
#endif
    }();
    return discovered;
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
    const std::string subnet = env_or("GCP_TEST_SUBNET_PREFIX", "default");
    // Every candidate zone is registered up front, not just the one a case
    // targets. `zone_for_node()` resolves a node's zone by probing the zones in
    // `subnetwork_by_group`, so a manager rebuilt onto a later ladder rung can
    // only still find (and decommission) nodes placed by an earlier rung if
    // every zone the ladder might use is present here.
    for (const auto& zone : candidate_zones()) {
        cfg.subnetwork_by_group[zone] = subnet;
    }
    return cfg;
}

/// Wraps `gcp_compute_quorum_manager` with the zone-laddering spot-first
/// escalation from `gcp_real_gce_test_support.hpp`.
///
/// Rebuilding the manager per rung is safe here even for nodes already placed
/// by an earlier rung, because `zone_for_node()` derives a node's zone by
/// probing `subnetwork_by_group` rather than from in-memory state — and
/// `base_compute_config()` registers every candidate zone. `assess_quorum` and
/// `decommission_node` therefore keep working across an escalation.
///
/// `provision()` returns the zone the node actually landed in, which the caller
/// must record as the node's group: GroupId *is* the zone in this manager, so a
/// substituted zone changes the node's group membership.
class escalating_gce_manager {
public:
    using manager_type = gcp_compute_quorum_manager<>;

    struct placement {
        peer_info<std::uint64_t, std::string> peer;
        std::string zone;
        std::string machine_type;
        bool spot{false};
        double price_per_hr{0.0};
    };

    explicit escalating_gce_manager(gcp_compute_quorum_manager_config<std::string> cfg)
        : _cfg(std::move(cfg)) {
        rebuild(_cfg.machine_type, _cfg.spot);
    }

    /// Provisions one node, preferring `preferred_zone` and escalating on a
    /// capacity or quota refusal. Zones already returned by a previous
    /// `provision()` on this instance are excluded, so a multi-zone case keeps
    /// getting distinct zones even when it has to substitute one.
    auto provision(const std::string& preferred_zone) -> placement {
        auto ladder =
            rank_gcp_launch_options(available_machine_types(), gcp_machine_type_requirements{},
                                    preferred_zone, _used_zones);
        if (ladder.empty()) {
            // Discovery unavailable: fall back to exactly the pre-escalation
            // behaviour rather than inventing a rung.
            ladder.push_back({.machine_type = _cfg.machine_type,
                              .zone = preferred_zone,
                              .spot = _cfg.spot,
                              .price_per_hr = gce_hourly_rate(_cfg.machine_type)});
        }
        // Each node walks its own ladder from the top: the ladder is already
        // rebuilt per call with the zones this case has consumed excluded, so
        // carrying a rung index across nodes (as the Azure walk does) would
        // index into a different list than the one it was measured against.
        std::size_t rung = 0;
        auto chosen = escalate_gcp_launch(
            ladder, rung, _escalations,
            [&](const gcp_launch_option& option) {
                rebuild(option.machine_type, option.spot);
                auto peer = std::move(_mgr->provision_node(option.zone, std::nullopt)).get();
                return placement{.peer = peer,
                                 .zone = option.zone,
                                 .machine_type = option.machine_type,
                                 .spot = option.spot,
                                 .price_per_hr = option.price_per_hr};
            },
            [](const gcp_launch_option& from, const gcp_launch_option& to, const char* message) {
                BOOST_TEST_MESSAGE("[gcp-spot] " << from.label() << " refused, escalating to "
                                                 << to.label() << ": " << message);
            });
        _used_zones.push_back(chosen.zone);
        return chosen;
    }

    auto operator->() -> manager_type* { return &*_mgr; }

    /// How many times a ladder rung was refused across this manager's lifetime.
    /// Zero on a healthy run; non-zero is what proves escalation actually ran.
    [[nodiscard]] auto escalations() const -> std::size_t { return _escalations; }

private:
    void rebuild(const std::string& machine_type, bool spot) {
        if (_mgr && _current_machine_type == machine_type && _current_spot == spot) {
            return;  // nothing about the manager's config would change
        }
        auto cfg = _cfg;
        cfg.machine_type = machine_type;
        cfg.spot = spot;
        _current_machine_type = machine_type;
        _current_spot = spot;
        _mgr.emplace(std::move(cfg));
    }

    gcp_compute_quorum_manager_config<std::string> _cfg;
    std::optional<manager_type> _mgr;
    std::string _current_machine_type;
    bool _current_spot{false};
    std::vector<std::string> _used_zones;
    std::size_t _escalations{0};
};

}  // namespace

// ── gcp_compute_quorum_manager real-GCE cases (Requirement 23 AC 18) ────────

BOOST_AUTO_TEST_SUITE(gcp_compute_real_gce)

BOOST_AUTO_TEST_CASE(provision_and_assess_single_zone) {
    auto cfg = base_compute_config();
    const std::string zone = env_or("GCP_REGION", "us-central1") + "-a";
    cfg.topology.groups.push_back({.group_id = zone, .target_count = 1});
    escalating_gce_manager mgr{cfg};

    auto placed = mgr.provision(zone);
    BOOST_TEST_MESSAGE("provisioned node " + std::to_string(placed.peer.node_id) + " at " +
                       placed.peer.address + " on " + placed.machine_type + " @ " + placed.zone);

    // placed.zone, not the requested zone: escalation may have substituted one,
    // and GroupId is the zone, so the node's group is wherever it landed.
    std::vector<node_placement<std::uint64_t, std::string>> cluster{
        {.node_id = placed.peer.node_id, .group_id = placed.zone}};
    auto health = std::move(mgr->assess_quorum(cluster)).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 1u);

    std::move(mgr->decommission_node(placed.peer.node_id)).get();
}

BOOST_AUTO_TEST_CASE(provision_multi_zone_topology) {
    auto cfg = base_compute_config();
    const std::string region = env_or("GCP_REGION", "us-central1");
    const std::string subnet = env_or("GCP_TEST_SUBNET_PREFIX", "default");
    std::vector<std::string> zones{region + "-a", region + "-b", region + "-c"};
    // Every candidate zone gets a subnetwork and a topology group, not just the
    // three preferred ones: escalation may place a node in a zone outside this
    // list, and the manager must be configured for wherever a node can land.
    for (const auto& z : candidate_zones()) {
        cfg.subnetwork_by_group[z] = subnet;
        cfg.topology.groups.push_back({.group_id = z, .target_count = 1});
    }
    escalating_gce_manager mgr{cfg};

    // This is the case that broke the Aug 3 2026 scheduled run, with
    // us-central1-c out of capacity. Each node names a preferred zone; the
    // ladder substitutes another only if that one refuses, and never one
    // already used by this case, so the topology keeps three distinct zones.
    std::vector<node_placement<std::uint64_t, std::string>> cluster;
    std::vector<std::string> placed_zones;
    for (const auto& z : zones) {
        auto placed = mgr.provision(z);
        cluster.push_back({.node_id = placed.peer.node_id, .group_id = placed.zone});
        placed_zones.push_back(placed.zone);
    }

    // The property that matters, and the reason the ladder tracks used zones:
    // three nodes in three *distinct* zones. Which three is not the point.
    std::vector<std::string> distinct = placed_zones;
    std::sort(distinct.begin(), distinct.end());
    distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
    BOOST_CHECK_EQUAL(distinct.size(), zones.size());

    auto health = std::move(mgr->assess_quorum(cluster)).get();
    BOOST_CHECK_EQUAL(health.live_node_count, zones.size());
    for (const auto& np : cluster) {
        std::move(mgr->decommission_node(np.node_id)).get();
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
    // Zero budget for reaching RUNNING. `provision_node`'s poll loop evaluates
    // `steady_clock::now() < deadline` before its first sleep, so a zero
    // timeout skips the loop entirely and takes the timeout branch every time.
    //
    // A small non-zero timeout does NOT work here, and the 1s this case used to
    // pass is why it failed against real GCE: `provision_timeout` bounds only
    // the poll-to-RUNNING phase *after* the insert-operation wait (see its
    // declaration in gcp_compute_quorum_manager_config), and that wait — bounded
    // separately by `gcp.api_timeout` — does not return until GCE has finished
    // creating the instance. By then it is already RUNNING, so the very first
    // poll succeeds and nothing is ever thrown.
    cfg.provision_timeout = std::chrono::seconds(0);
    const std::string zone = env_or("GCP_REGION", "us-central1") + "-a";
    cfg.topology.groups.push_back({.group_id = zone, .target_count = 1});
    gcp_compute_quorum_manager<> mgr{cfg};

    // Baseline taken first: asserting a delta rather than an absolute count
    // keeps this independent of anything another case (or an earlier crashed
    // run) left behind.
    const auto before = cluster_instance_names(cfg.gcp.project_id, zone, cfg.cluster_name);

    // The future is exceptional...
    BOOST_CHECK_THROW(std::move(mgr.provision_node(zone, std::nullopt)).get(), std::exception);

    // ...and the instance created before the timeout was deleted best-effort
    // rather than leaked. This is the half of the contract the case is named
    // for, and it went unasserted before — a leak here passed silently.
    // delete_best_effort waits on the delete operation, so the instance should
    // already be gone; the bounded retry only absorbs list eventual-consistency.
    std::vector<std::string> after;
    for (int i = 0; i < 15; ++i) {
        after = cluster_instance_names(cfg.gcp.project_id, zone, cfg.cluster_name);
        if (after.size() <= before.size()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    BOOST_CHECK_EQUAL(after.size(), before.size());
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

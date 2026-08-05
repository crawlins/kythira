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

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <unistd.h>

using namespace kythira;
using namespace kythira::testing::gcp_real_gce;
using kythira::testing::gcp_real_gce::g_active_gcp_fixture;
using kythira::testing::gcp_real_gce::signal_cleanup_target;

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

/// Line-buffers stdout and emits an unbuffered progress trace.
///
/// The Aug 5 2026 run of this suite hung for the full 3600s ctest timeout and
/// produced *no output whatsoever* — not even Boost's "Running N test cases".
/// The suite had genuinely run (the script's own `***Skipped` guard did not
/// fire), but stdout is block-buffered when it is a pipe rather than a tty, so
/// everything written was still in the buffer when ctest SIGKILLed the process
/// on timeout. An hour of execution yielded zero diagnostics, and two separate
/// hypotheses about where it hung could not be told apart.
///
/// `_IOLBF` makes Boost's own output survive a kill, and `trace()` writes to
/// `std::cerr`, which is unbuffered by default, so the last line printed marks
/// how far the run actually got.
struct UnbufferedOutputFixture {
    UnbufferedOutputFixture() { std::setvbuf(stdout, nullptr, _IOLBF, 0); }
};
BOOST_GLOBAL_FIXTURE(UnbufferedOutputFixture);

/// Unbuffered progress marker — survives a SIGKILL, unlike BOOST_TEST_MESSAGE.
void trace(std::string_view what) {
    std::cerr << "[gcp-trace] " << what << std::endl;  // NOLINT(performance-avoid-endl)
}

// Reports the whole suite as skipped (ctest "Not Run", return code 77) when the
// real-GCP prerequisites are absent — the GCP analogue of the AWS suite's
// PreflightSkipFixture. Its first action against GCP is a read-only instances
// list, mirroring the AWS sts:GetCallerIdentity pre-check (Requirement 23 AC 14).
struct PreflightSkipFixture {
    PreflightSkipFixture() {
        trace("preflight: start");
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
        trace("preflight: ok");
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

auto instances_client() -> google::cloud::compute_instances_v1::InstancesClient {
    google::cloud::Options opts;
    opts.set<google::cloud::UnifiedCredentialsOption>(
        google::cloud::MakeGoogleDefaultCredentials());
    return google::cloud::compute_instances_v1::InstancesClient(
        google::cloud::compute_instances_v1::MakeInstancesConnectionRest(opts));
}

/// Names of the instances in @p zone carrying this suite's cluster label — the
/// same `labels.kythira-cluster` filter `assess_quorum` uses. Read directly via
/// the SDK rather than through the manager so a leak assertion cannot be masked
/// by the manager's own bookkeeping.
auto cluster_instance_names(const std::string& project, const std::string& zone,
                            const std::string& cluster) -> std::vector<std::string> {
    auto client = instances_client();

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
            trace("machine-type discovery: constructing client");
            gcp_client_config gcp;
            gcp.project_id = env_or("GCP_PROJECT_ID", "");
            auto client = google::cloud::compute_machine_types_v1::MachineTypesClient(
                google::cloud::compute_machine_types_v1::MakeMachineTypesConnectionRest(
                    gcp_compute_detail::make_options(gcp)));
            for (const auto& zone : candidate_zones()) {
                trace("machine-type discovery: listing " + zone);
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
        trace("machine-type discovery: done");
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
                trace("launch attempt: " + option.label());
                if (_fault) {
                    _fault(option);
                }
                rebuild(option.machine_type, option.spot);
                auto peer = std::move(_mgr->provision_node(option.zone, std::nullopt)).get();
                trace("launch ok: " + option.label());
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

    /// Installs a hook run immediately before each launch attempt, so a test can
    /// make a chosen rung refuse.
    ///
    /// A zone stockout cannot be scheduled — that is the whole difficulty with
    /// verifying this ladder against real GCE — so the *refusal* is the one part
    /// of `provision_escalates_past_zone_stockout` that is synthetic. Everything
    /// downstream of it stays real: the next rung is a genuine
    /// `instances.insert` against GCE, and the assess/decommission that follow
    /// run against the VM it actually creates. The hook throws the verbatim
    /// error text GCE returned on Aug 3 2026, so the classifier
    /// (`is_gcp_capacity_or_quota_error`) is exercised on a real message rather
    /// than on one written to match it.
    void set_launch_fault(std::function<void(const gcp_launch_option&)> fault) {
        _fault = std::move(fault);
    }

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
    std::function<void(const gcp_launch_option&)> _fault;
};

/// The verbatim `instances.insert` error text GCE returned on Aug 3 2026, when
/// `provision_multi_zone_topology` failed on a us-central1-c stockout. Kept
/// literal, including GCP's own bracketed code and prose, because what the
/// forced-failure case is really checking is that
/// `is_gcp_capacity_or_quota_error` matches *this* string.
auto zone_stockout_message(const std::string& project, const std::string& zone) -> std::string {
    return "[ZONE_RESOURCE_POOL_EXHAUSTED] The zone 'projects/" + project + "/zones/" + zone +
           "' does not have enough resources available to fulfill the request.  "
           "Try a different zone, or try again later.";
}

/// Deletes any instance still carrying *this run's* `kythira-test-run` label,
/// in every candidate zone.
///
/// Why this exists at all: on Aug 3 2026 the scheduled run failed partway
/// through `provision_multi_zone_topology` — us-central1-c was out of capacity,
/// which is the failure the launch ladder was written for — and the case
/// aborted before reaching its decommission loop. The two nodes it had already
/// placed in us-central1-a and -b were still running two days later, on-demand,
/// each with a 20 GB disk. Every case here decommissions its own nodes on the
/// happy path, so nothing had ever swept the unhappy one. This suite was in
/// fact the only real-cloud suite that installed `GcpSignalHandlerFixture`
/// while implementing no `signal_cleanup_target` for it to invoke — the AWS,
/// Azure and GCP-PrivateCA suites all have one — so the signal handler had
/// nothing to clean up. The GCP counterpart of the Azure sweep in `76a76d7`.
///
/// Scoped by `kythira-test-run`, *not* by `kythira-cluster` as the Azure sweep
/// is. That difference is deliberate and matters: Azure's `cluster_name` is
/// unique per fixture instance, whereas `base_compute_config()` hardcodes
/// `cluster_name = "kythira-it"` for every case *and* every concurrent run, so
/// sweeping on it would delete a parallel run's instances out from under it.
/// `kythira-test-run` is `run_id()`, which is per-process.
///
/// One consequence, stated because it is a real limitation rather than an
/// oversight: this cannot adopt leaks from *earlier* runs, since those carry a
/// different run id. The Aug 3 pair had to be deleted by hand. A sweep broad
/// enough to reclaim them would be a sweep able to destroy a concurrent run.
struct GceLeakSweepFixture : signal_cleanup_target {
    GceLeakSweepFixture() { g_active_gcp_fixture.store(this, std::memory_order_release); }

    ~GceLeakSweepFixture() {
        g_active_gcp_fixture.store(nullptr, std::memory_order_release);
        teardown();
    }

    GceLeakSweepFixture(const GceLeakSweepFixture&) = delete;
    auto operator=(const GceLeakSweepFixture&) -> GceLeakSweepFixture& = delete;
    GceLeakSweepFixture(GceLeakSweepFixture&&) = delete;
    auto operator=(GceLeakSweepFixture&&) -> GceLeakSweepFixture& = delete;

    void teardown() noexcept override {
        // The destructor and the signal handler can both reach here.
        if (_torn_down.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        const std::string project = env_or("GCP_PROJECT_ID", "");
        if (project.empty()) {
            return;
        }
        try {
            auto client = instances_client();
            for (const auto& zone : candidate_zones()) {
                google::cloud::cpp::compute::instances::v1::ListInstancesRequest req;
                req.set_project(project);
                req.set_zone(zone);
                req.set_filter("labels.kythira-test-run = \"" + run_id() + "\"");
                std::vector<std::string> leaked;
                for (auto const& maybe : client.ListInstances(req)) {
                    if (!maybe) {
                        break;  // zone unreadable; the other zones still matter
                    }
                    leaked.push_back(maybe->name());
                }
                for (const auto& name : leaked) {
                    // Loud on purpose, exactly as the Azure sweep is: a sweep
                    // that quietly absorbed leaks would turn "this case leaks a
                    // VM every run" back into something invisible. The Aug 3
                    // pair went unnoticed for two days.
                    std::cerr << "[gcp_quorum_manager_real_gce_test] teardown: deleting leaked "
                                 "instance "
                              << name << " in " << zone << " (run " << run_id() << ")\n";
                    auto deleted = client.DeleteInstance(project, zone, name).get();
                    if (!deleted) {
                        std::cerr << "[gcp_quorum_manager_real_gce_test] teardown: FAILED to "
                                     "delete "
                                  << name << " in " << zone << " (" << deleted.status().message()
                                  << ") — delete it manually\n";
                    }
                }
            }
        } catch (const std::exception& ex) {
            std::cerr << "[gcp_quorum_manager_real_gce_test] teardown sweep failed: " << ex.what()
                      << "\n";
        } catch (...) {
            std::cerr << "[gcp_quorum_manager_real_gce_test] teardown sweep failed\n";
        }
    }

private:
    std::atomic<bool> _torn_down{false};
};

}  // namespace

BOOST_TEST_GLOBAL_FIXTURE(GceLeakSweepFixture);

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

/// Verifies the ladder under the failure condition it exists for, with a
/// control in the same run proving the precondition is what made the
/// difference.
///
/// The three preceding cases only ever exercise the ladder's happy path: on a
/// healthy day rung 0 launches and nothing escalates, so a green run says
/// nothing about whether escalation works. This case forces rung 0 to refuse
/// with GCE's own stockout text, then — on the same manager, with the fault
/// removed — provisions again and requires *zero* further escalations. Without
/// that second half, an escalation count of 1 would be equally consistent with
/// a harness that always escalates.
BOOST_AUTO_TEST_CASE(provision_escalates_past_zone_stockout) {
    auto cfg = base_compute_config();
    const std::string region = env_or("GCP_REGION", "us-central1");
    const std::string preferred = region + "-a";
    // Every candidate zone needs a topology group: escalation places the node
    // in a zone this case did not name, and GroupId is the zone.
    for (const auto& z : candidate_zones()) {
        cfg.topology.groups.push_back({.group_id = z, .target_count = 1});
    }

    // With machine-type discovery unavailable the ladder degenerates to the
    // single configured rung, and `escalate_gcp_launch` rethrows rather than
    // escalating off the end of it. Skip rather than report that as a failure
    // of the escalation logic, which is not what would have gone wrong.
    if (available_machine_types().empty()) {
        BOOST_TEST_MESSAGE(
            "machine-type discovery unavailable — ladder has a single rung, "
            "skipping the forced-stockout case");
        return;
    }

    escalating_gce_manager mgr{cfg};

    // ── Forced failure ─────────────────────────────────────────────────────
    // Refuse whatever rung 0 turns out to be, exactly once, and capture it so
    // the assertions compare against the rung that was actually refused rather
    // than against an assumption about how the ladder ranked today.
    std::optional<gcp_launch_option> refused;
    mgr.set_launch_fault([&](const gcp_launch_option& option) {
        if (refused.has_value()) {
            return;  // only the first attempt refuses
        }
        refused = option;
        throw std::runtime_error(zone_stockout_message(cfg.gcp.project_id, option.zone));
    });

    auto placed = mgr.provision(preferred);

    // The precondition fired at all -- without this the rest could pass
    // vacuously on a run where the hook was never reached.
    BOOST_REQUIRE(refused.has_value());
    BOOST_CHECK_EQUAL(refused->zone, preferred);
    BOOST_CHECK_EQUAL(mgr.escalations(), 1u);
    BOOST_TEST_MESSAGE("[gcp-spot] forced refusal of " << refused->label() << "; landed on "
                                                       << placed.machine_type << " @ "
                                                       << placed.zone);

    // The ladder's defining property: changing zone is free, growing the
    // machine type is not, so a zone stockout must move the zone and leave the
    // machine type (and the spot purchase model) alone.
    BOOST_CHECK_NE(placed.zone, refused->zone);
    BOOST_CHECK_EQUAL(placed.machine_type, refused->machine_type);
    BOOST_CHECK_EQUAL(placed.spot, refused->spot);

    // A real VM in a real, *different* group -- this is the part that would
    // have caught a GCP analogue of the Azure next_node_id() bug, where the
    // single-node happy path passed while anything past it was broken.
    std::vector<node_placement<std::uint64_t, std::string>> cluster{
        {.node_id = placed.peer.node_id, .group_id = placed.zone}};
    auto health = std::move(mgr->assess_quorum(cluster)).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 1u);

    // ── Control ────────────────────────────────────────────────────────────
    // Same manager, same ladder, fault removed. The preferred zone is still
    // free (only `placed.zone` was consumed), so a healthy run must take rung 0
    // and escalate zero further times. If this escalates too, the escalation
    // above was an artefact of the harness rather than of the injected refusal,
    // and the case above proves nothing.
    const std::size_t after_forced = mgr.escalations();
    mgr.set_launch_fault(nullptr);
    auto control = mgr.provision(preferred);
    BOOST_CHECK_EQUAL(mgr.escalations(), after_forced);
    BOOST_CHECK_EQUAL(control.zone, preferred);

    std::move(mgr->decommission_node(placed.peer.node_id)).get();
    std::move(mgr->decommission_node(control.peer.node_id)).get();
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

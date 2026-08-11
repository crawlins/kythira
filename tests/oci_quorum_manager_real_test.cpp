/// @file oci_quorum_manager_real_test.cpp
/// @brief `oci_instance_pool_quorum_manager` against a **real OCI tenancy**
///        (`.kiro/specs/oci-cloud-provider/`, Requirements 13.8-13.15).
///
/// Opt-in: built only when `KYTHIRA_OCI_REAL_TESTS` is defined, and never
/// registered with CTest, mirroring `KYTHIRA_AWS_REAL_EC2_TESTS`. It launches
/// billable instances.
///
/// **Why this tier exists, concretely.** Everything here was already green
/// against `tests/oci_mock_server.hpp` when the first real call was made — and
/// that first call failed, as did the next four. The defects were a wrong
/// endpoint domain, a `Host` header that disagreed with the signed one,
/// duplicate `Content-Type`, tagging an instance before OCI would accept the
/// write, and detaching while the pool was still `SCALING`
/// (`spike-notes.md` Findings 5-11). None was reachable locally: the mock
/// replaces the whole host, binds a non-default port, tolerates duplicate
/// headers, and materialises instances already `RUNNING`. Coverage was
/// irrelevant to all five.
///
/// The fixture skips rather than fails when credentials or resources are
/// absent (Requirement 13.9), creates nothing it did not receive an OCID for
/// (Requirement 13.11), and tears down unconditionally — including on a signal
/// (Requirement 14.5).

#define BOOST_TEST_MODULE oci_quorum_manager_real_test
#include <boost/test/unit_test.hpp>

#include <raft/oci_instance_pool_quorum_manager.hpp>

#include "oci_real_test_support.hpp"
#include "test_timeout_scale.hpp"

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <chrono>
#include <cstdint>
#include <ctime>
#include <optional>
#include <iostream>
#include <string>
#include <vector>

namespace {

using kythira::node_placement;
using kythira::oci_instance_pool_quorum_manager;
using kythira::oci_instance_pool_quorum_manager_config;
using kythira::testing::oci_real::BilledResource;
using kythira::testing::oci_real::CostSummaryFixture;
using kythira::testing::oci_real::g_active_oci_fixture;
using kythira::testing::oci_real::g_cost_accumulator;
using kythira::testing::oci_real::OciSignalHandlerFixture;
using kythira::testing::oci_real::real_test_config;
using kythira::testing::oci_real::signal_cleanup_target;
using kythira::testing::oci_real::TestCostReport;

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

BOOST_GLOBAL_FIXTURE(OciSignalHandlerFixture);
BOOST_GLOBAL_FIXTURE(CostSummaryFixture);

/// Exit code CTest is told to read as "not run" rather than "failed" when the
/// tenancy is not configured, matching `aws_quorum_manager_localstack_test`'s
/// convention.
constexpr int kSkipExitCode = 77;

/// @brief The live tenancy, or a skip.
///
/// Requirement 13.9: the **first** action is a lightweight, read-only,
/// always-permitted call — `ListRegions` — and any failure skips the entire
/// suite rather than failing it. A suite that fails on missing credentials
/// turns "this developer has no OCI tenancy" into a red build, which is how a
/// real-cloud tier ends up permanently disabled.
struct RealOciFixture : signal_cleanup_target {
    real_test_config cfg{real_test_config::from_environment()};
    oci_instance_pool_quorum_manager_config manager_cfg;
    std::vector<std::uint64_t> provisioned;
    std::optional<oci_instance_pool_quorum_manager<>> manager;
    bool usable{false};

    RealOciFixture() {
        if (!cfg.has_credentials() || cfg.compartment_id.empty() || cfg.instance_pool_id.empty()) {
            std::cerr << "[oci-real] SKIP: set KYTHIRA_OCI_REGION/TENANCY_ID/USER_ID/"
                         "FINGERPRINT/PRIVATE_KEY_PEM/COMPARTMENT_ID/INSTANCE_POOL_ID. "
                         "See scripts/ci-cloud-credentials/oci/README.md.\n";
            std::exit(kSkipExitCode);
        }

        manager_cfg.oci.region = cfg.region;
        manager_cfg.oci.tenancy_id = cfg.tenancy_id;
        manager_cfg.oci.user_id = cfg.user_id;
        manager_cfg.oci.fingerprint = cfg.fingerprint;
        manager_cfg.oci.private_key_pem = cfg.private_key_pem;
        manager_cfg.oci.api_timeout = std::chrono::seconds{60};
        manager_cfg.compartment_id = cfg.compartment_id;
        manager_cfg.instance_pool_id = cfg.instance_pool_id;
        manager_cfg.cluster_name = "kythira-real-test";
        manager_cfg.node_port = 7000;
        // A stock image runs no kythira process, so no heartbeat tag is ever
        // written. Zero selects lifecycleState-only liveness; anything else
        // would classify every instance unreachable the moment the grace
        // period expired and make the suite a slow way to observe nothing.
        manager_cfg.heartbeat_timeout = std::chrono::seconds{0};
        manager_cfg.provision_timeout = std::chrono::seconds{900};
        manager_cfg.poll_interval = std::chrono::milliseconds{10000};
        // Requirement 13.10: every resource this fixture touches is tagged with
        // a run marker, so an audit after a crash can tell what belonged to it.
        manager_cfg.extra_tags["kythira-test-run"] = run_id();

        // The preflight itself. Constructing the manager performs
        // GetInstancePool, which is read-only and is exactly the call the
        // constructor makes in production — so a tenancy that cannot serve it
        // could not run the suite anyway.
        try {
            kythira::oci_http_client probe{manager_cfg.oci};
            (void)probe.request("identity", "GET", "/20160918/regions");
        } catch (const std::exception& e) {
            std::cerr << "[oci-real] SKIP: preflight ListRegions failed: " << e.what() << "\n";
            std::exit(kSkipExitCode);
        }

        try {
            manager.emplace(manager_cfg);
        } catch (const std::exception& e) {
            std::cerr << "[oci-real] SKIP: could not construct the manager (GetInstancePool): "
                      << e.what() << "\n";
            std::exit(kSkipExitCode);
        }
        manager_cfg.topology.groups.push_back(
            {.group_id = manager->availability_domains().front(), .target_count = 1});
        usable = true;
        g_active_oci_fixture.store(this, std::memory_order_release);
    }

    ~RealOciFixture() {
        g_active_oci_fixture.store(nullptr, std::memory_order_release);
        teardown();
    }

    RealOciFixture(const RealOciFixture&) = delete;
    auto operator=(const RealOciFixture&) -> RealOciFixture& = delete;
    RealOciFixture(RealOciFixture&&) = delete;
    auto operator=(RealOciFixture&&) -> RealOciFixture& = delete;

    [[nodiscard]] static auto run_id() -> const std::string& {
        static const std::string id =
            "run-" + std::to_string(static_cast<long long>(std::time(nullptr)));
        return id;
    }

    [[nodiscard]] auto availability_domain() const -> std::string {
        return manager->availability_domains().front();
    }

    /// Fill in a @ref BilledResource's rate from the instance OCI actually
    /// launched: its `shape`, and for a flexible shape its `shapeConfig.ocpus`.
    ///
    /// Leaves `rate_known == false` when anything is unavailable, which the
    /// cost summary now reports loudly rather than folding into a $0 total.
    void price_from_live_instance(BilledResource& billed, std::uint64_t node) noexcept {
        try {
            kythira::oci_http_client http{manager_cfg.oci};
            const auto listed =
                http.request("iaas", "GET",
                             "/20160918/instancePools/" + manager_cfg.instance_pool_id +
                                 "/instances?compartmentId=" + manager_cfg.compartment_id);
            const auto* arr = listed.if_array();
            if (arr == nullptr) {
                return;
            }
            for (const auto& summary : *arr) {
                const auto* so = summary.if_object();
                const auto* id = so != nullptr ? so->if_contains("id") : nullptr;
                if (id == nullptr || !id->is_string()) {
                    continue;
                }
                const auto detail = http.request(
                    "iaas", "GET", "/20160918/instances/" + std::string(id->get_string()));
                const auto* d = detail.if_object();
                if (d == nullptr) {
                    continue;
                }
                // Only the instance carrying this node's tag.
                const auto* tags = d->if_contains("freeformTags");
                const auto* to = tags != nullptr ? tags->if_object() : nullptr;
                const auto* tag = to != nullptr ? to->if_contains("kythira-node-id") : nullptr;
                if (tag == nullptr || !tag->is_string() ||
                    std::string(tag->get_string()) != std::to_string(node)) {
                    continue;
                }
                const auto* shape = d->if_contains("shape");
                if (shape == nullptr || !shape->is_string()) {
                    return;
                }
                const std::string shape_name(shape->get_string());
                billed.label += " " + shape_name;
                if (const auto* sc = d->if_contains("shapeConfig");
                    sc != nullptr && sc->is_object()) {
                    if (const auto* ocpus = sc->get_object().if_contains("ocpus");
                        ocpus != nullptr && (ocpus->is_double() || ocpus->is_int64())) {
                        billed.ocpus = ocpus->is_double() ? ocpus->get_double()
                                                          : static_cast<double>(ocpus->get_int64());
                    }
                }
                if (const auto rate = kythira::testing::oci_real::ocpu_hourly_rate_usd(
                        kythira::testing::oci_real::shape_price_family(shape_name));
                    rate.has_value()) {
                    billed.ocpu_hourly_rate = *rate;
                    billed.rate_known = true;
                }
                return;
            }
        } catch (const std::exception& e) {
            std::cerr << "[oci-cost] could not price the launched instance: " << e.what() << "\n";
        }
    }

    /// Unconditional, best-effort, and safe to call twice — the destructor and
    /// the signal handler both reach it, and on the signal path the destructor
    /// never runs at all.
    void teardown() noexcept override {
        if (!manager.has_value()) {
            return;
        }
        auto pending = provisioned;
        provisioned.clear();
        for (const auto node : pending) {
            try {
                std::move(manager->decommission_node(node)).get();
                std::cerr << "[oci-real] torn down node " << node << "\n";
            } catch (const std::exception& e) {
                // Collected and printed, never thrown: one failed teardown must
                // not prevent the next, and a destructor that throws during
                // stack unwinding terminates the process.
                std::cerr << "[oci-real] TEARDOWN FAILED for node " << node << ": " << e.what()
                          << " — check for a leaked instance in " << cfg.compartment_id << "\n";
            }
        }
    }
};

}  // namespace

BOOST_FIXTURE_TEST_SUITE(oci_real_quorum_manager, RealOciFixture)

/// The whole lifecycle, which is what this tier is for.
///
/// Deliberately one case rather than three: provision, assess and decommission
/// share an instance, and splitting them would either launch three instances or
/// make the cases order-dependent. The AWS suite made the same call.
BOOST_AUTO_TEST_CASE(provision_assess_decommission,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(1800))) {
    TestCostReport cost{.test_name = "provision_assess_decommission", .resources = {}};
    const auto ad = availability_domain();

    BilledResource instance{.label = "instance (" + ad + ")"};

    auto peer = std::move(manager->provision_node(ad, std::nullopt)).get();
    provisioned.push_back(peer.node_id);
    std::cerr << "[oci-real] provisioned node " << peer.node_id << " at " << peer.address << "\n";

    // Price what actually launched, rather than what the config asked for.
    //
    // The shape lives on the pre-existing Instance Configuration, so the
    // manager never sees it and the test cannot assume it. Reading it back from
    // the instance is the only honest source — and skipping this step is what
    // made the first live run report **$0.000000 for a real instance**, since a
    // BilledResource with no rate contributes nothing to a total that then
    // reads as authoritative.
    price_from_live_instance(instance, peer.node_id);

    // The address must be a real private IP plus the configured port — proof
    // the ListVnicAttachments/GetVnic pair ran, which is the step that failed
    // against live OCI for two separate reasons before it worked.
    BOOST_CHECK_MESSAGE(peer.address.ends_with(":7000"),
                        "address does not carry the node port: " << peer.address);
    BOOST_CHECK_MESSAGE(peer.address.find('.') != std::string::npos,
                        "address does not look like an IPv4 literal: " << peer.address);

    const std::vector<node_placement<std::uint64_t, std::string>> cluster{
        {.node_id = peer.node_id, .group_id = ad}};
    auto health = std::move(manager->assess_quorum(cluster)).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 1U);
    BOOST_CHECK_EQUAL(health.total_node_count, 1U);
    BOOST_CHECK(health.unreachable_nodes.empty());

    // Decommission here rather than leaving it to teardown, so the *test*
    // asserts it worked. Teardown still runs and is idempotent.
    BOOST_CHECK_NO_THROW(std::move(manager->decommission_node(peer.node_id)).get());
    provisioned.clear();

    instance.finalize();
    cost.resources.push_back(instance);
    g_cost_accumulator.add(std::move(cost));
}

/// Requirement 7.2: decommissioning something that was never provisioned is a
/// no-op, not an error. Costs nothing — no instance is launched.
BOOST_AUTO_TEST_CASE(decommission_of_an_unknown_node_is_idempotent,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(300))) {
    BOOST_CHECK_NO_THROW(std::move(manager->decommission_node(std::uint64_t{987654})).get());
}

/// Requirement 6.1, against the real pool's real placement configuration.
/// Costs nothing.
BOOST_AUTO_TEST_CASE(provisioning_into_an_unconfigured_ad_is_rejected,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(300))) {
    auto fut = manager->provision_node("nosuch:PHX-AD-9", std::nullopt);
    BOOST_CHECK_THROW(std::move(fut).get(), std::invalid_argument);
}

/// The escalation ladder, exercised **under the failure condition** it exists
/// for (Requirement 13.15's verify note).
///
/// A ladder that is only ever observed on a run where the first choice happened
/// to succeed proves nothing — it is indistinguishable from a ladder with one
/// rung, or from no ladder at all. So this drives it against a real stockout:
/// `spike-notes.md` Finding 14 found `VM.Standard.A1.Flex` refused in one
/// Availability Domain while succeeding in two others, at the same moment.
///
/// Launches nothing. It walks the ladder with a launcher that *classifies*
/// OCI's answer without creating an instance, because what is under test is the
/// walking and the classification, not OCI's ability to boot Linux — and
/// because Requirement 13.14's real content is "a non-capacity error aborts
/// immediately", which is a decision, not a resource.
BOOST_AUTO_TEST_CASE(the_escalation_ladder_advances_past_a_real_stockout,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(900))) {
    using kythira::testing::oci_real::is_out_of_host_capacity;
    using kythira::testing::oci_real::preemptible_first_launch_options;

    // Every AD, so at least one rung meets a genuine shortage if there is one.
    kythira::oci_http_client http{manager_cfg.oci};
    std::vector<std::string> ads;
    {
        const auto listed = http.request(
            "identity", "GET",
            "/20160918/availabilityDomains?compartmentId=" + manager_cfg.compartment_id);
        if (const auto* arr = listed.if_array(); arr != nullptr) {
            for (const auto& entry : *arr) {
                if (const auto* eo = entry.if_object(); eo != nullptr) {
                    if (const auto* name = eo->if_contains("name");
                        name != nullptr && name->is_string()) {
                        ads.emplace_back(name->get_string());
                    }
                }
            }
        }
    }
    BOOST_REQUIRE_MESSAGE(!ads.empty(), "could not list availability domains");

    const auto ladder = preemptible_first_launch_options(ads, "VM.Standard.E2.1", 1.0, 6.0);
    BOOST_REQUIRE_MESSAGE(ladder.size() > 1,
                          "a one-rung ladder cannot demonstrate escalation at all");

    // Ordering is part of the contract: preemptible rungs first, the on-demand
    // fallback last (Requirement 13.12).
    BOOST_CHECK_MESSAGE(ladder.front().preemptible, "the ladder does not start preemptible");
    BOOST_CHECK_MESSAGE(!ladder.back().preemptible,
                        "the ladder does not end at the on-demand fallback");

    std::size_t attempted = 0;
    std::size_t stockouts = 0;
    bool aborted_on_non_capacity = false;
    std::string last_error;

    for (const auto& rung : ladder) {
        ++attempted;
        boost::json::object launch;
        launch["compartmentId"] = manager_cfg.compartment_id;
        launch["availabilityDomain"] = rung.availability_domain;
        launch["shape"] = rung.shape;
        launch["displayName"] = "kythira-ladder-probe";
        // Deliberately incomplete: no imageId/subnetId. OCI validates capacity
        // for the (shape, AD) pair, so a stockout still surfaces as such, while
        // anything that would otherwise succeed fails on the missing fields
        // instead of billing us for an instance.
        try {
            (void)http.request("iaas", "POST", "/20160918/instances",
                               boost::json::serialize(launch));
            BOOST_FAIL("an intentionally incomplete launch unexpectedly succeeded");
        } catch (const std::exception& e) {
            last_error = e.what();
            if (is_out_of_host_capacity(e)) {
                ++stockouts;
                std::cerr << "[oci-real] rung " << attempted << " (" << rung.describe()
                          << ") is out of capacity — advancing\n";
                continue;
            }
            // Requirement 13.14: anything that is not a stockout stops the walk.
            aborted_on_non_capacity = true;
            break;
        }
    }

    std::cerr << "[oci-real] ladder: " << ladder.size() << " rungs, " << attempted << " attempted, "
              << stockouts << " stockouts, aborted_on_non_capacity=" << aborted_on_non_capacity
              << "\n";

    // The classifier must discriminate, and this walk only proves half of it.
    //
    // An over-broad classifier — one that called every failure a stockout —
    // would walk all 12 rungs and leave this false, which is the failure
    // Requirement 13.14 names explicitly ("a real defect is not masked as a
    // stockout"). So this catches that.
    //
    // It does NOT catch an under-broad classifier: the probe's validation
    // error would abort the walk under any classifier that fails to match it,
    // including one that matched nothing at all. The two direct assertions
    // below cover that direction without needing OCI to actually be out of
    // capacity, using Finding 14's verbatim message.
    BOOST_CHECK_MESSAGE(aborted_on_non_capacity,
                        "no rung produced a non-capacity error, so this run cannot show that "
                        "the classifier rejects non-capacity failures. Last error: "
                            << last_error);
    BOOST_CHECK_MESSAGE(!is_out_of_host_capacity(last_error),
                        "a validation error was classified as a stockout: " << last_error);
    BOOST_CHECK_MESSAGE(
        is_out_of_host_capacity("oci_http_client: POST /20160918/instances failed with HTTP 500: "
                                "InternalError: Out of host capacity."),
        "the verbatim message Finding 14 recorded from live OCI is not classified as a stockout");
    // Not asserted: that a stockout occurred. Capacity is a property of OCI on
    // the day, not of this code, and a test that required one would fail on
    // every day the region happened to be healthy.
    if (stockouts == 0) {
        std::cerr << "[oci-real] NOTE: no rung was out of capacity on this run, so the "
                     "advance-past-stockout path was not exercised. That is OCI's state, "
                     "not a defect — re-run when a region is under pressure.\n";
    }
}

BOOST_AUTO_TEST_SUITE_END()

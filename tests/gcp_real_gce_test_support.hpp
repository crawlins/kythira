#pragma once

/// @file gcp_real_gce_test_support.hpp
/// @brief Shared real-GCP integration test infrastructure: GCE cost
/// estimation/reporting and signal-driven cleanup. The GCP-flavored equivalent
/// of `aws_real_ec2_test_support.hpp` (gcp-cloud-services spec Requirements
/// 24.4/24.5), re-priced against published GCE on-demand machine-type pricing.
///
/// Header-only, included directly by each real-GCP test .cpp. `g_cost_accumulator`
/// and `g_active_gcp_fixture` are `inline` so each including *binary* gets one
/// definition even across several separate test binaries.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <boost/test/unit_test.hpp>

namespace kythira::testing::gcp_real_gce {

// ── Cost estimation (Requirement 24.4) ─────────────────────────────────────
//
// Published on-demand us-central1 Linux prices ($/hr, approximate).
// Source: https://cloud.google.com/compute/vm-instance-pricing (2026-07).
inline auto gce_hourly_rate(const std::string& machine_type) -> double {
    static const std::map<std::string, double> kRates{
        {"e2-micro", 0.008376},      {"e2-small", 0.016751},      {"e2-medium", 0.033503},
        {"e2-standard-2", 0.06701},  {"e2-standard-4", 0.134012}, {"e2-standard-8", 0.268024},
        {"n2-standard-2", 0.097118}, {"n2-standard-4", 0.194236}, {"n1-standard-1", 0.0475},
        {"n1-standard-2", 0.095},    {"n1-standard-4", 0.19},
    };
    auto it = kRates.find(machine_type);
    return (it != kRates.end()) ? it->second : 0.033503;  // default to e2-medium
}

struct BilledResource {
    std::string label;
    double hourly_rate{0.0};
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
    std::optional<std::chrono::steady_clock::time_point> stop;

    void finalize() {
        if (!stop) {
            stop = std::chrono::steady_clock::now();
        }
    }
    [[nodiscard]] auto hours() const -> double {
        auto e = stop.value_or(std::chrono::steady_clock::now());
        return std::chrono::duration<double>(e - start).count() / 3600.0;
    }
    [[nodiscard]] auto minutes() const -> double { return hours() * 60.0; }
    [[nodiscard]] auto cost_usd() const -> double { return hours() * hourly_rate; }
};

struct TestCostReport {
    std::string test_name;
    std::vector<BilledResource> resources;

    [[nodiscard]] auto total_usd() const -> double {
        double t = 0.0;
        for (const auto& r : resources) {
            t += r.cost_usd();
        }
        return t;
    }

    [[nodiscard]] auto format() const -> std::string {
        std::ostringstream oss;
        oss << std::fixed;
        oss << "\n[gcp-cost] " << test_name << "\n";
        for (const auto& r : resources) {
            oss << "[gcp-cost]   " << std::left << std::setw(38) << r.label << std::right
                << std::setw(7) << std::setprecision(1) << r.minutes() << " min" << "   $"
                << std::setprecision(6) << r.cost_usd() << "\n";
        }
        oss << "[gcp-cost]   " << std::left << std::setw(38) << "TOTAL" << std::right
            << std::setw(11) << " " << "$" << std::setprecision(6) << total_usd() << "\n";
        return oss.str();
    }
};

struct CostAccumulator {
    std::mutex mtx;
    std::vector<TestCostReport> reports;

    void add(TestCostReport r) {
        std::lock_guard<std::mutex> lk{mtx};
        reports.push_back(std::move(r));
    }
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline CostAccumulator g_cost_accumulator;

struct CostSummaryFixture {
    ~CostSummaryFixture() {
        std::lock_guard<std::mutex> lk{g_cost_accumulator.mtx};
        const auto& reps = g_cost_accumulator.reports;
        if (reps.empty()) {
            return;
        }
        double grand = 0.0;
        for (const auto& r : reps) {
            grand += r.total_usd();
        }
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(6);
        oss << "\n================================================================\n";
        oss << " GCP Real-GCE Test Cost Estimate Summary\n";
        oss << "================================================================\n";
        for (const auto& r : reps) {
            oss << "  " << std::left << std::setw(52) << r.test_name << "  $" << r.total_usd()
                << "\n";
        }
        oss << "----------------------------------------------------------------\n";
        oss << "  " << std::left << std::setw(52) << "GRAND TOTAL" << "  $" << grand << "\n";
        oss << "================================================================\n";
        oss << " Pricing: on-demand Linux rates (approximate). Actual costs vary\n";
        oss << " by region and time. Use the GCP Billing console for authoritative\n";
        oss << " billing data.\n";
        oss << "================================================================\n";
        BOOST_TEST_MESSAGE(oss.str());
    }
};

// ── Signal-driven cleanup (Requirement 24.5) ───────────────────────────────
struct signal_cleanup_target {
    virtual void teardown() noexcept = 0;

protected:
    ~signal_cleanup_target() = default;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline std::atomic<signal_cleanup_target*> g_active_gcp_fixture{nullptr};

inline void gcp_signal_cleanup_handler(int sig) {
    signal_cleanup_target* f = g_active_gcp_fixture.exchange(nullptr, std::memory_order_acq_rel);
    if (f != nullptr) {
        f->teardown();
    }
    struct sigaction sa{};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, nullptr);
    raise(sig);
}

// SIGTERM/SIGINT/SIGHUP/SIGQUIT/SIGPIPE (the spec's set); teardown() from a
// signal handler is not strictly async-signal-safe — a deliberate, accepted
// trade-off for a test-cleanup path, made safe against re-entry by SA_RESETHAND.
inline void install_gcp_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = gcp_signal_cleanup_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    for (int sig : {SIGTERM, SIGINT, SIGHUP, SIGQUIT, SIGPIPE}) {
        sigaction(sig, &sa, nullptr);
    }
}

struct GcpSignalHandlerFixture {
    GcpSignalHandlerFixture() { install_gcp_signal_handlers(); }
};

// ── Spot-first launch escalation ───────────────────────────────────────────
//
// The GCP counterpart of `spot_first_launch_options()` in
// `tests/aws_quorum_manager_real_ec2_test.cpp` and the Azure ladder in
// `tests/azure_real_test_support.hpp`. Like both, it lives in the test support
// layer only and never leaks into `gcp_compute_quorum_manager`.
//
// It differs from those two in the dimension it escalates along, because the
// failure it exists to fix is different. The Aug 3 2026 scheduled real-cloud
// run failed with:
//
//   [ZONE_RESOURCE_POOL_EXHAUSTED] The zone
//   'projects/…/zones/us-central1-c' does not have enough resources available
//   to fulfill the request.  Try a different zone, or try again later.
//
// That is a *zone* stockout, not a machine-type one, and GCP's own error text
// prescribes the remedy. So the ladder's inner dimension is the zone and its
// outer dimension is the machine type — the opposite emphasis to Azure's,
// where the zone was fixed and only the SKU varied.
//
// The ordering follows from what each move costs. Moving to another zone is
// free: same machine type, same price. Moving to a larger machine type is not.
// So every zone is tried for a given (spot, machine type) before the machine
// type is allowed to grow.
//
// One structural consequence, unique to GCP among the three providers: in
// `gcp_compute_quorum_manager` the topology's GroupId *is* the zone. Escalating
// across zones therefore changes which group a node belongs to, so the walk has
// to report the zone it actually landed in and the caller has to record the
// node under that group — see `excluded_zones` below, and
// `escalating_gce_manager` in the real test.

/// One rung of the launch ladder: a concrete (machine type, zone, purchase
/// model) triple to attempt.
struct gcp_launch_option {
    std::string machine_type;
    std::string zone;
    bool spot{true};
    double price_per_hr{0.0};

    [[nodiscard]] auto label() const -> std::string {
        return machine_type + " @ " + zone + (spot ? " (spot)" : " (on-demand)");
    }
};

/// One machine type as offered in one zone, from `machineTypes.list`.
///
/// A plain struct rather than the SDK's generated proto so the ranking below
/// stays a pure function of its inputs and can be tested with no project, no
/// credentials, and — importantly here — no google-cloud-cpp compute component
/// installed, which most build environments for this repo do not have.
struct gcp_machine_type_info {
    std::string name;
    std::string zone;
    unsigned guest_cpus{0};
    unsigned memory_mb{0};
};

/// What a machine type must satisfy to be a candidate rung.
///
/// Defaults keep parity with the size the suite launched before escalation
/// existed (`e2-small`: 2 vCPU, 2 GiB) so no case loses headroom — the ladder
/// may only substitute something at least as capable, never something smaller
/// that happens to be cheaper.
struct gcp_machine_type_requirements {
    unsigned max_guest_cpus{2};
    unsigned min_memory_mb{2048};
    /// Families to consider, cheapest-first general purpose. Anything outside
    /// this list is ignored regardless of price: GPU/local-SSD/metal families
    /// carry extra required fields the quorum manager does not set, so they
    /// would fail with a hard error the classifier deliberately does not retry.
    std::vector<std::string> family_prefixes{"e2-", "t2d-", "n2d-", "n2-", "n1-"};
};

/// GCP's published Spot discount is 60–91% off on-demand depending on machine
/// family. 0.4 (a 60% discount) is the conservative end, used only for cost
/// *reporting* and never for ordering: a constant factor cannot reorder rungs
/// that are already compared within the same purchase model.
inline constexpr double kGcpSpotPriceFactor = 0.4;

inline auto gcp_to_lower_copy(std::string s) -> std::string {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// True when a `instances.insert` failure means "not here, not right now"
/// rather than "this request is wrong".
///
/// Escalation advances on these and aborts on everything else, so this must
/// stay tight: a bad image, subnetwork, or service account has to fail loudly
/// on the first rung instead of quietly walking the whole ladder first.
///
/// `gcp_compute_quorum_manager` surfaces these as a `std::runtime_error`
/// carrying the zone operation's error text, so — as on AWS and Azure — the
/// classifier is a case-insensitive substring match rather than a code lookup.
inline auto is_gcp_capacity_or_quota_error(std::string_view message) -> bool {
    const std::string haystack = gcp_to_lower_copy(std::string(message));
    // In order: the zonal stockout this ladder exists for and its regional
    // sibling, the generic exhaustion code, GCE's prose form of the same, and
    // quota refusals (which behave the same way — a different zone or a
    // smaller machine type may well fit where this one did not).
    static const std::vector<std::string> kNeedles{
        "zone_resource_pool_exhausted",
        "resource_pool_exhausted",
        "does not have enough resources",
        "quota",
        "rate_limit_exceeded",
        "resource_availability",
        "stockout",
    };
    return std::any_of(kNeedles.begin(), kNeedles.end(), [&](const std::string& needle) {
        return haystack.find(needle) != std::string::npos;
    });
}

/// True when `info` is a machine type this suite could actually launch.
inline auto gcp_machine_type_is_eligible(const gcp_machine_type_info& info,
                                         const gcp_machine_type_requirements& req) -> bool {
    if (info.guest_cpus == 0 || info.guest_cpus > req.max_guest_cpus ||
        info.memory_mb < req.min_memory_mb) {
        return false;
    }
    return std::any_of(req.family_prefixes.begin(), req.family_prefixes.end(),
                       [&](const std::string& prefix) { return info.name.rfind(prefix, 0) == 0; });
}

/// Builds the ordered launch ladder from the machine types available across a
/// region's zones.
///
/// Ordering, outermost first: spot before on-demand; then machine type by
/// ascending price; then zone, with `preferred_zone` first so an ordinary run
/// lands exactly where the caller asked and the ladder only matters when
/// something is actually wrong.
///
/// `excluded_zones` drops zones the caller has already placed a node in. That
/// exists because GroupId is the zone here: a multi-zone case wants N
/// *distinct* zones, so a substitute must not silently collapse two groups
/// into one and quietly weaken what the case tests.
inline auto rank_gcp_launch_options(const std::vector<gcp_machine_type_info>& available,
                                    const gcp_machine_type_requirements& req,
                                    const std::string& preferred_zone,
                                    const std::vector<std::string>& excluded_zones)
    -> std::vector<gcp_launch_option> {
    const auto excluded = [&](const std::string& zone) {
        return std::find(excluded_zones.begin(), excluded_zones.end(), zone) !=
               excluded_zones.end();
    };

    // machine type name → the zones offering it (eligible ones only).
    std::map<std::string, std::vector<std::string>> zones_by_type;
    for (const auto& info : available) {
        if (!gcp_machine_type_is_eligible(info, req) || excluded(info.zone)) {
            continue;
        }
        auto& zones = zones_by_type[info.name];
        if (std::find(zones.begin(), zones.end(), info.zone) == zones.end()) {
            zones.push_back(info.zone);
        }
    }

    std::vector<std::string> types;
    types.reserve(zones_by_type.size());
    for (const auto& [name, zones] : zones_by_type) {
        types.push_back(name);
    }
    std::sort(types.begin(), types.end(), [](const std::string& a, const std::string& b) {
        const double pa = gce_hourly_rate(a);
        const double pb = gce_hourly_rate(b);
        // Name is the tie-break so the ladder is reproducible from a CI log;
        // gce_hourly_rate() returns the same default for unknown types, which
        // would otherwise leave their relative order unspecified.
        return (pa != pb) ? pa < pb : a < b;
    });

    std::vector<gcp_launch_option> ladder;
    for (const bool spot : {true, false}) {
        for (const auto& type : types) {
            auto zones = zones_by_type[type];
            std::sort(zones.begin(), zones.end());
            // Preferred zone first, everything else in a stable order after it.
            std::stable_partition(zones.begin(), zones.end(),
                                  [&](const std::string& z) { return z == preferred_zone; });
            for (const auto& zone : zones) {
                ladder.push_back(
                    {.machine_type = type,
                     .zone = zone,
                     .spot = spot,
                     .price_per_hr = gce_hourly_rate(type) * (spot ? kGcpSpotPriceFactor : 1.0)});
            }
        }
    }
    return ladder;
}

/// Walks `ladder` forward from `rung`, invoking `attempt(ladder[rung])` until
/// one rung launches.
///
/// Advances (incrementing `escalations`) on a capacity or quota refusal;
/// rethrows immediately on anything else, and rethrows the last refusal once
/// the ladder is exhausted. `rung` is carried by reference so a caller
/// provisioning several nodes resumes where the previous one settled instead of
/// re-failing on rungs already known bad.
///
/// Separated from the manager it drives so the walk can be exercised offline
/// against a stub `attempt`. That matters more here than anywhere else in this
/// repo: the GCP compute SDK is absent from most build environments for this
/// project, so the real test does not even compile locally, and a zone stockout
/// is by definition something you cannot schedule.
template<typename Attempt, typename OnEscalate>
auto escalate_gcp_launch(const std::vector<gcp_launch_option>& ladder, std::size_t& rung,
                         std::size_t& escalations, Attempt&& attempt, OnEscalate&& on_escalate)
    -> decltype(attempt(ladder.front())) {
    for (;;) {
        try {
            return attempt(ladder.at(rung));
        } catch (const std::exception& ex) {
            if (!is_gcp_capacity_or_quota_error(ex.what()) || rung + 1 >= ladder.size()) {
                throw;
            }
            on_escalate(ladder[rung], ladder[rung + 1], ex.what());
            ++rung;
            ++escalations;
        }
    }
}

// ── Environment helpers ────────────────────────────────────────────────────
inline auto env_or(const char* key, const std::string& fallback) -> std::string {
    const char* v = std::getenv(key);
    return (v != nullptr && *v != '\0') ? std::string(v) : fallback;
}

/// True when KYTHIRA_GCP_REAL_TESTS=1 and both required env vars are present.
/// When false the suite should skip (not fail).
inline auto real_tests_enabled() -> bool {
    return env_or("KYTHIRA_GCP_REAL_TESTS", "") == "1" && !env_or("GCP_PROJECT_ID", "").empty() &&
           !env_or("GCP_REGION", "").empty();
}

}  // namespace kythira::testing::gcp_real_gce

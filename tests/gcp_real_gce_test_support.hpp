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

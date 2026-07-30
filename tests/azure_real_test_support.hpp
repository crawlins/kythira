#pragma once

/// @file azure_real_test_support.hpp
/// @brief Shared real-Azure integration test infrastructure: cost
/// estimation/reporting and signal-driven cleanup. Ported from
/// `aws_real_ec2_test_support.hpp` (aws-quorum-manager spec Requirements
/// 20/21) — the cost-accumulator and signal-handling shapes are
/// provider-agnostic, only the pricing table and fixture names differ.
///
/// Header-only, included directly by each real-Azure test .cpp. `g_cost_accumulator`
/// and `g_active_azure_fixture` are declared `inline` so each including
/// *binary* gets exactly one definition, even though more than one real-Azure
/// test binary includes this same header.

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

namespace kythira::testing::azure_real {

// ── Cost estimation (design.md's "CI wiring" section) ──────────────────────
//
// Published on-demand eastus Linux prices ($/hr, approximate).
// Source: https://azure.microsoft.com/en-us/pricing/details/virtual-machines/linux/
// (as of this writing — reverify before relying on this for real budgeting).
inline auto azure_vm_hourly_rate(const std::string& size) -> double {
    static const std::map<std::string, double> kRates{
        {"Standard_B1s", 0.0104},   {"Standard_B2s", 0.0416},     {"Standard_D2s_v5", 0.096},
        {"Standard_D4s_v5", 0.192}, {"Standard_D2als_v6", 0.086}, {"Standard_F2s_v2", 0.085},
    };
    auto it = kRates.find(size);
    return (it != kRates.end()) ? it->second : 0.096;
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
        oss << "\n[azure-cost] " << test_name << "\n";
        for (const auto& r : resources) {
            oss << "[azure-cost]   " << std::left << std::setw(38) << r.label << std::right
                << std::setw(7) << std::setprecision(1) << r.minutes() << " min"
                << "   $" << std::setprecision(6) << r.cost_usd() << "\n";
        }
        oss << "[azure-cost]   " << std::left << std::setw(38) << "TOTAL" << std::right
            << std::setw(11) << " "
            << "$" << std::setprecision(6) << total_usd() << "\n";
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
        oss << " Azure Real-Cloud Test Cost Estimate Summary\n";
        oss << "================================================================\n";
        for (const auto& r : reps) {
            oss << "  " << std::left << std::setw(52) << r.test_name << "  $" << r.total_usd()
                << "\n";
        }
        oss << "----------------------------------------------------------------\n";
        oss << "  " << std::left << std::setw(52) << "GRAND TOTAL"
            << "  $" << grand << "\n";
        oss << "================================================================\n";
        oss << " Pricing: on-demand Linux rates (approximate). Actual costs vary\n";
        oss << " by region and time. Use Azure Cost Management for authoritative\n";
        oss << " billing data.\n";
        oss << "================================================================\n";
        BOOST_TEST_MESSAGE(oss.str());
    }
};

// ── Signal-driven cleanup (mirrors aws_real_ec2_test_support.hpp exactly) ──

struct signal_cleanup_target {
    virtual void teardown() noexcept = 0;

protected:
    ~signal_cleanup_target() = default;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline std::atomic<signal_cleanup_target*> g_active_azure_fixture{nullptr};

inline void azure_signal_cleanup_handler(int sig) {
    signal_cleanup_target* f = g_active_azure_fixture.exchange(nullptr, std::memory_order_acq_rel);
    if (f != nullptr) {
        f->teardown();
    }

    struct sigaction sa{};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, nullptr);
    raise(sig);
}

inline void install_azure_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = azure_signal_cleanup_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    for (int sig : {SIGTERM, SIGINT, SIGHUP, SIGQUIT, SIGPIPE, SIGABRT, SIGSEGV, SIGBUS}) {
        sigaction(sig, &sa, nullptr);
    }
}

struct AzureSignalHandlerFixture {
    AzureSignalHandlerFixture() { install_azure_signal_handlers(); }
};

}  // namespace kythira::testing::azure_real

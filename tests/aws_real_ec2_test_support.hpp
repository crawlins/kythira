#pragma once

/// @file aws_real_ec2_test_support.hpp
/// @brief Shared real-EC2 integration test infrastructure: AWS cost
/// estimation/reporting and signal-driven cleanup.
///
/// Originally implemented once, only in aws_quorum_manager_real_ec2_test.cpp
/// (aws-quorum-manager spec Requirements 20/21). Extracted here
/// (.kiro/specs/ca-cluster-rpc-mtls-real-aws/, Requirements 6/7) so every
/// real-EC2 test binary in this project gets both, not just the first one
/// that needed them — ca_cluster_node_real_ec2_test.cpp had neither and
/// would otherwise leak a VPC and running EC2 instances if killed mid-run.
///
/// Header-only, included directly by each real-EC2 test .cpp (no separate
/// translation unit). `g_cost_accumulator` and `g_active_aws_fixture` are
/// declared `inline` (not `static`) so each including *binary* gets exactly
/// one definition, even though several separate test binaries each include
/// this same header — this is a header-only, multi-binary library, not a
/// single shared translation unit.

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

// ── Cost estimation (aws-quorum-manager Requirement 20) ────────────────────
//
// Published on-demand us-east-1 Linux prices ($/hr, approximate).
// Source: https://aws.amazon.com/ec2/pricing/on-demand/ (June 2025)
inline auto ec2_hourly_rate(const std::string& type) -> double {
    static const std::map<std::string, double> kRates{
        {"t3.nano", 0.0052},    {"t3.micro", 0.0104},   {"t3.small", 0.0208},
        {"t3.medium", 0.0416},  {"t3.large", 0.0832},   {"t3.xlarge", 0.1664},
        {"t3.2xlarge", 0.3328}, {"t2.nano", 0.0058},    {"t2.micro", 0.0116},
        {"t2.small", 0.0230},   {"t2.medium", 0.0464},  {"t2.large", 0.0928},
        {"m5.large", 0.0960},   {"m5.xlarge", 0.1920},  {"m5.2xlarge", 0.3840},
        {"m6i.large", 0.0960},  {"m6i.xlarge", 0.1920}, {"c5.large", 0.0850},
        {"c5.xlarge", 0.1700},  {"r5.large", 0.1260},   {"r5.xlarge", 0.2520},
    };
    auto it = kRates.find(type);
    return (it != kRates.end()) ? it->second : 0.0104;
}

constexpr double kNatGwHourly = 0.045;
constexpr double kEipHourly = 0.005;

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
        oss << "\n[aws-cost] " << test_name << "\n";
        for (const auto& r : resources) {
            oss << "[aws-cost]   " << std::left << std::setw(38) << r.label << std::right
                << std::setw(7) << std::setprecision(1) << r.minutes() << " min"
                << "   $" << std::setprecision(6) << r.cost_usd() << "\n";
        }
        oss << "[aws-cost]   " << std::left << std::setw(38) << "TOTAL" << std::right
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
        oss << " AWS Real-EC2 Test Cost Estimate Summary\n";
        oss << "================================================================\n";
        for (const auto& r : reps) {
            oss << "  " << std::left << std::setw(52) << r.test_name << "  $" << r.total_usd()
                << "\n";
        }
        oss << "----------------------------------------------------------------\n";
        oss << "  " << std::left << std::setw(52) << "GRAND TOTAL"
            << "  $" << grand << "\n";
        oss << "================================================================\n";
        oss << " Pricing: on-demand/spot Linux rates (approximate, queried at test\n";
        oss << " start where applicable). Actual costs vary by region and time.\n";
        oss << " Use AWS Cost Explorer for authoritative billing data.\n";
        oss << "================================================================\n";
        BOOST_TEST_MESSAGE(oss.str());
    }
};

// ── Signal-driven cleanup (aws-quorum-manager Requirement 21) ──────────────
//
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

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline std::atomic<signal_cleanup_target*> g_active_aws_fixture{nullptr};

inline void aws_signal_cleanup_handler(int sig) {
    // Call teardown on the active fixture, then re-raise with default
    // disposition so the process exits with the correct status / coredump
    // behaviour.
    signal_cleanup_target* f = g_active_aws_fixture.exchange(nullptr, std::memory_order_acq_rel);
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
    // SA_RESETHAND: restore default after first invocation so nested signals
    // are not swallowed if teardown itself faults.
    sa.sa_flags = SA_RESETHAND;
    for (int sig : {SIGTERM, SIGINT, SIGHUP, SIGQUIT, SIGPIPE}) {
        sigaction(sig, &sa, nullptr);
    }
}

struct AwsSignalHandlerFixture {
    AwsSignalHandlerFixture() { install_aws_signal_handlers(); }
};

}  // namespace kythira::testing::aws_real_ec2

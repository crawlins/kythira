// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file aws_real_test_support.hpp
/// @brief The service-independent half of the real-AWS test support: cost
/// estimation and reporting, signal-driven cleanup, and the retrying
/// teardown-delete helpers.
///
/// Split out of aws_real_ec2_test_support.hpp, which still includes it, so a
/// real-AWS suite that never touches EC2 (aws_acm_pca_provider_real_test)
/// gets the same cost report and signal teardown without linking the EC2 SDK
/// component. Everything stays in `kythira::testing::aws_real_ec2` so the
/// EC2 suites see no change.
///
/// `g_cost_accumulator` and `g_active_aws_fixture` are `inline` (not
/// `static`) so each including binary gets exactly one definition.

#include <atomic>
#include <chrono>
#include <csignal>
#include <functional>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <boost/test/unit_test.hpp>

namespace kythira::testing::aws_real_ec2 {

// ── Cost estimation (aws-quorum-manager Requirement 20) ────────────────────
//
// Published on-demand us-east-1 Linux prices ($/hr, approximate).
// Source: https://aws.amazon.com/ec2/pricing/on-demand/ (June 2025)
// The t4g rows are Graviton, the default type on arm64 runners; without
// them an arm64 run silently priced its t4g.micro at the t3.micro fallback.
// c6g.medium is the arm64 cluster-placement-group type in
// aws_ec2_launch_options_real_test (T-family types cannot join one).
inline auto ec2_hourly_rate(const std::string& type) -> double {
    static const std::map<std::string, double> kRates{
        {"t3.nano", 0.0052},    {"t3.micro", 0.0104},   {"t3.small", 0.0208},
        {"t3.medium", 0.0416},  {"t3.large", 0.0832},   {"t3.xlarge", 0.1664},
        {"t3.2xlarge", 0.3328}, {"t2.nano", 0.0058},    {"t2.micro", 0.0116},
        {"t2.small", 0.0230},   {"t2.medium", 0.0464},  {"t2.large", 0.0928},
        {"m5.large", 0.0960},   {"m5.xlarge", 0.1920},  {"m5.2xlarge", 0.3840},
        {"m6i.large", 0.0960},  {"m6i.xlarge", 0.1920}, {"c5.large", 0.0850},
        {"c5.xlarge", 0.1700},  {"r5.large", 0.1260},   {"r5.xlarge", 0.2520},
        {"t4g.nano", 0.0042},   {"t4g.micro", 0.0084},  {"t4g.small", 0.0168},
        {"t4g.medium", 0.0336}, {"t4g.large", 0.0672},  {"c6g.medium", 0.0340},
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
    /// A one-off charge added to the hourly cost, for resources billed per
    /// unit rather than per hour (an ACM Private CA certificate). Last, so
    /// the positional `{label, rate, start, stop}` initializers the EC2
    /// suites use keep their meaning.
    double fixed_usd{0.0};

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
    [[nodiscard]] auto cost_usd() const -> double { return (hours() * hourly_rate) + fixed_usd; }
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
        // Loud, not silent — see the same branch in gcp_real_gce_test_support.hpp.
        // A bare `return` makes "no case registered a cost" indistinguishable
        // from "this fixture never ran", and in the GCP suite the first of
        // those was silently true for every run ever made.
        if (reps.empty()) {
            std::cerr << "\n[aws-cost] WARNING: no cost reports were registered. Either no case "
                         "provisioned a billable resource, or a case failed to file its "
                         "TestCostReport. Real-cloud spend for this run is UNREPORTED.\n";
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
        // std::cerr, not BOOST_TEST_MESSAGE: Boost's default log level drops
        // `message`-level records, which is why no real-cloud suite's cost
        // summary has ever appeared in a CI log.
        std::cerr << oss.str() << std::flush;
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

// SIGABRT/SIGSEGV/SIGBUS, not just the "polite" termination signals
// (SIGTERM/SIGINT/SIGHUP/SIGQUIT): a real-EC2 run that crashes - an
// uncaught exception reaching std::terminate() (default handler calls
// abort(), raising SIGABRT), a failed assert(), a segfault in a
// dependency (libssh2, the AWS SDK's own OpenSSL usage) - previously hit
// none of the signals this handler covered and skipped teardown()
// entirely, leaking whatever VPC/instances/NACLs that run had already
// provisioned exactly like an uncaught ctest TIMEOUT kill would (SIGTERM,
// already covered, but a genuine crash is a distinct failure mode with
// the same leak consequence). Calling teardown() - AWS SDK network calls,
// heap allocation, mutex-protected containers - from a signal handler is
// not strictly async-signal-safe, and is riskiest for SIGSEGV/SIGBUS
// specifically, since those indicate the process's memory may already be
// corrupted; this is a deliberate, accepted trade-off for a test-cleanup
// path, not production service code: best case, the AWS resources this
// run owns get torn down before the process dies anyway; worst case,
// teardown() itself faults or hangs, which is no worse than today's
// unconditional leak. SA_RESETHAND (below) already makes that worst case
// safe - the disposition resets to default the moment this handler is
// invoked, so a second fault of the same signal during teardown() falls
// straight through to the kernel's default action instead of recursing
// into this handler again.
inline void install_aws_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = aws_signal_cleanup_handler;
    sigemptyset(&sa.sa_mask);
    // SA_RESETHAND: restore default after first invocation so nested signals
    // are not swallowed if teardown itself faults.
    sa.sa_flags = SA_RESETHAND;
    for (int sig : {SIGTERM, SIGINT, SIGHUP, SIGQUIT, SIGPIPE, SIGABRT, SIGSEGV, SIGBUS}) {
        sigaction(sig, &sa, nullptr);
    }
}

struct AwsSignalHandlerFixture {
    AwsSignalHandlerFixture() { install_aws_signal_handlers(); }
};

// ── Teardown helpers ────────────────────────────────────────────────────────
//
// See aws_real_ec2_test_support.hpp for why EC2 teardown retries its
// deletes; the helpers below are generic over any SDK outcome.

// Maps a delete call's outcome to std::nullopt when the resource is gone
// (including `*.NotFound`, i.e. an earlier attempt already removed it, and
// any extra codes the caller names as "already done"), else the error text.
template<class Outcome>
auto delete_outcome_error(const Outcome& out,
                          std::initializer_list<std::string_view> done_codes = {})
    -> std::optional<std::string> {
    if (out.IsSuccess()) {
        return std::nullopt;
    }
    const std::string code(out.GetError().GetExceptionName());
    if (code.ends_with(".NotFound")) {
        return std::nullopt;
    }
    for (auto done : done_codes) {
        if (code == done) {
            return std::nullopt;
        }
    }
    return code + ": " + std::string(out.GetError().GetMessage());
}

// One teardown delete: `attempt` returns std::nullopt once the resource is
// gone, else the error that kept it.
struct teardown_delete {
    std::string what;
    std::function<std::optional<std::string>()> attempt;
};

// Runs `steps` in order, then re-runs whichever failed, still in order,
// every `interval` until all succeed or `budget` runs out. Order the steps
// children first (security groups and subnets before the VPC) so each pass
// retries a parent only after its children's latest attempt. Logs every
// step left over with its last error, and returns whether none was.
inline auto run_teardown_deletes(std::vector<teardown_delete> steps, std::chrono::seconds budget,
                                 std::chrono::seconds interval, std::string_view log_tag) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    std::vector<std::pair<std::string, std::string>> failed;
    for (;;) {
        std::vector<teardown_delete> remaining;
        failed.clear();
        for (auto& step : steps) {
            if (auto err = step.attempt()) {
                failed.emplace_back(step.what, *err);
                remaining.push_back(std::move(step));
            }
        }
        steps = std::move(remaining);
        if (steps.empty()) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(interval);
    }
    for (const auto& [what, err] : failed) {
        std::cerr << "[" << log_tag << "] teardown: could not delete " << what << " within "
                  << budget.count() << "s (last error " << err << "); it is leaked\n";
    }
    return false;
}

}  // namespace kythira::testing::aws_real_ec2

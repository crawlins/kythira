#pragma once

/// @file alibaba_real_test_support.hpp
/// @brief Shared harness for the real-Alibaba suites: environment-driven
///        configuration, the exit-77 skip contract, a read-only preflight,
///        and signal-driven teardown.
///
/// Mirrors `oci_real_test_support.hpp`'s contract deliberately, because the
/// operational failure modes are identical across providers:
///
/// - **Exit 77 means skip.** A suite that *fails* for want of credentials
///   turns "this developer has no Alibaba account" into a red build, which is
///   how a real-cloud tier ends up permanently disabled. The fixture prints
///   `[alibaba-real] SKIP: ...` naming every missing value and exits 77.
/// - **A read-only preflight decides skip-vs-run.** `DescribeRegions` costs
///   nothing and answers "can these credentials reach the API at all?"; its
///   failure is a skip, not a failure, for the same reason.
/// - **Teardown runs on a signal, not only on a clean exit.** Every cloud
///   provider in this project leaked something on its first live run; a
///   cancelled CI job is exactly when leaks happen.
///
/// These suites are compiled whenever the Alibaba gates are on but are
/// **never registered with CTest** (spec Requirement 16.8) — they are run by
/// name, by the CI job, against a real account.

#include <raft/alibaba_client_config.hpp>
#include <raft/alibaba_http_client.hpp>

#include <boost/test/unit_test.hpp>

#include <csignal>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace alibaba_real {

/// The suites' skip code, matching `oci_quorum_manager_real_test`'s
/// convention and `aws_quorum_manager_localstack_test`'s before it.
inline constexpr int k_skip_exit_code = 77;

[[nodiscard]] inline auto env_or_empty(const char* name) -> std::string {
    const char* v = std::getenv(name);
    return (v != nullptr) ? std::string(v) : std::string{};
}

/// Everything the real suites read from the environment. The CI job exports
/// these from the STS credentials the official
/// `aliyun/configure-aliyun-credentials-action` federates.
struct real_test_config {
    std::string region;
    std::string access_key_id;
    std::string access_key_secret;
    std::string security_token;  // optional: set when using STS credentials
    std::string scaling_group_id;
    std::string oss_bucket;

    [[nodiscard]] static auto from_environment() -> real_test_config {
        return {
            .region = env_or_empty("KYTHIRA_ALIBABA_REGION"),
            .access_key_id = env_or_empty("KYTHIRA_ALIBABA_ACCESS_KEY_ID"),
            .access_key_secret = env_or_empty("KYTHIRA_ALIBABA_ACCESS_KEY_SECRET"),
            .security_token = env_or_empty("KYTHIRA_ALIBABA_SECURITY_TOKEN"),
            .scaling_group_id = env_or_empty("KYTHIRA_ALIBABA_SCALING_GROUP_ID"),
            .oss_bucket = env_or_empty("KYTHIRA_ALIBABA_OSS_BUCKET"),
        };
    }

    [[nodiscard]] auto client_config() const -> kythira::alibaba_client_config {
        kythira::alibaba_client_config cfg;
        cfg.region = region;
        cfg.access_key_id = access_key_id;
        cfg.access_key_secret = access_key_secret;
        cfg.security_token = security_token;
        return cfg;
    }
};

/// Exits 77 with a message naming every missing value. `extra` lets each
/// suite demand only the resources it actually uses, so the persistence
/// suite does not skip for want of a scaling group.
inline auto skip_unless_configured(const real_test_config& cfg,
                                   const std::vector<std::pair<const char*, std::string>>& extra)
    -> void {
    std::vector<std::string> missing;
    if (cfg.region.empty()) missing.emplace_back("KYTHIRA_ALIBABA_REGION");
    if (cfg.access_key_id.empty()) missing.emplace_back("KYTHIRA_ALIBABA_ACCESS_KEY_ID");
    if (cfg.access_key_secret.empty()) missing.emplace_back("KYTHIRA_ALIBABA_ACCESS_KEY_SECRET");
    for (const auto& [name, value] : extra) {
        if (value.empty()) missing.emplace_back(name);
    }
    if (missing.empty()) return;

    std::cerr << "[alibaba-real] SKIP: missing required environment:\n";
    for (const auto& name : missing) {
        std::cerr << "  - " << name << "\n";
    }
    std::cerr << "See scripts/ci-cloud-credentials/alibaba/README.md.\n";
    std::exit(k_skip_exit_code);
}

/// Read-only reachability probe. A failure here means the credentials cannot
/// reach the API at all, which is a skip rather than a failure — the
/// distinction that keeps an unconfigured checkout from going red.
inline auto skip_unless_reachable(const real_test_config& cfg) -> void {
    try {
        const kythira::alibaba_http_client client{cfg.client_config()};
        (void)client.rpc("ecs", "DescribeRegions", "2014-05-26", {});
    } catch (const std::exception& e) {
        std::cerr << "[alibaba-real] SKIP: preflight DescribeRegions failed: " << e.what() << "\n"
                  << "Credentials may be expired or lack ecs:DescribeRegions.\n";
        std::exit(k_skip_exit_code);
    }
}

/// Teardown callbacks run on a fatal signal as well as at scope exit. A
/// cancelled CI job is precisely when a leaked, billed instance happens.
inline auto teardown_registry() -> std::vector<std::function<void()>>& {
    static std::vector<std::function<void()>> registry;
    return registry;
}

inline auto run_teardowns() noexcept -> void {
    for (auto& fn : teardown_registry()) {
        try {
            fn();
        } catch (...) {
            // Best effort by construction: one failing teardown must not
            // prevent the rest from running.
        }
    }
    teardown_registry().clear();
}

extern "C" inline void alibaba_signal_handler(int signum) {
    std::cerr << "\n[alibaba-real] signal " << signum << " — running teardowns before exit\n";
    run_teardowns();
    std::signal(signum, SIG_DFL);
    std::raise(signum);
}

/// Installs the signal handlers once per process; declare with
/// BOOST_GLOBAL_FIXTURE in each real suite.
struct signal_handler_fixture {
    signal_handler_fixture() {
        for (const int sig : {SIGTERM, SIGINT, SIGHUP, SIGQUIT, SIGPIPE}) {
            std::signal(sig, alibaba_signal_handler);
        }
    }
    ~signal_handler_fixture() { run_teardowns(); }

    signal_handler_fixture(const signal_handler_fixture&) = delete;
    auto operator=(const signal_handler_fixture&) -> signal_handler_fixture& = delete;
    signal_handler_fixture(signal_handler_fixture&&) = delete;
    auto operator=(signal_handler_fixture&&) -> signal_handler_fixture& = delete;
};

}  // namespace alibaba_real

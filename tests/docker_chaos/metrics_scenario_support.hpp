#pragma once

/// @file metrics_scenario_support.hpp
/// @brief Shared helpers for the metrics-backend scenario tests
///        (prometheus/victoriametrics/telegraf/netdata_metrics_scenario_
///        test.cpp): compose lifecycle fixture, node health/command-submit
///        probes parameterized by the compose file's mapped control-plane
///        port, and a poll-until helper — the same shapes
///        otlp_collector_test.cpp uses, factored out because four tests
///        would otherwise carry four copies.

#include "os_faults.hpp"

#include <httplib.h>
#include <boost/json.hpp>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace metrics_scenario {

using namespace std::chrono_literals;

[[nodiscard]] inline auto compose_file(const char* env_var, const char* fallback) -> std::string {
    const char* env = std::getenv(env_var);
    if (env && *env) return env;
    return fallback;
}

[[nodiscard]] inline auto node_healthy(int http_port) -> bool {
    httplib::Client cli("localhost", http_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(3);
    auto res = cli.Get("/health");
    return res && res->status == 200;
}

/// Submits {"key":k,"value":v} to the node's HTTP control plane. A
/// single-node cluster's majority is 1, so it self-elects leader and this
/// succeeds once it has.
[[nodiscard]] inline auto submit_command(int http_port, const std::string& key,
                                         const std::string& value) -> bool {
    httplib::Client cli("localhost", http_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(5);
    boost::json::object body{{"key", key}, {"value", value}};
    auto res = cli.Post("/command", boost::json::serialize(body), "application/json");
    return res && res->status == 200;
}

/// Polls `probe` every 500 ms until it returns true or `timeout` elapses.
[[nodiscard]] inline auto poll_until(const std::function<bool()>& probe,
                                     std::chrono::milliseconds timeout) -> bool {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (probe()) return true;
        std::this_thread::sleep_for(500ms);
    }
    return false;
}

/// GET against a host-mapped agent API port; returns the body, or "" on any
/// failure — callers poll, so a transient failure just means another round.
[[nodiscard]] inline auto http_get_body(int port, const std::string& path) -> std::string {
    httplib::Client cli("localhost", port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(5);
    auto res = cli.Get(path);
    if (!res || res->status != 200) return "";
    return res->body;
}

/// Compose up/down around the test, failing hard (after a best-effort down)
/// if the node never reports healthy — identical lifecycle to
/// otlp_collector_test.cpp's fixture.
struct compose_fixture {
    std::string file;
    int node_http_port;

    compose_fixture(std::string compose_file_path, int http_port)
        : file(std::move(compose_file_path)), node_http_port(http_port) {
        docker_chaos::os::try_exec(docker_chaos::os::real_exec,
                                   docker_chaos::os::compose_down_cmd(file));
        docker_chaos::os::checked_exec(docker_chaos::os::real_exec,
                                       docker_chaos::os::compose_up_cmd(file));
        if (!poll_until([this] { return node_healthy(node_http_port); }, 60s)) {
            // Dump everything readable before tearing down — a CI failure
            // with no container logs is undebuggable from outside (the
            // repo's recurring "silent machinery" lesson).
            dump_diagnostics();
            docker_chaos::os::try_exec(docker_chaos::os::real_exec,
                                       docker_chaos::os::compose_down_cmd(file));
            BOOST_FAIL(
                "metrics scenario node did not become healthy within 60 s"
                " — container state and logs dumped above");
        }
    }

    /// Print `ps` for the compose project and the logs of every one of its
    /// containers to the test output.
    /// `compose ps` with `-a` where it is accepted, without it where it is
    /// not. **`podman-compose` rejects `-a` outright** ("unrecognized
    /// arguments: -a", exit 2), which made the whole diagnostic dump a
    /// no-op under rootless Podman — so a startup failure printed an error
    /// about `-a` instead of the container state and logs explaining why,
    /// and the actual cause was invisible. That violated CLAUDE.md's
    /// both-runtimes rule in the one code path whose entire job is
    /// explaining a failure.
    ///
    /// Falling back rather than dropping `-a` everywhere: on Docker it is
    /// what includes containers that already exited, which is precisely the
    /// case being diagnosed.
    [[nodiscard]] auto compose_ps(const std::vector<std::string>& extra) const
        -> docker_chaos::os::CmdResult {
        auto with_a = docker_chaos::os::compose_prefix();
        with_a.insert(with_a.end(), {"-f", file, "ps", "-a"});
        with_a.insert(with_a.end(), extra.begin(), extra.end());
        auto result = docker_chaos::os::real_exec(with_a);
        if (result.code == 0) return result;

        auto without_a = docker_chaos::os::compose_prefix();
        without_a.insert(without_a.end(), {"-f", file, "ps"});
        without_a.insert(without_a.end(), extra.begin(), extra.end());
        return docker_chaos::os::real_exec(without_a);
    }

    auto dump_diagnostics() const -> void {
        auto ps = compose_ps({});
        BOOST_TEST_MESSAGE("compose ps:\n" << ps.out);

        auto ids = compose_ps({"-q"});
        std::size_t start = 0;
        while (start < ids.out.size()) {
            auto end = ids.out.find('\n', start);
            if (end == std::string::npos) end = ids.out.size();
            auto id = ids.out.substr(start, end - start);
            start = end + 1;
            if (id.empty()) continue;
            auto logs =
                docker_chaos::os::real_exec({docker_chaos::os::container_runtime(), "logs", id});
            BOOST_TEST_MESSAGE("logs for container " << id << ":\n" << logs.out);
        }
    }

    ~compose_fixture() {
        try {
            docker_chaos::os::real_exec(docker_chaos::os::compose_down_cmd(file));
        } catch (...) {
        }
    }

    compose_fixture(const compose_fixture&) = delete;
    auto operator=(const compose_fixture&) -> compose_fixture& = delete;
    compose_fixture(compose_fixture&&) = delete;
    auto operator=(compose_fixture&&) -> compose_fixture& = delete;

    /// Drive real Raft activity so the backend has something to emit.
    auto drive_activity(const char* key_prefix) const -> void {
        if (!poll_until([&] { return submit_command(node_http_port, key_prefix, "value"); }, 30s)) {
            dump_diagnostics();
            BOOST_FAIL(
                "node never became leader / accepted a command within 30 s"
                " — container state and logs dumped above");
        }
    }

    /// Poll `probe` and, on timeout, dump container diagnostics plus the
    /// caller's own last-observed evidence (query body, output-file tail,
    /// ...) before failing — an assertion failure with no agent logs is as
    /// undebuggable as the health timeout was.
    auto require_eventually(const std::function<bool()>& probe, std::chrono::milliseconds timeout,
                            const std::function<std::string()>& evidence, const char* message) const
        -> void {
        if (poll_until(probe, timeout)) return;
        dump_diagnostics();
        BOOST_TEST_MESSAGE("last observed evidence:\n" << evidence());
        BOOST_FAIL(std::string(message) + " — container state, logs, and evidence dumped above");
    }
};

}  // namespace metrics_scenario

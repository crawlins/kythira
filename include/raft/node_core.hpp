// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file node_core.hpp
/// @brief What every Kythira node shares, whichever services it runs
///        (.kiro/specs/service-colocation/ task 2, Requirements 2.2, 2.4,
///        2.5, 8.1 and 8.3).
///
/// - `node_config`: the node-wide `KYTHIRA_NODE_*` settings (identity,
///   peers, Raft listener, TLS, data directory, metrics listener).
///   A stand-alone daemon passes its legacy names as aliases
///   (`with_aliases`), so `KYTHIRA_DYNAMODB_DATA_DIR` keeps working where
///   `KYTHIRA_NODE_DATA_DIR` is unset.
/// - `module_log`: one log sink for the node, with each line naming the
///   service that wrote it.
/// - `node_runner<Host>`: drives a `module_plan` (`module_registry.hpp`)
///   through its lifecycle. It configures every module and reports every
///   bad setting at once, makes each module's data subdirectory, registers
///   groups, publishes every interface, then starts the modules in plan
///   order. At shutdown it drains every module (newest first), stops them
///   (newest first), and only then stops the host, so requests in flight
///   finish while Raft still runs. A module that fails to start stops
///   the ones already started.
/// - Health and metrics rendering for the node's shared endpoint
///   (`node_endpoint.hpp` serves them).
/// - `wait_for_stop_signal`: blocks until SIGINT or SIGTERM.

#include <raft/module_registry.hpp>
#include <raft/service_module.hpp>

#include <csignal>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <pthread.h>

namespace kythira::node {

/// One Raft peer: its node id and Raft address.
struct node_peer {
    std::string id;
    std::string host;
    std::uint16_t port = 0;
};

/// The node-wide settings (Requirement 8.1). Each module reads its own
/// under its own prefix.
struct node_config {
    /// `KYTHIRA_NODE_ID`: this node's id, as the host's node id kind
    /// spells it.
    std::string node_id;
    /// `KYTHIRA_NODE_PEERS`: `id=host:port,...`, this node included. Empty
    /// runs a single node.
    std::vector<node_peer> peers;
    /// `KYTHIRA_NODE_RAFT_BIND` and `_RAFT_PORT`: the one Raft listener
    /// every module's groups share.
    std::string raft_bind = "0.0.0.0";
    std::uint16_t raft_port = 7400;
    /// `KYTHIRA_NODE_TLS_CERT`, `_TLS_KEY`, `_TLS_CA`: the node's one
    /// identity, for Raft and for every module's listeners unless a module
    /// is given its own.
    std::string tls_cert;
    std::string tls_key;
    std::string tls_ca;
    /// `KYTHIRA_NODE_INSECURE_PLAINTEXT=1`: no TLS anywhere (tests only).
    bool insecure_plaintext = false;
    /// `KYTHIRA_NODE_DATA_DIR`: each module keeps its files in its own
    /// subdirectory (`service_module::data_subdir`).
    std::filesystem::path data_dir;
    /// `KYTHIRA_NODE_METRICS_BIND` and `_METRICS_PORT`: `/healthz` and
    /// `/metrics` for every module. Port 0 picks a free one.
    std::string metrics_bind = "127.0.0.1";
    std::uint16_t metrics_port = 9400;
    /// `KYTHIRA_NODE_DRAIN_SECS`: how long a module may take to drain.
    std::uint32_t drain_seconds = 30;
};

/// `env`, with each name in `aliases` (new name to legacy name) falling
/// back to its legacy name when unset. A stand-alone daemon passes its
/// old variable names here (Requirement 3.2).
[[nodiscard]] inline auto with_aliases(env_lookup env, std::map<std::string, std::string> aliases)
    -> env_lookup {
    return [env = std::move(env),
            aliases = std::move(aliases)](std::string_view name) -> std::optional<std::string> {
        if (auto v = env(name)) {
            return v;
        }
        const auto it = aliases.find(std::string(name));
        return it == aliases.end() ? std::nullopt : env(it->second);
    };
}

namespace node_detail {

[[nodiscard]] inline auto parse_port(std::string_view text) -> std::optional<std::uint16_t> {
    if (text.empty() || text.size() > 5) {
        return std::nullopt;
    }
    std::uint32_t n = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        n = n * 10 + static_cast<std::uint32_t>(c - '0');
    }
    if (n > 65535) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(n);
}

/// `host:port` or `[v6]:port`.
[[nodiscard]] inline auto parse_address(std::string_view text)
    -> std::optional<std::pair<std::string, std::uint16_t>> {
    std::string_view host;
    std::string_view port;
    if (text.starts_with('[')) {
        const auto close = text.find("]:");
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        host = text.substr(1, close - 1);
        port = text.substr(close + 2);
    } else {
        const auto colon = text.rfind(':');
        if (colon == std::string_view::npos) {
            return std::nullopt;
        }
        host = text.substr(0, colon);
        port = text.substr(colon + 1);
    }
    const auto p = parse_port(port);
    if (host.empty() || !p || *p == 0) {
        return std::nullopt;
    }
    return std::pair{std::string(host), *p};
}

}  // namespace node_detail

/// Reads the `KYTHIRA_NODE_*` settings.
/// @returns the settings, or the first wrong one and why.
[[nodiscard]] inline auto node_config_from_env(const env_lookup& env)
    -> std::variant<node_config, std::string> {
    namespace nd = node_detail;
    node_config c;
    auto id = env("KYTHIRA_NODE_ID");
    if (!id) {
        return std::string("KYTHIRA_NODE_ID is required");
    }
    c.node_id = *id;
    auto dir = env("KYTHIRA_NODE_DATA_DIR");
    if (!dir) {
        return std::string("KYTHIRA_NODE_DATA_DIR is required");
    }
    c.data_dir = *dir;
    if (auto v = env("KYTHIRA_NODE_PEERS")) {
        bool self = false;
        for (const auto& item : split_services(*v)) {
            const auto eq = item.find('=');
            const auto addr = eq == std::string::npos
                                  ? std::nullopt
                                  : nd::parse_address(std::string_view(item).substr(eq + 1));
            if (eq == 0 || !addr) {
                return "KYTHIRA_NODE_PEERS: expected id=host:port, got '" + item + "'";
            }
            node_peer p{item.substr(0, eq), addr->first, addr->second};
            for (const auto& q : c.peers) {
                if (q.id == p.id) {
                    return "KYTHIRA_NODE_PEERS: node " + p.id + " is listed twice";
                }
            }
            self = self || p.id == c.node_id;
            c.peers.push_back(std::move(p));
        }
        if (!self) {
            return "KYTHIRA_NODE_PEERS does not list this node (" + c.node_id + ")";
        }
    }
    c.raft_bind = env("KYTHIRA_NODE_RAFT_BIND").value_or(c.raft_bind);
    if (auto v = env("KYTHIRA_NODE_RAFT_PORT")) {
        const auto p = nd::parse_port(*v);
        if (!p || *p == 0) {
            return "KYTHIRA_NODE_RAFT_PORT: not a port: '" + *v + "'";
        }
        c.raft_port = *p;
    }
    c.metrics_bind = env("KYTHIRA_NODE_METRICS_BIND").value_or(c.metrics_bind);
    if (auto v = env("KYTHIRA_NODE_METRICS_PORT")) {
        const auto p = nd::parse_port(*v);
        if (!p) {
            return "KYTHIRA_NODE_METRICS_PORT: not a port: '" + *v + "'";
        }
        c.metrics_port = *p;
    }
    if (auto v = env("KYTHIRA_NODE_INSECURE_PLAINTEXT")) {
        if (*v != "0" && *v != "1") {
            return "KYTHIRA_NODE_INSECURE_PLAINTEXT: expected 0 or 1, got '" + *v + "'";
        }
        c.insecure_plaintext = *v == "1";
    }
    c.tls_cert = env("KYTHIRA_NODE_TLS_CERT").value_or("");
    c.tls_key = env("KYTHIRA_NODE_TLS_KEY").value_or("");
    c.tls_ca = env("KYTHIRA_NODE_TLS_CA").value_or("");
    if (!c.insecure_plaintext && (c.tls_cert.empty() || c.tls_key.empty() || c.tls_ca.empty())) {
        return std::string(
            "KYTHIRA_NODE_TLS_CERT, _TLS_KEY and _TLS_CA are required "
            "(or KYTHIRA_NODE_INSECURE_PLAINTEXT=1 for tests)");
    }
    if (auto v = env("KYTHIRA_NODE_DRAIN_SECS")) {
        const auto p = nd::parse_port(*v);
        if (!p || *p == 0 || *p > 3600) {
            return "KYTHIRA_NODE_DRAIN_SECS: expected 1 to 3600, got '" + *v + "'";
        }
        c.drain_seconds = *p;
    }
    return c;
}

/// The `KYTHIRA_NODE_*` lines of `--help`.
[[nodiscard]] inline auto node_config_usage() -> std::string {
    return "  KYTHIRA_NODE_ID                  this node's id (required)\n"
           "  KYTHIRA_NODE_DATA_DIR            data directory; each service keeps a\n"
           "                                   subdirectory (required)\n"
           "  KYTHIRA_NODE_PEERS               id=host:port,... of every node's Raft\n"
           "                                   listener, this one included\n"
           "  KYTHIRA_NODE_RAFT_BIND           Raft bind address            [0.0.0.0]\n"
           "  KYTHIRA_NODE_RAFT_PORT           Raft port                    [7400]\n"
           "  KYTHIRA_NODE_TLS_CERT, _TLS_KEY, _TLS_CA\n"
           "                                   the node's certificate, key and CA bundle\n"
           "  KYTHIRA_NODE_INSECURE_PLAINTEXT  1: no TLS (tests only)       [0]\n"
           "  KYTHIRA_NODE_METRICS_BIND        /healthz and /metrics bind   [127.0.0.1]\n"
           "  KYTHIRA_NODE_METRICS_PORT        their port; 0 picks one      [9400]\n"
           "  KYTHIRA_NODE_DRAIN_SECS          time a service may take to drain [30]\n";
}

/// The node's one log sink: `(error, line)`.
using log_sink = std::function<void(bool, std::string_view)>;

/// A sink that writes each line as `service=<name> <line>` to `node`, so
/// one log carries every module's lines and says whose each is
/// (Requirement 8.3).
[[nodiscard]] inline auto module_log(const log_sink& node, std::string service) -> log_sink {
    return [node, service = std::move(service)](bool error, std::string_view line) {
        if (node) {
            node(error, "service=" + service + " " + std::string(line));
        }
    };
}

/// A module's health as the node reports it.
struct named_health {
    std::string module;
    module_health health;
};

/// Runs a plan's modules on a host.
template<typename Host> class node_runner {
public:
    node_runner(module_plan<Host> plan, node_config config, log_sink log = {})
        : _plan(std::move(plan)), _config(std::move(config)), _log(std::move(log)) {}

    node_runner(const node_runner&) = delete;
    auto operator=(const node_runner&) -> node_runner& = delete;
    node_runner(node_runner&&) = delete;
    auto operator=(node_runner&&) -> node_runner& = delete;
    ~node_runner() { stop(); }

    /// Configures, registers, publishes and starts every module.
    /// @throws std::invalid_argument naming every bad setting, before any
    ///         module touches the host.
    /// @throws std::runtime_error naming the module that failed to start;
    ///         the modules started before it are drained and stopped.
    void start(Host& host, const env_lookup& env) {
        std::string errors;
        for (auto& m : _plan.modules) {
            if (auto e = m->configure(env)) {
                errors += (errors.empty() ? "" : "; ") + std::string(m->name()) + ": " + *e;
            }
        }
        if (!errors.empty()) {
            throw std::invalid_argument(errors);
        }
        for (auto& m : _plan.modules) {
            std::filesystem::create_directories(_config.data_dir / m->data_subdir());
        }
        for (std::size_t i = 0; i < _plan.modules.size(); ++i) {
            _plan.modules[i]->register_groups(host, _plan.spaces[i]);
        }
        for (auto& m : _plan.modules) {
            m->publish(_registry);
        }
        for (auto& m : _plan.modules) {
            try {
                m->start(_registry);
            } catch (const std::exception& e) {
                const std::string name(m->name());
                log(true, name, std::string("failed to start: ") + e.what());
                shut_down_started();
                throw std::runtime_error(name + " failed to start: " + e.what());
            }
            log(false, std::string(m->name()), "started");
            ++_started;
        }
    }

    /// Drains then stops every started module, newest first, then calls
    /// `stop_host`: requests in flight finish while Raft still runs
    /// (Requirement 2.5). Safe to call twice.
    void stop(const std::function<void()>& stop_host = {}) {
        shut_down_started();
        if (stop_host && !_host_stopped) {
            _host_stopped = true;
            stop_host();
        }
    }

    [[nodiscard]] auto registry() -> module_registry& { return _registry; }
    [[nodiscard]] auto config() const -> const node_config& { return _config; }

    /// Every module's health; a module not started reports unhealthy.
    [[nodiscard]] auto health() const -> std::vector<named_health> {
        std::vector<named_health> out;
        for (std::size_t i = 0; i < _plan.modules.size(); ++i) {
            const auto& m = _plan.modules[i];
            out.push_back({std::string(m->name()),
                           i < _started ? m->health() : module_health{false, "not started"}});
        }
        return out;
    }

    /// `/healthz`: 200 with every module healthy, else 503, and a JSON body
    /// naming each module's state.
    [[nodiscard]] auto health_report() const -> std::pair<unsigned, std::string> {
        const auto all = health();
        bool ok = !all.empty();
        std::string body = R"({"node_id":")" + json_escape(_config.node_id) + R"(","services":{)";
        for (std::size_t i = 0; i < all.size(); ++i) {
            ok = ok && all[i].health.healthy;
            body += (i == 0 ? "" : ",") + std::string("\"") + json_escape(all[i].module) +
                    R"(":{"healthy":)" + (all[i].health.healthy ? "true" : "false");
            if (!all[i].health.detail.empty()) {
                body += R"(,"detail":")" + json_escape(all[i].health.detail) + "\"";
            }
            body += "}";
        }
        body += R"(},"healthy":)" + std::string(ok ? "true" : "false") + "}";
        return {ok ? 200U : 503U, body};
    }

    /// `/metrics` in the Prometheus text format: each module's metrics
    /// under `kythira_<module>_`, and `kythira_node_service_up` per module.
    [[nodiscard]] auto metrics_text() const -> std::string {
        std::ostringstream out;
        const auto all = health();
        out << "# TYPE kythira_node_service_up gauge\n";
        for (const auto& h : all) {
            out << R"(kythira_node_service_up{service=")" << h.module << "\"} "
                << (h.health.healthy ? 1 : 0) << '\n';
        }
        for (std::size_t i = 0; i < _plan.modules.size() && i < _started; ++i) {
            const auto& m = _plan.modules[i];
            for (const auto& metric : m->metrics()) {
                out << "kythira_" << m->name() << '_' << metric.name;
                if (!metric.labels.empty()) {
                    out << '{';
                    for (std::size_t j = 0; j < metric.labels.size(); ++j) {
                        out << (j == 0 ? "" : ",") << metric.labels[j].first << "=\""
                            << label_escape(metric.labels[j].second) << '"';
                    }
                    out << '}';
                }
                out << ' ' << metric.value << '\n';
            }
        }
        return out.str();
    }

private:
    void shut_down_started() {
        for (std::size_t i = _started; i-- > 0;) {
            guarded(*_plan.modules[i], "drain", [](service_module<Host>& m) { m.drain(); });
        }
        for (std::size_t i = _started; i-- > 0;) {
            guarded(*_plan.modules[i], "stop", [](service_module<Host>& m) { m.stop(); });
            log(false, std::string(_plan.modules[i]->name()), "stopped");
        }
        _started = 0;
    }

    /// One module's drain or stop: a failure is logged and the rest go on,
    /// so one module cannot keep the others' data unflushed.
    template<typename F> void guarded(service_module<Host>& m, std::string_view what, F f) {
        try {
            f(m);
        } catch (const std::exception& e) {
            log(true, std::string(m.name()), std::string(what) + " failed: " + e.what());
        }
    }

    void log(bool error, const std::string& service, const std::string& line) const {
        if (_log) {
            _log(error, "service=" + service + " " + line);
        }
    }

    [[nodiscard]] static auto json_escape(std::string_view s) -> std::string {
        std::string out;
        for (const char c : s) {
            if (c == '"' || c == '\\') {
                out += '\\';
                out += c;
            } else if (static_cast<unsigned char>(c) < 0x20) {
                constexpr std::string_view hex = "0123456789abcdef";
                out += "\\u00";
                out += hex[(static_cast<unsigned char>(c) >> 4) & 0xf];
                out += hex[static_cast<unsigned char>(c) & 0xf];
            } else {
                out += c;
            }
        }
        return out;
    }

    [[nodiscard]] static auto label_escape(std::string_view s) -> std::string {
        std::string out;
        for (const char c : s) {
            if (c == '"' || c == '\\') {
                out += '\\';
                out += c;
            } else if (c == '\n') {
                out += "\\n";
            } else {
                out += c;
            }
        }
        return out;
    }

    module_plan<Host> _plan;
    node_config _config;
    log_sink _log;
    module_registry _registry;
    std::size_t _started = 0;
    bool _host_stopped = false;
};

/// Blocks SIGINT and SIGTERM in the calling thread (call it first in
/// main, before any thread starts, so every thread inherits the mask),
/// then waits for one. @returns the signal number.
[[nodiscard]] inline auto wait_for_stop_signal() -> int {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    int sig = 0;
    sigwait(&set, &sig);
    return sig;
}

/// Blocks SIGINT and SIGTERM for the calling thread and the threads it
/// starts afterwards, so `wait_for_stop_signal` receives them.
inline void block_stop_signals() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
}

}  // namespace kythira::node

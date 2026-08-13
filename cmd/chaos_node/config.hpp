#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace chaos_node {

struct peer_info {
    std::uint64_t node_id;
    std::string host;
    std::uint16_t port;
};

struct node_config {
    std::uint64_t node_id{0};
    std::string rpc_address{"0.0.0.0"};
    std::uint16_t rpc_port{7000};
    std::uint16_t http_port{8080};
    std::uint16_t fiu_port{9000};
    std::string data_dir{"/var/lib/chaos_node"};
    std::vector<peer_info> peers;

    std::chrono::milliseconds election_timeout_min{150};
    std::chrono::milliseconds election_timeout_max{300};
    std::chrono::milliseconds heartbeat_interval{50};

    // OTLP telemetry backend (.kiro/specs/otlp-telemetry-backend/,
    // Requirement 5.2). All optional — chaos_node falls back to
    // console_logger/noop_metrics when OTLP_ENDPOINT is unset.
    std::optional<std::string> otlp_endpoint;
    std::vector<std::pair<std::string, std::string>> otlp_headers;
    std::string otlp_service_name{"kythira-chaos-node"};

    // Self-hosted metrics backends (doc/TODO.md "Metrics Backends"). Same
    // opt-in pattern as OTLP: each is enabled by its env var being set, and
    // at most one metrics backend (including OTLP) may be selected per run —
    // main.cpp enforces the precedence and prints which one won.
    std::optional<std::uint16_t> prometheus_metrics_port;  ///< PROMETHEUS_METRICS_PORT
    std::optional<std::string> victoriametrics_endpoint;   ///< VICTORIAMETRICS_ENDPOINT
    std::optional<std::pair<std::string, std::uint16_t>> telegraf_endpoint;  ///< TELEGRAF_ENDPOINT
    bool telegraf_tcp{false};  ///< TELEGRAF_PROTOCOL=tcp (default udp)
    std::optional<std::pair<std::string, std::uint16_t>>
        netdata_endpoint;  ///< NETDATA_STATSD_ENDPOINT

    // Parse from environment variables.
    // Throws std::invalid_argument if NODE_ID or PEERS are missing / malformed.
    static auto from_env() -> node_config {
        node_config cfg;

        auto get = [](const char* name, const char* def = nullptr) -> std::string {
            const char* v = ::getenv(name);
            if (v) return v;
            if (def) return def;
            throw std::invalid_argument(std::string("chaos_node: missing required env var ") +
                                        name);
        };

        auto get_opt = [](const char* name, const char* def) -> std::string {
            const char* v = ::getenv(name);
            return v ? v : def;
        };

        cfg.node_id = std::stoull(get("NODE_ID"));
        cfg.rpc_port = static_cast<std::uint16_t>(std::stoul(get_opt("RPC_PORT", "7000")));
        cfg.http_port = static_cast<std::uint16_t>(std::stoul(get_opt("HTTP_PORT", "8080")));
        cfg.fiu_port = static_cast<std::uint16_t>(std::stoul(get_opt("FIU_PORT", "9000")));
        cfg.data_dir = get_opt("DATA_DIR", "/var/lib/chaos_node");

        cfg.election_timeout_min =
            std::chrono::milliseconds(std::stoll(get_opt("ELECTION_TIMEOUT_MIN_MS", "150")));
        cfg.election_timeout_max =
            std::chrono::milliseconds(std::stoll(get_opt("ELECTION_TIMEOUT_MAX_MS", "300")));
        cfg.heartbeat_interval =
            std::chrono::milliseconds(std::stoll(get_opt("HEARTBEAT_INTERVAL_MS", "50")));

        // PEERS = "id:host:port,id:host:port,..."
        std::string peers_str = get("PEERS");
        std::string tok;
        for (char c : peers_str + ',') {
            if (c == ',') {
                if (!tok.empty()) {
                    auto c1 = tok.find(':');
                    auto c2 = tok.find(':', c1 + 1);
                    if (c1 == std::string::npos || c2 == std::string::npos)
                        throw std::invalid_argument("chaos_node: malformed PEERS token: " + tok);
                    peer_info p;
                    p.node_id = std::stoull(tok.substr(0, c1));
                    p.host = tok.substr(c1 + 1, c2 - c1 - 1);
                    p.port = static_cast<std::uint16_t>(std::stoul(tok.substr(c2 + 1)));
                    cfg.peers.push_back(p);
                    tok.clear();
                }
            } else {
                tok += c;
            }
        }

        if (cfg.node_id == 0)
            throw std::invalid_argument("chaos_node: NODE_ID must be a positive integer");

        // OTLP_ENDPOINT — unset/empty means OTLP support stays off.
        if (std::string otlp_endpoint_str = get_opt("OTLP_ENDPOINT", "");
            !otlp_endpoint_str.empty())
            cfg.otlp_endpoint = std::move(otlp_endpoint_str);

        cfg.otlp_service_name = get_opt("OTLP_SERVICE_NAME", "kythira-chaos-node");

        // OTLP_HEADERS = "key=value,key=value,..."
        std::string headers_str = get_opt("OTLP_HEADERS", "");
        std::string header_tok;
        for (char c : headers_str + ',') {
            if (c == ',') {
                if (!header_tok.empty()) {
                    auto eq = header_tok.find('=');
                    if (eq == std::string::npos)
                        throw std::invalid_argument("chaos_node: malformed OTLP_HEADERS token: " +
                                                    header_tok);
                    cfg.otlp_headers.emplace_back(header_tok.substr(0, eq),
                                                  header_tok.substr(eq + 1));
                    header_tok.clear();
                }
            } else {
                header_tok += c;
            }
        }

        // Self-hosted metrics backends — unset/empty means off, same as
        // OTLP_ENDPOINT above. host:port values split on the LAST ':' so a
        // bracketed IPv6 literal's colons don't confuse the parse.
        auto parse_host_port = [](const std::string& value,
                                  const char* var) -> std::pair<std::string, std::uint16_t> {
            auto colon = value.rfind(':');
            if (colon == std::string::npos || colon == 0 || colon + 1 == value.size()) {
                throw std::invalid_argument(std::string("chaos_node: malformed ") + var +
                                            " (want host:port): " + value);
            }
            return {value.substr(0, colon),
                    static_cast<std::uint16_t>(std::stoul(value.substr(colon + 1)))};
        };

        if (std::string port_str = get_opt("PROMETHEUS_METRICS_PORT", ""); !port_str.empty()) {
            cfg.prometheus_metrics_port = static_cast<std::uint16_t>(std::stoul(port_str));
        }

        if (std::string vm_str = get_opt("VICTORIAMETRICS_ENDPOINT", ""); !vm_str.empty()) {
            cfg.victoriametrics_endpoint = std::move(vm_str);
        }

        if (std::string tg_str = get_opt("TELEGRAF_ENDPOINT", ""); !tg_str.empty()) {
            cfg.telegraf_endpoint = parse_host_port(tg_str, "TELEGRAF_ENDPOINT");
        }

        std::string tg_proto = get_opt("TELEGRAF_PROTOCOL", "udp");
        if (tg_proto != "udp" && tg_proto != "tcp") {
            throw std::invalid_argument("chaos_node: TELEGRAF_PROTOCOL must be udp or tcp, got: " +
                                        tg_proto);
        }
        cfg.telegraf_tcp = (tg_proto == "tcp");

        if (std::string nd_str = get_opt("NETDATA_STATSD_ENDPOINT", ""); !nd_str.empty()) {
            cfg.netdata_endpoint = parse_host_port(nd_str, "NETDATA_STATSD_ENDPOINT");
        }

        return cfg;
    }

    // All node IDs in the cluster (self + peers).
    auto all_node_ids() const -> std::vector<std::uint64_t> {
        std::vector<std::uint64_t> ids;
        ids.push_back(node_id);
        for (const auto& p : peers) ids.push_back(p.node_id);
        return ids;
    }
};

}  // namespace chaos_node

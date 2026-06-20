#pragma once

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
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

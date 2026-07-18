#pragma once

// docker_quorum_manager — quorum_manager implementation that drives the Docker
// Engine REST API to provision and decommission kythira containers.
//
// Uses a single placement group; all containers belong to the same failure
// domain.  Communicates with the Docker daemon through the Unix domain socket
// at /var/run/docker.sock (or a configurable TCP endpoint) via cpp-httplib.
//
// Containers are named kythira-{cluster_name}-{node_id} and labelled with
// kythira.cluster and kythira.node_id so the manager can enumerate them with
// a single filtered GET /containers/json call independently of any in-process
// state.

#include <raft/future.hpp>
#include <raft/quorum_management.hpp>

#include <httplib.h>
#include <boost/json.hpp>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <vector>

namespace kythira {

// ============================================================================
// Configuration (Req 18 AC 1-2)
// ============================================================================

struct docker_quorum_manager_config {
    // Docker daemon endpoint.
    // "unix:///var/run/docker.sock" → Unix domain socket.
    // "http://host:port"            → TCP.
    std::string daemon_url{"unix:///var/run/docker.sock"};

    // Required fields (validated at construction time)
    std::string image;         // Image to use for new containers
    std::string cluster_name;  // Scopes labels and container names
    std::string network_name;  // Docker network containers are attached to

    // Optional with defaults
    std::uint16_t node_port{7000};
    std::string group_id{"default"};
    std::size_t target_count{3};
    std::chrono::milliseconds api_timeout{5000};

    std::vector<std::string> extra_env;   // Additional KEY=VALUE vars injected
    std::vector<std::string> extra_args;  // Additional CMD arguments
};

// ============================================================================
// docker_quorum_manager (Req 18 AC 3-4)
// ============================================================================

template<typename NodeId = std::uint64_t, typename Address = std::string>
requires node_id<NodeId>
class docker_quorum_manager {
public:
    using node_id_type = NodeId;
    using address_type = Address;
    using placement_group_id_type = std::string;

    explicit docker_quorum_manager(docker_quorum_manager_config cfg) : _cfg(std::move(cfg)) {
        // Req 18 AC 2 — validate required fields
        if (_cfg.image.empty()) {
            throw std::invalid_argument("docker_quorum_manager: image must be non-empty");
        }
        if (_cfg.cluster_name.empty()) {
            throw std::invalid_argument("docker_quorum_manager: cluster_name must be non-empty");
        }
        if (_cfg.network_name.empty()) {
            throw std::invalid_argument("docker_quorum_manager: network_name must be non-empty");
        }
        if (_cfg.target_count < 1) {
            throw std::invalid_argument("docker_quorum_manager: target_count must be >= 1");
        }
        if (_cfg.node_port == 0) {
            throw std::invalid_argument("docker_quorum_manager: node_port must be non-zero");
        }
    }

    // ── assess_quorum (Req 18 AC 7-10) ───────────────────────────────────────

    auto assess_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::Future<quorum_health<NodeId, std::string>> {
        try {
            auto cli = make_client();

            std::vector<NodeId> unreachable;
            std::size_t live = 0;

            for (const auto& np : cluster) {
                auto name = container_name(np.node_id);
                auto path = "/containers/" + name + "/json";
                auto res = cli->Get(path);

                bool is_live = false;
                if (res && res->status == 200) {
                    auto jv = boost::json::parse(res->body);
                    auto& obj = jv.as_object();
                    if (obj.contains("State")) {
                        auto& state = obj["State"].as_object();
                        if (state.contains("Status")) {
                            is_live = (state["Status"].as_string() == "running");
                        }
                    }
                }

                if (is_live) {
                    ++live;
                } else {
                    unreachable.push_back(np.node_id);
                }
            }

            std::size_t total = cluster.size();
            quorum_status status = compute_status(live, total);

            placement_group_health<NodeId, std::string> grp{
                .group_id = _cfg.group_id,
                .live_count = live,
                .target_count = _cfg.target_count,
                .unreachable_nodes = unreachable,
            };

            quorum_health<NodeId, std::string> report{
                .status = status,
                .live_node_count = live,
                .total_node_count = total,
                .unreachable_nodes = unreachable,
                .groups = {grp},
            };
            return FutureFactory::makeFuture(std::move(report));
        } catch (const std::exception& ex) {
            return FutureFactory::makeExceptionalFuture<quorum_health<NodeId, std::string>>(
                std::runtime_error(std::string("docker_quorum_manager::assess_quorum: ") +
                                   ex.what()));
        }
    }

    // ── provision_node (Req 18 AC 11-15) ─────────────────────────────────────

    auto provision_node(std::string /*target_group*/, std::optional<NodeId> replacing)
        -> kythira::Future<peer_info<NodeId, Address>> {
        try {
            auto cli = make_client();
            NodeId new_id = next_node_id(*cli);
            auto name = container_name(new_id);

            if (replacing.has_value()) {
                // Log hint only; single-group implementation ignores it otherwise
                (void)replacing;
            }

            // Build container-create body
            boost::json::object body;
            body["Image"] = _cfg.image;

            boost::json::object labels;
            labels["kythira.cluster"] = _cfg.cluster_name;
            labels["kythira.node_id"] = node_id_label(new_id);
            body["Labels"] = labels;

            // Environment
            boost::json::array env;
            env.emplace_back("KYTHIRA_NODE_ID=" + node_id_label(new_id));
            env.emplace_back("KYTHIRA_NODE_PORT=" + std::to_string(_cfg.node_port));
            env.emplace_back("KYTHIRA_CLUSTER=" + _cfg.cluster_name);
            for (const auto& e : _cfg.extra_env) {
                env.emplace_back(e);
            }
            body["Env"] = env;

            // Extra command arguments
            if (!_cfg.extra_args.empty()) {
                boost::json::array cmd;
                for (const auto& a : _cfg.extra_args) {
                    cmd.emplace_back(a);
                }
                body["Cmd"] = cmd;
            }

            // Attach to Docker network
            boost::json::object ep_config;
            ep_config[_cfg.network_name] = boost::json::object{};
            boost::json::object networking;
            networking["EndpointsConfig"] = ep_config;
            body["NetworkingConfig"] = networking;

            auto serialized = boost::json::serialize(body);
            auto create_path = "/containers/create?name=" + name;
            auto create_res = cli->Post(create_path, serialized, "application/json");

            if (!create_res || create_res->status < 200 || create_res->status >= 300) {
                auto msg =
                    create_res
                        ? ("HTTP " + std::to_string(create_res->status) + ": " + create_res->body)
                        : "connection failed";
                // Attempt cleanup of any partially-created container (Req 18 AC 14)
                try_remove(*cli, name);
                return FutureFactory::makeExceptionalFuture<peer_info<NodeId, Address>>(
                    std::runtime_error("docker_quorum_manager: create failed: " + msg));
            }

            auto start_path = "/containers/" + name + "/start";
            auto start_res = cli->Post(start_path, "", "application/json");

            if (!start_res || (start_res->status != 204 && start_res->status != 200)) {
                auto msg =
                    start_res
                        ? ("HTTP " + std::to_string(start_res->status) + ": " + start_res->body)
                        : "connection failed";
                try_remove(*cli, name);
                return FutureFactory::makeExceptionalFuture<peer_info<NodeId, Address>>(
                    std::runtime_error("docker_quorum_manager: start failed: " + msg));
            }

            // Address: container hostname resolves via Docker's embedded DNS
            Address addr = static_cast<Address>(name + ":" + std::to_string(_cfg.node_port));
            return FutureFactory::makeFuture(peer_info<NodeId, Address>{new_id, addr});

        } catch (const std::exception& ex) {
            return FutureFactory::makeExceptionalFuture<peer_info<NodeId, Address>>(
                std::runtime_error(std::string("docker_quorum_manager::provision_node: ") +
                                   ex.what()));
        }
    }

    // ── decommission_node (Req 18 AC 16-18) ──────────────────────────────────

    auto decommission_node(const NodeId& node) -> kythira::Future<void> {
        try {
            auto cli = make_client();
            auto name = container_name(node);
            auto path = "/containers/" + name + "?force=true";
            auto res = cli->Delete(path);

            if (res && res->status == 404) {
                // Idempotent — container already gone (Req 18 AC 17)
                return FutureFactory::makeFuture();
            }
            if (!res || res->status < 200 || res->status >= 300) {
                auto msg = res ? ("HTTP " + std::to_string(res->status) + ": " + res->body)
                               : "connection failed";
                return FutureFactory::makeExceptionalFuture<void>(
                    std::runtime_error("docker_quorum_manager: decommission failed: " + msg));
            }
            return FutureFactory::makeFuture();
        } catch (const std::exception& ex) {
            return FutureFactory::makeExceptionalFuture<void>(std::runtime_error(
                std::string("docker_quorum_manager::decommission_node: ") + ex.what()));
        }
    }

    // ── topology (Req 18 AC 19) ───────────────────────────────────────────────

    [[nodiscard]] auto topology() const -> desired_topology<std::string> {
        return desired_topology<std::string>{
            .groups = {{.group_id = _cfg.group_id, .target_count = _cfg.target_count}},
        };
    }

    // ── maintain_quorum (Req 19.1) ────────────────────────────────────────────

    auto maintain_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::Future<quorum_health<NodeId, std::string>> {
        quorum_health<NodeId, std::string> health;
        try {
            health = assess_quorum(cluster).get();
        } catch (...) {
            return FutureFactory::makeExceptionalFuture<quorum_health<NodeId, std::string>>(
                std::current_exception());
        }

        std::map<std::string, NodeId> last_replaced;
        for (const auto& nid : health.unreachable_nodes) {
            std::string grp = _cfg.group_id;
            for (const auto& np : cluster) {
                if (np.node_id == nid) {
                    grp = np.group_id;
                    break;
                }
            }
            try {
                decommission_node(nid).get();
                last_replaced[grp] = nid;
            } catch (const std::exception& ex) {
                std::cerr << "[docker_quorum_manager::maintain_quorum] decommission failed: "
                          << ex.what() << "\n";
            }
        }

        auto topo = topology();
        for (const auto& gt : topo.groups) {
            std::size_t live = 0;
            for (const auto& gh : health.groups) {
                if (gh.group_id == gt.group_id) {
                    live = gh.live_count;
                    break;
                }
            }
            auto deficit =
                static_cast<std::ptrdiff_t>(gt.target_count) - static_cast<std::ptrdiff_t>(live);
            for (std::ptrdiff_t i = 0; i < deficit; ++i) {
                std::optional<NodeId> hint;
                if (auto it = last_replaced.find(gt.group_id); it != last_replaced.end()) {
                    hint = it->second;
                }
                try {
                    provision_node(gt.group_id, hint).get();
                } catch (const std::exception& ex) {
                    std::cerr << "[docker_quorum_manager::maintain_quorum] provision failed: "
                              << ex.what() << "\n";
                }
            }
        }

        return FutureFactory::makeFuture(std::move(health));
    }

private:
    docker_quorum_manager_config _cfg;

    // Build a container name for a given node ID (Req 18 AC 5)
    [[nodiscard]] auto container_name(const NodeId& id) const -> std::string {
        return "kythira-" + _cfg.cluster_name + "-" + node_id_label(id);
    }

    // Serialize a NodeId to string for label / name purposes
    static auto node_id_label(const NodeId& id) -> std::string {
        if constexpr (std::is_same_v<NodeId, std::string>) {
            return id;
        } else {
            return std::to_string(id);
        }
    }

    // Determine the next node ID by finding the highest existing kythira.node_id
    // label and incrementing (Req 18 AC 11)
    auto next_node_id(httplib::Client& cli) const -> NodeId {
        auto filter = R"({"label":["kythira.cluster=)" + _cfg.cluster_name + R"("]})";
        // URL-encode curly braces
        std::string encoded;
        for (char c : filter) {
            if (c == '{') {
                encoded += "%7B";
            } else if (c == '}') {
                encoded += "%7D";
            } else if (c == '"') {
                encoded += "%22";
            } else if (c == ':') {
                encoded += "%3A";
            } else if (c == '[') {
                encoded += "%5B";
            } else if (c == ']') {
                encoded += "%5D";
            } else if (c == '=') {
                encoded += "%3D";
            } else {
                encoded += c;
            }
        }
        auto path = "/containers/json?filters=" + encoded;
        auto res = cli.Get(path);

        NodeId max_id{};
        if (res && res->status == 200) {
            auto jv = boost::json::parse(res->body);
            for (const auto& ct : jv.as_array()) {
                const auto& obj = ct.as_object();
                if (!obj.contains("Labels")) {
                    continue;
                }
                const auto& labels = obj.at("Labels").as_object();
                if (!labels.contains("kythira.node_id")) {
                    continue;
                }
                auto id_str = std::string(labels.at("kythira.node_id").as_string());
                NodeId id{};
                if constexpr (std::is_same_v<NodeId, std::string>) {
                    id = id_str;
                } else {
                    id = static_cast<NodeId>(std::stoull(id_str));
                }
                if (id > max_id) {
                    max_id = id;
                }
            }
        }

        // If no containers, start at 1; otherwise increment highest
        if constexpr (std::is_same_v<NodeId, std::string>) {
            if (max_id.empty()) {
                return "1";
            }
            return std::to_string(std::stoull(max_id) + 1);
        } else {
            return max_id + NodeId{1};
        }
    }

    // Best-effort cleanup of a partially-created container
    auto try_remove(httplib::Client& cli, const std::string& name) const -> void {
        try {
            cli.Delete("/containers/" + name + "?force=true");
        } catch (...) {
        }
    }

    // Compute quorum_status from live and total counts
    static auto compute_status(std::size_t live, std::size_t total) -> quorum_status {
        if (total == 0) {
            return quorum_status::healthy;
        }
        std::size_t majority = total / 2 + 1;
        if (live < majority) {
            return quorum_status::lost;
        }
        if (live == majority) {
            return quorum_status::critical;
        }
        // live > majority
        if (live < total) {
            return quorum_status::degraded;
        }
        return quorum_status::healthy;
    }

    // Create an httplib::Client from the configured daemon_url.
    // Supports "unix:///path" and "http://host:port" forms.
    [[nodiscard]] auto make_client() const -> std::unique_ptr<httplib::Client> {
        const auto& url = _cfg.daemon_url;
        auto timeout_sec = static_cast<int>(_cfg.api_timeout.count() / 1000);
        auto timeout_ms = static_cast<int>(_cfg.api_timeout.count() % 1000) * 1000;

        if (url.starts_with("unix://")) {
            // Strip the "unix://" prefix and any leading double-slash from the path
            std::string path = url.substr(7);  // remove "unix://"
            // The Docker convention is "unix:///var/run/docker.sock" so path starts with "/"
            auto cli = std::make_unique<httplib::Client>(path);
            cli->set_address_family(AF_UNIX);
            cli->set_connection_timeout(timeout_sec, timeout_ms);
            cli->set_read_timeout(timeout_sec, timeout_ms);
            return cli;
        }

        // TCP: http://host:port
        // Strip scheme
        std::string rest = url;
        auto scheme_end = rest.find("://");
        if (scheme_end != std::string::npos) {
            rest = rest.substr(scheme_end + 3);
        }
        auto colon = rest.rfind(':');
        std::string host;
        int port = 2375;
        if (colon != std::string::npos) {
            host = rest.substr(0, colon);
            port = std::stoi(rest.substr(colon + 1));
        } else {
            host = rest;
        }
        auto cli = std::make_unique<httplib::Client>(host, port);
        cli->set_connection_timeout(timeout_sec, timeout_ms);
        cli->set_read_timeout(timeout_sec, timeout_ms);
        return cli;
    }
};

// Req 18 AC 3 — static_assert that the concept is satisfied
static_assert(quorum_manager<docker_quorum_manager<std::uint64_t, std::string>, std::uint64_t,
                             std::string, std::string>,
              "docker_quorum_manager must satisfy quorum_manager");

}  // namespace kythira

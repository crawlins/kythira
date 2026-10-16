// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// docker_quorum_manager — quorum_manager implementation that drives the Docker
// Engine REST API to provision and decommission kythira containers.
//
// Uses a single placement group; all containers belong to the same failure
// domain.  Communicates with the Docker daemon through the Unix domain socket
// at /var/run/docker.sock (or a configurable TCP endpoint) via cpp-httplib.
//
// Containers are labelled with kythira.cluster and kythira.node_id so the
// manager can enumerate them with a single filtered GET /containers/json call
// independently of any in-process state. How a container is named depends on
// the NodeId type (spec cloud-composite-node-ids, Requirement 5):
//
// - docker_container_node_id, or std::string holding its canonical text: the
//   node id IS the container, {cluster_name, container name}. The manager
//   generates the name, kythira-{cluster_name}-{8 random base32 characters},
//   before the create, so the id is known up front and goes into
//   KYTHIRA_NODE_ID, the kythira.node_id label and any {NODE_ID} in
//   extra_env / extra_args as canonical text. Nothing is allocated.
// - an unsigned integer (numeric mode): the manager allocates max + 1 over the
//   kythira.node_id labels and names the container kythira-{cluster_name}-{id}.

#include <raft/composite_node_id.hpp>
#include <raft/future_default.hpp>
#include <raft/generated_node_name.hpp>
#include <raft/quorum_management.hpp>

#include <httplib.h>
#include <boost/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
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
    static_assert(std::unsigned_integral<NodeId> || std::same_as<NodeId, std::string> ||
                      std::same_as<NodeId, docker_container_node_id>,
                  "docker_quorum_manager: NodeId must be an unsigned integer, std::string or "
                  "docker_container_node_id");

public:
    using node_id_type = NodeId;
    using address_type = Address;
    using placement_group_id_type = std::string;

    /// True when the node id is the container itself (composite or string
    /// mode): its name, scoped by `cluster_name`.
    static constexpr bool name_is_node_id = node_id_traits<NodeId>::is_textual;

    /// Longest container name the manager generates in composite and string
    /// mode. The name is also the container's hostname and the DNS name its
    /// peers dial, so it is held to one DNS label (63 characters), which also
    /// keeps it within Linux's 64-character hostname limit.
    static constexpr std::size_t max_container_name_length = 63;

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
        if constexpr (name_is_node_id) {
            // The cluster is the id's scope, so it must be a valid one: 1-63
            // of [a-z0-9_-].
            if (!docker_container_rules::valid(_cfg.cluster_name, "kythira")) {
                throw std::invalid_argument(
                    "docker_quorum_manager: cluster_name '" + _cfg.cluster_name +
                    "' must be 1-63 of [a-z0-9_-] when the node id is the container");
            }
        }
    }

    // ── assess_quorum (Req 18 AC 7-10) ───────────────────────────────────────

    auto assess_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::future_default<quorum_health<NodeId, std::string>> {
        try {
            auto cli = make_client();

            std::vector<NodeId> unreachable;
            std::size_t live = 0;

            for (const auto& np : cluster) {
                note_node_id(np.node_id);
                // An id that is not one of this cluster's containers (another
                // cluster's scope, or text that is not a docker id at all) is
                // unreachable; it does not fail the assessment.
                auto name = container_name_of(np.node_id);
                if (!name) {
                    unreachable.push_back(np.node_id);
                    continue;
                }
                auto path = "/containers/" + *name + "/json";
                auto res = cli->Get(path);

                // Req 18 AC 10 — a daemon we cannot connect to says nothing
                // about the containers, so fail the assessment rather than
                // report every node unreachable (which reads as quorum loss).
                // A read that times out is per-container and stays
                // "unreachable" (AC 7).
                if (!res && daemon_unreachable(res.error())) {
                    throw std::runtime_error("cannot reach the Docker daemon at " +
                                             _cfg.daemon_url + ": " +
                                             httplib::to_string(res.error()));
                }

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
            quorum_status status = compute_status(live, total, _cfg.target_count);

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
            return future_factory_default::makeFuture(std::move(report));
        } catch (const std::exception& ex) {
            return future_factory_default::makeExceptionalFuture<
                quorum_health<NodeId, std::string>>(std::make_exception_ptr(std::runtime_error(
                std::string("docker_quorum_manager::assess_quorum: ") + ex.what())));
        }
    }

    // ── provision_node (Req 18 AC 11-15) ─────────────────────────────────────

    auto provision_node(std::string target_group, std::optional<NodeId> replacing)
        -> kythira::future_default<peer_info<NodeId, Address>> {
        return provision(std::move(target_group), replacing, std::nullopt);
    }

    // ── idempotency keys (elastic-shard-capacity Requirement 9.2) ────────────

    /// @brief `provision_node`, with `key` carried as the container label
    ///        `kythira.idempotency-key`.
    ///
    /// The label is what lets a capacity controller that died mid-call be
    /// reconciled by its successor: the container its call created is found
    /// by the key it recorded before the call, not guessed at by arrival.
    auto provision_node_keyed(std::string target_group, std::optional<NodeId> replacing,
                              const std::string& key)
        -> kythira::future_default<peer_info<NodeId, Address>> {
        return provision(std::move(target_group), replacing, key);
    }

    /// @brief The container this cluster created under `key`, running or not.
    ///
    /// Stopped containers count: one still exists and still holds its name,
    /// and a reaper that could not see it could not remove it.
    auto find_by_idempotency_key(const std::string& key)
        -> kythira::future_default<std::optional<peer_info<NodeId, Address>>> {
        using result = std::optional<peer_info<NodeId, Address>>;
        try {
            auto cli = make_client();
            const auto path = "/containers/json?all=1&filters=" +
                              url_encode(R"({"label":["kythira.cluster=)" + _cfg.cluster_name +
                                         R"(","kythira.idempotency-key=)" + key + R"("]})");
            auto res = cli->Get(path);
            if (!res || res->status != 200) {
                const auto msg = res ? "HTTP " + std::to_string(res->status) : "connection failed";
                return future_factory_default::makeExceptionalFuture<result>(
                    std::make_exception_ptr(std::runtime_error(
                        "docker_quorum_manager::find_by_idempotency_key: " + msg)));
            }
            // Held in a named value: a range-for over `parse(...).as_array()`
            // would iterate a temporary destroyed before the loop body ran.
            const auto parsed = boost::json::parse(res->body);
            for (const auto& ct : parsed.as_array()) {
                const auto& obj = ct.as_object();
                if constexpr (name_is_node_id) {
                    // The id is the container's own name, not its label.
                    if (auto id = listed_container_node_id(obj)) {
                        return future_factory_default::makeFuture(
                            result{peer_info<NodeId, Address>{*id, container_address(*id)}});
                    }
                    continue;
                }
                if (!obj.contains("Labels") || !obj.at("Labels").is_object()) {
                    continue;
                }
                const auto& labels = obj.at("Labels").as_object();
                if (!labels.contains("kythira.node_id")) {
                    continue;
                }
                const auto& label = labels.at("kythira.node_id");
                const auto id = label.is_string() ? parse_node_id(std::string(label.get_string()))
                                                  : std::nullopt;
                if (!id) {
                    // Skipped rather than failing the whole lookup: a
                    // container someone else labelled is not this key's.
                    std::cerr << "[docker_quorum_manager] find_by_idempotency_key: ignoring "
                                 "a container with an unparseable kythira.node_id label\n";
                    continue;
                }
                return future_factory_default::makeFuture(
                    result{peer_info<NodeId, Address>{*id, container_address(*id)}});
            }
            return future_factory_default::makeFuture(result{});
        } catch (const std::exception& ex) {
            return future_factory_default::makeExceptionalFuture<result>(
                std::make_exception_ptr(std::runtime_error(
                    std::string("docker_quorum_manager::find_by_idempotency_key: ") + ex.what())));
        }
    }

private:
    auto provision(std::string /*target_group*/, std::optional<NodeId> replacing,
                   std::optional<std::string> key)
        -> kythira::future_default<peer_info<NodeId, Address>> {
        try {
            auto cli = make_client();
            // Log hint only; single-group implementation ignores it otherwise
            (void)replacing;

            if constexpr (name_is_node_id) {
                // The name is generated here, so the id is known before the
                // create. Docker refuses a second container under a name in
                // use (409), which a random suffix makes all but impossible;
                // a refusal is retried with a fresh suffix, never cleaned up.
                for (std::size_t attempt = 1;; ++attempt) {
                    const auto name = generated_node_name(_cfg.cluster_name, random_name_suffix(),
                                                          max_container_name_length);
                    const auto new_id = node_id_for_container(_cfg.cluster_name, name);
                    if (!new_id) {
                        throw std::runtime_error("generated container name " + name +
                                                 " is not a valid docker node id");
                    }
                    if (auto peer = create_and_start(*cli, *new_id, name, key)) {
                        return future_factory_default::makeFuture(std::move(*peer));
                    }
                    if (attempt >= max_id_claim_attempts) {
                        throw std::runtime_error("generated container name " + name +
                                                 " is already in use and " +
                                                 std::to_string(max_id_claim_attempts) +
                                                 " attempts to claim a free name all collided");
                    }
                }
            } else {
                // The daemon is the arbiter of which process gets an id: the
                // container name embeds it and Docker refuses a second container
                // under a name in use (409).  next_node_id() is only a guess --
                // another leader's manager, with its own in-memory floor, may have
                // listed the same containers a moment earlier and be creating the
                // same id right now -- so a 409 means "taken, try the next one",
                // never "clean up": the container under that name is someone
                // else's, quite possibly a live member.
                for (std::size_t attempt = 1;; ++attempt) {
                    NodeId new_id = next_node_id(*cli);
                    // Spent from here on, whatever the create does.
                    note_node_id(new_id);
                    if (auto peer = create_and_start(*cli, new_id, container_name(new_id), key)) {
                        return future_factory_default::makeFuture(std::move(*peer));
                    }
                    if (attempt >= max_id_claim_attempts) {
                        throw std::runtime_error("node id " + node_id_label(new_id) +
                                                 " is already in use and " +
                                                 std::to_string(max_id_claim_attempts) +
                                                 " attempts to claim a free id all collided");
                    }
                }
            }
        } catch (const std::exception& ex) {
            return future_factory_default::makeExceptionalFuture<peer_info<NodeId, Address>>(
                std::make_exception_ptr(std::runtime_error(
                    std::string("docker_quorum_manager::provision_node: ") + ex.what())));
        }
    }

    // Creates and starts the container `name` for `new_id`.  Returns nullopt,
    // having touched nothing, when Docker reports the name is already in use;
    // throws when the create or start fails for any other reason.
    auto create_and_start(httplib::Client& cli, const NodeId& new_id, const std::string& name,
                          const std::optional<std::string>& key)
        -> std::optional<peer_info<NodeId, Address>> {
        // Marks the container as this call's own, so a cleanup after an
        // ambiguous create (no response) removes it only if it is ours.
        const auto claim = make_claim_token();

        // Build container-create body
        boost::json::object body;
        body["Image"] = _cfg.image;

        boost::json::object labels;
        labels["kythira.cluster"] = _cfg.cluster_name;
        labels["kythira.node_id"] = node_id_label(new_id);
        if (key) {
            labels["kythira.idempotency-key"] = *key;
        }
        labels[claim_label] = claim;
        body["Labels"] = labels;
        // The container's name is the name its peers dial (Docker's and
        // aardvark-dns's embedded DNS both resolve it), so make it the
        // hostname too: a node that advertises its own hostname then
        // advertises something its peers can resolve.
        body["Hostname"] = name;

        // Environment
        boost::json::array env;
        env.emplace_back("KYTHIRA_NODE_ID=" + node_id_label(new_id));
        env.emplace_back("KYTHIRA_NODE_PORT=" + std::to_string(_cfg.node_port));
        env.emplace_back("KYTHIRA_CLUSTER=" + _cfg.cluster_name);
        for (const auto& e : _cfg.extra_env) {
            env.emplace_back(substitute_node_id(e, new_id));
        }
        body["Env"] = env;

        // Extra command arguments
        if (!_cfg.extra_args.empty()) {
            boost::json::array cmd;
            for (const auto& a : _cfg.extra_args) {
                cmd.emplace_back(substitute_node_id(a, new_id));
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
        auto create_res = cli.Post(create_path, serialized, "application/json");

        if (create_res && create_res->status == 409) {
            return std::nullopt;
        }
        if (!create_res || create_res->status < 200 || create_res->status >= 300) {
            auto msg =
                create_res
                    ? ("HTTP " + std::to_string(create_res->status) + ": " + create_res->body)
                    : "connection failed";
            // Attempt cleanup of any partially-created container (Req 18
            // AC 14), but only one carrying this call's claim: with no
            // answer, the name may belong to another process's container.
            remove_if_claimed(cli, name, claim);
            throw std::runtime_error("create failed: " + msg);
        }

        // From here the container is known to be ours: remove it by the
        // id Docker just assigned rather than by name.
        std::string container_id = name;
        try {
            auto created = boost::json::parse(create_res->body);
            if (const auto* id = created.as_object().if_contains("Id");
                id != nullptr && id->is_string() && !id->as_string().empty()) {
                container_id = std::string(id->as_string());
            }
        } catch (const std::exception&) {
            // Fall back to the name, which this create just claimed.
        }

        auto start_path = "/containers/" + name + "/start";
        auto start_res = cli.Post(start_path, "", "application/json");

        if (!start_res || (start_res->status != 204 && start_res->status != 200)) {
            auto msg = start_res
                           ? ("HTTP " + std::to_string(start_res->status) + ": " + start_res->body)
                           : "connection failed";
            try_remove(cli, container_id);
            throw std::runtime_error("start failed: " + msg);
        }

        // Address: container hostname resolves via Docker's embedded DNS
        Address addr = static_cast<Address>(name + ":" + std::to_string(_cfg.node_port));
        return peer_info<NodeId, Address>{new_id, addr};
    }

public:
    /// How many ids one provision tries before giving up when each turns out
    /// to be taken by another process (see provision()).
    static constexpr std::size_t max_id_claim_attempts = 5;

    // ── decommission_node (Req 18 AC 16-18) ──────────────────────────────────

    auto decommission_node(const NodeId& node) -> kythira::future_default<void> {
        try {
            auto name = container_name_of(node);
            if (!name) {
                throw std::invalid_argument("node " + node_id_label(node) +
                                            " is not a container in cluster " + _cfg.cluster_name);
            }
            auto cli = make_client();
            auto path = "/containers/" + *name + "?force=true";
            auto res = cli->Delete(path);

            if (res && res->status == 404) {
                // Idempotent — container already gone (Req 18 AC 17)
                return future_factory_default::makeFuture();
            }
            if (!res || res->status < 200 || res->status >= 300) {
                auto msg = res ? ("HTTP " + std::to_string(res->status) + ": " + res->body)
                               : "connection failed";
                return future_factory_default::makeExceptionalFuture<void>(std::make_exception_ptr(
                    std::runtime_error("docker_quorum_manager: decommission failed: " + msg)));
            }
            return future_factory_default::makeFuture();
        } catch (const std::exception& ex) {
            return future_factory_default::makeExceptionalFuture<void>(
                std::make_exception_ptr(std::runtime_error(
                    std::string("docker_quorum_manager::decommission_node: ") + ex.what())));
        }
    }

    // ── topology (Req 18 AC 19) ───────────────────────────────────────────────

    [[nodiscard]] auto topology() const -> desired_topology<std::string> {
        return desired_topology<std::string>{
            .groups = {{.group_id = _cfg.group_id, .target_count = _cfg.target_count}},
        };
    }

    // ── node id <-> container name ────────────────────────────────────────────

    /// The container a node runs in, or nullopt when there is none. Numeric
    /// mode: `kythira-{cluster_name}-{id}`. Composite and string mode: the id's
    /// own name, a field read; nullopt when the id is not canonical docker text
    /// or is scoped to another cluster.
    [[nodiscard]] auto container_name_of(const NodeId& nid) const -> std::optional<std::string> {
        if constexpr (name_is_node_id) {
            return container_name_for_node(_cfg.cluster_name, nid);
        } else {
            return container_name(nid);
        }
    }

    /// Composite and string mode: `{cluster, name}` as a NodeId, or nullopt when
    /// `name` is not a valid container name or `cluster` not a valid scope.
    static auto node_id_for_container(std::string_view cluster, std::string_view name)
        -> std::optional<NodeId>
    requires name_is_node_id
    {
        try {
            docker_container_node_id id{std::string(cluster), std::string(name)};
            if constexpr (std::same_as<NodeId, std::string>) {
                return id.to_string();
            } else {
                return id;
            }
        } catch (const std::invalid_argument&) {
            return std::nullopt;
        }
    }

    /// Composite and string mode: the container name, or nullopt when `nid` is
    /// not canonical docker text or is scoped to a cluster other than `cluster`.
    static auto container_name_for_node(std::string_view cluster, const NodeId& nid)
        -> std::optional<std::string>
    requires name_is_node_id
    {
        std::optional<docker_container_node_id> id;
        if constexpr (std::same_as<NodeId, std::string>) {
            id = docker_container_node_id::parse(nid);
        } else {
            id = nid;
        }
        if (!id || id->scope() != cluster) {
            return std::nullopt;
        }
        return std::string(id->native());
    }

    // ── maintain_quorum (Req 19.1) ────────────────────────────────────────────

    auto maintain_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::future_default<quorum_health<NodeId, std::string>> {
        quorum_health<NodeId, std::string> health;
        try {
            health = std::move(assess_quorum(cluster)).get();
        } catch (...) {
            return future_factory_default::makeExceptionalFuture<
                quorum_health<NodeId, std::string>>(std::current_exception());
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
                std::move(decommission_node(nid)).get();
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
                    std::move(provision_node(gt.group_id, hint)).get();
                } catch (const std::exception& ex) {
                    std::cerr << "[docker_quorum_manager::maintain_quorum] provision failed: "
                              << ex.what() << "\n";
                }
            }
        }

        return future_factory_default::makeFuture(std::move(health));
    }

private:
    docker_quorum_manager_config _cfg;

    // Highest node ID seen in any assess_quorum call.  Raft members that this
    // manager did not create (e.g. bootstrap containers started by compose)
    // carry no kythira.node_id label, so the label scan alone would hand out
    // an ID that is already a live member.  The leader always assesses before
    // it provisions, so this floor is re-learned after a manager restart.
    // Shared so the manager stays copyable (node_config takes it by value).
    numeric_node_id_floor _max_seen_node_id;

    // Build a container name for a given node ID (Req 18 AC 5). Numeric mode
    // only: in composite and string mode the name is the id's own.
    [[nodiscard]] auto container_name(const NodeId& id) const -> std::string
    requires(!name_is_node_id)
    {
        return "kythira-" + _cfg.cluster_name + "-" + node_id_label(id);
    }

    // The address peers dial: the container name, resolved by the network's
    // embedded DNS, and the node port. Only called with an id that names a
    // container of this cluster.
    [[nodiscard]] auto container_address(const NodeId& id) const -> Address {
        return static_cast<Address>(container_name_of(id).value_or(std::string{}) + ":" +
                                    std::to_string(_cfg.node_port));
    }

    // Composite and string mode: the node id of one /containers/json entry,
    // from its own name (Docker lists it as "/name"), or nullopt.
    [[nodiscard]] auto listed_container_node_id(const boost::json::object& obj) const
        -> std::optional<NodeId>
    requires name_is_node_id
    {
        const auto* names = obj.if_contains("Names");
        if (names == nullptr || !names->is_array()) {
            return std::nullopt;
        }
        for (const auto& n : names->as_array()) {
            if (!n.is_string()) {
                continue;
            }
            std::string_view name = n.get_string();
            if (name.starts_with('/')) {
                name.remove_prefix(1);
            }
            if (auto id = node_id_for_container(_cfg.cluster_name, name)) {
                return id;
            }
        }
        return std::nullopt;
    }

    // Composite and string mode: {NODE_ID} in an extra_env entry or extra
    // argument becomes the id's canonical text, which is known before the
    // create. Numeric mode passes both through unchanged, as it always has.
    static auto substitute_node_id(std::string text, const NodeId& id) -> std::string {
        if constexpr (name_is_node_id) {
            static constexpr std::string_view placeholder = "{NODE_ID}";
            const auto value = node_id_label(id);
            for (auto pos = text.find(placeholder); pos != std::string::npos;
                 pos = text.find(placeholder, pos + value.size())) {
                text.replace(pos, placeholder.size(), value);
            }
        }
        return text;
    }

    // Serialize a NodeId to string for label / name purposes
    static auto node_id_label(const NodeId& id) -> std::string {
        return node_id_traits<NodeId>::to_text(id);
    }

    // The NodeId a kythira.node_id label names, or nullopt for one this
    // manager could not have written (for a numeric NodeId: anything but
    // plain decimal that fits).
    static auto parse_node_id(std::string_view label) -> std::optional<NodeId> {
        return node_id_traits<NodeId>::from_text(label);
    }

    // Percent-encodes the characters a Docker `filters=` JSON argument uses.
    static auto url_encode(const std::string& in) -> std::string {
        std::string out;
        for (char c : in) {
            switch (c) {
                case '{':
                    out += "%7B";
                    break;
                case '}':
                    out += "%7D";
                    break;
                case '"':
                    out += "%22";
                    break;
                case ':':
                    out += "%3A";
                    break;
                case '[':
                    out += "%5B";
                    break;
                case ']':
                    out += "%5D";
                    break;
                case '=':
                    out += "%3D";
                    break;
                case ',':
                    out += "%2C";
                    break;
                default:
                    out += c;
            }
        }
        return out;
    }

    // Determine the next node ID by finding the highest existing kythira.node_id
    // label and incrementing (Req 18 AC 11).  A candidate only: provision()
    // claims it by creating the container, which Docker refuses if the name
    // is already taken. Numeric mode only.
    auto next_node_id(httplib::Client& cli) const -> NodeId
    requires(!name_is_node_id)
    {
        const auto encoded =
            url_encode(R"({"label":["kythira.cluster=)" + _cfg.cluster_name + R"("]})");
        // all=true: an exited container still owns its name and ID.
        auto path = "/containers/json?all=true&filters=" + encoded;
        auto res = cli.Get(path);

        // Without the listing, the floor alone could name an existing (even
        // exited) container; the create would then only 409, but guessing is
        // pointless when the daemon cannot answer, so fail the provision.
        if (!res || res->status != 200) {
            throw std::runtime_error(
                "cannot list containers to allocate a node id: " +
                (res ? "HTTP " + std::to_string(res->status) : httplib::to_string(res.error())));
        }
        std::uint64_t max_id = 0;
        {
            auto jv = boost::json::parse(res->body);
            for (const auto& ct : jv.as_array()) {
                const auto& obj = ct.as_object();
                if (!obj.contains("Labels") || !obj.at("Labels").is_object()) {
                    continue;
                }
                const auto& labels = obj.at("Labels").as_object();
                if (!labels.contains("kythira.node_id")) {
                    continue;
                }
                // A label we did not write (not plain decimal, or past
                // NodeId's range) cannot collide with ours and is skipped.
                // std::stoull read "-1" as the largest uint64, and the + 1
                // then wrapped to node 0.
                const auto& label = labels.at("kythira.node_id");
                if (const auto parsed = label.is_string() ? parse_numeric_node_id<NodeId>(
                                                                std::string(label.get_string()))
                                                          : std::nullopt) {
                    max_id = std::max(max_id, *parsed);
                }
            }
        }

        // If no containers and no members were seen, start at 1. Past the
        // NodeId type's maximum this throws rather than wrapping.
        return numeric_node_id_as<NodeId>(
            _max_seen_node_id.next_above(max_id, numeric_node_id_ceiling<NodeId>()));
    }

    // Best-effort cleanup of a partially-created container, by name or id.
    auto try_remove(httplib::Client& cli, const std::string& name) const -> void {
        try {
            cli.Delete("/containers/" + name + "?force=true");
        } catch (...) {
        }
    }

    // Label carrying a provision call's random claim token.
    static constexpr const char* claim_label = "kythira.provision-claim";

    static auto make_claim_token() -> std::string {
        std::random_device rd;
        std::string out;
        static constexpr const char* hex = "0123456789abcdef";
        for (int i = 0; i < 4; ++i) {
            auto v = rd();
            for (int j = 0; j < 8; ++j) {
                out.push_back(hex[v & 0xFU]);
                v >>= 4U;
            }
        }
        return out;
    }

    // Removes the container named `name` only if it carries `claim`, i.e. only
    // if this call created it.  Used when a create got no answer: the create
    // may or may not have happened, and the name may instead belong to a
    // container another process created under the same id.
    auto remove_if_claimed(httplib::Client& cli, const std::string& name,
                           const std::string& claim) const -> void {
        try {
            auto res = cli.Get("/containers/" + name + "/json");
            if (!res || res->status != 200) {
                return;
            }
            auto jv = boost::json::parse(res->body);
            const auto* labels = jv.as_object().if_contains("Config");
            if (labels != nullptr && labels->is_object()) {
                labels = labels->as_object().if_contains("Labels");
            }
            if (labels == nullptr || !labels->is_object()) {
                return;
            }
            const auto* mine = labels->as_object().if_contains(claim_label);
            if (mine != nullptr && mine->is_string() && mine->as_string() == claim) {
                try_remove(cli, name);
            }
        } catch (...) {
        }
    }

    // Compute quorum_status from the live, total and target counts (Req 4.4,
    // Req 18 AC 9).  Quorum is judged against the configured members; the
    // target only decides whether an intact quorum is healthy or degraded,
    // so a cluster that is all-live but smaller than target_count reports
    // degraded and the leader provisions the missing nodes.
    static auto compute_status(std::size_t live, std::size_t total, std::size_t target)
        -> quorum_status {
        if (total == 0) {
            return quorum_status::healthy;  // nothing to assess
        }
        std::size_t majority = total / 2 + 1;
        if (live < majority) {
            return quorum_status::lost;
        }
        if (live == majority && live < total) {
            return quorum_status::critical;
        }
        if (live < total || live < target) {
            return quorum_status::degraded;
        }
        return quorum_status::healthy;
    }

    // Connection-level failures mean the daemon itself is unreachable.
    static auto daemon_unreachable(httplib::Error err) -> bool {
        return err == httplib::Error::Connection || err == httplib::Error::ConnectionTimeout;
    }

    // Records the highest node ID seen in an assess_quorum cluster vector.
    // Numeric mode only: composite and string mode allocate nothing.
    auto note_node_id(const NodeId& id) -> void {
        if constexpr (!name_is_node_id) {
            _max_seen_node_id.raise(id);
        }
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
            // cpp-httplib would send the socket path as the Host header, and
            // the daemon's Go HTTP server rejects a Host containing '/' with
            // 400 before routing — so every call over the socket failed, and
            // assess_quorum reported every container unreachable. Any valid
            // host will do; the socket already chose the daemon.
            cli->set_default_headers({{"Host", "localhost"}});
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
static_assert(quorum_manager<docker_quorum_manager<docker_container_node_id, std::string>,
                             docker_container_node_id, std::string, std::string>,
              "docker_quorum_manager<docker_container_node_id> must satisfy quorum_manager");
static_assert(quorum_manager<docker_quorum_manager<std::string, std::string>, std::string,
                             std::string, std::string>,
              "docker_quorum_manager<std::string> must satisfy quorum_manager");

}  // namespace kythira

#pragma once

/// @file gcp_compute_quorum_manager.hpp
/// @brief `quorum_manager` implementation that provisions and monitors Raft
///        nodes as GCE VM instances via the Compute Engine API. The GCP
///        analogue of `aws_ec2_quorum_manager` / `docker_quorum_manager`, and
///        the primary GCP quorum manager.
///
/// Node identity is minted by kythira, not GCP: `provision_node` draws a random
/// 63-bit `NodeId` and encodes it into a deterministic instance name
/// (`kythira-{cluster}-{node_id}`), so `node_id_to_instance_name` /
/// `instance_name_to_node_id` are pure functions requiring no API call — the
/// same statelessness `aws_ec2_quorum_manager` gets from the EC2 instance ID,
/// via a different mechanism (see `.kiro/specs/gcp-cloud-services/design.md`,
/// "The Core Design Decision: Node Identity").

#include <raft/fault_injection.hpp>
#include <raft/future_default.hpp>
#include <raft/gcp_client_config.hpp>
#include <raft/gcp_operation_wait.hpp>
#include <raft/quorum_management.hpp>

#ifdef KYTHIRA_HAS_GCP_SDK

#include <google/cloud/compute/instances/v1/instances_client.h>
#include <google/cloud/compute/zone_operations/v1/zone_operations_client.h>
#include <google/cloud/credentials.h>
#include <google/cloud/options.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace kythira {

namespace gcp_compute_detail {

/// Builds the shared `google::cloud::Options` (credentials + endpoint override)
/// used to construct every Compute Engine REST client for one manager.
inline auto make_options(const gcp_client_config& gcp) -> google::cloud::Options {
    google::cloud::Options opts;
    if (!gcp.credentials_json.empty()) {
        opts.set<google::cloud::UnifiedCredentialsOption>(
            google::cloud::MakeServiceAccountCredentials(gcp.credentials_json));
    } else {
        opts.set<google::cloud::UnifiedCredentialsOption>(
            google::cloud::MakeGoogleDefaultCredentials());
    }
    if (!gcp.endpoint_override.empty()) {
        opts.set<google::cloud::EndpointOption>(gcp.endpoint_override);
    }
    return opts;
}

inline auto make_instances_client(const gcp_client_config& gcp)
    -> google::cloud::compute_instances_v1::InstancesClient {
    return google::cloud::compute_instances_v1::InstancesClient(
        google::cloud::compute_instances_v1::MakeInstancesConnectionRest(make_options(gcp)));
}

inline auto make_zone_operations_client(const gcp_client_config& gcp)
    -> google::cloud::compute_zone_operations_v1::ZoneOperationsClient {
    return google::cloud::compute_zone_operations_v1::ZoneOperationsClient(
        google::cloud::compute_zone_operations_v1::MakeZoneOperationsConnectionRest(
            make_options(gcp)));
}

}  // namespace gcp_compute_detail

// ============================================================================
// Placement policy configuration
// ============================================================================

/// @brief Kind of placement policy applied to a per-group batch of nodes.
enum class gcp_placement_policy_kind : std::uint8_t {
    none,        ///< No `resourcePolicies` entry; GCE chooses placement freely.
    collocated,  ///< A `COLLOCATED` placement policy packs instances on close hardware.
};

/// @brief Placement-policy settings for a single quorum group (GCP zone).
///
/// GCP has no direct analogue of AWS's `spread`/`partition` Placement Group
/// strategies; per-zone kythira placement groups already provide zone-level
/// spread (Requirement 12 AC 5).
struct gcp_placement_policy_config {
    /// Resource-policy self-link to attach via `resourcePolicies`. Empty = none.
    std::string self_link;
    /// Informational; mirrors the policy's own `COLLOCATED` setting. Drives the
    /// value of the `kythira-placement` label (`"compact"` vs `"none"`).
    bool collocation{false};
};

// ============================================================================
// gcp_compute_quorum_manager_config
// ============================================================================

/// @brief Configuration for `gcp_compute_quorum_manager`.
template<typename GroupId = std::string> struct gcp_compute_quorum_manager_config {
    /// Project, credentials, endpoint override, timeouts.
    gcp_client_config gcp{};
    /// Logical cluster name; scopes labels and prefixes the instance name.
    /// Must satisfy `is_valid_gcp_label`. Required.
    std::string cluster_name;
    /// GCE machine type in short form (zone-qualified internally).
    std::string machine_type{"e2-medium"};
    /// Image reference passed to `initializeParams.sourceImage`. Required.
    std::string boot_disk_image;
    /// Boot persistent-disk size in GiB.
    std::uint32_t boot_disk_size_gb{20};
    /// VPC network self-link or short name. A short name is expanded to
    /// `global/networks/<name>` by `gcp_qualify_network` when the instance
    /// resource is built; a self-link or `projects/…` reference is used as-is.
    std::string network{"default"};
    /// GroupId (zone) → subnetwork self-link or short name. A short name is
    /// expanded to `regions/<region>/subnetworks/<name>` by
    /// `gcp_qualify_subnetwork`, with the region derived from the group's zone.
    std::map<GroupId, std::string> subnetwork_by_group{};
    /// Attached service-account email; empty = no service account attached.
    std::string service_account_email{};
    /// OAuth scopes for the attached service account.
    std::vector<std::string> service_account_scopes{};
    /// TCP port the kythira process listens on; written into the returned address.
    std::uint16_t node_port{7000};
    /// Shell script written to the `startup-script` metadata key. `{NODE_ID}`,
    /// `{NODE_PORT}`, `{CLUSTER}`, `{ZONE}` are substituted at provision time.
    std::string startup_script_template{};
    /// Target node counts per zone.
    desired_topology<GroupId> topology{};
    /// Max time to wait for a new instance to reach `RUNNING` (after the
    /// insert-operation wait).
    std::chrono::seconds provision_timeout{300};
    /// Interval between `instances.get` polls during provisioning.
    std::chrono::milliseconds poll_interval{5000};
    /// GroupId (zone) → placement-policy config; absent = no policy for that zone.
    std::map<GroupId, gcp_placement_policy_config> placement_by_group{};
    /// When `true`, instances are requested with `scheduling.provisioningModel = SPOT`.
    bool spot{false};
    /// Additional labels on every managed instance; each key/value SHALL satisfy
    /// `is_valid_gcp_label`.
    std::map<std::string, std::string> extra_labels{};
};

// ============================================================================
// gcp_compute_quorum_manager
// ============================================================================

/// @brief `quorum_manager` implementation backed by direct Compute Engine
///        instance management (`instances.insert`/`.list`/`.get`/`.delete`).
///
/// @tparam NodeId  Node identifier type; defaults to `std::uint64_t`.
/// @tparam Address Network address type; defaults to `std::string`.
template<typename NodeId = std::uint64_t, typename Address = std::string>
requires node_id<NodeId>
class gcp_compute_quorum_manager {
public:
    using node_id_type = NodeId;
    using address_type = Address;
    using placement_group_id_type = std::string;

    /// @brief Constructs the manager and validates the configuration.
    ///
    /// @throws std::invalid_argument if `cluster_name`/`boot_disk_image` are
    ///   empty, `node_port` is zero, `cluster_name` is not a valid GCP label,
    ///   any `extra_labels` key/value is not a valid GCP label, any topology
    ///   group has no `subnetwork_by_group` entry, or the worst-case instance
    ///   name (`cluster_name` + max `NodeId`) is not a valid GCE resource name.
    explicit gcp_compute_quorum_manager(gcp_compute_quorum_manager_config<std::string> config)
        : _config(std::move(config)),
          _instances(gcp_compute_detail::make_instances_client(_config.gcp)),
          _zone_ops(gcp_compute_detail::make_zone_operations_client(_config.gcp)) {
        if (_config.gcp.project_id.empty()) {
            throw std::invalid_argument("gcp_compute_quorum_manager: project_id must be non-empty");
        }
        if (_config.cluster_name.empty()) {
            throw std::invalid_argument(
                "gcp_compute_quorum_manager: cluster_name must be non-empty");
        }
        if (_config.boot_disk_image.empty()) {
            throw std::invalid_argument(
                "gcp_compute_quorum_manager: boot_disk_image must be non-empty");
        }
        if (_config.node_port == 0) {
            throw std::invalid_argument("gcp_compute_quorum_manager: node_port must be non-zero");
        }
        if (!is_valid_gcp_label(_config.cluster_name)) {
            throw std::invalid_argument("gcp_compute_quorum_manager: cluster_name '" +
                                        _config.cluster_name + "' is not a valid GCP label");
        }
        for (const auto& [k, v] : _config.extra_labels) {
            if (!is_valid_gcp_label(k) || !is_valid_gcp_label(v)) {
                throw std::invalid_argument("gcp_compute_quorum_manager: extra_labels entry '" + k +
                                            "'='" + v + "' is not a valid GCP label key/value");
            }
        }
        for (const auto& gt : _config.topology.groups) {
            if (_config.subnetwork_by_group.find(gt.group_id) ==
                _config.subnetwork_by_group.end()) {
                throw std::invalid_argument(
                    "gcp_compute_quorum_manager: no subnetwork configured for group: " +
                    gt.group_id);
            }
        }
        // Validate the worst-case instance name length up front so a provision
        // call can never fail on a name-too-long error introduced by an unlucky
        // random NodeId draw (Requirement 6 AC 3). The widest decimal NodeId is
        // the max uint64_t (20 digits); a signed NodeId is narrower, so this is
        // a safe upper bound.
        const std::string worst_case = "kythira-" + _config.cluster_name + "-18446744073709551615";
        if (!is_valid_gcp_resource_name(worst_case)) {
            throw std::invalid_argument(
                "gcp_compute_quorum_manager: cluster_name '" + _config.cluster_name +
                "' is too long — the derived instance name can exceed 63 characters");
        }
    }

    gcp_compute_quorum_manager(gcp_compute_quorum_manager&&) noexcept = default;
    gcp_compute_quorum_manager& operator=(gcp_compute_quorum_manager&&) noexcept = default;
    gcp_compute_quorum_manager(const gcp_compute_quorum_manager&) = delete;
    gcp_compute_quorum_manager& operator=(const gcp_compute_quorum_manager&) = delete;

    /// @brief Reports which nodes are live at the GCE infrastructure layer.
    ///
    /// Groups the cluster vector by zone and issues one label-filtered
    /// `instances.list` per distinct zone (GCE's list is zone-scoped, unlike
    /// AWS's single multi-AZ call). A node is live iff its computed instance
    /// name appears in its zone's results with `status == RUNNING`.
    auto assess_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::future_default<quorum_health<NodeId, std::string>> {
        try {
            if (cluster.empty()) {
                return build_health(cluster, {});
            }

            std::vector<std::string> zones;
            for (const auto& np : cluster) {
                if (std::find(zones.begin(), zones.end(), np.group_id) == zones.end()) {
                    zones.push_back(np.group_id);
                }
            }

            // instance name → running? across all zones.
            std::map<std::string, bool> name_running;
            for (const auto& zone : zones) {
                fiu_do_on("raft/gcp/compute/list_instances",
                          throw std::runtime_error("fault: raft/gcp/compute/list_instances"););

                google::cloud::cpp::compute::instances::v1::ListInstancesRequest req;
                req.set_project(_config.gcp.project_id);
                req.set_zone(zone);
                req.set_filter("labels.kythira-cluster = \"" + _config.cluster_name + "\"");

                for (auto const& maybe_inst : _instances.ListInstances(req)) {
                    if (!maybe_inst) {
                        throw std::runtime_error("gcp instances.list (" + zone +
                                                 "): " + maybe_inst.status().message());
                    }
                    name_running[maybe_inst->name()] = (maybe_inst->status() == "RUNNING");
                }
            }

            std::map<std::string, bool> live_map;
            for (const auto& np : cluster) {
                const std::string name = node_id_to_instance_name(_config.cluster_name, np.node_id);
                auto it = name_running.find(name);
                live_map[node_id_str(np.node_id)] = (it != name_running.end() && it->second);
            }
            return build_health(cluster, live_map);
        } catch (const std::exception& ex) {
            return future_factory_default::makeExceptionalFuture<
                quorum_health<NodeId, std::string>>(std::make_exception_ptr(std::runtime_error(
                std::string("gcp_compute_quorum_manager::assess_quorum: ") + ex.what())));
        }
    }

    /// @brief Assesses quorum, decommissions unreachable nodes, and provisions
    ///        replacements to meet the desired topology.
    ///
    /// Decommission/provision errors are logged to stderr but do not abort; the
    /// returned health reflects the pre-maintenance cluster state.
    auto maintain_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::future_default<quorum_health<NodeId, std::string>> {
        try {
            fiu_do_on("raft/gcp/compute/maintain_quorum",
                      throw std::runtime_error("fault: raft/gcp/compute/maintain_quorum"););
        } catch (...) {
            return future_factory_default::makeExceptionalFuture<
                quorum_health<NodeId, std::string>>(std::current_exception());
        }

        quorum_health<NodeId, std::string> pre_health;
        try {
            pre_health = std::move(assess_quorum(cluster)).get();
        } catch (...) {
            return future_factory_default::makeExceptionalFuture<
                quorum_health<NodeId, std::string>>(std::current_exception());
        }

        std::map<std::string, NodeId> last_replaced;
        for (const auto& nid : pre_health.unreachable_nodes) {
            std::string grp;
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
                std::cerr << "[gcp_compute_quorum_manager::maintain_quorum] decommission of "
                          << node_id_str(nid) << " failed: " << ex.what() << "\n";
            }
        }

        for (const auto& gt : _config.topology.groups) {
            std::size_t live = 0;
            for (const auto& gh : pre_health.groups) {
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
                    std::cerr << "[gcp_compute_quorum_manager::maintain_quorum] provision in "
                              << gt.group_id << " failed: " << ex.what() << "\n";
                }
            }
        }

        return future_factory_default::makeFuture(std::move(pre_health));
    }

    /// @brief Launches a new GCE instance in @p target_group's subnetwork, waits
    ///        for `RUNNING`, and returns its node ID and internal address.
    ///
    /// Generates the `NodeId` before `instances.insert` and derives the instance
    /// name from it. Retries with a fresh `NodeId` on a name collision, up to 5
    /// attempts (Requirement 5 AC 4).
    auto provision_node(std::string target_group, std::optional<NodeId> replacing)
        -> kythira::future_default<peer_info<NodeId, Address>> {
        try {
            auto sit = _config.subnetwork_by_group.find(target_group);
            if (sit == _config.subnetwork_by_group.end()) {
                throw std::invalid_argument(
                    "gcp_compute_quorum_manager: no subnetwork for group: " + target_group);
            }
            const std::string& subnetwork = sit->second;

            if (replacing.has_value()) {
                std::cerr << "[gcp_compute_quorum_manager::provision_node] replacing node "
                          << node_id_str(*replacing) << " in " << target_group << "\n";
            }

            constexpr int kMaxAttempts = 5;
            std::exception_ptr last_error;
            for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
                NodeId new_id = generate_node_id();
                const std::string name = node_id_to_instance_name(_config.cluster_name, new_id);

                fiu_do_on("raft/gcp/compute/insert_instance",
                          throw std::runtime_error("fault: raft/gcp/compute/insert_instance"););

                auto instance = build_instance(name, new_id, target_group, subnetwork);
                auto insert =
                    _instances.InsertInstance(_config.gcp.project_id, target_group, instance).get();
                if (!insert) {
                    if (insert.status().code() == google::cloud::StatusCode::kAlreadyExists) {
                        last_error = std::make_exception_ptr(
                            std::runtime_error("instance name already in use: " + name));
                        continue;  // NodeId collision — regenerate and retry.
                    }
                    throw std::runtime_error("gcp instances.insert: " + insert.status().message());
                }

                // Await the insert zone operation. A NAME_IN_USE error surfaces
                // here as an operation error; treat it as a collision and retry.
                try {
                    std::move(wait_for_zone_operation(
                                  _zone_ops, _config.gcp.project_id, target_group, insert->name(),
                                  _config.gcp.api_timeout, _config.gcp.operation_poll_interval))
                        .get();
                } catch (const std::exception& op_ex) {
                    const std::string what = op_ex.what();
                    if (what.find("ALREADY_EXISTS") != std::string::npos ||
                        what.find("NAME_IN_USE") != std::string::npos) {
                        last_error = std::current_exception();
                        continue;  // NodeId collision — regenerate and retry.
                    }
                    throw;  // Non-collision operation failure: no instance running, no cleanup.
                }

                // Poll instances.get until RUNNING or provision_timeout.
                std::string internal_ip;
                const auto deadline = std::chrono::steady_clock::now() + _config.provision_timeout;
                while (std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(_config.poll_interval);
                    auto got = _instances.GetInstance(_config.gcp.project_id, target_group, name);
                    if (!got) {
                        continue;
                    }
                    if (got->status() == "RUNNING") {
                        if (got->network_interfaces_size() > 0) {
                            internal_ip = got->network_interfaces(0).network_ip();
                        }
                        if (!internal_ip.empty()) {
                            break;
                        }
                    }
                }

                if (internal_ip.empty()) {
                    // Timed out reaching RUNNING: best-effort delete so we don't
                    // leak a half-provisioned instance, then reject.
                    delete_best_effort(target_group, name);
                    throw gcp_operation_timeout("gcp provision timeout for instance " + name);
                }

                Address addr =
                    static_cast<Address>(internal_ip + ":" + std::to_string(_config.node_port));
                return future_factory_default::makeFuture(peer_info<NodeId, Address>{new_id, addr});
            }

            // Exhausted collision retries.
            if (last_error) {
                std::rethrow_exception(last_error);
            }
            throw std::runtime_error("gcp provision: exhausted NodeId collision retries");
        } catch (const std::exception& ex) {
            return future_factory_default::makeExceptionalFuture<peer_info<NodeId, Address>>(
                std::make_exception_ptr(std::runtime_error(
                    std::string("gcp_compute_quorum_manager::provision_node: ") + ex.what())));
        }
    }

    /// @brief Deletes the GCE instance for @p node_id and awaits the delete zone
    ///        operation. `NOT_FOUND` is treated as idempotent success.
    auto decommission_node(const NodeId& node_id) -> kythira::future_default<void> {
        try {
            fiu_do_on("raft/gcp/compute/delete_instance",
                      throw std::runtime_error("fault: raft/gcp/compute/delete_instance"););

            const std::string name = node_id_to_instance_name(_config.cluster_name, node_id);
            const std::string zone = zone_for_node(node_id);

            auto del = _instances.DeleteInstance(_config.gcp.project_id, zone, name).get();
            if (!del) {
                if (del.status().code() == google::cloud::StatusCode::kNotFound) {
                    return future_factory_default::makeFuture();  // Already gone.
                }
                throw std::runtime_error("gcp instances.delete: " + del.status().message());
            }
            return wait_for_zone_operation(_zone_ops, _config.gcp.project_id, zone, del->name(),
                                           _config.gcp.api_timeout,
                                           _config.gcp.operation_poll_interval);
        } catch (const std::exception& ex) {
            return future_factory_default::makeExceptionalFuture<void>(
                std::make_exception_ptr(std::runtime_error(
                    std::string("gcp_compute_quorum_manager::decommission_node: ") + ex.what())));
        }
    }

    /// @brief Returns the desired topology from the configuration (synchronous).
    [[nodiscard]] auto topology() const -> desired_topology<std::string> {
        return _config.topology;
    }

    // ── Pure name <-> NodeId functions (no API call) ────────────────────────

    /// @brief Renders `"kythira-{cluster_name}-{node_id}"`. Public for testing
    ///        (Requirement 23 AC 10).
    [[nodiscard]] static std::string node_id_to_instance_name(const std::string& cluster_name,
                                                              NodeId node_id) {
        return "kythira-" + cluster_name + "-" + node_id_str(node_id);
    }

    /// @brief Inverse of `node_id_to_instance_name`. Returns `std::nullopt` on
    ///        any mismatch (wrong prefix, non-numeric suffix) rather than
    ///        throwing, so it can defensively scan `instances.list` results.
    [[nodiscard]] static std::optional<NodeId> instance_name_to_node_id(
        const std::string& cluster_name, const std::string& instance_name) {
        const std::string prefix = "kythira-" + cluster_name + "-";
        if (instance_name.size() <= prefix.size() ||
            instance_name.compare(0, prefix.size(), prefix) != 0) {
            return std::nullopt;
        }
        const std::string suffix = instance_name.substr(prefix.size());
        return parse_node_id(suffix);
    }

private:
    gcp_compute_quorum_manager_config<std::string> _config;
    google::cloud::compute_instances_v1::InstancesClient _instances;
    google::cloud::compute_zone_operations_v1::ZoneOperationsClient _zone_ops;

    // ── NodeId helpers ──────────────────────────────────────────────────────

    static auto node_id_str(const NodeId& id) -> std::string {
        if constexpr (std::is_same_v<NodeId, std::string>) {
            return id;
        } else {
            return std::to_string(id);
        }
    }

    static auto parse_node_id(const std::string& s) -> std::optional<NodeId> {
        if (s.empty()) {
            return std::nullopt;
        }
        for (char c : s) {
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
        }
        try {
            std::uint64_t v = std::stoull(s);
            if constexpr (std::is_same_v<NodeId, std::string>) {
                return std::to_string(v);
            } else {
                return static_cast<NodeId>(v);
            }
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    /// Draws a cryptographically-strong 63-bit random value (top bit clear so it
    /// is representable whether `NodeId` is signed or unsigned) — Requirement 5.
    static auto generate_node_id() -> NodeId {
        std::random_device rd;
        std::uint64_t hi = rd();
        std::uint64_t lo = rd();
        std::uint64_t v = ((hi << 32) | lo) & 0x7FFF'FFFF'FFFF'FFFFULL;
        if (v == 0) {
            v = 1;  // Never mint the reserved 0 id.
        }
        if constexpr (std::is_same_v<NodeId, std::string>) {
            return std::to_string(v);
        } else {
            return static_cast<NodeId>(v);
        }
    }

    // ── Zone resolution ─────────────────────────────────────────────────────

    /// Resolves the zone a node lives in from `topology`/`subnetwork_by_group`.
    /// With direct-Compute instances the name encodes the NodeId but not the
    /// zone, so decommission scans the configured zones. When only one zone is
    /// configured the answer is unambiguous; otherwise `instances.delete` is
    /// attempted per zone until one succeeds or all report NOT_FOUND.
    auto zone_for_node(const NodeId& node_id) -> std::string {
        const std::string name = node_id_to_instance_name(_config.cluster_name, node_id);
        for (const auto& [zone, _] : _config.subnetwork_by_group) {
            auto got = _instances.GetInstance(_config.gcp.project_id, zone, name);
            if (got) {
                return zone;
            }
        }
        // Fall back to the first configured zone; delete will report NOT_FOUND
        // there and be treated as idempotent success.
        if (!_config.subnetwork_by_group.empty()) {
            return _config.subnetwork_by_group.begin()->first;
        }
        throw std::runtime_error("gcp_compute_quorum_manager: no zones configured");
    }

    void delete_best_effort(const std::string& zone, const std::string& name) {
        try {
            auto del = _instances.DeleteInstance(_config.gcp.project_id, zone, name).get();
            if (del) {
                // Bounded wait so cleanup cannot hang indefinitely.
                std::move(wait_for_zone_operation(_zone_ops, _config.gcp.project_id, zone,
                                                  del->name(), std::chrono::seconds{30},
                                                  _config.gcp.operation_poll_interval))
                    .get();
            }
        } catch (const std::exception& ex) {
            std::cerr << "[gcp_compute_quorum_manager] best-effort cleanup of " << name
                      << " failed: " << ex.what() << "\n";
        }
    }

    // ── Instance-resource construction ──────────────────────────────────────

    auto placement_label(const std::string& target_group) const -> std::string {
        if (auto it = _config.placement_by_group.find(target_group);
            it != _config.placement_by_group.end() && !it->second.self_link.empty()) {
            return "compact";
        }
        return "none";
    }

    auto build_instance(const std::string& name, const NodeId& node_id,
                        const std::string& target_group, const std::string& subnetwork) const
        -> google::cloud::cpp::compute::v1::Instance {
        google::cloud::cpp::compute::v1::Instance instance;
        instance.set_name(name);
        instance.set_machine_type("zones/" + target_group + "/machineTypes/" +
                                  _config.machine_type);

        auto* disk = instance.add_disks();
        disk->set_boot(true);
        disk->set_auto_delete(true);
        auto* params = disk->mutable_initialize_params();
        params->set_source_image(_config.boot_disk_image);
        // Compute encodes int64 fields like diskSizeGb as decimal strings.
        params->set_disk_size_gb(std::to_string(_config.boot_disk_size_gb));

        // Both fields accept a short name or a reference; `instances.insert`
        // accepts only the latter, so qualify short names here.
        auto* ni = instance.add_network_interfaces();
        ni->set_network(gcp_qualify_network(_config.network));
        ni->set_subnetwork(gcp_qualify_subnetwork(subnetwork, target_group));

        if (!_config.service_account_email.empty()) {
            auto* sa = instance.add_service_accounts();
            sa->set_email(_config.service_account_email);
            for (const auto& scope : _config.service_account_scopes) {
                sa->add_scopes(scope);
            }
        }

        // Metadata: startup-script (rendered) + enable-guest-attributes=TRUE.
        auto* metadata = instance.mutable_metadata();
        if (!_config.startup_script_template.empty()) {
            auto* item = metadata->add_items();
            item->set_key("startup-script");
            item->set_value(render_startup_script(node_id, target_group));
        }
        {
            auto* item = metadata->add_items();
            item->set_key("enable-guest-attributes");
            item->set_value("TRUE");
        }

        // Labels: the six standard labels plus extra_labels (which never
        // override the standard set).
        auto& labels = *instance.mutable_labels();
        labels["kythira-cluster"] = _config.cluster_name;
        labels["kythira-node-id"] = node_id_str(node_id);
        labels["kythira-group"] = target_group;
        labels["kythira-managed-by"] = "kythira-gce-quorum-manager";
        labels["kythira-market"] = _config.spot ? "spot" : "standard";
        labels["kythira-placement"] = placement_label(target_group);
        for (const auto& [k, v] : _config.extra_labels) {
            if (labels.find(k) == labels.end()) {
                labels[k] = v;
            }
        }

        if (_config.spot) {
            instance.mutable_scheduling()->set_provisioning_model("SPOT");
        }

        if (auto it = _config.placement_by_group.find(target_group);
            it != _config.placement_by_group.end() && !it->second.self_link.empty()) {
            instance.add_resource_policies(it->second.self_link);
        }

        return instance;
    }

    [[nodiscard]] auto render_startup_script(const NodeId& node_id, const std::string& zone) const
        -> std::string {
        std::string result = _config.startup_script_template;
        auto replace_all = [&](const std::string& from, const std::string& to) {
            std::size_t pos = 0;
            while ((pos = result.find(from, pos)) != std::string::npos) {
                result.replace(pos, from.size(), to);
                pos += to.size();
            }
        };
        replace_all("{NODE_ID}", node_id_str(node_id));
        replace_all("{NODE_PORT}", std::to_string(_config.node_port));
        replace_all("{CLUSTER}", _config.cluster_name);
        replace_all("{ZONE}", zone);
        return result;
    }

    // ── Health computation (shared shape with the AWS managers) ─────────────

    [[nodiscard]] auto build_health(const std::vector<node_placement<NodeId, std::string>>& cluster,
                                    const std::map<std::string, bool>& live_map) const
        -> kythira::future_default<quorum_health<NodeId, std::string>> {
        std::vector<NodeId> unreachable;
        std::size_t live_count = 0;
        std::map<std::string, std::size_t> group_live;
        for (const auto& np : cluster) {
            auto key = node_id_str(np.node_id);
            auto it = live_map.find(key);
            bool is_live = (it != live_map.end() && it->second);
            if (is_live) {
                ++live_count;
                group_live[np.group_id]++;
            } else {
                unreachable.push_back(np.node_id);
            }
        }

        std::vector<placement_group_health<NodeId, std::string>> groups;
        for (const auto& gt : _config.topology.groups) {
            std::size_t gl = 0;
            if (auto it = group_live.find(gt.group_id); it != group_live.end()) {
                gl = it->second;
            }
            std::vector<NodeId> g_unreach;
            for (const auto& nid : unreachable) {
                for (const auto& np : cluster) {
                    if (np.node_id == nid && np.group_id == gt.group_id) {
                        g_unreach.push_back(nid);
                    }
                }
            }
            groups.push_back({.group_id = gt.group_id,
                              .live_count = gl,
                              .target_count = gt.target_count,
                              .unreachable_nodes = std::move(g_unreach)});
        }

        std::size_t total = cluster.size();
        return future_factory_default::makeFuture(quorum_health<NodeId, std::string>{
            .status = compute_status(live_count, total),
            .live_node_count = live_count,
            .total_node_count = total,
            .unreachable_nodes = std::move(unreachable),
            .groups = std::move(groups),
        });
    }

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
        if (live < total) {
            return quorum_status::degraded;
        }
        return quorum_status::healthy;
    }
};

static_assert(quorum_manager<gcp_compute_quorum_manager<std::uint64_t, std::string>, std::uint64_t,
                             std::string, std::string>,
              "gcp_compute_quorum_manager must satisfy quorum_manager");

}  // namespace kythira

#endif  // KYTHIRA_HAS_GCP_SDK

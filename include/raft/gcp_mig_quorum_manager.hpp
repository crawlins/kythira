#pragma once

/// @file gcp_mig_quorum_manager.hpp
/// @brief `quorum_manager` implementation that drives one zonal Managed Instance
///        Group (MIG) per placement group. The GCP analogue of
///        `aws_asg_quorum_manager`.
///
/// Unlike `gcp_compute_quorum_manager`, the MIG — not kythira — assigns each
/// instance's name when it creates a replica during a `resize`, so kythira
/// cannot pre-compute a name from a `NodeId`. This manager instead mints the
/// `NodeId`, writes it as a `kythira-node-id` instance label after the instance
/// appears, and reads that label back via `instances.list` to resolve
/// `NodeId → name` in `assess_quorum`/`decommission_node`. This is the one
/// respect in which it is not purely stateless — a direct consequence of MIGs
/// owning instance naming (see the design doc, "The Core Design Decision").

#include <raft/fault_injection.hpp>
#include <raft/future_default.hpp>
#include <raft/gcp_client_config.hpp>
#include <raft/gcp_client_options.hpp>
#include <raft/gcp_operation_wait.hpp>
#include <raft/quorum_management.hpp>

#ifdef KYTHIRA_HAS_GCP_SDK

#include <google/cloud/compute/instance_group_managers/v1/instance_group_managers_client.h>
#include <google/cloud/compute/instance_group_managers/v1/instance_group_managers_options.h>
#include <google/cloud/compute/instances/v1/instances_client.h>
#include <google/cloud/compute/instances/v1/instances_options.h>
#include <google/cloud/compute/zone_operations/v1/zone_operations_client.h>
#include <google/cloud/compute/zone_operations/v1/zone_operations_options.h>
#include <google/cloud/credentials.h>
#include <google/cloud/options.h>
#include <google/cloud/polling_policy.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace kythira {

namespace gcp_mig_detail {

/// Credentials, endpoint override and transport time bounds — see
/// `gcp_detail::make_base_options`, which this used to duplicate verbatim
/// alongside `gcp_compute_detail` and `gcp_privateca_detail`.
inline auto make_options(const gcp_client_config& gcp) -> google::cloud::Options {
    return gcp_detail::make_base_options(gcp);
}

inline auto make_migs_client(const gcp_client_config& gcp)
    -> google::cloud::compute_instance_group_managers_v1::InstanceGroupManagersClient {
    namespace ig = google::cloud::compute_instance_group_managers_v1;
    // MIG `resize` is long-running, and slower than an instance insert/delete —
    // so the `operation_timeout`-scaled polling policy matters here most of all.
    auto opts = gcp_detail::with_retry_policies<
        ig::InstanceGroupManagersRetryPolicyOption, ig::InstanceGroupManagersLimitedTimeRetryPolicy,
        ig::InstanceGroupManagersBackoffPolicyOption, ig::InstanceGroupManagersPollingPolicyOption>(
        gcp_detail::make_base_options(gcp), gcp);
    return ig::InstanceGroupManagersClient(
        ig::MakeInstanceGroupManagersConnectionRest(std::move(opts)));
}

inline auto make_instances_client(const gcp_client_config& gcp)
    -> google::cloud::compute_instances_v1::InstancesClient {
    namespace ci = google::cloud::compute_instances_v1;
    auto opts = gcp_detail::with_retry_policies<
        ci::InstancesRetryPolicyOption, ci::InstancesLimitedTimeRetryPolicy,
        ci::InstancesBackoffPolicyOption, ci::InstancesPollingPolicyOption>(
        gcp_detail::make_base_options(gcp), gcp);
    return ci::InstancesClient(ci::MakeInstancesConnectionRest(std::move(opts)));
}

inline auto make_zone_operations_client(const gcp_client_config& gcp)
    -> google::cloud::compute_zone_operations_v1::ZoneOperationsClient {
    namespace zo = google::cloud::compute_zone_operations_v1;
    auto opts = gcp_detail::with_retry_policies_no_lro<zo::ZoneOperationsRetryPolicyOption,
                                                       zo::ZoneOperationsLimitedTimeRetryPolicy,
                                                       zo::ZoneOperationsBackoffPolicyOption>(
        gcp_detail::make_base_options(gcp), gcp);
    return zo::ZoneOperationsClient(zo::MakeZoneOperationsConnectionRest(std::move(opts)));
}

/// Returns the last `/`-delimited segment of a GCE self-link/URL — the short
/// resource name. MIG-managed-instance and instance self-links carry the name
/// as their final path component.
inline auto last_path_segment(const std::string& url) -> std::string {
    auto pos = url.find_last_of('/');
    return pos == std::string::npos ? url : url.substr(pos + 1);
}

}  // namespace gcp_mig_detail

// ============================================================================
// gcp_mig_quorum_manager_config
// ============================================================================

/// @brief Configuration for `gcp_mig_quorum_manager`.
template<typename GroupId = std::string> struct gcp_mig_quorum_manager_config {
    /// Project, credentials, endpoint override, timeouts.
    gcp_client_config gcp{};
    /// Logical cluster name; scopes the `kythira-cluster` label filter. Must
    /// satisfy `is_valid_gcp_label`. Required.
    std::string cluster_name;
    /// GroupId (zone) → zonal Managed Instance Group name. Required, ≥1 entry.
    std::map<GroupId, std::string> mig_by_group;
    /// TCP port the kythira process listens on; written into the returned address.
    std::uint16_t node_port{7000};
    /// Max time to wait for a new instance to appear and reach `RUNNING`.
    std::chrono::seconds provision_timeout{300};
    /// Interval between `instanceGroupManagers.listManagedInstances` polls.
    std::chrono::milliseconds poll_interval{5000};
    /// Desired counts per group (used by `topology()` and validation only; the
    /// MIG target size is not set at construction).
    desired_topology<GroupId> topology{};
};

// ============================================================================
// gcp_mig_quorum_manager
// ============================================================================

/// @brief `quorum_manager` implementation backed by zonal Managed Instance
///        Groups (one per placement group / zone).
///
/// @tparam NodeId  Node identifier type; defaults to `std::uint64_t`.
/// @tparam Address Network address type; defaults to `std::string`.
template<typename NodeId = std::uint64_t, typename Address = std::string>
requires node_id<NodeId>
class gcp_mig_quorum_manager {
public:
    using node_id_type = NodeId;
    using address_type = Address;
    using placement_group_id_type = std::string;

    /// @brief Constructs the manager and validates the configuration.
    ///
    /// @throws std::invalid_argument if `cluster_name` is empty or not a valid
    ///   GCP label, `mig_by_group` is empty, `node_port` is zero, any topology
    ///   group has no `mig_by_group` entry, or any configured MIG has a non-empty
    ///   `autoHealingPolicies` (Requirement 18 — an autohealer racing kythira's
    ///   own remediation loop would risk split-brain).
    explicit gcp_mig_quorum_manager(gcp_mig_quorum_manager_config<std::string> config)
        : _config(std::move(config)),
          _migs(gcp_mig_detail::make_migs_client(_config.gcp)),
          _instances(gcp_mig_detail::make_instances_client(_config.gcp)),
          _zone_ops(gcp_mig_detail::make_zone_operations_client(_config.gcp)) {
        if (_config.gcp.project_id.empty()) {
            throw std::invalid_argument("gcp_mig_quorum_manager: project_id must be non-empty");
        }
        if (_config.cluster_name.empty()) {
            throw std::invalid_argument("gcp_mig_quorum_manager: cluster_name must be non-empty");
        }
        if (!is_valid_gcp_label(_config.cluster_name)) {
            throw std::invalid_argument("gcp_mig_quorum_manager: cluster_name '" +
                                        _config.cluster_name + "' is not a valid GCP label");
        }
        if (_config.mig_by_group.empty()) {
            throw std::invalid_argument("gcp_mig_quorum_manager: mig_by_group must be non-empty");
        }
        if (_config.node_port == 0) {
            throw std::invalid_argument("gcp_mig_quorum_manager: node_port must be non-zero");
        }
        for (const auto& gt : _config.topology.groups) {
            if (_config.mig_by_group.find(gt.group_id) == _config.mig_by_group.end()) {
                throw std::invalid_argument(
                    "gcp_mig_quorum_manager: no MIG configured for group: " + gt.group_id);
            }
        }
        // Reject any MIG with an autohealing policy (Requirement 18 AC 1).
        bool validate = true;
        fiu_do_on("raft/gcp/mig/skip_autohealing_validation", validate = false;);
        if (validate) {
            for (const auto& [zone, mig_name] : _config.mig_by_group) {
                auto mig = _migs.GetInstanceGroupManager(_config.gcp.project_id, zone, mig_name);
                if (!mig) {
                    throw std::invalid_argument(
                        "gcp_mig_quorum_manager: instanceGroupManagers.get failed for '" +
                        mig_name + "': " + mig.status().message());
                }
                if (mig->auto_healing_policies_size() > 0) {
                    throw std::invalid_argument(
                        "gcp_mig_quorum_manager: MIG '" + mig_name +
                        "' has an autoHealingPolicies entry; kythira requires it be unset to avoid "
                        "split-brain (see Requirement 18)");
                }
            }
        }
    }

    gcp_mig_quorum_manager(gcp_mig_quorum_manager&&) noexcept = default;
    gcp_mig_quorum_manager& operator=(gcp_mig_quorum_manager&&) noexcept = default;
    gcp_mig_quorum_manager(const gcp_mig_quorum_manager&) = delete;
    gcp_mig_quorum_manager& operator=(const gcp_mig_quorum_manager&) = delete;

    /// @brief Reports which nodes are live at the GCE layer, keyed by the
    ///        `kythira-node-id` label the MIG-created instances carry.
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

            // kythira-node-id label value → running?
            std::map<std::string, bool> id_running;
            for (const auto& zone : zones) {
                fiu_do_on("raft/gcp/mig/list_instances",
                          throw std::runtime_error("fault: raft/gcp/mig/list_instances"););

                google::cloud::cpp::compute::instances::v1::ListInstancesRequest req;
                req.set_project(_config.gcp.project_id);
                req.set_zone(zone);
                req.set_filter("labels.kythira-cluster = \"" + _config.cluster_name + "\"");

                for (auto const& maybe_inst : _instances.ListInstances(req)) {
                    if (!maybe_inst) {
                        throw std::runtime_error("gcp instances.list (" + zone +
                                                 "): " + maybe_inst.status().message());
                    }
                    const auto& labels = maybe_inst->labels();
                    auto it = labels.find("kythira-node-id");
                    if (it != labels.end()) {
                        id_running[it->second] = (maybe_inst->status() == "RUNNING");
                    }
                }
            }

            std::map<std::string, bool> live_map;
            for (const auto& np : cluster) {
                auto key = node_id_str(np.node_id);
                auto it = id_running.find(key);
                live_map[key] = (it != id_running.end() && it->second);
            }
            return build_health(cluster, live_map);
        } catch (const std::exception& ex) {
            return future_factory_default::makeExceptionalFuture<
                quorum_health<NodeId, std::string>>(std::make_exception_ptr(std::runtime_error(
                std::string("gcp_mig_quorum_manager::assess_quorum: ") + ex.what())));
        }
    }

    /// @brief Assesses quorum, decommissions unreachable nodes, and provisions
    ///        replacements to meet the desired topology (see the AWS analogue).
    auto maintain_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::future_default<quorum_health<NodeId, std::string>> {
        try {
            fiu_do_on("raft/gcp/mig/maintain_quorum",
                      throw std::runtime_error("fault: raft/gcp/mig/maintain_quorum"););
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
                std::cerr << "[gcp_mig_quorum_manager::maintain_quorum] decommission of "
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
                    std::cerr << "[gcp_mig_quorum_manager::maintain_quorum] provision in "
                              << gt.group_id << " failed: " << ex.what() << "\n";
                }
            }
        }

        return future_factory_default::makeFuture(std::move(pre_health));
    }

    /// @brief Grows the target MIG by one, waits for the new instance to appear,
    ///        mints a `NodeId`, and durably records it as a `kythira-node-id`
    ///        label so `assess_quorum`/`decommission_node` can find it later.
    auto provision_node(std::string target_group, std::optional<NodeId> replacing)
        -> kythira::future_default<peer_info<NodeId, Address>> {
        try {
            auto mit = _config.mig_by_group.find(target_group);
            if (mit == _config.mig_by_group.end()) {
                throw std::invalid_argument("gcp_mig_quorum_manager: no MIG for group: " +
                                            target_group);
            }
            const std::string& mig_name = mit->second;
            const std::string& zone = target_group;

            if (replacing.has_value()) {
                std::cerr << "[gcp_mig_quorum_manager::provision_node] replacing node "
                          << node_id_str(*replacing) << " in " << target_group << "\n";
            }

            fiu_do_on("raft/gcp/mig/resize",
                      throw std::runtime_error("fault: raft/gcp/mig/resize"););

            // Read the current target size, then resize +1.
            auto mig = _migs.GetInstanceGroupManager(_config.gcp.project_id, zone, mig_name);
            if (!mig) {
                throw std::runtime_error("gcp instanceGroupManagers.get: " +
                                         mig.status().message());
            }
            const std::int32_t original_size = mig->target_size();

            auto resize =
                _migs.Resize(_config.gcp.project_id, zone, mig_name, original_size + 1).get();
            if (!resize) {
                throw std::runtime_error("gcp instanceGroupManagers.resize: " +
                                         resize.status().message());
            }
            std::move(wait_for_zone_operation(_zone_ops, _config.gcp.project_id, zone,
                                              resize->name(), _config.gcp.api_timeout,
                                              _config.gcp.operation_poll_interval))
                .get();

            // Poll listManagedInstances for a fully-created (currentAction ==
            // NONE) instance that does not yet carry a kythira-node-id label.
            std::string new_instance_name;
            const auto deadline = std::chrono::steady_clock::now() + _config.provision_timeout;
            while (std::chrono::steady_clock::now() < deadline && new_instance_name.empty()) {
                std::this_thread::sleep_for(_config.poll_interval);
                // listManagedInstances returns a single response object (not a
                // paginated StreamRange like instances.list), so unwrap the
                // StatusOr and iterate its managed_instances() field.
                auto managed = _migs.ListManagedInstances(_config.gcp.project_id, zone, mig_name);
                if (!managed) {
                    continue;  // Transient list failure — retry on the next poll.
                }
                for (const auto& mi : managed->managed_instances()) {
                    if (mi.current_action() != "NONE") {
                        continue;
                    }
                    const std::string cand = gcp_mig_detail::last_path_segment(mi.instance());
                    if (cand.empty()) {
                        continue;
                    }
                    auto inst = _instances.GetInstance(_config.gcp.project_id, zone, cand);
                    if (!inst) {
                        continue;
                    }
                    if (inst->labels().find("kythira-node-id") == inst->labels().end()) {
                        new_instance_name = cand;
                        break;
                    }
                }
            }

            if (new_instance_name.empty()) {
                // Best-effort rollback: restore the original target size.
                auto rollback =
                    _migs.Resize(_config.gcp.project_id, zone, mig_name, original_size).get();
                (void)rollback;
                throw gcp_operation_timeout(
                    "gcp mig provision timeout: no unlabelled instance appeared in " +
                    target_group);
            }

            // Mint a NodeId and label the instance, retrying on collisions and
            // on fingerprint-mismatch (another writer raced the labels).
            NodeId new_id{};
            std::string internal_ip;
            constexpr int kIdAttempts = 5;
            bool labelled = false;
            for (int id_attempt = 0; id_attempt < kIdAttempts && !labelled; ++id_attempt) {
                new_id = generate_node_id();

                constexpr int kFingerprintAttempts = 3;
                for (int fp_attempt = 0; fp_attempt < kFingerprintAttempts; ++fp_attempt) {
                    auto inst =
                        _instances.GetInstance(_config.gcp.project_id, zone, new_instance_name);
                    if (!inst) {
                        throw std::runtime_error("gcp instances.get (pre-setLabels): " +
                                                 inst.status().message());
                    }
                    // Collision guard: another instance already using this id.
                    if (label_id_in_use(zone, node_id_str(new_id), new_instance_name)) {
                        break;  // Regenerate NodeId.
                    }

                    google::cloud::cpp::compute::v1::InstancesSetLabelsRequest set_req;
                    set_req.set_label_fingerprint(inst->label_fingerprint());
                    auto& labels = *set_req.mutable_labels();
                    for (const auto& [k, v] : inst->labels()) {
                        labels[k] = v;
                    }
                    labels["kythira-cluster"] = _config.cluster_name;
                    labels["kythira-node-id"] = node_id_str(new_id);
                    labels["kythira-group"] = zone;
                    labels["kythira-managed-by"] = "kythira-mig-quorum-manager";

                    auto set =
                        _instances
                            .SetLabels(_config.gcp.project_id, zone, new_instance_name, set_req)
                            .get();
                    if (!set) {
                        if (set.status().code() == google::cloud::StatusCode::kFailedPrecondition) {
                            continue;  // Fingerprint mismatch — re-read and retry.
                        }
                        throw std::runtime_error("gcp instances.setLabels: " +
                                                 set.status().message());
                    }
                    std::move(wait_for_zone_operation(_zone_ops, _config.gcp.project_id, zone,
                                                      set->name(), _config.gcp.api_timeout,
                                                      _config.gcp.operation_poll_interval))
                        .get();
                    if (inst->network_interfaces_size() > 0) {
                        internal_ip = inst->network_interfaces(0).network_ip();
                    }
                    labelled = true;
                    break;
                }
            }

            if (!labelled) {
                throw std::runtime_error(
                    "gcp mig provision: exhausted NodeId/fingerprint label retries");
            }
            if (internal_ip.empty()) {
                auto inst = _instances.GetInstance(_config.gcp.project_id, zone, new_instance_name);
                if (inst && inst->network_interfaces_size() > 0) {
                    internal_ip = inst->network_interfaces(0).network_ip();
                }
            }

            Address addr =
                static_cast<Address>(internal_ip + ":" + std::to_string(_config.node_port));
            return future_factory_default::makeFuture(peer_info<NodeId, Address>{new_id, addr});
        } catch (const std::exception& ex) {
            return future_factory_default::makeExceptionalFuture<peer_info<NodeId, Address>>(
                std::make_exception_ptr(std::runtime_error(
                    std::string("gcp_mig_quorum_manager::provision_node: ") + ex.what())));
        }
    }

    /// @brief Removes a node from its MIG and deletes it, decrementing the MIG's
    ///        target size atomically. `NOT_FOUND` is idempotent success.
    auto decommission_node(const NodeId& node_id) -> kythira::future_default<void> {
        try {
            fiu_do_on("raft/gcp/mig/delete_instances",
                      throw std::runtime_error("fault: raft/gcp/mig/delete_instances"););

            for (const auto& [zone, mig_name] : _config.mig_by_group) {
                auto found = std::move(find_instance_by_node_id(zone, node_id)).get();
                if (!found.has_value()) {
                    continue;
                }
                const std::string instance_self_link = *found;

                google::cloud::cpp::compute::v1::InstanceGroupManagersDeleteInstancesRequest req;
                req.add_instances(instance_self_link);

                auto del = _migs.DeleteInstances(_config.gcp.project_id, zone, mig_name, req).get();
                if (!del) {
                    if (del.status().code() == google::cloud::StatusCode::kNotFound) {
                        return future_factory_default::makeFuture();  // Already gone.
                    }
                    throw std::runtime_error("gcp instanceGroupManagers.deleteInstances: " +
                                             del.status().message());
                }
                return wait_for_zone_operation(_zone_ops, _config.gcp.project_id, zone, del->name(),
                                               _config.gcp.api_timeout,
                                               _config.gcp.operation_poll_interval);
            }
            // No matching instance in any zone — idempotent success.
            return future_factory_default::makeFuture();
        } catch (const std::exception& ex) {
            return future_factory_default::makeExceptionalFuture<void>(
                std::make_exception_ptr(std::runtime_error(
                    std::string("gcp_mig_quorum_manager::decommission_node: ") + ex.what())));
        }
    }

    /// @brief Returns the desired topology from the configuration (synchronous).
    [[nodiscard]] auto topology() const -> desired_topology<std::string> {
        return _config.topology;
    }

private:
    gcp_mig_quorum_manager_config<std::string> _config;
    google::cloud::compute_instance_group_managers_v1::InstanceGroupManagersClient _migs;
    google::cloud::compute_instances_v1::InstancesClient _instances;
    google::cloud::compute_zone_operations_v1::ZoneOperationsClient _zone_ops;

    /// Resolves NodeId → instance self-link in @p zone via a labelled
    /// `instances.list` lookup. Unlike `gcp_compute_quorum_manager`, this is a
    /// real API call, not a pure function.
    [[nodiscard]] auto find_instance_by_node_id(const std::string& zone, NodeId node_id)
        -> kythira::future_default<std::optional<std::string>> {
        try {
            google::cloud::cpp::compute::instances::v1::ListInstancesRequest req;
            req.set_project(_config.gcp.project_id);
            req.set_zone(zone);
            req.set_filter("labels.kythira-node-id = \"" + node_id_str(node_id) + "\"");
            for (auto const& maybe_inst : _instances.ListInstances(req)) {
                if (!maybe_inst) {
                    throw std::runtime_error("gcp instances.list (" + zone +
                                             "): " + maybe_inst.status().message());
                }
                return future_factory_default::makeFuture(
                    std::optional<std::string>{maybe_inst->self_link()});
            }
            return future_factory_default::makeFuture(std::optional<std::string>{std::nullopt});
        } catch (const std::exception&) {
            return future_factory_default::makeExceptionalFuture<std::optional<std::string>>(
                std::current_exception());
        }
    }

    /// True if some *other* instance in @p zone already carries this
    /// `kythira-node-id` label value (a collision — practically unreachable
    /// given the 63-bit keyspace, checked for correctness).
    auto label_id_in_use(const std::string& zone, const std::string& id_str,
                         const std::string& except_name) -> bool {
        google::cloud::cpp::compute::instances::v1::ListInstancesRequest req;
        req.set_project(_config.gcp.project_id);
        req.set_zone(zone);
        req.set_filter("labels.kythira-node-id = \"" + id_str + "\"");
        for (auto const& maybe_inst : _instances.ListInstances(req)) {
            if (maybe_inst && maybe_inst->name() != except_name) {
                return true;
            }
        }
        return false;
    }

    static auto node_id_str(const NodeId& id) -> std::string {
        if constexpr (std::is_same_v<NodeId, std::string>) {
            return id;
        } else {
            return std::to_string(id);
        }
    }

    static auto generate_node_id() -> NodeId {
        std::random_device rd;
        std::uint64_t hi = rd();
        std::uint64_t lo = rd();
        std::uint64_t v = ((hi << 32) | lo) & 0x7FFF'FFFF'FFFF'FFFFULL;
        if (v == 0) {
            v = 1;
        }
        if constexpr (std::is_same_v<NodeId, std::string>) {
            return std::to_string(v);
        } else {
            return static_cast<NodeId>(v);
        }
    }

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

static_assert(quorum_manager<gcp_mig_quorum_manager<std::uint64_t, std::string>, std::uint64_t,
                             std::string, std::string>,
              "gcp_mig_quorum_manager must satisfy quorum_manager");

}  // namespace kythira

#endif  // KYTHIRA_HAS_GCP_SDK

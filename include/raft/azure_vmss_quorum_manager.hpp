// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file azure_vmss_quorum_manager.hpp
/// @brief `quorum_manager` implementation that provisions and monitors Raft nodes
///        through Azure Virtual Machine Scale Set (VMSS) instance-count changes.
///
/// Shares its ARM REST calling convention with `azure_vm_quorum_manager` (same
/// `Azure::Core::Http::_internal::HttpPipeline` construction, same tag-scan
/// `next_node_id()`), but intentionally does NOT share a base class with it —
/// see design.md's "Shared Private Helpers" section for the rationale, which
/// mirrors the AWS spec's identical non-sharing decision between
/// `aws_ec2_quorum_manager` and `aws_asg_quorum_manager`.

#include <raft/azure_client_config.hpp>
#include <raft/composite_node_id.hpp>
#include <raft/fault_injection.hpp>
#include <raft/future_default.hpp>
#include <raft/group_scale_rollback.hpp>
#include <raft/quorum_management.hpp>

#ifdef KYTHIRA_HAS_AZURE_SDK

#include <azure/core/context.hpp>
#include <azure/core/http/http.hpp>
#include <azure/core/http/policies/policy.hpp>
#include <azure/core/internal/client_options.hpp>
#include <azure/core/internal/http/pipeline.hpp>
#include <azure/core/io/body_stream.hpp>
#include <azure/core/url.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace kythira {

namespace azure_vmss_detail {

/// Per-VM scale-in protection on a Flexible scale set needs this API version
/// or later (group-scale-up-rollback design, task 1.4).
inline constexpr const char* protection_api_version = "2023-09-01";

/// @brief Map a member's `provisioningState` onto the rollback planner's
///        three classes: `Succeeded` serves, `Deleting` is leaving, and
///        anything else (`Creating`, `Updating`, `Failed`, ...) is treated as
///        on its way in.
[[nodiscard]] inline auto rollback_state(std::string_view provisioning_state)
    -> group_rollback::member_state {
    if (provisioning_state == "Succeeded") {
        return group_rollback::member_state::live;
    }
    if (provisioning_state == "Deleting") {
        return group_rollback::member_state::terminal;
    }
    return group_rollback::member_state::pending;
}

/// True when an ARM error text is a refusal for lack of permission, which
/// `parse_response` renders as `ARM request failed (403): AuthorizationFailed: ...`.
[[nodiscard]] inline auto is_permission_error(std::string_view message) -> bool {
    return message.find("(403)") != std::string_view::npos ||
           message.find("AuthorizationFailed") != std::string_view::npos;
}

}  // namespace azure_vmss_detail

// ============================================================================
// azure_vmss_quorum_manager_config
// ============================================================================

/// Configuration for `azure_vmss_quorum_manager`.
///
/// Spot/regular priority, zones, and custom data are all configured on the
/// scale set's own model (its launch-template equivalent) at scale-set
/// creation time — an out-of-band operator action, exactly like the AWS
/// spec's ASG launch-template/mixed-instances policy being out of scope for
/// `aws_asg_quorum_manager_config`. `provision_node` only changes
/// `sku.capacity` and tags the resulting instance; it never touches the scale
/// set's model.
///
/// Every scale set named here must use **Flexible** orchestration; the manager
/// refuses Uniform at construction. See `azure_vmss_quorum_manager`.
struct azure_vmss_quorum_manager_config {
    /// Azure subscription/resource-group/location/credential settings.
    azure_client_config azure{};
    /// Logical cluster name; used as the `kythira:cluster` tag value.
    std::string cluster_name;
    /// Maps each topology group_id to the VMSS name responsible for that group.
    std::map<std::string, std::string> scale_set_by_group;
    /// TCP port on which each node listens.
    std::uint16_t node_port{7000};
    /// Maximum time to wait for a newly provisioned instance to reach
    /// PowerState/running and be found untagged.
    std::chrono::seconds provision_timeout{300};
    /// Maximum time `decommission_node` waits for the deleted member to stop
    /// being listed.
    ///
    /// 600s, not the 30s this was hardcoded to. Under Flexible orchestration a
    /// member stays listed in `Deleting` for minutes -- the real-cloud
    /// workflow's own VMSS audit allows 900s for the same drain, and was
    /// measured needing most of it. The 30s was chosen when this manager
    /// targeted Uniform scale sets and was never exercised against either mode,
    /// so `vmss_decommission_removes_instance` failed the moment the case
    /// actually ran (run 36749754552).
    std::chrono::seconds decommission_timeout{600};
    /// Sleep interval between instance-list polls during provisioning.
    std::chrono::milliseconds poll_interval{5000};
    /// Target node counts per placement group.
    desired_topology<std::string> topology{};
};

// ============================================================================
// azure_vmss_quorum_manager
// ============================================================================

/// `quorum_manager` implementation that provisions and monitors Raft nodes
/// through `sku.capacity` changes on Azure Virtual Machine Scale Sets.
///
/// `azure_vmss_quorum_manager` cannot reuse a scale-set-local instance ID as
/// NodeId (unique only within one scale set, not across the several scale
/// sets one Raft cluster's placement groups may use), so it reuses the exact
/// same cluster-wide tag-scan `next_node_id()` as `azure_vm_quorum_manager`,
/// applying the resulting tag to the instance rather than encoding it into a
/// resource name (scale-set member names are Azure-assigned and not
/// renameable). Like that manager it allocates above an in-memory floor, so a
/// removed instance's id is not handed out again by the same manager.
///
/// That tag is why every scale set this manager is given must use **Flexible**
/// orchestration: a Flexible member is an ordinary
/// `Microsoft.Compute/virtualMachines` resource with tags of its own, while a
/// Uniform member has none and merely reflects the scale set's. The constructor
/// enforces it.
template<typename NodeId = std::uint64_t, typename Address = std::string>
requires kythira::node_id<NodeId>
class azure_vmss_quorum_manager {
public:
    using node_id_type = NodeId;
    using address_type = Address;
    using placement_group_id_type = std::string;

    /// Constructs the manager, validates the configuration, and verifies of
    /// every configured scale set that it uses **Flexible** orchestration and
    /// is NOT in `upgradePolicy.mode == "Automatic"`.
    ///
    /// Automatic mode (Requirement 10.3) lets a scale set autonomously replace
    /// instances outside this manager's control, violating Property 4 ("no
    /// autonomous replacement outside the quorum manager's control"). Uniform
    /// orchestration cannot store the per-instance tag this manager identifies
    /// nodes by, and fails to store it *silently*; see `tag_instance`.
    ///
    /// Then protects from scale-in every adopted member of this cluster that
    /// is not protected yet (group-scale-up-rollback Requirement 4.3), and
    /// throws `std::invalid_argument` if ARM refuses that for lack of
    /// permission.
    explicit azure_vmss_quorum_manager(azure_vmss_quorum_manager_config cfg)
        : _cfg(std::move(cfg)) {
        if (_cfg.cluster_name.empty()) {
            throw std::invalid_argument(
                "azure_vmss_quorum_manager: cluster_name must be non-empty");
        }
        if (_cfg.scale_set_by_group.empty()) {
            throw std::invalid_argument(
                "azure_vmss_quorum_manager: scale_set_by_group must be non-empty");
        }
        if (_cfg.node_port == 0) {
            throw std::invalid_argument("azure_vmss_quorum_manager: node_port must be non-zero");
        }
        for (const auto& gt : _cfg.topology.groups) {
            if (_cfg.scale_set_by_group.find(gt.group_id) == _cfg.scale_set_by_group.end()) {
                throw std::invalid_argument(
                    "azure_vmss_quorum_manager: no scale set configured for group: " + gt.group_id);
            }
        }

        _arm_endpoint = _cfg.azure.arm_endpoint_override.empty() ? "https://management.azure.com"
                                                                 : _cfg.azure.arm_endpoint_override;
        _arm_base = _arm_endpoint + "/subscriptions/" + _cfg.azure.subscription_id +
                    "/resourceGroups/" + _cfg.azure.resource_group;

        auto credential =
            _cfg.azure.credential ? _cfg.azure.credential : make_default_credential_chain();
        Azure::Core::Credentials::TokenRequestContext token_ctx;
        token_ctx.Scopes = {"https://management.azure.com/.default"};

        std::vector<std::unique_ptr<Azure::Core::Http::Policies::HttpPolicy>> per_retry_policies;
        per_retry_policies.emplace_back(
            std::make_unique<
                Azure::Core::Http::Policies::_internal::BearerTokenAuthenticationPolicy>(
                credential, std::move(token_ctx)));

        Azure::Core::_internal::ClientOptions client_options;
        client_options.Retry.MaxRetries = 3;
        if (_cfg.azure.transport) {
            client_options.Transport.Transport = _cfg.azure.transport;
        }
        _pipeline = std::make_shared<Azure::Core::Http::_internal::HttpPipeline>(
            client_options, "kythira-azure-vmss-quorum-manager", "1.0.0",
            std::move(per_retry_policies),
            std::vector<std::unique_ptr<Azure::Core::Http::Policies::HttpPolicy>>{});

        // Mirrors aws_asg_quorum_manager's raft/aws/asg/skip_health_check_validation
        // fault point: lets unit tests construct a working instance (to then
        // exercise fault-injection points on assess_quorum/provision_node/etc.)
        // without live Azure credentials or a real scale set to query.
        bool validate_upgrade_policy = true;
        fiu_do_on("raft/azure/vmss/skip_upgrade_policy_validation",
                  validate_upgrade_policy = false;);
        if (validate_upgrade_policy) {
            for (const auto& [group, scale_set] : _cfg.scale_set_by_group) {
                auto body = arm_get("/providers/Microsoft.Compute/virtualMachineScaleSets/" +
                                    scale_set + "?api-version=" + compute_api_version);

                // Orchestration mode first, because getting this wrong is
                // silent. This manager identifies each node by a
                // `kythira:node-id` tag on its instance, and a Uniform scale
                // set's members cannot hold tags of their own -- every write is
                // accepted, reports success, and changes nothing (see
                // `tag_instance`). Provisioning would appear to work and every
                // subsequent assess/decommission would fail to find the node.
                // Refusing here turns that into one clear error at construction.
                std::string orchestration;
                try {
                    orchestration =
                        std::string(body.at("properties").at("orchestrationMode").as_string());
                } catch (const std::exception&) {
                    orchestration.clear();
                }
                if (orchestration == "Uniform") {
                    throw std::invalid_argument(
                        "azure_vmss_quorum_manager: scale set '" + scale_set + "' (group '" +
                        group +
                        "') uses Uniform orchestration; kythira requires Flexible, because "
                        "Uniform scale-set members cannot carry the per-instance "
                        "kythira:node-id tag this manager identifies nodes by");
                }

                std::string mode;
                try {
                    mode = std::string(
                        body.at("properties").at("upgradePolicy").at("mode").as_string());
                } catch (const std::exception&) {
                    mode.clear();
                }
                if (mode == "Automatic") {
                    throw std::invalid_argument("azure_vmss_quorum_manager: scale set '" +
                                                scale_set + "' (group '" + group +
                                                "') uses upgradePolicy.mode=Automatic; kythira "
                                                "requires Manual or Rolling so "
                                                "only this manager replaces instances");
                }
                reconcile_scale_in_protection(scale_set);
            }
        }
    }

    /// Assesses cluster health from the resource group's VM list plus one power
    /// state read per node in `cluster`. Instances lacking a `kythira:node-id`
    /// tag are skipped — they belong to no kythira cluster node yet.
    ///
    /// This was one expanded list call per scale set while the manager targeted
    /// Uniform scale sets. Flexible offers no equivalent: the scale set's own
    /// instance list refuses `$expand=instanceView`, and the VM list only
    /// accepts that expansion alongside a scale-set filter that ARM rejected in
    /// every encoding tried. See `scale_set_vms` and `vm_is_ready`.
    auto assess_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::future_default<quorum_health<NodeId, std::string>> {
        try {
            fiu_do_on("raft/azure/vmss/list_instances",
                      throw std::runtime_error("fault: raft/azure/vmss/list_instances"););

            if (cluster.empty()) {
                return build_health(cluster, {});
            }

            // Only the node IDs this call was asked about, so an unrelated
            // cluster sharing the scale set costs nothing to assess.
            std::set<std::string> wanted;
            for (const auto& np : cluster) {
                wanted.insert(node_id_str(np.node_id));
                _id_floor.raise(np.node_id);
            }

            std::map<std::string, bool> live_map;
            for (const auto& [group, scale_set] : _cfg.scale_set_by_group) {
                (void)group;
                for (const auto& [vm_name, vm] : scale_set_vms(scale_set)) {
                    auto nid_tag = our_node_id_tag(vm);
                    if (!nid_tag || !wanted.contains(*nid_tag)) {
                        continue;
                    }
                    live_map[*nid_tag] = vm_is_ready(vm_name);
                }
            }

            return build_health(cluster, live_map);
        } catch (const std::exception& ex) {
            return future_factory_default::makeExceptionalFuture<
                quorum_health<NodeId, std::string>>(std::make_exception_ptr(std::runtime_error(
                std::string("azure_vmss_quorum_manager::assess_quorum: ") + ex.what())));
        }
    }

    /// Assesses quorum, decommissions unreachable nodes, and provisions
    /// replacements to meet the desired topology. Decommission/provision errors
    /// are logged to stderr but do not abort the operation.
    auto maintain_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::future_default<quorum_health<NodeId, std::string>> {
        try {
            fiu_do_on("raft/azure/vmss/maintain_quorum",
                      throw std::runtime_error("fault: raft/azure/vmss/maintain_quorum"););
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
                std::cerr << "[azure_vmss_quorum_manager::maintain_quorum] decommission of "
                          << node_id_str(nid) << " failed: " << ex.what() << "\n";
            }
        }

        for (const auto& gt : _cfg.topology.groups) {
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
                try {
                    std::move(provision_node(gt.group_id, std::nullopt)).get();
                } catch (const std::exception& ex) {
                    std::cerr << "[azure_vmss_quorum_manager::maintain_quorum] provision in "
                              << gt.group_id << " failed: " << ex.what() << "\n";
                }
            }
        }

        return future_factory_default::makeFuture(std::move(pre_health));
    }

    /// Increments `target_group`'s scale set capacity by one, waits for a
    /// `PowerState/running` instance that was not a member before the
    /// increment and lacks a `kythira:node-id` tag, then tags it with a
    /// freshly-assigned NodeId and protects it from scale-in.
    ///
    /// On timeout the increment is undone without letting the scale set choose
    /// a victim (group-scale-up-rollback Requirements 2-3): every member the
    /// increment created is deleted by name through the scale set's `delete`
    /// action, which also lowers `sku.capacity`, and the capacity is restored
    /// by a `PATCH` only when none was created. The error ends with what the
    /// rollback did, e.g. `rollback: removed kythira_abc123 (fresh, Creating)`.
    auto provision_node(std::string target_group, std::optional<NodeId> /*replacing*/)
        -> kythira::future_default<peer_info<NodeId, Address>> {
        try {
            fiu_do_on("raft/azure/vmss/update_capacity",
                      throw std::runtime_error("fault: raft/azure/vmss/update_capacity"););

            auto sit = _cfg.scale_set_by_group.find(target_group);
            if (sit == _cfg.scale_set_by_group.end()) {
                throw std::invalid_argument("azure_vmss_quorum_manager: no scale set for group: " +
                                            target_group);
            }
            const std::string& scale_set = sit->second;

            auto vmss_body = arm_get("/providers/Microsoft.Compute/virtualMachineScaleSets/" +
                                     scale_set + "?api-version=" + compute_api_version);
            std::int64_t orig_capacity = vmss_body.at("sku").at("capacity").as_int64();

            // Every member before the increment, in any state, so neither the
            // adoption below nor the rollback can mistake one for the new
            // instance: an untagged member that predates this call (another
            // provision's leftover, or one still being created) is not ours.
            std::vector<std::string> pre_growth;
            for (const auto& [vm_name, vm] : scale_set_vms(scale_set)) {
                (void)vm;
                pre_growth.push_back(vm_name);
            }

            boost::json::object patch_body;
            patch_body["sku"] = boost::json::object{{"capacity", orig_capacity + 1}};
            (void)arm_patch("/providers/Microsoft.Compute/virtualMachineScaleSets/" + scale_set +
                                "?api-version=" + compute_api_version,
                            patch_body);

            std::string found_instance_id;
            boost::json::object found_vm;
            auto deadline = std::chrono::steady_clock::now() + _cfg.provision_timeout;
            while (found_instance_id.empty() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(_cfg.poll_interval);
                try {
                    for (const auto& [vm_name, vm] : scale_set_vms(scale_set)) {
                        // Untagged, so not yet claimed by any node. Under
                        // Flexible this is a real VM resource, so its absence
                        // of a tag is durable state rather than an artifact of
                        // a list that never returns tags.
                        if (node_id_tag(vm) ||
                            std::ranges::find(pre_growth, vm_name) != pre_growth.end()) {
                            continue;
                        }
                        if (vm_is_ready(vm_name)) {
                            found_instance_id = vm_name;
                            found_vm = vm;
                            break;
                        }
                    }
                } catch (const std::exception&) {
                    // Keep polling; a transient list error shouldn't abort provisioning.
                }
            }

            if (found_instance_id.empty()) {
                std::vector<group_rollback::listed_member> final_listing;
                try {
                    final_listing = rollback_listing(scale_set_vms(scale_set));
                } catch (const std::exception&) {
                    // An empty listing plans a capacity restore: with nothing
                    // known to be fresh there is nothing to delete by name.
                }
                const auto rollback =
                    undo_scale_up(scale_set, pre_growth, std::move(final_listing), orig_capacity);
                throw std::runtime_error("provision timeout for scale set: " + scale_set + "; " +
                                         group_rollback::describe(rollback, orig_capacity + 1));
            }

            NodeId new_id = next_node_id();
            // Spent from here on, even if tagging fails below.
            _id_floor.raise(new_id);
            tag_instance(found_instance_id, found_vm, new_id, target_group);
            // Not fatal: the node is up and tagged, and the constructor's
            // reconcile protects it on the next start. Failing the provision
            // here would orphan a running, tagged instance.
            if (const auto error = set_scale_in_protection(scale_set, found_instance_id);
                !error.empty()) {
                std::cerr << "[azure_vmss_quorum_manager::provision_node] scale-in protection for "
                          << found_instance_id << " failed: " << error << "\n";
            }

            std::string private_ip;
            try {
                const auto& nic_refs = found_vm.at("properties")
                                           .at("networkProfile")
                                           .at("networkInterfaces")
                                           .as_array();
                if (!nic_refs.empty()) {
                    // `id` is an ARM *resource id* -- a path beginning
                    // "/subscriptions/...", not a URL. Passing it to
                    // arm_get_absolute unprefixed produced a request with no
                    // scheme or host, which the Azure SDK refuses to
                    // authenticate at all: "authentication is not permitted for
                    // non TLS protected (https) endpoints", an error naming TLS
                    // that reads like a transport misconfiguration rather than
                    // a malformed URL. Measured in run 36732744311, where it
                    // failed every provision.
                    std::string nic_id(nic_refs[0].at("id").as_string());
                    auto nic_body = arm_get_absolute(_arm_endpoint + nic_id +
                                                     "?api-version=" + network_api_version);
                    const auto& ip_configs =
                        nic_body.at("properties").at("ipConfigurations").as_array();
                    if (!ip_configs.empty()) {
                        private_ip = std::string(
                            ip_configs[0].at("properties").at("privateIPAddress").as_string());
                    }
                }
            } catch (const std::exception& ex) {
                throw std::runtime_error("failed to read instance NIC private IP: " +
                                         std::string(ex.what()));
            }
            if (private_ip.empty()) {
                throw std::runtime_error("instance " + found_instance_id + " has no private IP");
            }

            Address addr = static_cast<Address>(private_ip + ":" + std::to_string(_cfg.node_port));
            return future_factory_default::makeFuture(peer_info<NodeId, Address>{new_id, addr});
        } catch (const std::exception& ex) {
            return future_factory_default::makeExceptionalFuture<peer_info<NodeId, Address>>(
                std::make_exception_ptr(std::runtime_error(
                    std::string("azure_vmss_quorum_manager::provision_node: ") + ex.what())));
        }
    }

    /// Deletes the scale-set member identified by `node_id`, idempotently.
    ///
    /// The scale set's own `POST .../delete` action, taking the member's VM
    /// name in `instanceIds` -- under Flexible there is no separate numeric
    /// instance id, the name is the id. Measured: this decrements
    /// `sku.capacity` on its own, so no corrective PATCH is needed and the
    /// scale set does not relaunch a replacement. Under Uniform it did not, and
    /// the empty slot was filled by the next provision's increment -- worth
    /// knowing when reading the AWS spec's comparison with
    /// `TerminateInstanceInAutoScalingGroup(ShouldDecrementDesiredCapacity=true)`.
    auto decommission_node(const NodeId& node_id) -> kythira::future_default<void> {
        try {
            fiu_do_on("raft/azure/vmss/delete_instance",
                      throw std::runtime_error("fault: raft/azure/vmss/delete_instance"););

            auto found = find_instance(node_id);
            if (!found) {
                return future_factory_default::makeFuture();
            }
            const auto& [scale_set, instance_id] = *found;
            // A protected member is deleted as it is: ARM documents that
            // user-initiated instance deletes are not blocked by protection.
            if (!delete_member(scale_set, instance_id)) {
                return future_factory_default::makeFuture();
            }

            // Throws on expiry rather than returning. The previous version
            // broke out of this loop and reported success, so a delete that had
            // not landed looked identical to one that had -- the caller then
            // asserted the member was gone and failed with nothing pointing at
            // the wait. A decommission that cannot confirm the member is gone
            // has not done its job, and says so.
            auto deadline = std::chrono::steady_clock::now() + _cfg.decommission_timeout;
            for (;;) {
                if (!find_instance(node_id)) {
                    return future_factory_default::makeFuture();
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    throw std::runtime_error("member for node " + node_id_str(node_id) +
                                             " was still listed " +
                                             std::to_string(_cfg.decommission_timeout.count()) +
                                             "s after its delete was accepted");
                }
                std::this_thread::sleep_for(_cfg.poll_interval);
            }
        } catch (const std::exception& ex) {
            return future_factory_default::makeExceptionalFuture<void>(
                std::make_exception_ptr(std::runtime_error(
                    std::string("azure_vmss_quorum_manager::decommission_node: ") + ex.what())));
        }
    }

    /// Returns the desired topology from the configuration.
    [[nodiscard]] auto topology() const -> desired_topology<std::string> { return _cfg.topology; }

private:
    static constexpr const char* compute_api_version = "2024-07-01";
    static constexpr const char* network_api_version = "2024-05-01";

    struct arm_not_found : std::runtime_error {
        arm_not_found() : std::runtime_error("ARM resource not found (404)") {}
    };

    azure_vmss_quorum_manager_config _cfg;
    std::shared_ptr<Azure::Core::Http::_internal::HttpPipeline> _pipeline;
    std::string _arm_endpoint;
    std::string _arm_base;

    /// Every VM belonging to `scale_set`, as (vm_name, vm_object) pairs.
    ///
    /// This is the class's one source of instance identity, and it does NOT
    /// use the scale set's own `/virtualMachines` collection, because under
    /// **Flexible** orchestration that collection carries neither `tags` nor an
    /// instance view: it answers with bare `{id, instanceId, name, location}`
    /// entries, and asking it for `$expand=instanceView` is rejected outright
    /// with `BadRequest: Operation 'VirtualMachineScaleSets.virtualMachines.GET'
    /// is not allowed`.
    ///
    /// A Flexible scale set's members are ordinary
    /// `Microsoft.Compute/virtualMachines` resources that carry a
    /// `properties.virtualMachineScaleSet.id` back-reference, so the resource
    /// group's VM list is the read that returns tags, and membership is
    /// recovered from that back-reference rather than from the scale set.
    ///
    /// Flexible is a requirement of this manager rather than a preference. It
    /// identifies nodes by a `kythira:node-id` tag written onto the instance,
    /// and a **Uniform** scale set cannot store per-instance tags at all: its
    /// instances only ever reflect the scale set's own tags, and every write
    /// -- an ARM `PUT`, `az vmss update --instance-id --set tags`, and
    /// `az resource tag` alike -- is accepted, reports success, and changes
    /// nothing. The constructor rejects Uniform for exactly that reason; a
    /// silent no-op is far worse here than a refusal.
    [[nodiscard]] auto scale_set_vms(const std::string& scale_set) const
        -> std::vector<std::pair<std::string, boost::json::object>> {
        std::vector<std::pair<std::string, boost::json::object>> out;
        auto body =
            arm_get(std::string("/providers/Microsoft.Compute/virtualMachines?api-version=") +
                    compute_api_version);
        if (!body.is_object() || !body.as_object().contains("value")) {
            return out;
        }
        for (const auto& vm : body.at("value").as_array()) {
            if (!vm.is_object() || !vm.as_object().contains("name")) {
                continue;
            }
            std::string owner;
            try {
                owner = std::string(
                    vm.at("properties").at("virtualMachineScaleSet").at("id").as_string());
            } catch (const std::exception&) {
                continue;  // Not a scale-set member; an ordinary VM in the group.
            }
            if (!resource_id_names_scale_set(owner, scale_set)) {
                continue;
            }
            out.emplace_back(std::string(vm.at("name").as_string()), vm.as_object());
        }
        return out;
    }

    /// True when `resource_id`'s final segment is `scale_set`.
    ///
    /// Compared case-insensitively on the last segment only: ARM echoes a
    /// resource id with whatever casing the caller used for the resource group
    /// and provider, so a whole-string comparison against a locally built id
    /// mismatches on casing alone.
    [[nodiscard]] static auto resource_id_names_scale_set(const std::string& resource_id,
                                                          const std::string& scale_set) -> bool {
        auto pos = resource_id.find_last_of('/');
        if (pos == std::string::npos) {
            return false;
        }
        std::string leaf = resource_id.substr(pos + 1);
        if (leaf.size() != scale_set.size()) {
            return false;
        }
        return std::equal(leaf.begin(), leaf.end(), scale_set.begin(), [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
    }

    /// Whether `vm_name` is fully provisioned **and** running.
    ///
    /// Both conditions, and the `provisioningState` half is not
    /// belt-and-braces: a scale-set member reports `PowerState/running` while
    /// its `provisioningState` is still `Creating`, because the capacity
    /// `PATCH` that creates it is a long-running operation this manager does
    /// not wait on. Treating such a member as provisioned meant
    /// `decommission_node` deleted a VM whose create was still in flight, and
    /// ARM unwinds that instead of performing a normal delete: measured at
    /// **over nine minutes** in `Deleting`, against **24-37s** for a member
    /// whose create had settled. That is what failed the 600s decommission
    /// wait in `vmss_provision_increments_capacity` and
    /// `vmss_decommission_removes_instance` in run 36791903874, while
    /// `vmss_assess_detects_not_running` passed -- that case deallocates
    /// first, and the deallocate forces the create to settle.
    ///
    /// Five cheaper explanations were measured against the live scale set and
    /// refuted first: Flexible deletes being slow in general, only deallocated
    /// members deleting quickly, the delete racing a freshly-running member,
    /// the manager's own tagging blocking the delete, and the poll's read
    /// pressure throttling ARM. Every one of those deletes in 24-37s by hand.
    /// The difference was never the delete; it was what counted as ready.
    ///
    /// One call per VM, because the batch shapes do not work here: the scale
    /// set's expanded list is refused under Flexible, the resource group's VM
    /// list rejects `$expand=instanceView` without a scale-set filter, and that
    /// filter (`virtualMachineScaleSet/id eq '...'`) was refused as
    /// `BadRequest: The request URL is not valid.` in every encoding tried
    /// against api-version 2024-07-01. `azure_vm_quorum_manager` reads power
    /// state per VM for the same reason, and clusters here are small.
    [[nodiscard]] auto vm_is_ready(const std::string& vm_name) const -> bool {
        try {
            auto body = arm_get("/providers/Microsoft.Compute/virtualMachines/" + vm_name +
                                "?$expand=instanceView&api-version=" + compute_api_version);
            std::string provisioning_state;
            try {
                provisioning_state =
                    std::string(body.at("properties").at("provisioningState").as_string());
            } catch (const std::exception&) {
                return false;
            }
            if (provisioning_state != "Succeeded") {
                return false;
            }
            const auto& statuses =
                body.at("properties").at("instanceView").at("statuses").as_array();
            for (const auto& st : statuses) {
                if (st.is_object() && st.as_object().contains("code") &&
                    st.at("code").as_string() == "PowerState/running") {
                    return true;
                }
            }
        } catch (const std::exception&) {
            return false;
        }
        return false;
    }

    /// The `kythira:node-id` tag on `vm` when `vm` is this cluster's (its
    /// `kythira:cluster` tag matches). A scale set can hold another cluster's
    /// members, and a node id is only unique within one cluster, so assess,
    /// allocation and lookup all read the id through this: otherwise another
    /// cluster's node 3 could be assessed, allocated around, or deleted as
    /// ours.
    [[nodiscard]] auto our_node_id_tag(const boost::json::object& vm) const
        -> std::optional<std::string> {
        if (cluster_tag(vm) != _cfg.cluster_name) {
            return std::nullopt;
        }
        return node_id_tag(vm);
    }

    /// The `kythira:node-id` tag on `vm`, if it carries one.
    [[nodiscard]] static auto node_id_tag(const boost::json::object& vm)
        -> std::optional<std::string> {
        const auto* it = vm.find("tags");
        if (it == vm.end() || !it->value().is_object()) {
            return std::nullopt;
        }
        const auto& tags = it->value().as_object();
        const auto* tag = tags.find("kythira:node-id");
        if (tag == tags.end() || !tag->value().is_string()) {
            return std::nullopt;
        }
        return std::string(tag->value().as_string());
    }

    /// Every node id this manager has assessed or allocated; see
    /// `next_node_id`. Kept across copies and moves of this manager.
    numeric_node_id_floor _id_floor;

    /// Identical cluster-wide tag-scan bookkeeping as
    /// `azure_vm_quorum_manager::next_node_id` — deliberately copied, not
    /// shared, per design.md's non-sharing decision — including its floor:
    /// one above both the highest tag still listed and `_id_floor`. The scan
    /// alone would hand out a removed instance's id again (an eviction, a
    /// manual delete, or the highest-numbered node's decommission). The floor
    /// is per process; across leader changes the Raft node's refusal of a
    /// replacement id that is already a member is what keeps a reuse safe.
    [[nodiscard]] auto next_node_id() const -> NodeId {
        std::uint64_t max_id = 0;
        for (const auto& [group, scale_set] : _cfg.scale_set_by_group) {
            (void)group;
            for (const auto& [vm_name, vm] : scale_set_vms(scale_set)) {
                auto tag = our_node_id_tag(vm);
                if (!tag) {
                    continue;
                }
                // A tag this manager did not write (not plain decimal, or
                // past NodeId's range) is skipped: std::stoull read "-1" as
                // the largest uint64, after which every provision failed.
                if (const auto parsed = parse_numeric_node_id<NodeId>(*tag)) {
                    max_id = std::max(max_id, *parsed);
                } else {
                    std::cerr << "[azure_vmss_quorum_manager] ignoring member " << vm_name << " of "
                              << scale_set << ": unparseable kythira:node-id tag '" << *tag
                              << "'\n";
                }
            }
        }
        return numeric_node_id_as<NodeId>(
            _id_floor.next_above(max_id, numeric_node_id_ceiling<NodeId>()));
    }

    /// Scans every scale set in `scale_set_by_group` for an instance whose
    /// `kythira:node-id` tag matches `node_id`.
    [[nodiscard]] auto find_instance(const NodeId& node_id) const
        -> std::optional<std::pair<std::string, std::string>> {
        std::string target = node_id_str(node_id);
        for (const auto& [group, scale_set] : _cfg.scale_set_by_group) {
            (void)group;
            std::vector<std::pair<std::string, boost::json::object>> vms;
            try {
                vms = scale_set_vms(scale_set);
            } catch (const std::exception&) {
                continue;
            }
            for (const auto& [vm_name, vm] : vms) {
                if (auto tag = our_node_id_tag(vm); tag && *tag == target) {
                    // The VM's name, which is what the scale set's delete and
                    // deallocate actions take as an `instanceIds` entry under
                    // Flexible -- there is no separate numeric instance id.
                    return std::make_pair(scale_set, vm_name);
                }
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] auto do_send(const Azure::Core::Http::HttpMethod& method,
                               const std::string& url_str, const boost::json::value* body) const
        -> std::unique_ptr<Azure::Core::Http::RawResponse> {
        Azure::Core::Url url(url_str);
        std::string serialized;
        std::unique_ptr<Azure::Core::IO::MemoryBodyStream> stream;
        Azure::Core::Http::Request request = [&]() {
            if (body != nullptr) {
                serialized = boost::json::serialize(*body);
                stream = std::make_unique<Azure::Core::IO::MemoryBodyStream>(
                    reinterpret_cast<const std::uint8_t*>(serialized.data()), serialized.size());
                Azure::Core::Http::Request req(method, url, stream.get());
                req.SetHeader("Content-Type", "application/json");
                return req;
            }
            return Azure::Core::Http::Request(method, url);
        }();

        Azure::Core::Context context;
        if (_cfg.azure.api_timeout.count() > 0) {
            context = context.WithDeadline(
                Azure::DateTime(std::chrono::system_clock::now() + _cfg.azure.api_timeout));
        }
        return _pipeline->Send(request, context);
    }

    [[nodiscard]] auto parse_response(Azure::Core::Http::RawResponse& response) const
        -> boost::json::value {
        auto code = static_cast<int>(response.GetStatusCode());
        const auto& body = response.GetBody();
        std::string body_str(body.begin(), body.end());
        if (code == 404) {
            throw arm_not_found{};
        }
        if (code < 200 || code >= 300) {
            std::string message = body_str;
            try {
                auto parsed = boost::json::parse(body_str);
                if (parsed.is_object() && parsed.as_object().contains("error")) {
                    const auto& err = parsed.at("error");
                    message = std::string(err.at("code").as_string()) + ": " +
                              std::string(err.at("message").as_string());
                }
            } catch (const std::exception&) {
                // Fall back to the raw body text.
            }
            throw std::runtime_error("ARM request failed (" + std::to_string(code) +
                                     "): " + message);
        }
        if (body_str.empty()) {
            return boost::json::object{};
        }
        return boost::json::parse(body_str);
    }

    [[nodiscard]] auto arm_get(const std::string& path) const -> boost::json::value {
        auto response = do_send(Azure::Core::Http::HttpMethod::Get, _arm_base + path, nullptr);
        return parse_response(*response);
    }

    [[nodiscard]] auto arm_get_absolute(const std::string& full_url) const -> boost::json::value {
        auto response = do_send(Azure::Core::Http::HttpMethod::Get, full_url, nullptr);
        return parse_response(*response);
    }

    [[nodiscard]] auto arm_patch(const std::string& path, const boost::json::value& body) const
        -> boost::json::value {
        auto response = do_send(Azure::Core::Http::HttpMethod::Patch, _arm_base + path, &body);
        return parse_response(*response);
    }

    [[nodiscard]] auto arm_post(const std::string& path, const boost::json::value& body) const
        -> boost::json::value {
        auto response = do_send(Azure::Core::Http::HttpMethod::Post, _arm_base + path, &body);
        return parse_response(*response);
    }

    [[nodiscard]] auto arm_put(const std::string& path, const boost::json::value& body) const
        -> boost::json::value {
        auto response = do_send(Azure::Core::Http::HttpMethod::Put, _arm_base + path, &body);
        return parse_response(*response);
    }

    /// @brief The scale set's `delete` action for one member, which also lowers
    ///        `sku.capacity` under Flexible orchestration.
    ///
    /// @return False when ARM reports the member already gone, true when the
    ///         delete was accepted. Any other refusal throws.
    [[nodiscard]] auto delete_member(const std::string& scale_set, const std::string& vm_name) const
        -> bool {
        boost::json::object body;
        body["instanceIds"] = boost::json::array{vm_name};
        try {
            (void)arm_post("/providers/Microsoft.Compute/virtualMachineScaleSets/" + scale_set +
                               "/delete?api-version=" + compute_api_version,
                           body);
            return true;
        } catch (const arm_not_found&) {
            return false;
        } catch (const std::exception& ex) {
            // ARM's VMSS per-instance delete reports an already-gone instance ID
            // as a 400 with a "not found"-shaped message, not a 404 — unlike the
            // plain resource GET/DELETE calls elsewhere in this class. Treat that
            // shape as idempotent success too, matching the AWS design's identical
            // ValidationError/"not found" carve-out for TerminateInstanceInAutoScalingGroup.
            std::string lower_msg = ex.what();
            std::transform(lower_msg.begin(), lower_msg.end(), lower_msg.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (lower_msg.find("not found") != std::string::npos) {
                return false;
            }
            throw;
        }
    }

    /// @brief Protect one member from the scale set's own scale-in.
    ///
    /// The member's entry under the scale set, not its VM resource:
    /// `protectionPolicy` is a scale-set-member property, set with the
    /// scale set's VM `PUT` at api-version 2023-09-01 or later.
    ///
    /// @return The error text, or empty on success.
    [[nodiscard]] auto set_scale_in_protection(const std::string& scale_set,
                                               const std::string& vm_name) const -> std::string {
        try {
            boost::json::object body;
            body["properties"] = boost::json::object{
                {"protectionPolicy", boost::json::object{{"protectFromScaleIn", true}}}};
            (void)arm_put("/providers/Microsoft.Compute/virtualMachineScaleSets/" + scale_set +
                              "/virtualMachines/" + vm_name +
                              "?api-version=" + azure_vmss_detail::protection_api_version,
                          body);
            return {};
        } catch (const std::exception& ex) {
            return ex.what();
        }
    }

    /// True when the VM list reports @p vm already protected from scale-in.
    [[nodiscard]] static auto is_protected(const boost::json::object& vm) -> bool {
        try {
            return vm.at("properties").at("protectionPolicy").at("protectFromScaleIn").as_bool();
        } catch (const std::exception&) {
            return false;
        }
    }

    /// @brief Protect every adopted member of this cluster in @p scale_set
    ///        that is not yet protected (group-scale-up-rollback
    ///        Requirement 4.3).
    ///
    /// Covers clusters adopted before protection existed, and repairs an
    /// adoption whose protection call failed. A permission error throws
    /// `std::invalid_argument`, like the constructor's other checks; anything
    /// else is logged, because a transient failure at startup is not a reason
    /// to refuse to run.
    void reconcile_scale_in_protection(const std::string& scale_set) const {
        std::vector<std::pair<std::string, boost::json::object>> vms;
        try {
            vms = scale_set_vms(scale_set);
        } catch (const std::exception& ex) {
            std::cerr << "[azure_vmss_quorum_manager] scale-in protection reconcile for "
                      << scale_set << " skipped: " << ex.what() << "\n";
            return;
        }
        for (const auto& [vm_name, vm] : vms) {
            if (!node_id_tag(vm) || cluster_tag(vm) != _cfg.cluster_name || is_protected(vm)) {
                continue;
            }
            const auto error = set_scale_in_protection(scale_set, vm_name);
            if (error.empty()) {
                continue;
            }
            if (azure_vmss_detail::is_permission_error(error)) {
                throw std::invalid_argument(
                    "azure_vmss_quorum_manager: scale-in protection on scale set '" + scale_set +
                    "' was refused (" + error +
                    "); the identity needs Microsoft.Compute/virtualMachineScaleSets/"
                    "virtualMachines/write");
            }
            std::cerr << "[azure_vmss_quorum_manager] scale-in protection for " << vm_name
                      << " failed: " << error << "\n";
        }
    }

    /// The `kythira:cluster` tag on `vm`, or empty.
    [[nodiscard]] static auto cluster_tag(const boost::json::object& vm) -> std::string {
        try {
            return std::string(vm.at("tags").at("kythira:cluster").as_string());
        } catch (const std::exception&) {
            return {};
        }
    }

    [[nodiscard]] static auto rollback_listing(
        const std::vector<std::pair<std::string, boost::json::object>>& vms)
        -> std::vector<group_rollback::listed_member> {
        std::vector<group_rollback::listed_member> out;
        out.reserve(vms.size());
        for (const auto& [vm_name, vm] : vms) {
            std::string state;
            try {
                state = std::string(vm.at("properties").at("provisioningState").as_string());
            } catch (const std::exception&) {
                state.clear();
            }
            out.push_back({.id = vm_name,
                           .state = azure_vmss_detail::rollback_state(state),
                           .lifecycle = state,
                           .node = node_id_tag(vm).value_or("")});
        }
        return out;
    }

    /// Undo a timed-out capacity increment of @p scale_set; see `provision_node`.
    [[nodiscard]] auto undo_scale_up(const std::string& scale_set,
                                     const std::vector<std::string>& pre_growth,
                                     std::vector<group_rollback::listed_member> final_listing,
                                     std::int64_t orig_capacity) const noexcept
        -> group_rollback::rollback_outcome {
        try {
            return group_rollback::execute_rollback(
                pre_growth, std::move(final_listing), orig_capacity,
                [this, &scale_set](const std::string& name) -> std::string {
                    (void)delete_member(scale_set, name);
                    return {};
                },
                [this, &scale_set](std::int64_t capacity) {
                    boost::json::object body;
                    body["sku"] = boost::json::object{{"capacity", capacity}};
                    (void)arm_patch("/providers/Microsoft.Compute/virtualMachineScaleSets/" +
                                        scale_set + "?api-version=" + compute_api_version,
                                    body);
                },
                [this, &scale_set] { return rollback_listing(scale_set_vms(scale_set)); },
                group_rollback::settle_window(_cfg.provision_timeout, _cfg.poll_interval),
                _cfg.poll_interval);
        } catch (const std::exception& ex) {
            group_rollback::rollback_outcome outcome;
            outcome.restore_error = std::string("rollback aborted: ") + ex.what();
            return outcome;
        }
    }

    static auto node_id_str(const NodeId& id) -> std::string {
        return node_id_traits<NodeId>::to_text(id);
    }

    /// Applies this manager's tags to one scale-set member.
    ///
    /// A `PATCH` on the member's own `Microsoft.Compute/virtualMachines`
    /// resource, which under Flexible orchestration is what a scale-set member
    /// actually is. It applies synchronously: the tag is readable in the very
    /// next list call.
    ///
    /// None of the routes through the *scale set* work, and their failure modes
    /// are worth recording because two of them are silent:
    ///
    ///   * `PATCH .../virtualMachineScaleSets/{n}/virtualMachines/{id}` is
    ///     refused outright -- `405 The requested resource does not support
    ///     http method 'PATCH'` -- even for an instance id that does not exist,
    ///     so it is the method being rejected and not the resource missed.
    ///     `VirtualMachineScaleSetVMs_Update` is a `PUT`, unlike the
    ///     standalone `VirtualMachines_Update` this class was modelled on.
    ///   * That `PUT`, against a **Uniform** set, is accepted, reports
    ///     `provisioningState: Succeeded`, and does not apply the tag. So do
    ///     `az vmss update --instance-id --set tags` and `az resource tag`.
    ///     Uniform members cannot hold tags of their own at all; they only
    ///     reflect the scale set's. The constructor therefore refuses a Uniform
    ///     scale set rather than letting node identity quietly evaporate.
    ///   * `Microsoft.Resources/tags/default` is rejected at scale-set-member
    ///     scope with `HttpMethodIsNotSupported`.
    ///
    /// `PATCH` on a VM replaces the whole tags collection rather than merging
    /// into it, so `vm`'s existing tags are carried over explicitly -- Azure
    /// puts its own `VirtualMachineProfileTimeCreated` there, and an operator
    /// may have added more.
    void tag_instance(const std::string& vm_name, const boost::json::object& vm, const NodeId& nid,
                      const std::string& group) const {
        auto tags = build_tags(nid, group);
        if (const auto* it = vm.find("tags"); it != vm.end() && it->value().is_object()) {
            for (const auto& existing : it->value().as_object()) {
                if (!tags.contains(existing.key())) {
                    tags[existing.key()] = existing.value();
                }
            }
        }
        boost::json::object body;
        body["tags"] = std::move(tags);
        (void)arm_patch("/providers/Microsoft.Compute/virtualMachines/" + vm_name +
                            "?api-version=" + compute_api_version,
                        body);
    }

    [[nodiscard]] auto build_tags(const NodeId& nid, const std::string& group) const
        -> boost::json::object {
        boost::json::object tags;
        tags["kythira:cluster"] = _cfg.cluster_name;
        tags["kythira:node-id"] = node_id_str(nid);
        tags["kythira:group"] = group;
        tags["kythira:managed-by"] = "kythira-azure-vmss-quorum-manager";
        return tags;
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
        for (const auto& gt : _cfg.topology.groups) {
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
            .status = compute_quorum_status(live_count, total),
            .live_node_count = live_count,
            .total_node_count = total,
            .unreachable_nodes = std::move(unreachable),
            .groups = std::move(groups),
        });
    }

    static auto compute_quorum_status(std::size_t live, std::size_t total) -> quorum_status {
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

static_assert(quorum_manager<azure_vmss_quorum_manager<std::uint64_t, std::string>, std::uint64_t,
                             std::string, std::string>,
              "azure_vmss_quorum_manager must satisfy quorum_manager");

}  // namespace kythira

#endif  // KYTHIRA_HAS_AZURE_SDK

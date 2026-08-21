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
#include <raft/fault_injection.hpp>
#include <raft/future_default.hpp>
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
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace kythira {

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
/// applying the resulting tag via a `PATCH` on the specific VMSS instance
/// rather than encoding it into a resource name (VMSS instance names are
/// Azure-assigned and not renameable).
template<typename NodeId = std::uint64_t, typename Address = std::string>
requires kythira::node_id<NodeId>
class azure_vmss_quorum_manager {
public:
    using node_id_type = NodeId;
    using address_type = Address;
    using placement_group_id_type = std::string;

    /// Constructs the manager, validates the configuration, and verifies that
    /// every configured scale set is NOT in `upgradePolicy.mode == "Automatic"`
    /// (Requirement 10.3): an Automatic-mode scale set can autonomously replace
    /// instances outside this manager's control, violating Property 4 ("no
    /// autonomous replacement outside the quorum manager's control").
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

        _arm_base = (_cfg.azure.arm_endpoint_override.empty() ? "https://management.azure.com"
                                                              : _cfg.azure.arm_endpoint_override) +
                    "/subscriptions/" + _cfg.azure.subscription_id + "/resourceGroups/" +
                    _cfg.azure.resource_group;

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
            }
        }
    }

    /// Assesses cluster health via one `?$expand=instanceView` list call per
    /// scale set (a true batch read, unlike `azure_vm_quorum_manager`'s
    /// per-node calls). Instances lacking a `kythira:node-id` tag are skipped —
    /// they belong to no kythira cluster node yet.
    auto assess_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::future_default<quorum_health<NodeId, std::string>> {
        try {
            fiu_do_on("raft/azure/vmss/list_instances",
                      throw std::runtime_error("fault: raft/azure/vmss/list_instances"););

            if (cluster.empty()) {
                return build_health(cluster, {});
            }

            std::map<std::string, bool> live_map;
            for (const auto& [group, scale_set] : _cfg.scale_set_by_group) {
                (void)group;
                auto body = arm_get(
                    "/providers/Microsoft.Compute/virtualMachineScaleSets/" + scale_set +
                    "/virtualMachines?$expand=instanceView&api-version=" + compute_api_version);
                if (!body.is_object() || !body.as_object().contains("value")) {
                    continue;
                }
                for (const auto& inst : body.at("value").as_array()) {
                    if (!inst.is_object()) {
                        continue;
                    }
                    std::string nid_tag;
                    if (inst.as_object().contains("tags") && inst.at("tags").is_object() &&
                        inst.at("tags").as_object().contains("kythira:node-id")) {
                        nid_tag = std::string(inst.at("tags").at("kythira:node-id").as_string());
                    } else {
                        continue;
                    }
                    bool running = false;
                    try {
                        const auto& statuses =
                            inst.at("properties").at("instanceView").at("statuses").as_array();
                        for (const auto& st : statuses) {
                            if (st.is_object() && st.as_object().contains("code") &&
                                st.at("code").as_string() == "PowerState/running") {
                                running = true;
                                break;
                            }
                        }
                    } catch (const std::exception&) {
                        running = false;
                    }
                    live_map[nid_tag] = running;
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

    /// Increments `target_group`'s scale set capacity by one, waits for a new
    /// `PowerState/running` instance lacking a `kythira:node-id` tag to appear,
    /// then tags it with a freshly-assigned NodeId. On timeout, restores the
    /// original capacity (best-effort) before returning an exceptional Future.
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

            boost::json::object patch_body;
            patch_body["sku"] = boost::json::object{{"capacity", orig_capacity + 1}};
            (void)arm_patch("/providers/Microsoft.Compute/virtualMachineScaleSets/" + scale_set +
                                "?api-version=" + compute_api_version,
                            patch_body);

            std::string found_instance_id;
            auto deadline = std::chrono::steady_clock::now() + _cfg.provision_timeout;
            while (found_instance_id.empty() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(_cfg.poll_interval);
                try {
                    auto list_body = arm_get(
                        "/providers/Microsoft.Compute/virtualMachineScaleSets/" + scale_set +
                        "/virtualMachines?$expand=instanceView&api-version=" + compute_api_version);
                    if (!list_body.is_object() || !list_body.as_object().contains("value")) {
                        continue;
                    }
                    for (const auto& inst : list_body.at("value").as_array()) {
                        if (!inst.is_object()) {
                            continue;
                        }
                        bool has_tag = inst.as_object().contains("tags") &&
                                       inst.at("tags").is_object() &&
                                       inst.at("tags").as_object().contains("kythira:node-id");
                        if (has_tag) {
                            continue;
                        }
                        bool running = false;
                        try {
                            const auto& statuses =
                                inst.at("properties").at("instanceView").at("statuses").as_array();
                            for (const auto& st : statuses) {
                                if (st.is_object() && st.as_object().contains("code") &&
                                    st.at("code").as_string() == "PowerState/running") {
                                    running = true;
                                    break;
                                }
                            }
                        } catch (const std::exception&) {
                            running = false;
                        }
                        if (running && inst.as_object().contains("instanceId")) {
                            found_instance_id = std::string(inst.at("instanceId").as_string());
                            break;
                        }
                    }
                } catch (const std::exception&) {
                    // Keep polling; a transient list error shouldn't abort provisioning.
                }
            }

            if (found_instance_id.empty()) {
                boost::json::object rollback;
                rollback["sku"] = boost::json::object{{"capacity", orig_capacity}};
                try {
                    (void)arm_patch("/providers/Microsoft.Compute/virtualMachineScaleSets/" +
                                        scale_set + "?api-version=" + compute_api_version,
                                    rollback);
                } catch (const std::exception& ex) {
                    std::cerr
                        << "[azure_vmss_quorum_manager::provision_node] capacity rollback for "
                        << scale_set << " failed: " << ex.what() << "\n";
                }
                throw std::runtime_error("provision timeout for scale set: " + scale_set);
            }

            NodeId new_id = next_node_id();
            boost::json::object tag_patch;
            tag_patch["tags"] = build_tags(new_id, target_group);
            (void)arm_patch("/providers/Microsoft.Compute/virtualMachineScaleSets/" + scale_set +
                                "/virtualMachines/" + found_instance_id +
                                "?api-version=" + compute_api_version,
                            tag_patch);

            std::string private_ip;
            try {
                auto instance_body =
                    arm_get("/providers/Microsoft.Compute/virtualMachineScaleSets/" + scale_set +
                            "/virtualMachines/" + found_instance_id +
                            "?$expand=instanceView&api-version=" + compute_api_version);
                const auto& nic_refs = instance_body.at("properties")
                                           .at("networkProfile")
                                           .at("networkInterfaces")
                                           .as_array();
                if (!nic_refs.empty()) {
                    std::string nic_id(nic_refs[0].at("id").as_string());
                    auto nic_body =
                        arm_get_absolute(nic_id + "?api-version=" + network_api_version);
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

    /// Deletes the VMSS instance identified by `node_id`, idempotently.
    /// Unlike AWS's `TerminateInstanceInAutoScalingGroup(ShouldDecrementDesiredCapacity=true)`,
    /// VMSS's per-instance delete has no capacity-decrement flag: deleting an
    /// instance leaves `sku.capacity` counting a now-empty slot, which the next
    /// `provision_node` call's capacity increment fills with a genuinely new
    /// instance. No corrective PATCH is needed here.
    auto decommission_node(const NodeId& node_id) -> kythira::future_default<void> {
        try {
            fiu_do_on("raft/azure/vmss/delete_instance",
                      throw std::runtime_error("fault: raft/azure/vmss/delete_instance"););

            auto found = find_instance(node_id);
            if (!found) {
                return future_factory_default::makeFuture();
            }
            const auto& [scale_set, instance_id] = *found;

            boost::json::object body;
            body["instanceIds"] = boost::json::array{instance_id};
            try {
                (void)arm_post("/providers/Microsoft.Compute/virtualMachineScaleSets/" + scale_set +
                                   "/delete?api-version=" + compute_api_version,
                               body);
            } catch (const arm_not_found&) {
                return future_factory_default::makeFuture();
            } catch (const std::exception& ex) {
                // ARM's VMSS per-instance delete reports an already-gone instance ID
                // as a 400 with a "not found"-shaped message, not a 404 — unlike the
                // plain resource GET/DELETE calls elsewhere in this class. Treat that
                // shape as idempotent success too, matching the AWS design's identical
                // ValidationError/"not found" carve-out for TerminateInstanceInAutoScalingGroup.
                std::string msg = ex.what();
                std::string lower_msg = msg;
                std::transform(lower_msg.begin(), lower_msg.end(), lower_msg.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                if (lower_msg.find("not found") != std::string::npos) {
                    return future_factory_default::makeFuture();
                }
                throw;
            }

            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
            while (std::chrono::steady_clock::now() < deadline) {
                if (!find_instance(node_id)) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::seconds{2});
            }

            return future_factory_default::makeFuture();
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
    std::string _arm_base;

    /// Identical cluster-wide tag-scan bookkeeping as
    /// `azure_vm_quorum_manager::next_node_id` — deliberately copied, not
    /// shared, per design.md's non-sharing decision.
    [[nodiscard]] auto next_node_id() const -> NodeId {
        std::uint64_t max_id = 0;
        for (const auto& [group, scale_set] : _cfg.scale_set_by_group) {
            (void)group;
            auto body = arm_get("/providers/Microsoft.Compute/virtualMachineScaleSets/" +
                                scale_set + "/virtualMachines?api-version=" + compute_api_version);
            if (!body.is_object() || !body.as_object().contains("value")) {
                continue;
            }
            for (const auto& inst : body.at("value").as_array()) {
                if (!inst.is_object() || !inst.as_object().contains("tags")) {
                    continue;
                }
                const auto& tags = inst.at("tags");
                if (!tags.is_object() || !tags.as_object().contains("kythira:node-id")) {
                    continue;
                }
                try {
                    auto v = static_cast<std::uint64_t>(
                        std::stoull(std::string(tags.at("kythira:node-id").as_string())));
                    max_id = std::max(max_id, v);
                } catch (const std::exception&) {
                    // Skip unparseable tag values.
                }
            }
        }
        std::uint64_t next = max_id + 1;
        if constexpr (std::is_same_v<NodeId, std::string>) {
            return std::to_string(next);
        } else {
            return static_cast<NodeId>(next);
        }
    }

    /// Scans every scale set in `scale_set_by_group` for an instance whose
    /// `kythira:node-id` tag matches `node_id`.
    [[nodiscard]] auto find_instance(const NodeId& node_id) const
        -> std::optional<std::pair<std::string, std::string>> {
        std::string target = node_id_str(node_id);
        for (const auto& [group, scale_set] : _cfg.scale_set_by_group) {
            (void)group;
            boost::json::value body;
            try {
                body = arm_get("/providers/Microsoft.Compute/virtualMachineScaleSets/" + scale_set +
                               "/virtualMachines?api-version=" + compute_api_version);
            } catch (const std::exception&) {
                continue;
            }
            if (!body.is_object() || !body.as_object().contains("value")) {
                continue;
            }
            for (const auto& inst : body.at("value").as_array()) {
                if (!inst.is_object() || !inst.as_object().contains("tags")) {
                    continue;
                }
                const auto& tags = inst.at("tags");
                if (tags.is_object() && tags.as_object().contains("kythira:node-id") &&
                    std::string(tags.at("kythira:node-id").as_string()) == target &&
                    inst.as_object().contains("instanceId")) {
                    return std::make_pair(scale_set,
                                          std::string(inst.at("instanceId").as_string()));
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

    static auto node_id_str(const NodeId& id) -> std::string {
        if constexpr (std::is_same_v<NodeId, std::string>) {
            return id;
        } else {
            return std::to_string(id);
        }
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

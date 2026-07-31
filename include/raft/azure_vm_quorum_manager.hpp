#pragma once

/// @file azure_vm_quorum_manager.hpp
/// @brief `quorum_manager` implementation that provisions and monitors Raft nodes
///        as direct Azure Resource Manager (ARM) `Microsoft.Compute/virtualMachines`
///        resources.
///
/// Unlike the AWS quorum managers, there is no generated ARM management-plane
/// client for Compute/Network in the Azure SDK for C++. This class builds and
/// sends ARM REST requests directly over
/// `Azure::Core::Http::_internal::HttpPipeline` — the same internal pipeline
/// class every real Azure SDK for C++ service client (including
/// `Azure::Security::KeyVault::Keys::KeyClient`, used by
/// `azure_key_vault_ca_provider`) is itself built on top of.

#include <raft/azure_client_config.hpp>
#include <raft/fault_injection.hpp>
#include <raft/future_default.hpp>
#include <raft/quorum_management.hpp>

#ifdef KYTHIRA_HAS_AZURE_SDK

#include <azure/core/base64.hpp>
#include <azure/core/context.hpp>
#include <azure/core/http/http.hpp>
#include <azure/core/http/policies/policy.hpp>
#include <azure/core/internal/client_options.hpp>
#include <azure/core/internal/http/pipeline.hpp>
#include <azure/core/io/body_stream.hpp>
#include <azure/core/url.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace kythira {

// ============================================================================
// Shared placement/priority/image types (Requirements 16, 18, 3.2)
// ============================================================================

/// How a node in a given placement group is spread across the underlying Azure
/// fault domains.
enum class azure_placement_kind : std::uint8_t {
    availability_zone,          ///< A numbered Availability Zone ("1"/"2"/"3").
    proximity_placement_group,  ///< An existing Proximity Placement Group (low latency).
    availability_set,           ///< An existing Availability Set (rack-level fault isolation).
};

/// Placement settings applied to every node provisioned into one topology group.
struct azure_placement_config {
    azure_placement_kind kind{azure_placement_kind::availability_zone};
    std::string zone;         ///< "1"/"2"/"3"; used when kind == availability_zone.
    std::string resource_id;  ///< PPG or AvSet ARM resource ID; used for the other two kinds.
};

/// Purchase model applied to every node provisioned by azure_vm_quorum_manager.
enum class azure_vm_priority : std::uint8_t {
    regular,
    spot
};

/// What Azure does when a Spot VM is evicted.
enum class azure_eviction_policy : std::uint8_t {
    deallocate,
    delete_vm
};

/// Spot-VM options applied when `azure_vm_quorum_manager_config::priority == spot`.
struct azure_spot_options {
    /// Maximum hourly price in USD. -1.0 (the default) means "uncapped, i.e. up
    /// to the on-demand price" — Azure's own sentinel value for this field.
    double max_price{-1.0};
    azure_eviction_policy eviction_policy{azure_eviction_policy::deallocate};
};

/// Selects the OS image used for every provisioned VM. Exactly one of the two
/// forms must be populated: the four-part Marketplace reference, or a Shared
/// Image Gallery image version resource ID.
struct azure_image_reference {
    std::string publisher;
    std::string offer;
    std::string sku;
    std::string version;
    std::string shared_gallery_image_id;

    [[nodiscard]] auto is_marketplace_form() const -> bool {
        return !publisher.empty() && !offer.empty() && !sku.empty() && !version.empty();
    }
    [[nodiscard]] auto is_gallery_form() const -> bool { return !shared_gallery_image_id.empty(); }
};

// ============================================================================
// azure_vm_quorum_manager_config
// ============================================================================

/// Configuration for `azure_vm_quorum_manager`.
struct azure_vm_quorum_manager_config {
    /// Azure subscription/resource-group/location/credential settings.
    azure_client_config azure{};
    /// Logical cluster name; used as the `kythira:cluster` tag and VM-name prefix.
    std::string cluster_name;
    /// OS image used for every provisioned VM.
    azure_image_reference image_reference;
    /// Azure VM size (e.g. "Standard_D2s_v5").
    std::string vm_size{"Standard_D2s_v5"};
    /// Linux admin username.
    std::string admin_username{"kythira"};
    /// SSH public key material. Empty = password auth is left to the image default
    /// (not recommended; most images require an explicit auth mechanism).
    std::string ssh_public_key;
    /// Maps each topology group_id to the ARM subnet resource ID used for that group.
    std::map<std::string, std::string> subnet_id_by_group;
    /// Network Security Group ARM resource ID applied to every provisioned NIC.
    /// Empty = no NSG association at the NIC level.
    std::string network_security_group_id;
    /// TCP port on which each node listens.
    std::uint16_t node_port{7000};
    /// Custom-data (cloud-init) template. Supports {NODE_ID}, {NODE_PORT},
    /// {CLUSTER}, {GROUP} substitutions.
    std::string custom_data_template;
    /// Target node counts per placement group.
    desired_topology<std::string> topology{};
    /// Per-group placement settings. Groups absent from this map get no explicit
    /// placement (Azure chooses freely within the region).
    std::map<std::string, azure_placement_config> placement_by_group{};
    /// Purchase model for every provisioned VM.
    azure_vm_priority priority{azure_vm_priority::regular};
    /// Spot-VM options; only consulted when priority == spot.
    std::optional<azure_spot_options> spot_options{};
    /// Maximum time to wait for a newly created VM to reach PowerState/running.
    std::chrono::seconds provision_timeout{300};
    /// Sleep interval between instanceView polls during provisioning.
    std::chrono::milliseconds poll_interval{5000};
    /// Additional ARM tags applied alongside the standard kythira:* tags.
    std::map<std::string, std::string> extra_tags{};
};

// ============================================================================
// azure_vm_quorum_manager
// ============================================================================

/// `quorum_manager` implementation that provisions and monitors Raft nodes as
/// direct `Microsoft.Compute/virtualMachines` ARM resources.
///
/// Unlike `aws_ec2_quorum_manager` (which derives NodeId *from* the AWS-assigned
/// EC2 instance ID), ARM's `PUT .../virtualMachines/{vmName}` requires the
/// *caller* to choose `vmName` up front. NodeId assignment therefore runs in the
/// opposite direction: `next_node_id()` scans existing `kythira:node-id` tags for
/// the current maximum (the same tag-scan bookkeeping `aws_asg_quorum_manager`
/// uses), and `node_id_to_vm_name`/`vm_name_to_node_id` are pure string
/// computations in both directions once that ID is known.
///
/// Liveness is determined solely from ARM `instanceView` power state — there is
/// no application-level heartbeat tag in this design (see design.md's "ARM Tag
/// Schema" section for the scope rationale).
template<typename NodeId = std::uint64_t, typename Address = std::string>
requires kythira::node_id<NodeId>
class azure_vm_quorum_manager {
public:
    using node_id_type = NodeId;
    using address_type = Address;
    using placement_group_id_type = std::string;

    /// Constructs the manager and validates the configuration.
    /// Throws std::invalid_argument on any of: empty cluster_name/subscription_id/
    /// resource_group/location, zero node_port, a malformed image_reference (not
    /// exactly one of the two forms populated), or a topology group missing from
    /// subnet_id_by_group.
    explicit azure_vm_quorum_manager(azure_vm_quorum_manager_config cfg) : _cfg(std::move(cfg)) {
        if (_cfg.cluster_name.empty()) {
            throw std::invalid_argument("azure_vm_quorum_manager: cluster_name must be non-empty");
        }
        if (_cfg.azure.subscription_id.empty()) {
            throw std::invalid_argument(
                "azure_vm_quorum_manager: azure.subscription_id must be non-empty");
        }
        if (_cfg.azure.resource_group.empty()) {
            throw std::invalid_argument(
                "azure_vm_quorum_manager: azure.resource_group must be non-empty");
        }
        if (_cfg.azure.location.empty()) {
            throw std::invalid_argument(
                "azure_vm_quorum_manager: azure.location must be non-empty");
        }
        if (_cfg.node_port == 0) {
            throw std::invalid_argument("azure_vm_quorum_manager: node_port must be non-zero");
        }
        bool marketplace = _cfg.image_reference.is_marketplace_form();
        bool gallery = _cfg.image_reference.is_gallery_form();
        if (marketplace == gallery) {
            throw std::invalid_argument(
                "azure_vm_quorum_manager: image_reference must set exactly one of the "
                "Marketplace 4-tuple or shared_gallery_image_id");
        }
        for (const auto& gt : _cfg.topology.groups) {
            if (_cfg.subnet_id_by_group.find(gt.group_id) == _cfg.subnet_id_by_group.end()) {
                throw std::invalid_argument(
                    "azure_vm_quorum_manager: no subnet configured for group: " + gt.group_id);
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
            client_options, "kythira-azure-vm-quorum-manager", "1.0.0",
            std::move(per_retry_policies),
            std::vector<std::unique_ptr<Azure::Core::Http::Policies::HttpPolicy>>{});
    }

    /// Assesses cluster health via one `instanceView` GET per node (bounded
    /// concurrency). A node is live iff its instanceView reports
    /// `PowerState/running`. A 404 is treated as unreachable; any other HTTP
    /// error aborts the whole call with an exceptional Future.
    auto assess_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::future_default<quorum_health<NodeId, std::string>> {
        try {
            fiu_do_on("raft/azure/vm/get_instance_view",
                      throw std::runtime_error("fault: raft/azure/vm/get_instance_view"););

            if (cluster.empty()) {
                return build_health(cluster, {});
            }

            std::map<std::string, bool> live_map;
            // Bounded concurrency: ARM has no batch instanceView read across
            // arbitrary VM names, unlike EC2's DescribeInstanceStatus, so this
            // issues one GET per node, capped to avoid overwhelming ARM's
            // per-subscription throttling budget.
            constexpr std::size_t max_in_flight = 16;
            for (std::size_t start = 0; start < cluster.size(); start += max_in_flight) {
                std::size_t end = std::min(start + max_in_flight, cluster.size());
                // Bridged onto kythira::future_default/promise_default (not
                // std::async/std::future) via a plain std::thread per node,
                // joined below -- this project's future/promise machinery is
                // backend-agnostic (Folly/stdexec/boost), and
                // complete_conversion_validation_property_test enforces no
                // remaining std::future usage anywhere in the codebase
                // (Requirement 9.1). Behavior is otherwise identical to the
                // std::async version: bounded concurrency, wait for the whole
                // batch before moving to the next one.
                std::vector<kythira::future_default<std::pair<std::string, std::optional<bool>>>>
                    futures;
                std::vector<std::thread> threads;
                for (std::size_t i = start; i < end; ++i) {
                    const auto& np = cluster[i];
                    kythira::promise_default<std::pair<std::string, std::optional<bool>>> promise;
                    futures.push_back(promise.getFuture());
                    threads.emplace_back([this, np, promise = std::move(promise)]() mutable {
                        std::string vm_name = node_id_to_vm_name(np.node_id);
                        try {
                            auto body =
                                arm_get("/providers/Microsoft.Compute/virtualMachines/" + vm_name +
                                        "/instanceView?api-version=" + compute_api_version);
                            bool running = false;
                            if (body.is_object() && body.as_object().contains("statuses")) {
                                for (const auto& st : body.at("statuses").as_array()) {
                                    if (st.is_object() && st.as_object().contains("code") &&
                                        st.at("code").as_string() == "PowerState/running") {
                                        running = true;
                                        break;
                                    }
                                }
                            }
                            promise.setValue(std::make_pair(node_id_str(np.node_id),
                                                            std::optional<bool>(running)));
                        } catch (const arm_not_found&) {
                            promise.setValue(std::make_pair(node_id_str(np.node_id),
                                                            std::optional<bool>(false)));
                        } catch (...) {
                            promise.setException(std::current_exception());
                        }
                    });
                }
                for (auto& t : threads) {
                    t.join();
                }
                for (auto& f : futures) {
                    auto [key, live] = std::move(f).get();
                    if (!live) {
                        throw std::runtime_error("azure instanceView: request failed for a node");
                    }
                    live_map[key] = *live;
                }
            }

            return build_health(cluster, live_map);
        } catch (const std::exception& ex) {
            return future_factory_default::makeExceptionalFuture<
                quorum_health<NodeId, std::string>>(std::make_exception_ptr(std::runtime_error(
                std::string("azure_vm_quorum_manager::assess_quorum: ") + ex.what())));
        }
    }

    /// Assesses quorum, decommissions unreachable nodes, and provisions
    /// replacements to meet the desired topology. Decommission/provision errors
    /// are logged to stderr but do not abort the operation; the returned health
    /// reflects the pre-maintenance cluster state.
    auto maintain_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::future_default<quorum_health<NodeId, std::string>> {
        try {
            fiu_do_on("raft/azure/vm/maintain_quorum",
                      throw std::runtime_error("fault: raft/azure/vm/maintain_quorum"););
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
                std::cerr << "[azure_vm_quorum_manager::maintain_quorum] decommission of "
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
                    std::cerr << "[azure_vm_quorum_manager::maintain_quorum] provision in "
                              << gt.group_id << " failed: " << ex.what() << "\n";
                }
            }
        }

        return future_factory_default::makeFuture(std::move(pre_health));
    }

    /// Provisions a new VM into `target_group`'s subnet. Creates the NIC first,
    /// then the VM; on any failure after the NIC is created, best-effort deletes
    /// what was already created before returning an exceptional Future.
    auto provision_node(std::string target_group, std::optional<NodeId> /*replacing*/)
        -> kythira::future_default<peer_info<NodeId, Address>> {
        try {
            fiu_do_on("raft/azure/vm/create_vm",
                      throw std::runtime_error("fault: raft/azure/vm/create_vm"););

            auto sit = _cfg.subnet_id_by_group.find(target_group);
            if (sit == _cfg.subnet_id_by_group.end()) {
                throw std::invalid_argument("azure_vm_quorum_manager: no subnet for group: " +
                                            target_group);
            }
            const std::string& subnet_id = sit->second;

            NodeId new_id = next_node_id();
            std::string vm_name = node_id_to_vm_name(new_id);
            std::string nic_name = vm_name + "-nic";

            boost::json::object nic_ip_config;
            nic_ip_config["name"] = "ipconfig1";
            boost::json::object nic_ip_props;
            nic_ip_props["subnet"] = boost::json::object{{"id", subnet_id}};
            nic_ip_config["properties"] = std::move(nic_ip_props);

            boost::json::object nic_props;
            nic_props["ipConfigurations"] = boost::json::array{std::move(nic_ip_config)};
            if (!_cfg.network_security_group_id.empty()) {
                nic_props["networkSecurityGroup"] =
                    boost::json::object{{"id", _cfg.network_security_group_id}};
            }
            boost::json::object nic_body;
            nic_body["location"] = _cfg.azure.location;
            nic_body["properties"] = std::move(nic_props);

            std::string nic_path = "/providers/Microsoft.Network/networkInterfaces/" + nic_name +
                                   "?api-version=" + network_api_version;
            std::string nic_id;
            try {
                auto nic_result = arm_put(nic_path, nic_body);
                nic_id = _arm_base + "/providers/Microsoft.Network/networkInterfaces/" + nic_name;
                (void)nic_result;
            } catch (const std::exception& ex) {
                throw std::runtime_error("NIC creation failed: " + std::string(ex.what()));
            }

            try {
                std::string rendered = render_custom_data(new_id, target_group);
                std::vector<std::uint8_t> rendered_bytes(rendered.begin(), rendered.end());
                std::string custom_data_b64 = Azure::Core::Convert::Base64Encode(rendered_bytes);

                boost::json::object hw_profile;
                hw_profile["vmSize"] = _cfg.vm_size;

                boost::json::object storage_profile;
                boost::json::object image_ref;
                if (_cfg.image_reference.is_marketplace_form()) {
                    image_ref["publisher"] = _cfg.image_reference.publisher;
                    image_ref["offer"] = _cfg.image_reference.offer;
                    image_ref["sku"] = _cfg.image_reference.sku;
                    image_ref["version"] = _cfg.image_reference.version;
                } else {
                    image_ref["id"] = _cfg.image_reference.shared_gallery_image_id;
                }
                storage_profile["imageReference"] = std::move(image_ref);

                boost::json::object os_profile;
                os_profile["computerName"] = vm_name;
                os_profile["adminUsername"] = _cfg.admin_username;
                os_profile["customData"] = custom_data_b64;
                if (!_cfg.ssh_public_key.empty()) {
                    boost::json::object linux_cfg;
                    linux_cfg["disablePasswordAuthentication"] = true;
                    boost::json::object ssh_key;
                    ssh_key["path"] = "/home/" + _cfg.admin_username + "/.ssh/authorized_keys";
                    ssh_key["keyData"] = _cfg.ssh_public_key;
                    boost::json::object ssh_cfg;
                    ssh_cfg["publicKeys"] = boost::json::array{std::move(ssh_key)};
                    linux_cfg["ssh"] = std::move(ssh_cfg);
                    os_profile["linuxConfiguration"] = std::move(linux_cfg);
                }

                boost::json::object nic_ref;
                nic_ref["id"] = nic_id;
                boost::json::object network_profile;
                network_profile["networkInterfaces"] = boost::json::array{std::move(nic_ref)};

                boost::json::object vm_body_props;
                vm_body_props["hardwareProfile"] = std::move(hw_profile);
                vm_body_props["storageProfile"] = std::move(storage_profile);
                vm_body_props["osProfile"] = std::move(os_profile);
                vm_body_props["networkProfile"] = std::move(network_profile);

                boost::json::object vm_body;
                vm_body["location"] = _cfg.azure.location;
                vm_body["tags"] = build_tags(new_id, target_group);
                apply_placement_fields(vm_body, vm_body_props, target_group);
                apply_priority_fields(vm_body_props);
                vm_body["properties"] = std::move(vm_body_props);

                std::string vm_path = "/providers/Microsoft.Compute/virtualMachines/" + vm_name +
                                      "?api-version=" + compute_api_version;
                (void)arm_put(vm_path, vm_body);
            } catch (const std::exception& ex) {
                best_effort_delete_nic(nic_name);
                throw std::runtime_error("VM creation failed: " + std::string(ex.what()));
            }

            std::string private_ip;
            bool running = false;
            auto deadline = std::chrono::steady_clock::now() + _cfg.provision_timeout;
            while (!running && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(_cfg.poll_interval);
                try {
                    auto body = arm_get("/providers/Microsoft.Compute/virtualMachines/" + vm_name +
                                        "/instanceView?api-version=" + compute_api_version);
                    if (body.is_object() && body.as_object().contains("statuses")) {
                        for (const auto& st : body.at("statuses").as_array()) {
                            if (st.is_object() && st.as_object().contains("code") &&
                                st.at("code").as_string() == "PowerState/running") {
                                running = true;
                                break;
                            }
                        }
                    }
                } catch (const std::exception&) {
                    // Keep polling; a transient error here shouldn't abort provisioning.
                }
            }
            if (!running) {
                best_effort_delete_vm(vm_name);
                best_effort_delete_nic(nic_name);
                throw std::runtime_error("provision timeout waiting for " + vm_name + " to run");
            }
            try {
                auto nic_body = arm_get("/providers/Microsoft.Network/networkInterfaces/" +
                                        nic_name + "?api-version=" + network_api_version);
                const auto& ip_configs =
                    nic_body.at("properties").at("ipConfigurations").as_array();
                if (!ip_configs.empty()) {
                    private_ip = std::string(
                        ip_configs[0].at("properties").at("privateIPAddress").as_string());
                }
            } catch (const std::exception& ex) {
                best_effort_delete_vm(vm_name);
                best_effort_delete_nic(nic_name);
                throw std::runtime_error("failed to read NIC private IP: " +
                                         std::string(ex.what()));
            }
            if (private_ip.empty()) {
                best_effort_delete_vm(vm_name);
                best_effort_delete_nic(nic_name);
                throw std::runtime_error("NIC has no private IP for " + vm_name);
            }

            Address addr = static_cast<Address>(private_ip + ":" + std::to_string(_cfg.node_port));
            return future_factory_default::makeFuture(peer_info<NodeId, Address>{new_id, addr});
        } catch (const std::exception& ex) {
            return future_factory_default::makeExceptionalFuture<peer_info<NodeId, Address>>(
                std::make_exception_ptr(std::runtime_error(
                    std::string("azure_vm_quorum_manager::provision_node: ") + ex.what())));
        }
    }

    /// Deletes the VM identified by node_id, then its NIC. A 404 on the VM
    /// delete is treated as success (idempotent). NIC deletion failures are
    /// logged but never propagated — they don't affect whether the node is
    /// considered decommissioned.
    auto decommission_node(const NodeId& node_id) -> kythira::future_default<void> {
        try {
            fiu_do_on("raft/azure/vm/delete_vm",
                      throw std::runtime_error("fault: raft/azure/vm/delete_vm"););

            std::string vm_name = node_id_to_vm_name(node_id);
            std::string nic_name = vm_name + "-nic";

            auto async_url = arm_delete("/providers/Microsoft.Compute/virtualMachines/" + vm_name +
                                        "?api-version=" + compute_api_version);
            if (async_url) {
                poll_lro(*async_url, std::chrono::seconds{30});
            }

            try {
                (void)arm_delete("/providers/Microsoft.Network/networkInterfaces/" + nic_name +
                                 "?api-version=" + network_api_version);
            } catch (const std::exception& ex) {
                std::cerr << "[azure_vm_quorum_manager::decommission_node] NIC delete for "
                          << nic_name << " failed (ignored): " << ex.what() << "\n";
            }

            return future_factory_default::makeFuture();
        } catch (const std::exception& ex) {
            return future_factory_default::makeExceptionalFuture<void>(
                std::make_exception_ptr(std::runtime_error(
                    std::string("azure_vm_quorum_manager::decommission_node: ") + ex.what())));
        }
    }

    /// Returns the desired topology from the configuration.
    [[nodiscard]] auto topology() const -> desired_topology<std::string> { return _cfg.topology; }

    /// Pure string computation: no ARM call. `"kythira-" + cluster_name + "-" + nid`.
    [[nodiscard]] auto node_id_to_vm_name(const NodeId& nid) const -> std::string {
        return "kythira-" + _cfg.cluster_name + "-" + node_id_str(nid);
    }

    /// Inverse of `node_id_to_vm_name`. Returns nullopt if `name` doesn't match
    /// this cluster's naming scheme or the trailing segment isn't a valid NodeId.
    [[nodiscard]] auto vm_name_to_node_id(const std::string& name) const -> std::optional<NodeId> {
        std::string prefix = "kythira-" + _cfg.cluster_name + "-";
        if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) {
            return std::nullopt;
        }
        std::string suffix = name.substr(prefix.size());
        try {
            if constexpr (std::is_same_v<NodeId, std::string>) {
                std::size_t consumed = 0;
                std::stoull(suffix, &consumed);
                if (consumed != suffix.size()) {
                    return std::nullopt;
                }
                return suffix;
            } else {
                std::size_t consumed = 0;
                auto v = std::stoull(suffix, &consumed);
                if (consumed != suffix.size()) {
                    return std::nullopt;
                }
                return static_cast<NodeId>(v);
            }
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

private:
    // ARM API versions used by this class. Pinned to a known-good published
    // version at the time this spec was written; reverify against the current
    // published Microsoft.Compute/Microsoft.Network API surface before relying
    // on this in a real deployment (the same "confirm against reality before
    // finalizing" discipline tasks.md already calls out for the vcpkg/CMake
    // target names).
    static constexpr const char* compute_api_version = "2024-07-01";
    static constexpr const char* network_api_version = "2024-05-01";

    /// Thrown internally by arm_get/arm_delete on a 404; callers translate this
    /// into whatever "not found" means for that call site.
    struct arm_not_found : std::runtime_error {
        arm_not_found() : std::runtime_error("ARM resource not found (404)") {}
    };

    azure_vm_quorum_manager_config _cfg;
    std::shared_ptr<Azure::Core::Http::_internal::HttpPipeline> _pipeline;
    std::string _arm_base;

    /// Scans every VM tagged `kythira:cluster = {cluster_name}` (all power
    /// states — a VM mid-deletion is still counted) and returns
    /// `max(kythira:node-id) + 1`, or 1 when none exist. This has the same
    /// TOCTOU race as its AWS counterpart if two leaders call provision_node
    /// concurrently; the quorum-management spec's concurrency rules already
    /// prevent that (a single manager instance is never called concurrently
    /// for the same slot).
    [[nodiscard]] auto next_node_id() const -> NodeId {
        std::uint64_t max_id = 0;
        // The $filter value's spaces/quotes are OData syntax, not part of the
        // URL grammar -- they must be percent-encoded before this becomes a
        // request line, or the raw literal spaces make it an invalid HTTP
        // request that Azure's edge rejects instantly with a generic "400:
        // the request is badly formed" (observed directly against a real
        // subscription; do_send()'s Azure::Core::Url(_arm_base + path)
        // constructor parses path as an already-valid URL string and does
        // not encode it for you).
        std::string filter_value =
            "tagName eq 'kythira:cluster' and tagValue eq '" + _cfg.cluster_name + "'";
        auto body =
            arm_get("/providers/Microsoft.Compute/virtualMachines?$filter=" +
                    Azure::Core::Url::Encode(filter_value) + "&api-version=" + compute_api_version);
        if (body.is_object() && body.as_object().contains("value")) {
            for (const auto& vm : body.at("value").as_array()) {
                if (!vm.is_object() || !vm.as_object().contains("tags")) {
                    continue;
                }
                const auto& tags = vm.at("tags");
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

    [[nodiscard]] auto do_send(const Azure::Core::Http::HttpMethod& method, const std::string& path,
                               const boost::json::value* body) const
        -> std::unique_ptr<Azure::Core::Http::RawResponse> {
        Azure::Core::Url url(_arm_base + path);
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
        auto response = do_send(Azure::Core::Http::HttpMethod::Get, path, nullptr);
        return parse_response(*response);
    }

    [[nodiscard]] auto arm_put(const std::string& path, const boost::json::value& body) const
        -> boost::json::value {
        auto response = do_send(Azure::Core::Http::HttpMethod::Put, path, &body);
        return parse_response(*response);
    }

    /// Returns the `Azure-AsyncOperation` header URL when present (202
    /// response), so the caller can poll it to completion. Returns nullopt when
    /// the resource was already gone (404, treated as success) or the delete
    /// completed synchronously (200/204).
    [[nodiscard]] auto arm_delete(const std::string& path) const -> std::optional<std::string> {
        auto response = do_send(Azure::Core::Http::HttpMethod::Delete, path, nullptr);
        auto code = static_cast<int>(response->GetStatusCode());
        if (code == 404) {
            return std::nullopt;
        }
        if (code < 200 || code >= 300) {
            throw std::runtime_error("ARM delete failed (" + std::to_string(code) + ") for " +
                                     path);
        }
        const auto& headers = response->GetHeaders();
        if (auto it = headers.find("Azure-AsyncOperation"); it != headers.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    /// Polls an `Azure-AsyncOperation` URL until it reports "Succeeded",
    /// "Failed"/"Canceled" (both treated as errors), or `timeout` elapses.
    void poll_lro(const std::string& async_operation_url, std::chrono::seconds timeout) const {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            Azure::Core::Url url(async_operation_url);
            Azure::Core::Http::Request request(Azure::Core::Http::HttpMethod::Get, url);
            Azure::Core::Context context;
            auto response = _pipeline->Send(request, context);
            auto body = parse_response(*response);
            if (body.is_object() && body.as_object().contains("status")) {
                std::string status(body.at("status").as_string());
                if (status == "Succeeded") {
                    return;
                }
                if (status == "Failed" || status == "Canceled") {
                    throw std::runtime_error("ARM long-running operation reported status: " +
                                             status);
                }
            }
            std::this_thread::sleep_for(std::chrono::seconds{2});
        }
        // Timing out here is not treated as fatal by callers (decommission_node
        // proceeds to delete the NIC regardless); the VM delete was already
        // accepted by ARM even if this poll gave up waiting for confirmation.
    }

    void best_effort_delete_vm(const std::string& vm_name) const {
        try {
            (void)arm_delete("/providers/Microsoft.Compute/virtualMachines/" + vm_name +
                             "?api-version=" + compute_api_version);
        } catch (const std::exception& ex) {
            std::cerr << "[azure_vm_quorum_manager] best-effort delete of VM " << vm_name
                      << " failed: " << ex.what() << "\n";
        }
    }

    void best_effort_delete_nic(const std::string& nic_name) const {
        try {
            (void)arm_delete("/providers/Microsoft.Network/networkInterfaces/" + nic_name +
                             "?api-version=" + network_api_version);
        } catch (const std::exception& ex) {
            std::cerr << "[azure_vm_quorum_manager] best-effort delete of NIC " << nic_name
                      << " failed: " << ex.what() << "\n";
        }
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
        std::string placement_val = "none";
        if (auto it = _cfg.placement_by_group.find(group); it != _cfg.placement_by_group.end()) {
            switch (it->second.kind) {
                case azure_placement_kind::availability_zone:
                    placement_val = "availability-zone";
                    break;
                case azure_placement_kind::proximity_placement_group:
                    placement_val = "proximity-placement-group";
                    break;
                case azure_placement_kind::availability_set:
                    placement_val = "availability-set";
                    break;
            }
        }
        boost::json::object tags;
        tags["kythira:cluster"] = _cfg.cluster_name;
        tags["kythira:node-id"] = node_id_str(nid);
        tags["kythira:group"] = group;
        tags["kythira:managed-by"] = "kythira-azure-vm-quorum-manager";
        tags["kythira:priority"] = _cfg.priority == azure_vm_priority::spot ? "spot" : "regular";
        tags["kythira:placement"] = placement_val;
        for (const auto& [k, v] : _cfg.extra_tags) {
            tags[k] = v;
        }
        return tags;
    }

    /// Sets `zones` (on the top-level VM resource) or
    /// `properties.proximityPlacementGroup`/`properties.availabilitySet` (on
    /// the nested properties object, which is still under construction at this
    /// point — it is moved into `vm_body["properties"]` by the caller only
    /// after this and `apply_priority_fields` have both run) per
    /// `placement_by_group[target_group]`.
    void apply_placement_fields(boost::json::object& vm_body, boost::json::object& vm_body_props,
                                const std::string& target_group) const {
        auto it = _cfg.placement_by_group.find(target_group);
        if (it == _cfg.placement_by_group.end()) {
            return;
        }
        const auto& placement = it->second;
        switch (placement.kind) {
            case azure_placement_kind::availability_zone:
                if (!placement.zone.empty()) {
                    vm_body["zones"] = boost::json::array{placement.zone};
                }
                break;
            case azure_placement_kind::proximity_placement_group:
                vm_body_props["proximityPlacementGroup"] =
                    boost::json::object{{"id", placement.resource_id}};
                break;
            case azure_placement_kind::availability_set:
                vm_body_props["availabilitySet"] =
                    boost::json::object{{"id", placement.resource_id}};
                break;
        }
    }

    /// Sets `priority`/`evictionPolicy`/`billingProfile.maxPrice` on the VM
    /// properties object when `_cfg.priority == spot`.
    void apply_priority_fields(boost::json::object& vm_body_props) const {
        if (_cfg.priority != azure_vm_priority::spot) {
            return;
        }
        vm_body_props["priority"] = "Spot";
        const auto& spot = _cfg.spot_options.value_or(azure_spot_options{});
        vm_body_props["evictionPolicy"] =
            spot.eviction_policy == azure_eviction_policy::delete_vm ? "Delete" : "Deallocate";
        if (spot.max_price >= 0.0) {
            boost::json::object billing;
            billing["maxPrice"] = spot.max_price;
            vm_body_props["billingProfile"] = std::move(billing);
        }
    }

    /// Substitutes {NODE_ID}, {NODE_PORT}, {CLUSTER}, {GROUP} in
    /// custom_data_template.
    [[nodiscard]] auto render_custom_data(const NodeId& nid, const std::string& group) const
        -> std::string {
        std::string result = _cfg.custom_data_template;
        auto replace_all = [&](const std::string& from, const std::string& to) {
            std::size_t pos = 0;
            while ((pos = result.find(from, pos)) != std::string::npos) {
                result.replace(pos, from.size(), to);
                pos += to.size();
            }
        };
        replace_all("{NODE_ID}", node_id_str(nid));
        replace_all("{NODE_PORT}", std::to_string(_cfg.node_port));
        replace_all("{CLUSTER}", _cfg.cluster_name);
        replace_all("{GROUP}", group);
        return result;
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

static_assert(quorum_manager<azure_vm_quorum_manager<std::uint64_t, std::string>, std::uint64_t,
                             std::string, std::string>,
              "azure_vm_quorum_manager must satisfy quorum_manager");

}  // namespace kythira

#endif  // KYTHIRA_HAS_AZURE_SDK

// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <raft/aws_client_config.hpp>
#include <raft/composite_node_id.hpp>
#include <raft/fault_injection.hpp>
#include <raft/future_default.hpp>
#include <raft/quorum_management.hpp>

#ifdef KYTHIRA_HAS_AWS_SDK

#include <aws/core/client/ClientConfiguration.h>
#include <aws/ec2/EC2Client.h>
#include <aws/ec2/model/CreateTagsRequest.h>
#include <aws/ec2/model/DescribeInstanceStatusRequest.h>
#include <aws/ec2/model/DescribeInstancesRequest.h>
#include <aws/ec2/model/Filter.h>
#include <aws/ec2/model/IamInstanceProfileSpecification.h>
#include <aws/ec2/model/InstanceMarketOptionsRequest.h>
#include <aws/ec2/model/InstanceType.h>
#include <aws/ec2/model/Placement.h>
#include <aws/ec2/model/ResourceType.h>
#include <aws/ec2/model/RunInstancesRequest.h>
#include <aws/ec2/model/SpotMarketOptions.h>
#include <aws/ec2/model/Tag.h>
#include <aws/ec2/model/TagSpecification.h>
#include <aws/ec2/model/TerminateInstancesRequest.h>
#include <aws/core/utils/Array.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/base64/Base64.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace kythira {

// ============================================================================
// Spot and placement group configuration
// ============================================================================

/// What EC2 does when a Spot instance is interrupted by AWS.
enum class ec2_spot_interruption_behavior : std::uint8_t {
    terminate,  ///< Instance is terminated (default; safest for stateless nodes).
    stop,       ///< Instance is stopped; EBS root volume is preserved.
    hibernate,  ///< Instance RAM is written to EBS and the instance is stopped.
};

/// Spot-instance market options applied to every node provisioned by aws_ec2_quorum_manager.
struct ec2_spot_options {
    /// Maximum hourly price in USD (e.g. "0.05"). Empty string = current Spot price.
    std::string max_price;
    /// Action taken when AWS reclaims the instance. Default: terminate.
    ec2_spot_interruption_behavior interruption_behavior{ec2_spot_interruption_behavior::terminate};
};

/// EC2 placement-group strategy applied to a per-group batch of nodes.
enum class ec2_placement_group_strategy : std::uint8_t {
    none,       ///< No placement group; EC2 chooses placement freely.
    cluster,    ///< All instances in a single rack; maximum bandwidth, correlated failure risk.
    spread,     ///< Each instance on distinct hardware; maximises fault isolation.
    partition,  ///< Instances distributed across named partitions of a rack group.
};

/// Placement-group settings for a single quorum group.
struct ec2_placement_group_config {
    /// Name of an existing EC2 placement group to join. Empty = no placement group.
    std::string name;
    /// Strategy that was used when creating the placement group.
    ec2_placement_group_strategy strategy{ec2_placement_group_strategy::none};
    /// Partition index (1-based) within a partition placement group. 0 = let EC2 choose.
    std::uint32_t partition_number{0};
};

// ============================================================================
// aws_ec2_quorum_manager_config
// ============================================================================

/// Configuration for aws_ec2_quorum_manager.
struct aws_ec2_quorum_manager_config {
    /// Logical cluster name; used as a tag prefix on every provisioned instance.
    std::string cluster_name;
    /// AMI ID used for all provisioned nodes (e.g. "ami-0abcdef1234567890").
    std::string image_id;
    /// EC2 instance type string (e.g. "m6i.large"). Default: "t3.micro".
    std::string instance_type{"t3.micro"};
    /// TCP port on which each node listens. Written into the node address returned by
    /// provision_node.
    std::uint16_t node_port{7000};
    /// Target node counts per placement group; must be non-empty.
    desired_topology<std::string> topology;
    /// Maps each topology group_id to the VPC subnet ID used when launching nodes in that group.
    std::map<std::string, std::string> subnet_by_group;
    /// VPC security group IDs applied to every provisioned instance.
    std::vector<std::string> security_group_ids;
    /// IAM instance-profile name attached to each instance. Empty = no profile.
    std::string iam_instance_profile;
    /// EC2 key pair name attached to each instance for SSH access. Empty = no key pair
    /// (the default — most deployments manage nodes without direct SSH access).
    std::string key_name;
    /// EC2 user-data template. Supports {NODE_ID}, {NODE_PORT}, {CLUSTER}, {AZ} substitutions.
    std::string user_data_template;
    /// Additional EC2 tags applied alongside the standard kythira:* tags.
    std::map<std::string, std::string> extra_tags;
    /// Per-group placement-group settings. Groups absent from this map get no placement group.
    std::map<std::string, ec2_placement_group_config> placement_by_group;
    /// When set, nodes are launched as Spot instances with these options.
    std::optional<ec2_spot_options> spot_options;
    /// Maximum time to wait for a newly launched instance to reach "running" state.
    std::chrono::seconds provision_timeout{120};
    /// Sleep interval between DescribeInstances polls during provisioning.
    std::chrono::seconds poll_interval{5};
    /// AWS client settings (region, endpoint override, credentials, timeout).
    aws_client_config aws;
};

// ============================================================================
// aws_ec2_quorum_manager
// ============================================================================

/// quorum_manager implementation that provisions and monitors Raft nodes as EC2 instances.
///
/// Node identity depends on `NodeId` (spec cloud-composite-node-ids, R4 and R11):
///
/// - `aws_ec2_node_id`, or `std::string` holding its canonical text: the node id IS
///   the instance, `{configured region, instance id}`. Mapping a node to its
///   instance is a field read; nothing is parsed as a number, so every EC2 id
///   (17 hex digits with any first digit, or a legacy 8-digit id) fits.
/// - an unsigned integer (numeric mode, today's default): the manager allocates
///   `max(kythira:node-id over the cluster) + 1` before launch, puts it in the
///   launch request's tags and `{NODE_ID}`, and maps an id back to its instance
///   with DescribeInstances filtered on `kythira:cluster` and `kythira:node-id`.
///   An instance launched by the old derivation (id = the instance id's hex value)
///   is found by the same tag; one whose best-effort tag is missing falls back to
///   that derivation (R11.4). Two managers provisioning into one cluster at the same
///   moment can allocate the same id, as with every max-plus-one allocator (R11.5).
///
/// A node is live iff its instance state is `running`, read from DescribeInstances
/// (not heartbeats).
template<typename NodeId = std::uint64_t, typename Address = std::string>
requires node_id<NodeId>
class aws_ec2_quorum_manager {
    static_assert(std::unsigned_integral<NodeId> || std::same_as<NodeId, std::string> ||
                      std::same_as<NodeId, aws_ec2_node_id>,
                  "aws_ec2_quorum_manager: NodeId must be an unsigned integer, std::string or "
                  "aws_ec2_node_id");

public:
    using node_id_type = NodeId;
    using address_type = Address;
    using placement_group_id_type = std::string;

    /// True when the node id is the instance itself (composite or string mode).
    static constexpr bool instance_is_node_id = node_id_traits<NodeId>::is_textual;

    /// Constructs the manager and validates the configuration.
    /// Throws std::invalid_argument if cluster_name, image_id, or node_port are empty/zero,
    /// or if topology references a group not present in subnet_by_group.
    explicit aws_ec2_quorum_manager(aws_ec2_quorum_manager_config cfg) : _cfg(std::move(cfg)) {
        if (_cfg.cluster_name.empty()) {
            throw std::invalid_argument("aws_ec2_quorum_manager: cluster_name must be non-empty");
        }
        if (_cfg.image_id.empty()) {
            throw std::invalid_argument("aws_ec2_quorum_manager: image_id must be non-empty");
        }
        if (_cfg.node_port == 0) {
            throw std::invalid_argument("aws_ec2_quorum_manager: node_port must be non-zero");
        }
        for (const auto& gt : _cfg.topology.groups) {
            if (_cfg.subnet_by_group.find(gt.group_id) == _cfg.subnet_by_group.end()) {
                throw std::invalid_argument(
                    "aws_ec2_quorum_manager: no subnet configured for group: " + gt.group_id);
            }
        }
        if constexpr (instance_is_node_id) {
            if (!aws_ec2_rules::valid_region(_cfg.aws.region)) {
                throw std::invalid_argument(
                    "aws_ec2_quorum_manager: aws.region must name a region when the node id is "
                    "the instance (got '" +
                    _cfg.aws.region + "')");
            }
        }
        _ec2 = make_ec2_client(_cfg.aws);
    }

    /// Returns the health of the current cluster by calling DescribeInstanceStatus.
    /// A node is live iff its instance state is "running" (SetIncludeAllInstances=true
    /// so transitioning instances are visible). Returns an exceptional Future on API error.
    auto assess_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::future_default<quorum_health<NodeId, std::string>> {
        try {
            fiu_do_on("raft/aws/ec2/describe_instance_status",
                      throw std::runtime_error("fault: raft/aws/ec2/describe_instance_status"););

            if (cluster.empty()) {
                return build_health(cluster, {});
            }

            std::vector<NodeId> ids;
            ids.reserve(cluster.size());
            for (const auto& np : cluster) {
                ids.push_back(np.node_id);
                raise_id_floor(np.node_id);
            }
            return build_health(cluster,
                                live_by_node(*_ec2, _cfg.aws.region, _cfg.cluster_name, ids));
        } catch (const std::exception& ex) {
            return future_factory_default::makeExceptionalFuture<
                quorum_health<NodeId, std::string>>(std::make_exception_ptr(std::runtime_error(
                std::string("aws_ec2_quorum_manager::assess_quorum: ") + ex.what())));
        }
    }

    /// Assesses quorum, terminates unreachable nodes, and provisions replacements to meet topology.
    /// Decommission and provision errors are logged to stderr but do not abort the operation;
    /// the returned health reflects the pre-maintenance cluster state.
    auto maintain_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::future_default<quorum_health<NodeId, std::string>> {
        try {
            fiu_do_on("raft/aws/ec2/maintain_quorum",
                      throw std::runtime_error("fault: raft/aws/ec2/maintain_quorum"););
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
                std::cerr << "[aws_ec2_quorum_manager::maintain_quorum] decommission of "
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
                std::optional<NodeId> hint;
                if (auto it = last_replaced.find(gt.group_id); it != last_replaced.end()) {
                    hint = it->second;
                }
                try {
                    std::move(provision_node(gt.group_id, hint)).get();
                } catch (const std::exception& ex) {
                    std::cerr << "[aws_ec2_quorum_manager::maintain_quorum] provision in "
                              << gt.group_id << " failed: " << ex.what() << "\n";
                }
            }
        }

        return future_factory_default::makeFuture(std::move(pre_health));
    }

    /// Launches a new EC2 instance in the subnet mapped to target_group.
    /// In composite and string mode the NodeId is the instance RunInstances returns; in numeric
    /// mode it is allocated before launch. replacing is accepted by the interface but not used
    /// (the new instance gets a new id regardless).
    /// Returns an exceptional Future if RunInstances fails or the provision_timeout is exceeded.
    auto provision_node(std::string target_group, std::optional<NodeId> /*replacing*/)
        -> kythira::future_default<peer_info<NodeId, Address>> {
        try {
            fiu_do_on("raft/aws/ec2/run_instances",
                      throw std::runtime_error("fault: raft/aws/ec2/run_instances"););

            auto sit = _cfg.subnet_by_group.find(target_group);
            if (sit == _cfg.subnet_by_group.end()) {
                throw std::invalid_argument("aws_ec2_quorum_manager: no subnet for group: " +
                                            target_group);
            }
            const std::string& subnet_id = sit->second;

            Aws::EC2::Model::RunInstancesRequest run_req;
            run_req.SetImageId(_cfg.image_id);
            run_req.SetInstanceType(
                Aws::EC2::Model::InstanceTypeMapper::GetInstanceTypeForName(_cfg.instance_type));
            run_req.SetMinCount(1);
            run_req.SetMaxCount(1);
            run_req.SetSubnetId(subnet_id);
            for (const auto& sg : _cfg.security_group_ids) {
                run_req.AddSecurityGroupIds(sg);
            }
            if (!_cfg.iam_instance_profile.empty()) {
                Aws::EC2::Model::IamInstanceProfileSpecification iam_spec;
                iam_spec.SetName(_cfg.iam_instance_profile);
                run_req.SetIamInstanceProfile(iam_spec);
            }
            if (!_cfg.key_name.empty()) {
                run_req.SetKeyName(_cfg.key_name);
            }
            // Numeric mode knows the id before launch, so it goes into the
            // launch tags and {NODE_ID}. When the id is the instance it does
            // not exist until RunInstances answers: {NODE_ID} is left as is
            // and Name/kythira:node-id are applied afterwards.
            std::optional<NodeId> pre_launch_id;
            if constexpr (!instance_is_node_id) {
                pre_launch_id =
                    allocate_numeric_node_id(*_ec2, _cfg.cluster_name, _id_floor->load());
                raise_id_floor(*pre_launch_id);
            }

            if (!_cfg.user_data_template.empty()) {
                std::string rendered = render_user_data(pre_launch_id, target_group);
                Aws::Utils::ByteBuffer user_data_bytes(
                    reinterpret_cast<const unsigned char*>(rendered.data()), rendered.size());
                run_req.SetUserData(Aws::Utils::Base64::Base64().Encode(user_data_bytes));
            }
            if (auto pit = _cfg.placement_by_group.find(target_group);
                pit != _cfg.placement_by_group.end() && !pit->second.name.empty()) {
                Aws::EC2::Model::Placement placement;
                placement.SetGroupName(pit->second.name);
                if (pit->second.strategy == ec2_placement_group_strategy::partition &&
                    pit->second.partition_number > 0) {
                    placement.SetPartitionNumber(static_cast<int>(pit->second.partition_number));
                }
                run_req.SetPlacement(placement);
            }
            if (_cfg.spot_options) {
                Aws::EC2::Model::InstanceMarketOptionsRequest market_opts;
                market_opts.SetMarketType(Aws::EC2::Model::MarketType::spot);
                Aws::EC2::Model::SpotMarketOptions spot_opts;
                spot_opts.SetSpotInstanceType(Aws::EC2::Model::SpotInstanceType::one_time);
                if (!_cfg.spot_options->max_price.empty()) {
                    spot_opts.SetMaxPrice(_cfg.spot_options->max_price);
                }
                spot_opts.SetInstanceInterruptionBehavior(
                    to_aws_interruption_behavior(_cfg.spot_options->interruption_behavior));
                market_opts.SetSpotOptions(spot_opts);
                run_req.SetInstanceMarketOptions(market_opts);
            }

            // Tag in the launch request, so every tag that can be known before
            // the instance exists is applied atomically with it. RunInstances
            // and CreateTags are two round trips, and an instance orphaned
            // between them carries no tags at all -- invisible to the post-run
            // leak audit, which filters on kythira:managed-by. That is how a
            // t4g.micro bastion (the same shape of gap, in the real-EC2 fixture)
            // billed ~44h on 2026-09-27 while the audit reported clean.
            //
            // In numeric mode Name and kythira:node-id come along too, so the
            // tag the reverse lookup reads can never be missing. When the node
            // id is the instance neither value exists until RunInstances has
            // answered; apply_identity_tags writes them afterwards, and they are
            // cosmetic for leak detection -- the tags the audit actually reads
            // are all in here.
            const std::string market_tag = _cfg.spot_options ? "spot" : "on-demand";
            run_req.AddTagSpecifications(
                launch_tag_specification(target_group, market_tag, pre_launch_id));

            auto outcome = _ec2->RunInstances(run_req);
            if (!outcome.IsSuccess()) {
                throw std::runtime_error("ec2 RunInstances: " +
                                         std::string(outcome.GetError().GetMessage()));
            }
            const auto& instances = outcome.GetResult().GetInstances();
            if (instances.empty()) {
                throw std::runtime_error("ec2 RunInstances: no instances in response");
            }
            std::string ec2_id(instances[0].GetInstanceId());

            NodeId new_id{};
            if constexpr (instance_is_node_id) {
                auto from_instance = node_id_for_instance(_cfg.aws.region, ec2_id);
                if (!from_instance) {
                    Aws::EC2::Model::TerminateInstancesRequest term;
                    term.AddInstanceIds(ec2_id);
                    _ec2->TerminateInstances(term);
                    throw std::runtime_error(
                        "ec2 RunInstances returned an instance id that is "
                        "not a valid EC2 id in " +
                        _cfg.aws.region + ": '" + ec2_id + "'");
                }
                new_id = std::move(*from_instance);
                apply_identity_tags(ec2_id, new_id);
            } else {
                new_id = *pre_launch_id;
            }

            // Poll until running or timeout — DescribeInstances is used here for
            // provisioning state (not liveness determination).
            std::string private_ip;
            auto deadline = std::chrono::steady_clock::now() + _cfg.provision_timeout;
            while (std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(_cfg.poll_interval);
                Aws::EC2::Model::DescribeInstancesRequest poll_req;
                poll_req.AddInstanceIds(ec2_id);
                auto poll = _ec2->DescribeInstances(poll_req);
                if (!poll.IsSuccess()) {
                    continue;
                }
                const auto& res = poll.GetResult().GetReservations();
                if (res.empty() || res[0].GetInstances().empty()) {
                    continue;
                }
                const auto& inst = res[0].GetInstances()[0];
                auto st = inst.GetState().GetName();
                if (st == Aws::EC2::Model::InstanceStateName::running) {
                    private_ip = std::string(inst.GetPrivateIpAddress());
                    if (!private_ip.empty()) {
                        break;
                    }
                }
                if (st == Aws::EC2::Model::InstanceStateName::terminated ||
                    st == Aws::EC2::Model::InstanceStateName::shutting_down) {
                    throw std::runtime_error("ec2 provision: instance " + ec2_id +
                                             " terminated during startup");
                }
            }
            if (private_ip.empty()) {
                Aws::EC2::Model::TerminateInstancesRequest term;
                term.AddInstanceIds(ec2_id);
                _ec2->TerminateInstances(term);
                throw std::runtime_error("ec2 provision timeout for " + ec2_id);
            }

            Address addr = static_cast<Address>(private_ip + ":" + std::to_string(_cfg.node_port));
            return future_factory_default::makeFuture(peer_info<NodeId, Address>{new_id, addr});
        } catch (const std::exception& ex) {
            return future_factory_default::makeExceptionalFuture<peer_info<NodeId, Address>>(
                std::make_exception_ptr(std::runtime_error(
                    std::string("aws_ec2_quorum_manager::provision_node: ") + ex.what())));
        }
    }

    /// Terminates the EC2 instance identified by node_id and waits up to 30 s for the
    /// state to leave "running", so a subsequent assess_quorum call sees it as unreachable.
    /// Returns successfully if the instance was already gone (InvalidInstanceID.NotFound).
    auto decommission_node(const NodeId& node_id) -> kythira::future_default<void> {
        try {
            fiu_do_on("raft/aws/ec2/terminate_instances",
                      throw std::runtime_error("fault: raft/aws/ec2/terminate_instances"););

            auto found = instance_id_of(node_id);
            if (!found) {
                if constexpr (instance_is_node_id) {
                    throw std::invalid_argument("node " + node_id_str(node_id) +
                                                " is not an EC2 instance in " + _cfg.aws.region);
                } else {
                    // No instance carries this id any more: already gone.
                    return future_factory_default::makeFuture();
                }
            }
            const std::string ec2_id = *found;
            Aws::EC2::Model::TerminateInstancesRequest req;
            req.AddInstanceIds(ec2_id);
            auto outcome = _ec2->TerminateInstances(req);
            if (!outcome.IsSuccess()) {
                const auto& err = outcome.GetError();
                // InvalidInstanceID.NotFound → instance is already gone; treat as success.
                if (std::string(err.GetExceptionName()).find("InvalidInstanceID") !=
                    std::string::npos) {
                    return future_factory_default::makeFuture();
                }
                throw std::runtime_error("ec2 TerminateInstances: " +
                                         std::string(err.GetMessage()));
            }
            // TerminateInstances is acknowledged before the state actually changes.
            // Poll until the instance is no longer running so that a subsequent
            // assess_quorum call reliably sees it as unreachable.
            {
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
                while (std::chrono::steady_clock::now() < deadline) {
                    Aws::EC2::Model::DescribeInstanceStatusRequest poll;
                    poll.AddInstanceIds(ec2_id);
                    poll.SetIncludeAllInstances(true);
                    auto ps = _ec2->DescribeInstanceStatus(poll);
                    if (!ps.IsSuccess()) {
                        break;
                    }
                    const auto& sv = ps.GetResult().GetInstanceStatuses();
                    if (sv.empty()) {
                        break;
                    }
                    if (sv[0].GetInstanceState().GetName() !=
                        Aws::EC2::Model::InstanceStateName::running) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::seconds{2});
                }
            }
            return future_factory_default::makeFuture();
        } catch (const std::exception& ex) {
            return future_factory_default::makeExceptionalFuture<void>(
                std::make_exception_ptr(std::runtime_error(
                    std::string("aws_ec2_quorum_manager::decommission_node: ") + ex.what())));
        }
    }

    /// Returns the desired topology from the configuration.
    [[nodiscard]] auto topology() const -> desired_topology<std::string> { return _cfg.topology; }

    /// The instance a node runs on, or nullopt when there is none. In composite and
    /// string mode this is a field read, and nullopt means the id is not an EC2
    /// instance in the configured region. In numeric mode it is a tag lookup
    /// (DescribeInstances), preferring an instance that is not terminated.
    [[nodiscard]] auto instance_id_of(const NodeId& nid) const -> std::optional<std::string> {
        if constexpr (instance_is_node_id) {
            return instance_id_for_node(_cfg.aws.region, nid);
        } else {
            auto found = find_numeric_instances(*_ec2, _cfg.cluster_name, {nid});
            auto it = found.find(node_id_traits<NodeId>::to_text(nid));
            if (it == found.end() || it->second.empty()) {
                return std::nullopt;
            }
            return preferred_instance(it->second).instance_id;
        }
    }

    /// The node id of an instance, or nullopt when it is not one of this
    /// cluster's nodes (numeric mode) or not a valid EC2 id (composite and string mode).
    [[nodiscard]] auto node_id_of_instance(const std::string& ec2_id) const
        -> std::optional<NodeId> {
        if constexpr (instance_is_node_id) {
            return node_id_for_instance(_cfg.aws.region, ec2_id);
        } else {
            return numeric_node_id_of_instance(*_ec2, _cfg.cluster_name, ec2_id);
        }
    }

    // ── id mapping shared with aws_asg_quorum_manager ────────────────────────

    /// One instance as the id mapping sees it.
    struct cluster_instance {
        std::string instance_id;
        Aws::EC2::Model::InstanceStateName state{Aws::EC2::Model::InstanceStateName::NOT_SET};
        std::optional<std::string> node_id_tag;
        std::optional<std::string> cluster_tag;
    };

    /// Composite and string mode: `{region, ec2_id}` as a NodeId, or nullopt when
    /// `ec2_id` is not a valid EC2 instance id. Never parses it as a number.
    static auto node_id_for_instance(std::string_view region, const std::string& ec2_id)
        -> std::optional<NodeId>
    requires instance_is_node_id
    {
        try {
            aws_ec2_node_id id{std::string(region), ec2_id};
            if constexpr (std::same_as<NodeId, std::string>) {
                return id.to_string();
            } else {
                return id;
            }
        } catch (const std::invalid_argument&) {
            return std::nullopt;
        }
    }

    /// Composite and string mode: the instance id, or nullopt when `nid` is not
    /// canonical aws-ec2 text or names another region (R4.4).
    static auto instance_id_for_node(std::string_view region, const NodeId& nid)
        -> std::optional<std::string>
    requires instance_is_node_id
    {
        std::optional<aws_ec2_node_id> id;
        if constexpr (std::same_as<NodeId, std::string>) {
            id = aws_ec2_node_id::parse(nid);
        } else {
            id = nid;
        }
        if (!id || id->scope() != region) {
            return std::nullopt;
        }
        return std::string(id->native());
    }

    /// Every instance matching `filters`, across pages. Throws on API error.
    static auto describe_instances(Aws::EC2::EC2Client& ec2,
                                   const Aws::Vector<Aws::EC2::Model::Filter>& filters)
        -> std::vector<cluster_instance> {
        std::vector<cluster_instance> out;
        Aws::String token;
        do {
            Aws::EC2::Model::DescribeInstancesRequest req;
            req.SetFilters(filters);
            if (!token.empty()) {
                req.SetNextToken(token);
            }
            auto outcome = ec2.DescribeInstances(req);
            if (!outcome.IsSuccess()) {
                throw std::runtime_error("ec2 DescribeInstances: " +
                                         std::string(outcome.GetError().GetMessage()));
            }
            for (const auto& r : outcome.GetResult().GetReservations()) {
                for (const auto& inst : r.GetInstances()) {
                    out.push_back({.instance_id = std::string(inst.GetInstanceId()),
                                   .state = inst.GetState().GetName(),
                                   .node_id_tag = find_tag(inst.GetTags(), "kythira:node-id"),
                                   .cluster_tag = find_tag(inst.GetTags(), "kythira:cluster")});
                }
            }
            token = outcome.GetResult().GetNextToken();
        } while (!token.empty());
        return out;
    }

    /// Live/dead per node, keyed by node_id_traits::to_text. A node with no
    /// instance, or whose id is foreign, is absent (reported unreachable).
    static auto live_by_node(Aws::EC2::EC2Client& ec2, std::string_view region,
                             const std::string& cluster, const std::vector<NodeId>& ids)
        -> std::map<std::string, bool> {
        std::map<std::string, bool> live;
        auto running = [](const cluster_instance& ci) {
            return ci.state == Aws::EC2::Model::InstanceStateName::running;
        };
        if constexpr (instance_is_node_id) {
            std::map<std::string, std::string> node_by_instance;
            for (const auto& nid : ids) {
                if (auto iid = instance_id_for_node(region, nid)) {
                    node_by_instance.emplace(*iid, node_id_traits<NodeId>::to_text(nid));
                }
            }
            std::vector<std::string> iids;
            for (const auto& [iid, _] : node_by_instance) {
                iids.push_back(iid);
            }
            for (const auto& chunk : chunked(iids)) {
                for (const auto& ci :
                     describe_instances(ec2, {make_filter("instance-id", chunk)})) {
                    if (auto it = node_by_instance.find(ci.instance_id);
                        it != node_by_instance.end()) {
                        live[it->second] = live[it->second] || running(ci);
                    }
                }
            }
        } else {
            for (const auto& [text, instances] : find_numeric_instances(ec2, cluster, ids)) {
                live[text] = std::ranges::any_of(instances, running);
            }
        }
        return live;
    }

    /// Numeric mode: the next id, one above both `floor` and the highest
    /// kythira:node-id tag on any of the cluster's instances in any state, so a
    /// terminated node's id is not reused while EC2 still lists it. Unparseable
    /// tags are skipped with a log line. Throws std::overflow_error rather than
    /// wrapping.
    ///
    /// EC2 stops listing a terminated instance after about an hour, so the tags
    /// alone could hand out an id a still-configured member had. The managers
    /// pass, as `floor`, the highest id they have allocated or been asked to
    /// assess, which covers every id the caller's membership still holds.
    ///
    /// Untagged instances are not counted: the old derivation spread their ids
    /// (an instance id's hex value) across the whole 64-bit range, far above
    /// anything this allocator reaches by counting up, and a not-yet-adopted ASG
    /// instance would otherwise push the next id there.
    static auto allocate_numeric_node_id(Aws::EC2::EC2Client& ec2, const std::string& cluster,
                                         std::uint64_t floor = 0) -> NodeId
    requires(!instance_is_node_id)
    {
        std::uint64_t max_seen = floor;
        for (const auto& ci :
             describe_instances(ec2, {make_filter("tag:kythira:cluster", {cluster})})) {
            if (!ci.node_id_tag) {
                continue;
            }
            auto v = node_id_traits<std::uint64_t>::from_text(*ci.node_id_tag);
            if (!v) {
                std::cerr << "[aws_ec2_quorum_manager] ignoring unparseable kythira:node-id '"
                          << *ci.node_id_tag << "' on " << ci.instance_id << "\n";
                continue;
            }
            max_seen = std::max(max_seen, *v);
        }
        if (max_seen >= static_cast<std::uint64_t>(std::numeric_limits<NodeId>::max())) {
            throw std::overflow_error("aws_ec2_quorum_manager: node id space of cluster " +
                                      cluster + " exhausted (highest kythira:node-id " +
                                      std::to_string(max_seen) + ")");
        }
        return next_numeric_node_id(static_cast<NodeId>(max_seen));
    }

    /// Numeric mode: the instances carrying each id, keyed by its decimal text.
    /// Found by `tag:kythira:cluster` + `tag:kythira:node-id`; an id with no
    /// tagged instance falls back to the old derivation (R11.4): the instance
    /// whose id is `i-` and the id as 17 hex digits, accepted only when it has no
    /// kythira:node-id tag and no other cluster's tag, and logged.
    static auto find_numeric_instances(Aws::EC2::EC2Client& ec2, const std::string& cluster,
                                       const std::vector<NodeId>& ids)
        -> std::map<std::string, std::vector<cluster_instance>>
    requires(!instance_is_node_id)
    {
        std::map<std::string, std::vector<cluster_instance>> found;
        std::vector<std::string> texts;
        for (const auto& nid : ids) {
            texts.push_back(node_id_traits<NodeId>::to_text(nid));
        }
        std::ranges::sort(texts);
        texts.erase(std::unique(texts.begin(), texts.end()), texts.end());
        for (const auto& chunk : chunked(texts)) {
            for (auto& ci : describe_instances(ec2, {make_filter("tag:kythira:cluster", {cluster}),
                                                     make_filter("tag:kythira:node-id", chunk)})) {
                if (ci.node_id_tag) {
                    found[*ci.node_id_tag].push_back(std::move(ci));
                }
            }
        }

        std::map<std::string, std::string> legacy;  // instance id -> node id text
        for (const auto& text : texts) {
            if (!found.contains(text)) {
                auto v = node_id_traits<std::uint64_t>::from_text(text);
                if (v) {
                    legacy.emplace(legacy_instance_id(*v), text);
                }
            }
        }
        std::vector<std::string> legacy_ids;
        for (const auto& [iid, _] : legacy) {
            legacy_ids.push_back(iid);
        }
        for (const auto& chunk : chunked(legacy_ids)) {
            for (auto& ci : describe_instances(ec2, {make_filter("instance-id", chunk)})) {
                if (ci.node_id_tag || (ci.cluster_tag && *ci.cluster_tag != cluster)) {
                    continue;
                }
                const auto& text = legacy.at(ci.instance_id);
                std::cerr << "[aws_ec2_quorum_manager] " << ci.instance_id
                          << " has no kythira:node-id tag; identified as node " << text
                          << " by the pre-tag derivation from its instance id\n";
                found[text].push_back(std::move(ci));
            }
        }
        return found;
    }

    /// Numeric mode: the node id an instance carries, by tag or by the R11.4
    /// fallback, or nullopt when it is not one of `cluster`'s nodes.
    static auto numeric_node_id_of_instance(Aws::EC2::EC2Client& ec2, const std::string& cluster,
                                            const std::string& ec2_id) -> std::optional<NodeId>
    requires(!instance_is_node_id)
    {
        auto instances = describe_instances(ec2, {make_filter("instance-id", {ec2_id})});
        if (instances.empty()) {
            return std::nullopt;
        }
        const auto& ci = instances.front();
        if (ci.cluster_tag && *ci.cluster_tag != cluster) {
            return std::nullopt;
        }
        if (ci.node_id_tag) {
            if (!ci.cluster_tag) {
                return std::nullopt;
            }
            return node_id_traits<NodeId>::from_text(*ci.node_id_tag);
        }
        auto legacy = legacy_node_id(ec2_id);
        if (!legacy || *legacy > static_cast<std::uint64_t>(std::numeric_limits<NodeId>::max())) {
            return std::nullopt;
        }
        return static_cast<NodeId>(*legacy);
    }

    /// The instance a decommission or lookup acts on when several carry one id
    /// (a duplicate from the R11.5 race): a running one, else any not terminated,
    /// else the first.
    static auto preferred_instance(const std::vector<cluster_instance>& instances)
        -> const cluster_instance& {
        using enum Aws::EC2::Model::InstanceStateName;
        for (const auto& ci : instances) {
            if (ci.state == running) {
                return ci;
            }
        }
        for (const auto& ci : instances) {
            if (ci.state != terminated && ci.state != shutting_down) {
                return ci;
            }
        }
        return instances.front();
    }

    /// The pre-tag derivation's instance id for a numeric id: `i-` and the value
    /// as 17 lower-case hex digits.
    static auto legacy_instance_id(std::uint64_t v) -> std::string {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "i-%017llx", static_cast<unsigned long long>(v));
        return buf;
    }

    /// The pre-tag derivation's numeric id for an instance id: its 17 hex digits
    /// as a number, or nullopt when they do not fit 64 bits (first digit not 0)
    /// or the id is not that shape. The old code threw on those, so it never
    /// launched one.
    static auto legacy_node_id(std::string_view ec2_id) -> std::optional<std::uint64_t> {
        if (!ec2_id.starts_with("i-") || ec2_id.size() != 19 || ec2_id[2] != '0') {
            return std::nullopt;
        }
        std::uint64_t v = 0;
        auto digits = ec2_id.substr(2);
        auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), v, 16);
        if (ec != std::errc{} || end != digits.data() + digits.size() ||
            !std::ranges::all_of(
                digits, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) {
            return std::nullopt;
        }
        return v;
    }

    /// Returns the value of the first tag with the given key, or nullopt if absent.
    static auto find_tag(const Aws::Vector<Aws::EC2::Model::Tag>& tags, const std::string& key)
        -> std::optional<std::string> {
        for (const auto& tag : tags) {
            if (std::string(tag.GetKey()) == key) {
                return std::string(tag.GetValue());
            }
        }
        return std::nullopt;
    }

    /// Returns NodeId as a string suitable for use as an EC2 tag value or map key.
    static auto node_id_str(const NodeId& id) -> std::string {
        return node_id_traits<NodeId>::to_text(id);
    }

private:
    /// EC2 caps the values of one filter; lookups are split into chunks this size.
    static constexpr std::size_t filter_chunk = 100;

    static auto chunked(const std::vector<std::string>& values)
        -> std::vector<std::vector<std::string>> {
        std::vector<std::vector<std::string>> out;
        for (std::size_t i = 0; i < values.size(); i += filter_chunk) {
            out.emplace_back(values.begin() + static_cast<std::ptrdiff_t>(i),
                             values.begin() + static_cast<std::ptrdiff_t>(
                                                  std::min(values.size(), i + filter_chunk)));
        }
        return out;
    }

    static auto make_filter(const std::string& name, const std::vector<std::string>& values)
        -> Aws::EC2::Model::Filter {
        Aws::EC2::Model::Filter f;
        f.SetName(name);
        for (const auto& v : values) {
            f.AddValues(v);
        }
        return f;
    }

    aws_ec2_quorum_manager_config _cfg;
    std::shared_ptr<Aws::EC2::EC2Client> _ec2;
    /// Numeric mode: the highest id allocated or assessed; see allocate_numeric_node_id.
    std::shared_ptr<std::atomic<std::uint64_t>> _id_floor =
        std::make_shared<std::atomic<std::uint64_t>>(0);

    void raise_id_floor(const NodeId& nid) {
        if constexpr (!instance_is_node_id) {
            auto v = static_cast<std::uint64_t>(nid);
            auto cur = _id_floor->load();
            while (cur < v && !_id_floor->compare_exchange_weak(cur, v)) {
            }
        }
    }

    /// Builds an EC2Client from aws_client_config, using credentials_provider when set.
    static auto make_ec2_client(const aws_client_config& aws)
        -> std::shared_ptr<Aws::EC2::EC2Client> {
        Aws::Client::ClientConfiguration client_cfg;
        if (!aws.region.empty()) {
            client_cfg.region = aws.region;
        }
        if (!aws.endpoint_override.empty()) {
            client_cfg.endpointOverride = aws.endpoint_override;
        }
        auto ms = static_cast<long>(aws.api_timeout.count() * 1000);
        client_cfg.requestTimeoutMs = ms;
        client_cfg.connectTimeoutMs = ms;
        if (aws.credentials_provider) {
            return std::make_shared<Aws::EC2::EC2Client>(aws.credentials_provider, client_cfg);
        }
        return std::make_shared<Aws::EC2::EC2Client>(client_cfg);
    }

    /// Builds a quorum_health from cluster membership and a live/dead map keyed by node_id_str.
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
            .status = compute_status(live_count, total),
            .live_node_count = live_count,
            .total_node_count = total,
            .unreachable_nodes = std::move(unreachable),
            .groups = std::move(groups),
        });
    }

    /// Maps live/total counts to quorum_status: lost < majority, critical == majority, degraded <
    /// total.
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

    [[nodiscard]] static auto make_ec2_tag(const std::string& k, const std::string& v)
        -> Aws::EC2::Model::Tag {
        Aws::EC2::Model::Tag t;
        t.SetKey(k);
        t.SetValue(v);
        return t;
    }

    /// The kythira:* tags and extra_tags that are known before the instance
    /// exists, as a TagSpecification for RunInstances so they are applied
    /// atomically with the launch.
    ///
    /// kythira:managed-by is the one that matters most for being here: the
    /// post-run leak audit filters instances on that key alone, so an instance
    /// orphaned before a follow-up CreateTags could run would be invisible to it.
    [[nodiscard]] auto launch_tag_specification(const std::string& group, const std::string& market,
                                                const std::optional<NodeId>& nid) const
        -> Aws::EC2::Model::TagSpecification {
        std::string placement_strategy_val = "none";
        if (auto pit = _cfg.placement_by_group.find(group); pit != _cfg.placement_by_group.end()) {
            switch (pit->second.strategy) {
                case ec2_placement_group_strategy::cluster:
                    placement_strategy_val = "cluster";
                    break;
                case ec2_placement_group_strategy::spread:
                    placement_strategy_val = "spread";
                    break;
                case ec2_placement_group_strategy::partition:
                    placement_strategy_val = "partition";
                    break;
                default:
                    break;
            }
        }

        Aws::EC2::Model::TagSpecification spec;
        spec.SetResourceType(Aws::EC2::Model::ResourceType::instance);
        spec.AddTags(make_ec2_tag("kythira:cluster", _cfg.cluster_name));
        spec.AddTags(make_ec2_tag("kythira:group", group));
        spec.AddTags(make_ec2_tag("kythira:managed-by", "ec2_quorum_manager"));
        spec.AddTags(make_ec2_tag("kythira:placement-strategy", placement_strategy_val));
        spec.AddTags(make_ec2_tag("kythira:market", market));
        if (nid) {
            spec.AddTags(make_ec2_tag("Name", name_tag(*nid)));
            spec.AddTags(make_ec2_tag("kythira:node-id", node_id_str(*nid)));
        }
        for (const auto& [k, v] : _cfg.extra_tags) {
            spec.AddTags(make_ec2_tag(k, v));
        }
        return spec;
    }

    /// The Name tag: the cluster and the node id, or the instance id alone when the
    /// node id is the instance (its canonical text holds `:`, and repeats the
    /// instance id the console already shows).
    [[nodiscard]] auto name_tag(const NodeId& nid) const -> std::string {
        if constexpr (instance_is_node_id) {
            return "kythira-" + _cfg.cluster_name + "-" +
                   instance_id_for_node(_cfg.aws.region, nid).value_or(node_id_str(nid));
        } else {
            return "kythira-" + _cfg.cluster_name + "-" + node_id_str(nid);
        }
    }

    /// Applies the two tags that cannot be set at launch when the node id is the
    /// instance id (composite and string mode).
    ///
    /// Best-effort, but no longer silently so: losing these leaves an instance
    /// the audit can still find by kythira:managed-by, so a failure here is a
    /// legibility problem rather than an invisible leak, and worth a line.
    void apply_identity_tags(const std::string& ec2_id, const NodeId& nid) {
        Aws::EC2::Model::CreateTagsRequest req;
        req.AddResources(ec2_id);
        req.AddTags(make_ec2_tag("Name", name_tag(nid)));
        req.AddTags(make_ec2_tag("kythira:node-id", node_id_str(nid)));
        auto outcome = _ec2->CreateTags(req);
        if (!outcome.IsSuccess()) {
            std::cerr << "[aws_ec2_quorum_manager] Name/kythira:node-id tags for " << ec2_id
                      << " failed: " << outcome.GetError().GetMessage()
                      << " (the instance still carries kythira:managed-by, so the leak audit "
                         "can find it)\n";
        }
    }

    /// Substitutes {NODE_ID} (when the id is known before launch), {NODE_PORT}, {CLUSTER}
    /// and {AZ} placeholders in user_data_template.
    [[nodiscard]] auto render_user_data(const std::optional<NodeId>& nid,
                                        const std::string& az) const -> std::string {
        std::string result = _cfg.user_data_template;
        auto replace_all = [&](const std::string& from, const std::string& to) {
            std::size_t pos = 0;
            while ((pos = result.find(from, pos)) != std::string::npos) {
                result.replace(pos, from.size(), to);
                pos += to.size();
            }
        };
        if (nid) {
            replace_all("{NODE_ID}", node_id_str(*nid));
        }
        replace_all("{NODE_PORT}", std::to_string(_cfg.node_port));
        replace_all("{CLUSTER}", _cfg.cluster_name);
        replace_all("{AZ}", az);
        return result;
    }

    /// Converts ec2_spot_interruption_behavior to the AWS SDK enum value.
    static auto to_aws_interruption_behavior(ec2_spot_interruption_behavior b)
        -> Aws::EC2::Model::InstanceInterruptionBehavior {
        switch (b) {
            case ec2_spot_interruption_behavior::stop:
                return Aws::EC2::Model::InstanceInterruptionBehavior::stop;
            case ec2_spot_interruption_behavior::hibernate:
                return Aws::EC2::Model::InstanceInterruptionBehavior::hibernate;
            default:
                return Aws::EC2::Model::InstanceInterruptionBehavior::terminate;
        }
    }
};

static_assert(quorum_manager<aws_ec2_quorum_manager<std::uint64_t, std::string>, std::uint64_t,
                             std::string, std::string>,
              "aws_ec2_quorum_manager must satisfy quorum_manager");
static_assert(quorum_manager<aws_ec2_quorum_manager<aws_ec2_node_id, std::string>, aws_ec2_node_id,
                             std::string, std::string>,
              "aws_ec2_quorum_manager<aws_ec2_node_id> must satisfy quorum_manager");

}  // namespace kythira

#endif  // KYTHIRA_HAS_AWS_SDK

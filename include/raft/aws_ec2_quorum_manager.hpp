#pragma once

#include <raft/aws_client_config.hpp>
#include <raft/fault_injection.hpp>
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
#include <aws/ec2/model/RunInstancesRequest.h>
#include <aws/ec2/model/SpotMarketOptions.h>
#include <aws/ec2/model/Tag.h>
#include <aws/ec2/model/TerminateInstancesRequest.h>
#include <aws/core/utils/Array.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/base64/Base64.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
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
/// Node identity is the numeric value of the EC2 instance ID hex suffix, making the
/// node-to-EC2 mapping a pure computation (no tag scans). Liveness is determined via
/// DescribeInstanceStatus (instance state == running), not DescribeInstances or heartbeats.
template<typename NodeId = std::uint64_t, typename Address = std::string>
requires node_id<NodeId>
class aws_ec2_quorum_manager {
public:
    using node_id_type = NodeId;
    using address_type = Address;
    using placement_group_id_type = std::string;

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
        _ec2 = make_ec2_client(_cfg.aws);
    }

    /// Returns the health of the current cluster by calling DescribeInstanceStatus.
    /// A node is live iff its instance state is "running" (SetIncludeAllInstances=true
    /// so transitioning instances are visible). Returns an exceptional Future on API error.
    auto assess_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::Future<quorum_health<NodeId, std::string>> {
        try {
            fiu_do_on("raft/aws/ec2/describe_instance_status",
                      throw std::runtime_error("fault: raft/aws/ec2/describe_instance_status"););

            if (cluster.empty()) {
                return build_health(cluster, {});
            }

            Aws::EC2::Model::DescribeInstanceStatusRequest req;
            for (const auto& np : cluster) {
                req.AddInstanceIds(node_id_to_ec2_id(np.node_id));
            }
            req.SetIncludeAllInstances(true);

            auto outcome = _ec2->DescribeInstanceStatus(req);
            if (!outcome.IsSuccess()) {
                throw std::runtime_error("ec2 DescribeInstanceStatus: " +
                                         std::string(outcome.GetError().GetMessage()));
            }

            std::map<std::string, bool> live_map;
            for (const auto& status : outcome.GetResult().GetInstanceStatuses()) {
                bool running = (status.GetInstanceState().GetName() ==
                                Aws::EC2::Model::InstanceStateName::running);
                auto nid = ec2_id_to_node_id(std::string(status.GetInstanceId()));
                live_map[node_id_str(nid)] = running;
            }

            return build_health(cluster, live_map);
        } catch (const std::exception& ex) {
            return FutureFactory::makeExceptionalFuture<quorum_health<NodeId, std::string>>(
                std::runtime_error(std::string("aws_ec2_quorum_manager::assess_quorum: ") +
                                   ex.what()));
        }
    }

    /// Assesses quorum, terminates unreachable nodes, and provisions replacements to meet topology.
    /// Decommission and provision errors are logged to stderr but do not abort the operation;
    /// the returned health reflects the pre-maintenance cluster state.
    auto maintain_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::Future<quorum_health<NodeId, std::string>> {
        try {
            fiu_do_on("raft/aws/ec2/maintain_quorum",
                      throw std::runtime_error("fault: raft/aws/ec2/maintain_quorum"););
        } catch (...) {
            return FutureFactory::makeExceptionalFuture<quorum_health<NodeId, std::string>>(
                std::current_exception());
        }

        quorum_health<NodeId, std::string> pre_health;
        try {
            pre_health = assess_quorum(cluster).get();
        } catch (...) {
            return FutureFactory::makeExceptionalFuture<quorum_health<NodeId, std::string>>(
                std::current_exception());
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
                decommission_node(nid).get();
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
                    provision_node(gt.group_id, hint).get();
                } catch (const std::exception& ex) {
                    std::cerr << "[aws_ec2_quorum_manager::maintain_quorum] provision in "
                              << gt.group_id << " failed: " << ex.what() << "\n";
                }
            }
        }

        return FutureFactory::makeFuture(std::move(pre_health));
    }

    /// Launches a new EC2 instance in the subnet mapped to target_group.
    /// The NodeId is derived from the instance ID returned by RunInstances; replacing is accepted
    /// by the interface but not used (the new instance gets a new ID regardless).
    /// Returns an exceptional Future if RunInstances fails or the provision_timeout is exceeded.
    auto provision_node(std::string target_group, std::optional<NodeId> /*replacing*/)
        -> kythira::Future<peer_info<NodeId, Address>> {
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
            if (!_cfg.user_data_template.empty()) {
                // render_user_data() needs the post-launch instance-derived
                // NodeId for {NODE_ID}, which doesn't exist yet at this
                // point - substitute only what's already known
                // ({NODE_PORT}, {CLUSTER}, {AZ}) and set it directly.
                // Templates using {NODE_ID} are a known limitation; none of
                // this project's current callers use it. Previously this
                // was entirely unset (the two no-op comment blocks further
                // down are what's left of that), so user_data_template
                // silently never reached any instance regardless of
                // whether it needed substitution at all.
                std::string rendered = _cfg.user_data_template;
                auto replace_all = [&](const std::string& from, const std::string& to) {
                    std::size_t pos = 0;
                    while ((pos = rendered.find(from, pos)) != std::string::npos) {
                        rendered.replace(pos, from.size(), to);
                        pos += to.size();
                    }
                };
                replace_all("{NODE_PORT}", std::to_string(_cfg.node_port));
                replace_all("{CLUSTER}", _cfg.cluster_name);
                replace_all("{AZ}", target_group);
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

            // Node identity is the numeric value of the EC2 instance ID.
            NodeId new_id = ec2_id_to_node_id(ec2_id);

            std::string market_tag = _cfg.spot_options ? "spot" : "on-demand";
            apply_tags(ec2_id, new_id, target_group, market_tag);

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
            return FutureFactory::makeFuture(peer_info<NodeId, Address>{new_id, addr});
        } catch (const std::exception& ex) {
            return FutureFactory::makeExceptionalFuture<peer_info<NodeId, Address>>(
                std::runtime_error(std::string("aws_ec2_quorum_manager::provision_node: ") +
                                   ex.what()));
        }
    }

    /// Terminates the EC2 instance identified by node_id and waits up to 30 s for the
    /// state to leave "running", so a subsequent assess_quorum call sees it as unreachable.
    /// Returns successfully if the instance was already gone (InvalidInstanceID.NotFound).
    auto decommission_node(const NodeId& node_id) -> kythira::Future<void> {
        try {
            fiu_do_on("raft/aws/ec2/terminate_instances",
                      throw std::runtime_error("fault: raft/aws/ec2/terminate_instances"););

            std::string ec2_id = node_id_to_ec2_id(node_id);
            Aws::EC2::Model::TerminateInstancesRequest req;
            req.AddInstanceIds(ec2_id);
            auto outcome = _ec2->TerminateInstances(req);
            if (!outcome.IsSuccess()) {
                const auto& err = outcome.GetError();
                // InvalidInstanceID.NotFound → instance is already gone; treat as success.
                if (std::string(err.GetExceptionName()).find("InvalidInstanceID") !=
                    std::string::npos) {
                    return FutureFactory::makeFuture();
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
            return FutureFactory::makeFuture();
        } catch (const std::exception& ex) {
            return FutureFactory::makeExceptionalFuture<void>(std::runtime_error(
                std::string("aws_ec2_quorum_manager::decommission_node: ") + ex.what()));
        }
    }

    /// Returns the desired topology from the configuration.
    [[nodiscard]] auto topology() const -> desired_topology<std::string> { return _cfg.topology; }

    /// Converts an EC2 instance ID ("i-0{16 hex digits}") to a NodeId.
    /// The numeric value of the 17-char hex suffix is used; fits in uint64_t for all modern IDs.
    static auto ec2_id_to_node_id(const std::string& ec2_id) -> NodeId {
        std::uint64_t v = std::stoull(ec2_id.substr(2), nullptr, 16);
        if constexpr (std::is_same_v<NodeId, std::string>) {
            return std::to_string(v);
        } else {
            return static_cast<NodeId>(v);
        }
    }

    /// Converts a NodeId back to its EC2 instance ID string ("i-0{16 hex digits}").
    static auto node_id_to_ec2_id(const NodeId& nid) -> std::string {
        std::uint64_t v{};
        if constexpr (std::is_same_v<NodeId, std::string>) {
            v = std::stoull(nid);
        } else {
            v = static_cast<std::uint64_t>(nid);
        }
        char buf[20];
        std::snprintf(buf, sizeof(buf), "i-%017llx", static_cast<unsigned long long>(v));
        return buf;
    }

private:
    aws_ec2_quorum_manager_config _cfg;
    std::shared_ptr<Aws::EC2::EC2Client> _ec2;

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

    /// Returns NodeId as a string suitable for use as an EC2 tag value or map key.
    static auto node_id_str(const NodeId& id) -> std::string {
        if constexpr (std::is_same_v<NodeId, std::string>) {
            return id;
        } else {
            return std::to_string(id);
        }
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

    /// Builds a quorum_health from cluster membership and a live/dead map keyed by node_id_str.
    [[nodiscard]] auto build_health(const std::vector<node_placement<NodeId, std::string>>& cluster,
                                    const std::map<std::string, bool>& live_map) const
        -> kythira::Future<quorum_health<NodeId, std::string>> {
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
        return FutureFactory::makeFuture(quorum_health<NodeId, std::string>{
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

    /// Applies standard kythira:* tags plus extra_tags to a newly provisioned instance.
    void apply_tags(const std::string& ec2_id, const NodeId& nid, const std::string& group,
                    const std::string& market) {
        auto make_tag = [](const std::string& k, const std::string& v) {
            Aws::EC2::Model::Tag t;
            t.SetKey(k);
            t.SetValue(v);
            return t;
        };

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

        Aws::EC2::Model::CreateTagsRequest req;
        req.AddResources(ec2_id);
        req.AddTags(make_tag("Name", "kythira-" + _cfg.cluster_name + "-" + node_id_str(nid)));
        req.AddTags(make_tag("kythira:cluster", _cfg.cluster_name));
        req.AddTags(make_tag("kythira:node-id", node_id_str(nid)));
        req.AddTags(make_tag("kythira:group", group));
        req.AddTags(make_tag("kythira:managed-by", "ec2_quorum_manager"));
        req.AddTags(make_tag("kythira:placement-strategy", placement_strategy_val));
        req.AddTags(make_tag("kythira:market", market));
        for (const auto& [k, v] : _cfg.extra_tags) {
            req.AddTags(make_tag(k, v));
        }
        _ec2->CreateTags(req);
    }

    /// Substitutes {NODE_ID}, {NODE_PORT}, {CLUSTER}, {AZ} placeholders in user_data_template.
    [[nodiscard]] auto render_user_data(const NodeId& nid, const std::string& az) const
        -> std::string {
        std::string result = _cfg.user_data_template;
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

}  // namespace kythira

#endif  // KYTHIRA_HAS_AWS_SDK

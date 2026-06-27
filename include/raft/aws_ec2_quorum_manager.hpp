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
#include <aws/core/utils/HashingUtils.h>

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

enum class ec2_spot_interruption_behavior : std::uint8_t {
    terminate,
    stop,
    hibernate,
};

struct ec2_spot_options {
    std::string max_price;
    ec2_spot_interruption_behavior interruption_behavior{ec2_spot_interruption_behavior::terminate};
};

enum class ec2_placement_group_strategy : std::uint8_t {
    none,
    cluster,
    spread,
    partition,
};

struct ec2_placement_group_config {
    std::string name;
    ec2_placement_group_strategy strategy{ec2_placement_group_strategy::none};
    std::uint32_t partition_number{0};
};

// ============================================================================
// aws_ec2_quorum_manager_config
// ============================================================================

struct aws_ec2_quorum_manager_config {
    std::string cluster_name;
    std::string image_id;
    std::string instance_type{"t3.micro"};
    std::uint16_t node_port{7000};
    desired_topology<std::string> topology;
    std::map<std::string, std::string> subnet_by_group;
    std::vector<std::string> security_group_ids;
    std::string iam_instance_profile;
    std::string user_data_template;
    std::map<std::string, std::string> extra_tags;
    std::map<std::string, ec2_placement_group_config> placement_by_group;
    std::optional<ec2_spot_options> spot_options;
    std::chrono::seconds provision_timeout{120};
    std::chrono::seconds poll_interval{5};
    aws_client_config aws;
};

// ============================================================================
// aws_ec2_quorum_manager
// ============================================================================

template<typename NodeId = std::uint64_t, typename Address = std::string>
requires node_id<NodeId>
class aws_ec2_quorum_manager {
public:
    using node_id_type = NodeId;
    using address_type = Address;
    using placement_group_id_type = std::string;

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

    // ── assess_quorum ─────────────────────────────────────────────────────────
    // Liveness is determined via DescribeInstanceStatus, not DescribeInstances.
    // A node is live when EC2 reports its instance state as "running".
    // The node_id encodes the EC2 instance ID directly (see ec2_id_to_node_id /
    // node_id_to_ec2_id), so no tag lookup is required.

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

    // ── maintain_quorum ───────────────────────────────────────────────────────

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

    // ── provision_node ────────────────────────────────────────────────────────
    // The NodeId is derived directly from the EC2 instance ID returned by
    // RunInstances — no separate ID counter or tag scan is needed.

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

            if (!_cfg.user_data_template.empty()) {
                // user_data was set before RunInstances; rendered via separate pass here
                // for tag-based placeholders (node_id is now known).
            }

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

            if (!_cfg.user_data_template.empty()) {
                // UserData was already encoded and passed at RunInstances time;
                // placeholder rendering must happen before RunInstances. Since
                // new_id is now known, callers that need {NODE_ID} substitution
                // should use render_user_data and set UserData before this call.
                // This path is intentionally a no-op here.
            }

            Address addr = static_cast<Address>(private_ip + ":" + std::to_string(_cfg.node_port));
            return FutureFactory::makeFuture(peer_info<NodeId, Address>{new_id, addr});
        } catch (const std::exception& ex) {
            return FutureFactory::makeExceptionalFuture<peer_info<NodeId, Address>>(
                std::runtime_error(std::string("aws_ec2_quorum_manager::provision_node: ") +
                                   ex.what()));
        }
    }

    // ── decommission_node ─────────────────────────────────────────────────────
    // The EC2 instance ID is derived directly from node_id — no DescribeInstances
    // lookup is required. TerminateInstances is idempotent for already-terminated
    // instances (AWS returns the terminal state without error).

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

    // ── topology ─────────────────────────────────────────────────────────────

    [[nodiscard]] auto topology() const -> desired_topology<std::string> { return _cfg.topology; }

    // ── ID conversion utilities ───────────────────────────────────────────────
    // EC2 instance IDs have the format "i-0{16 hex digits}" (post-2016).
    // The numeric value of the 17-char hex suffix fits in uint64_t when the
    // leading digit is 0 (always true for modern IDs).

    static auto ec2_id_to_node_id(const std::string& ec2_id) -> NodeId {
        std::uint64_t v = std::stoull(ec2_id.substr(2), nullptr, 16);
        if constexpr (std::is_same_v<NodeId, std::string>) {
            return std::to_string(v);
        } else {
            return static_cast<NodeId>(v);
        }
    }

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

    static auto node_id_str(const NodeId& id) -> std::string {
        if constexpr (std::is_same_v<NodeId, std::string>) {
            return id;
        } else {
            return std::to_string(id);
        }
    }

    static auto find_tag(const Aws::Vector<Aws::EC2::Model::Tag>& tags, const std::string& key)
        -> std::optional<std::string> {
        for (const auto& tag : tags) {
            if (std::string(tag.GetKey()) == key) {
                return std::string(tag.GetValue());
            }
        }
        return std::nullopt;
    }

    auto build_health(const std::vector<node_placement<NodeId, std::string>>& cluster,
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

    auto render_user_data(const NodeId& nid, const std::string& az) const -> std::string {
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

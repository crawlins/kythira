// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file aws_ec2_peer_discovery.hpp
/// @brief A `peer_discovery` back-end that reads the cloud's own inventory.
///
/// `.kiro/specs/multi-machine-placement/` Requirement 3 and task 8: bring N
/// hosts up without every one of them knowing every address in advance.
///
/// **This is a small implementation of a concept the tree already has, not a
/// new subsystem.** `peer_discovery` is `register_node` plus `find_peers`, and
/// `aws_ec2_quorum_manager` already models instance discovery by tag — it
/// derives node identity from the EC2 instance id and reads instance state
/// back out of `DescribeInstances`. What is added here is the mapping in the
/// other direction: an instance publishes its node id and Raft address AS
/// TAGS ON ITSELF, and every peer finds it with one tag-filtered describe.
///
/// ## Why the cloud's inventory rather than one of the three DNS back-ends
///
/// The tree holds `rfc1035_peer_discovery`, `rfc6763_peer_discovery`,
/// `rfc6763_ldns_peer_discovery`, `rfc2136_dns_sd_discovery` and
/// `poco_peer_discovery`, and the design considered each:
///
/// - The DNS-SD ones need a server that accepts dynamic updates, which on AWS
///   means standing up BIND or driving Route 53 — a second moving part inside
///   a measurement, and one whose own latency lands in the window.
/// - mDNS needs a flat L2 segment. A VPC subnet is not one; multicast does not
///   cross it. `poco_peer_discovery` would return an empty set and look like a
///   cluster that had not started yet.
///
/// The instance metadata is already there, already authoritative, and already
/// carries the run tag every other part of Shape 2 keys off.
///
/// ## THE STATIC LIST REMAINS THE DEFAULT FOR A MEASURED ROW
///
/// Requirement 3.4, and it is not a hedge. `find_peers` is a control-plane API
/// call: DescribeInstances is rate-limited, takes tens to hundreds of
/// milliseconds, and is served by a system this project is not measuring. A
/// row whose numbers include one of these per tick is measuring EC2. Discovery
/// exists to prove the cluster can FORM without a hand-edited file; the static
/// list is what a row is taken with. Both are exercised, only one is measured
/// through, and the row records which it got.

#include <raft/aws_client_config.hpp>
#include <raft/future_default.hpp>
#include <raft/peer_discovery.hpp>

#ifdef KYTHIRA_HAS_AWS_SDK

#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/internal/AWSHttpResourceClient.h>
#include <aws/ec2/EC2Client.h>
#include <aws/ec2/model/CreateTagsRequest.h>
#include <aws/ec2/model/DescribeInstancesRequest.h>
#include <aws/ec2/model/Filter.h>
#include <aws/ec2/model/Tag.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace kythira {

/// @brief Configuration for @ref aws_ec2_peer_discovery.
struct aws_ec2_peer_discovery_config {
    /// Region, endpoint override and credentials, shared with every other AWS
    /// back-end in the tree.
    aws_client_config aws;

    /// The run-scoped tag that scopes the search. This is the SAME contract
    /// the leak audit keys off: a resource created without it is invisible to
    /// the audit, and an instance created without it is invisible here. One
    /// tag, one run, one meaning.
    std::string run_tag_key{"kythira-perf-run"};
    std::string run_tag_value;

    /// Where a node publishes its identity and its Raft address. Tags rather
    /// than a side channel because the tag is written at the same moment the
    /// instance becomes findable, and because `DescribeInstances` returns them
    /// in the response that already had to be made.
    std::string node_id_tag_key{"kythira-node-id"};
    std::string address_tag_key{"kythira-node-address"};

    /// Optional role filter. Shape 2 launches N hosts and one driver under one
    /// run tag, and the driver is not a peer. Left empty, every tagged
    /// instance is returned; set to "host", only the hosts are.
    std::string role_tag_key{"kythira-role"};
    std::string role_tag_prefix{};

    /// This instance's own id. Left empty, it is read from IMDS, which is the
    /// only way a process can learn it without being told. Injectable because
    /// a test has no IMDS and because a controller driving this from outside
    /// the instance does know the id.
    std::string instance_id{};

    /// How often @ref aws_ec2_peer_discovery::await_peers re-scans while converging.
    std::chrono::milliseconds poll_interval{2000};
};

/// @brief Peer discovery by EC2 tag scan.
///
/// @tparam NodeId  Must be constructible from the node-id tag's value.
/// @tparam Address Network address type; `std::string` in every current use.
template<typename NodeId = std::uint64_t, typename Address = std::string>
class aws_ec2_peer_discovery {
public:
    using node_id_type = NodeId;
    using address_type = Address;

    explicit aws_ec2_peer_discovery(aws_ec2_peer_discovery_config cfg) : _cfg(std::move(cfg)) {
        if (_cfg.run_tag_value.empty()) {
            // Refused rather than defaulted. An empty tag value would filter
            // on `tag:kythira-perf-run=""`, match nothing, and present as a
            // cluster whose peers have not booted yet — a silent, patient,
            // wrong answer. The failures this project keeps finding are all of
            // that shape.
            throw std::invalid_argument(
                "aws_ec2_peer_discovery: run_tag_value must be non-empty; an empty "
                "run tag matches no instances and is indistinguishable from a "
                "cluster that has not started");
        }
        _ec2 = make_ec2_client(_cfg.aws);
    }

    /// @brief Publish this node's id and address as tags on its own instance.
    ///
    /// After this returns, every peer running `find_peers` can see this node.
    /// The instance is already tagged with the run tag by whatever provisioned
    /// it; what this adds is the identity, which only the process knows.
    auto register_node(NodeId self_id, Address self_address) -> kythira::future_default<void> {
        try {
            const auto id = resolve_instance_id();
            Aws::EC2::Model::CreateTagsRequest req;
            req.AddResources(id);
            Aws::EC2::Model::Tag node_tag;
            node_tag.SetKey(_cfg.node_id_tag_key);
            node_tag.SetValue(to_string(self_id));
            Aws::EC2::Model::Tag addr_tag;
            addr_tag.SetKey(_cfg.address_tag_key);
            addr_tag.SetValue(std::string(self_address));
            req.AddTags(node_tag);
            req.AddTags(addr_tag);

            auto outcome = _ec2->CreateTags(req);
            if (!outcome.IsSuccess()) {
                return future_factory_default::makeExceptionalFuture<void>(
                    std::make_exception_ptr(std::runtime_error(
                        "aws_ec2_peer_discovery::register_node: CreateTags on " + id +
                        " failed: " + std::string(outcome.GetError().GetMessage()))));
            }
            return future_factory_default::makeFuture();
        } catch (const std::exception& ex) {
            return future_factory_default::makeExceptionalFuture<void>(
                std::make_exception_ptr(std::runtime_error(
                    std::string("aws_ec2_peer_discovery::register_node: ") + ex.what())));
        }
    }

    /// @brief One tag-filtered `DescribeInstances`, mapped to peers.
    ///
    /// `timeout` bounds the call rather than a convergence loop: a snapshot is
    /// what the concept asks for, and waiting for a quorum to appear is
    /// @ref await_peers's job. An instance that is tagged but not yet running,
    /// or running but not yet registered, is simply absent from the snapshot —
    /// which is the truth at that moment.
    [[nodiscard]] auto find_peers(std::chrono::milliseconds timeout)
        -> kythira::future_default<std::vector<peer_info<NodeId, Address>>> {
        try {
            return future_factory_default::makeFuture(scan(timeout));
        } catch (const std::exception& ex) {
            return future_factory_default::makeExceptionalFuture<
                std::vector<peer_info<NodeId, Address>>>(std::make_exception_ptr(std::runtime_error(
                std::string("aws_ec2_peer_discovery::find_peers: ") + ex.what())));
        }
    }

    /// @brief Poll until `expected` distinct peers are visible, or fail saying
    ///        which were never seen.
    ///
    /// **Requirement 3.5.** The alternative — carry on with whatever was found
    /// — measures a cluster with a replica missing and reports a number for
    /// it, and nothing downstream can tell that from a healthy row. So this
    /// throws, and the message names the gap rather than stating a count: "saw
    /// 2 of 3" sends a reader to the logs, while "never saw node 3" is already
    /// the diagnosis.
    ///
    /// @param expected  How many peers must be visible, this node included.
    /// @param budget    Total time allowed to converge.
    /// @param required  Optional exact node ids expected. When given, the
    ///                  failure names the missing ones instead of the count.
    [[nodiscard]] auto await_peers(std::size_t expected, std::chrono::milliseconds budget,
                                   const std::vector<NodeId>& required = {})
        -> std::vector<peer_info<NodeId, Address>> {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        std::vector<peer_info<NodeId, Address>> seen;
        for (;;) {
            seen = scan(budget);
            if (seen.size() >= expected) {
                if (required.empty() || missing_from(seen, required).empty()) {
                    return seen;
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                break;
            }
            std::this_thread::sleep_for(_cfg.poll_interval);
        }

        std::ostringstream msg;
        msg << "aws_ec2_peer_discovery::await_peers: discovery did not converge within "
            << budget.count() << " ms. Expected " << expected << " peer(s) tagged "
            << _cfg.run_tag_key << '=' << _cfg.run_tag_value << ", saw " << seen.size() << ": ";
        for (std::size_t i = 0; i < seen.size(); ++i) {
            msg << (i ? ", " : "") << to_string(seen[i].node_id) << '@' << seen[i].address;
        }
        const auto missing = missing_from(seen, required);
        if (!missing.empty()) {
            msg << ". NEVER SEEN: ";
            for (std::size_t i = 0; i < missing.size(); ++i) {
                msg << (i ? ", " : "") << to_string(missing[i]);
            }
        }
        msg << ". Refusing to measure a cluster with a replica missing.";
        throw std::runtime_error(msg.str());
    }

private:
    aws_ec2_peer_discovery_config _cfg;
    std::shared_ptr<Aws::EC2::EC2Client> _ec2;

    /// The tag-filtered describe, shared by `find_peers` and `await_peers`.
    [[nodiscard]] auto scan(std::chrono::milliseconds /*timeout*/)
        -> std::vector<peer_info<NodeId, Address>> {
        Aws::EC2::Model::DescribeInstancesRequest req;

        Aws::EC2::Model::Filter run_filter;
        run_filter.SetName("tag:" + _cfg.run_tag_key);
        run_filter.AddValues(_cfg.run_tag_value);
        req.AddFilters(run_filter);

        // Only instances that are actually up can be peers. Filtering here
        // rather than in the loop keeps a terminated instance from lingering
        // in a peer set until something else notices it is gone -- the same
        // reason the leak audit excludes `terminated` and `shutting-down`.
        Aws::EC2::Model::Filter state_filter;
        state_filter.SetName("instance-state-name");
        state_filter.AddValues("running");
        req.AddFilters(state_filter);

        // Only instances that have registered. An instance carrying the run
        // tag but no node-id tag is one that has booted and not yet called
        // register_node, which is a normal transient state and not a peer.
        Aws::EC2::Model::Filter registered_filter;
        registered_filter.SetName("tag-key");
        registered_filter.AddValues(_cfg.node_id_tag_key);
        req.AddFilters(registered_filter);

        if (!_cfg.role_tag_prefix.empty()) {
            Aws::EC2::Model::Filter role_filter;
            role_filter.SetName("tag:" + _cfg.role_tag_key);
            // A wildcard, because Shape 2 tags its hosts host-1, host-2, ...
            // and its driver `driver`; the driver is not a peer.
            role_filter.AddValues(_cfg.role_tag_prefix + "*");
            req.AddFilters(role_filter);
        }

        std::vector<peer_info<NodeId, Address>> peers;
        std::set<std::string> seen_ids;
        std::string token;
        do {
            if (!token.empty()) {
                req.SetNextToken(token);
            }
            auto outcome = _ec2->DescribeInstances(req);
            if (!outcome.IsSuccess()) {
                throw std::runtime_error("DescribeInstances failed: " +
                                         std::string(outcome.GetError().GetMessage()));
            }
            // Paginated on purpose. The default page is 1000 instances, which
            // no cluster this project runs will reach -- but a truncated peer
            // set is a split brain, and "it will never be that big" is how a
            // discovery back-end silently drops a replica.
            for (const auto& reservation : outcome.GetResult().GetReservations()) {
                for (const auto& instance : reservation.GetInstances()) {
                    std::string node_id_value;
                    std::string address_value;
                    for (const auto& tag : instance.GetTags()) {
                        if (tag.GetKey() == _cfg.node_id_tag_key) {
                            node_id_value = tag.GetValue();
                        } else if (tag.GetKey() == _cfg.address_tag_key) {
                            address_value = tag.GetValue();
                        }
                    }
                    if (node_id_value.empty()) {
                        continue;
                    }
                    if (address_value.empty()) {
                        // Registered its id but not its address: half-written
                        // tags, which CreateTags makes unlikely but does not
                        // forbid. Skipped rather than returned with an empty
                        // address, which would be a peer nothing can reach.
                        continue;
                    }
                    // An instance id may appear twice across pages during a
                    // concurrent change; a node id appearing twice would make
                    // a cluster look larger than it is.
                    if (!seen_ids.insert(node_id_value).second) {
                        continue;
                    }
                    peers.push_back(peer_info<NodeId, Address>{from_string(node_id_value),
                                                               Address(address_value)});
                }
            }
            token = outcome.GetResult().GetNextToken();
        } while (!token.empty());

        std::ranges::sort(peers, {}, &peer_info<NodeId, Address>::node_id);
        return peers;
    }

    [[nodiscard]] static auto missing_from(const std::vector<peer_info<NodeId, Address>>& seen,
                                           const std::vector<NodeId>& required)
        -> std::vector<NodeId> {
        std::vector<NodeId> missing;
        for (const auto& want : required) {
            const auto it = std::ranges::find(seen, want, &peer_info<NodeId, Address>::node_id);
            if (it == seen.end()) {
                missing.push_back(want);
            }
        }
        return missing;
    }

    /// This instance's id, from configuration or from IMDS.
    [[nodiscard]] auto resolve_instance_id() -> std::string {
        if (!_cfg.instance_id.empty()) {
            return _cfg.instance_id;
        }
        // The SDK's own metadata client rather than a hand-rolled request to
        // 169.254.169.254: Shape 2 launches with HttpTokens=required, so IMDSv1
        // is refused and a naive GET returns 401. This client does the
        // PUT-token dance.
        Aws::Internal::EC2MetadataClient imds;
        auto id = imds.GetResource("/latest/meta-data/instance-id");
        if (id.empty()) {
            throw std::runtime_error(
                "could not read this instance's id from IMDS, and none was configured. "
                "Off an EC2 instance, set aws_ec2_peer_discovery_config::instance_id");
        }
        return std::string(id.c_str());
    }

    [[nodiscard]] static auto to_string(const NodeId& id) -> std::string {
        if constexpr (std::is_same_v<NodeId, std::string>) {
            return id;
        } else {
            return std::to_string(id);
        }
    }

    [[nodiscard]] static auto from_string(const std::string& s) -> NodeId {
        if constexpr (std::is_same_v<NodeId, std::string>) {
            return s;
        } else {
            return static_cast<NodeId>(std::stoull(s));
        }
    }

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
};

static_assert(
    peer_discovery<aws_ec2_peer_discovery<std::uint64_t, std::string>, std::uint64_t, std::string>,
    "aws_ec2_peer_discovery must satisfy the peer_discovery concept — it is an "
    "implementation of an existing concept, not a new subsystem");

}  // namespace kythira

#endif  // KYTHIRA_HAS_AWS_SDK

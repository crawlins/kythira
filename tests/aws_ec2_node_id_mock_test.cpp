// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Requirement 13.1 of the cloud-composite-node-ids spec: both AWS quorum
// managers, in composite (aws_ec2_node_id), string and numeric mode, driven
// with the instance ids that broke the old stoull derivation:
//
//   i-f0123456789abcdef   17 hex digits, non-zero first digit (overflowed)
//   i-1234abcd            legacy 8-digit id (round-tripped to the wrong id)
//   i-0123456789abcdef0   the shape real AWS has issued so far
//
// The EC2 and Auto Scaling query APIs are served by a small stateful fake on
// loopback (endpoint_override), so provision, assess and decommission run the
// managers' real request and response paths without a cloud account.

#define BOOST_TEST_MODULE aws_ec2_node_id_mock_test
#include <boost/test/unit_test.hpp>

#ifdef KYTHIRA_HAS_AWS_SDK

#include <raft/aws_asg_quorum_manager.hpp>
#include <raft/aws_ec2_quorum_manager.hpp>
#include <raft/composite_node_id.hpp>

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/utils/base64/Base64.h>
#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#include <memory>
#endif

using namespace std::chrono_literals;

namespace {

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = boost::unit_test::framework::master_test_suite().argc;
        char** argv = boost::unit_test::framework::master_test_suite().argv;
        _init = std::make_unique<folly::Init>(&argc, &argv, false);
    }
    std::unique_ptr<folly::Init> _init;
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

struct AwsSdkFixture {
    AwsSdkFixture() {
        // Never probe the link-local metadata service from a test.
        ::setenv("AWS_EC2_METADATA_DISABLED", "true", 1);
        Aws::InitAPI(opts);
    }
    ~AwsSdkFixture() { Aws::ShutdownAPI(opts); }
    Aws::SDKOptions opts;
};
BOOST_GLOBAL_FIXTURE(AwsSdkFixture);

struct static_credentials_chain : Aws::Auth::AWSCredentialsProviderChain {
    static_credentials_chain() {
        AddProvider(Aws::MakeShared<Aws::Auth::SimpleAWSCredentialsProvider>(
            "aws_ec2_node_id_mock_test", "AKIDEXAMPLE",
            "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY"));
    }
};

constexpr const char* region = "us-east-1";
const std::vector<std::string> awkward_ids{"i-f0123456789abcdef", "i-1234abcd",
                                           "i-0123456789abcdef0"};

// ── the fake ────────────────────────────────────────────────────────────────

struct fake_instance {
    std::string id{};
    std::string state{"running"};
    std::string private_ip{};
    std::map<std::string, std::string> tags{};
    std::string user_data{};  // decoded
    std::string asg{};        // owning group, if launched by one
    bool protected_from_scale_in{false};
};

/// Query-protocol params: `Name=value&...` from the form body.
auto parse_form(const std::string& body) -> std::map<std::string, std::string> {
    std::map<std::string, std::string> out;
    httplib::Params params;
    httplib::detail::parse_query_text(body, params);
    for (const auto& [k, v] : params) {
        out[k] = v;
    }
    return out;
}

/// Values of `<prefix>.1`, `<prefix>.2`, ... until one is missing.
auto list_param(const std::map<std::string, std::string>& p, const std::string& prefix)
    -> std::vector<std::string> {
    std::vector<std::string> out;
    for (int i = 1;; ++i) {
        auto it = p.find(prefix + "." + std::to_string(i));
        if (it == p.end()) {
            return out;
        }
        out.push_back(it->second);
    }
}

auto xml_escape(const std::string& s) -> std::string {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            default:
                out += c;
        }
    }
    return out;
}

/// A stateful EC2 + Auto Scaling stand-in. Instance ids are handed out from
/// `next_ids` in order. DescribeInstances returns at most `page_size`
/// instances per page so the managers' pagination is exercised.
struct fake_aws {
    httplib::Server server;
    std::thread thread;
    int port{0};

    std::mutex mu;
    std::deque<std::string> next_ids;
    std::map<std::string, fake_instance> instances;  // ordered: stable pages
    std::map<std::string, int> asg_desired;          // group name -> desired capacity
    std::vector<std::map<std::string, std::string>> run_requests;
    std::vector<std::string> actions;
    std::size_t page_size{2};
    int ip_counter{10};

    fake_aws() {
        server.Post("/.*", [this](const httplib::Request& req, httplib::Response& res) {
            auto p = parse_form(req.body);
            std::lock_guard lock(mu);
            actions.push_back(p["Action"]);
            handle(p, res);
        });
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
        for (int i = 0; i < 400 && !server.is_running(); ++i) {
            std::this_thread::sleep_for(5ms);
        }
    }
    ~fake_aws() {
        server.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }
    fake_aws(const fake_aws&) = delete;
    auto operator=(const fake_aws&) -> fake_aws& = delete;

    [[nodiscard]] auto origin() const -> std::string {
        return "http://127.0.0.1:" + std::to_string(port);
    }

    auto add_instance(fake_instance inst) -> void {
        std::lock_guard lock(mu);
        if (inst.private_ip.empty()) {
            inst.private_ip = "10.0.0." + std::to_string(ip_counter++);
        }
        instances[inst.id] = std::move(inst);
    }

    auto get(const std::string& id) -> fake_instance {
        std::lock_guard lock(mu);
        return instances.at(id);
    }

    auto set_state(const std::string& id, const std::string& state) -> void {
        std::lock_guard lock(mu);
        instances.at(id).state = state;
    }

    auto count(const std::string& action) -> std::size_t {
        std::lock_guard lock(mu);
        return static_cast<std::size_t>(std::ranges::count(actions, action));
    }

private:
    static auto state_code(const std::string& s) -> int {
        if (s == "pending") return 0;
        if (s == "running") return 16;
        if (s == "shutting-down") return 32;
        if (s == "terminated") return 48;
        if (s == "stopping") return 64;
        return 80;
    }

    auto launch(const std::string& asg) -> fake_instance& {
        BOOST_REQUIRE_MESSAGE(!next_ids.empty(), "fake_aws: no instance id left to hand out");
        fake_instance inst;
        inst.id = next_ids.front();
        next_ids.pop_front();
        inst.private_ip = "10.0.0." + std::to_string(ip_counter++);
        inst.asg = asg;
        return instances[inst.id] = std::move(inst);
    }

    auto matches(const fake_instance& inst, const std::map<std::string, std::string>& p) const
        -> bool {
        auto ids = list_param(p, "InstanceId");
        if (!ids.empty() && std::ranges::find(ids, inst.id) == ids.end()) {
            return false;
        }
        for (int f = 1;; ++f) {
            auto name_it = p.find("Filter." + std::to_string(f) + ".Name");
            if (name_it == p.end()) {
                return true;
            }
            auto values = list_param(p, "Filter." + std::to_string(f) + ".Value");
            const auto& name = name_it->second;
            std::optional<std::string> actual;
            if (name == "instance-id") {
                actual = inst.id;
            } else if (name.starts_with("tag:")) {
                if (auto t = inst.tags.find(name.substr(4)); t != inst.tags.end()) {
                    actual = t->second;
                }
            } else {
                BOOST_ERROR("fake_aws: unsupported filter " << name);
                return false;
            }
            if (!actual || std::ranges::find(values, *actual) == values.end()) {
                return false;
            }
        }
    }

    static auto instance_xml(const fake_instance& inst) -> std::string {
        std::string x = "<item><instanceId>" + inst.id + "</instanceId><instanceState><code>" +
                        std::to_string(state_code(inst.state)) + "</code><name>" + inst.state +
                        "</name></instanceState><privateIpAddress>" + inst.private_ip +
                        "</privateIpAddress><tagSet>";
        for (const auto& [k, v] : inst.tags) {
            x +=
                "<item><key>" + xml_escape(k) + "</key><value>" + xml_escape(v) + "</value></item>";
        }
        return x + "</tagSet></item>";
    }

    static auto ec2_reply(httplib::Response& res, const std::string& action,
                          const std::string& inner) -> void {
        res.set_content("<?xml version=\"1.0\" encoding=\"UTF-8\"?><" + action +
                            "Response xmlns=\"http://ec2.amazonaws.com/doc/2016-11-15/\">"
                            "<requestId>r</requestId>" +
                            inner + "</" + action + "Response>",
                        "text/xml");
    }

    static auto asg_reply(httplib::Response& res, const std::string& action,
                          const std::string& inner) -> void {
        res.set_content(
            "<" + action + "Response xmlns=\"http://autoscaling.amazonaws.com/doc/2011-01-01/\"><" +
                action + "Result>" + inner + "</" + action +
                "Result>"
                "<ResponseMetadata><RequestId>r</RequestId></ResponseMetadata></" +
                action + "Response>",
            "text/xml");
    }

    auto asg_xml(const std::string& name) -> std::string {
        std::string x = "<member><AutoScalingGroupName>" + name +
                        "</AutoScalingGroupName><DesiredCapacity>" +
                        std::to_string(asg_desired[name]) +
                        "</DesiredCapacity><MinSize>0</MinSize><MaxSize>10</MaxSize>"
                        "<HealthCheckType>EC2</HealthCheckType><Instances>";
        for (const auto& [id, inst] : instances) {
            if (inst.asg == name && inst.state != "terminated") {
                x += "<member><InstanceId>" + id +
                     "</InstanceId><AvailabilityZone>us-east-1a</AvailabilityZone>"
                     "<LifecycleState>InService</LifecycleState><HealthStatus>Healthy"
                     "</HealthStatus><ProtectedFromScaleIn>" +
                     std::string(inst.protected_from_scale_in ? "true" : "false") +
                     "</ProtectedFromScaleIn></member>";
            }
        }
        return x + "</Instances></member>";
    }

    auto handle(std::map<std::string, std::string>& p, httplib::Response& res) -> void {
        const auto& action = p["Action"];
        if (action == "RunInstances") {
            run_requests.push_back(p);
            auto& inst = launch("");
            for (int t = 1;; ++t) {
                auto k = p.find("TagSpecification.1.Tag." + std::to_string(t) + ".Key");
                if (k == p.end()) {
                    break;
                }
                inst.tags[k->second] = p["TagSpecification.1.Tag." + std::to_string(t) + ".Value"];
            }
            if (auto ud = p.find("UserData"); ud != p.end()) {
                auto bytes = Aws::Utils::Base64::Base64().Decode(ud->second);
                inst.user_data.assign(reinterpret_cast<const char*>(bytes.GetUnderlyingData()),
                                      bytes.GetLength());
            }
            ec2_reply(res, action,
                      "<reservationId>r-1</reservationId><instancesSet>" + instance_xml(inst) +
                          "</instancesSet>");
        } else if (action == "DescribeInstances") {
            std::vector<const fake_instance*> hits;
            for (const auto& [id, inst] : instances) {
                if (matches(inst, p)) {
                    hits.push_back(&inst);
                }
            }
            std::size_t start = p.contains("NextToken") ? std::stoul(p["NextToken"]) : 0;
            std::size_t end = std::min(hits.size(), start + page_size);
            std::string body = "<reservationSet>";
            for (std::size_t i = start; i < end; ++i) {
                body += "<item><reservationId>r-" + std::to_string(i) +
                        "</reservationId><instancesSet>" + instance_xml(*hits[i]) +
                        "</instancesSet></item>";
            }
            body += "</reservationSet>";
            if (end < hits.size()) {
                body += "<nextToken>" + std::to_string(end) + "</nextToken>";
            }
            ec2_reply(res, action, body);
        } else if (action == "DescribeInstanceStatus") {
            std::string body = "<instanceStatusSet>";
            for (const auto& id : list_param(p, "InstanceId")) {
                if (auto it = instances.find(id); it != instances.end()) {
                    body += "<item><instanceId>" + id + "</instanceId><instanceState><code>" +
                            std::to_string(state_code(it->second.state)) + "</code><name>" +
                            it->second.state + "</name></instanceState></item>";
                }
            }
            ec2_reply(res, action, body + "</instanceStatusSet>");
        } else if (action == "CreateTags") {
            for (const auto& id : list_param(p, "ResourceId")) {
                for (int t = 1;; ++t) {
                    auto k = p.find("Tag." + std::to_string(t) + ".Key");
                    if (k == p.end()) {
                        break;
                    }
                    instances.at(id).tags[k->second] = p["Tag." + std::to_string(t) + ".Value"];
                }
            }
            ec2_reply(res, action, "<return>true</return>");
        } else if (action == "TerminateInstances") {
            std::string body = "<instancesSet>";
            for (const auto& id : list_param(p, "InstanceId")) {
                auto it = instances.find(id);
                if (it == instances.end()) {
                    res.status = 400;
                    res.set_content(
                        "<Response><Errors><Error><Code>InvalidInstanceID.NotFound</Code><Message>"
                        "The instance ID '" +
                            id +
                            "' does not exist</Message></Error></Errors>"
                            "<RequestID>r</RequestID></Response>",
                        "text/xml");
                    return;
                }
                it->second.state = "terminated";
                body += "<item><instanceId>" + id +
                        "</instanceId><currentState><code>48</code><name>terminated</name>"
                        "</currentState></item>";
            }
            ec2_reply(res, action, body + "</instancesSet>");
        } else if (action == "DescribeAutoScalingGroups") {
            std::string body = "<AutoScalingGroups>";
            for (const auto& name : list_param(p, "AutoScalingGroupNames.member")) {
                body += asg_xml(name);
            }
            asg_reply(res, action, body + "</AutoScalingGroups>");
        } else if (action == "UpdateAutoScalingGroup") {
            const auto& name = p["AutoScalingGroupName"];
            int want = std::stoi(p["DesiredCapacity"]);
            while (asg_desired[name] < want) {
                launch(name);
                ++asg_desired[name];
            }
            asg_desired[name] = want;
            asg_reply(res, action, "");
        } else if (action == "SetInstanceProtection") {
            // The ASG manager protects every instance it adopts, so a voter is
            // never the group's own scale-in victim.
            for (const auto& id : list_param(p, "InstanceIds.member")) {
                instances.at(id).protected_from_scale_in = p["ProtectedFromScaleIn"] == "true";
            }
            asg_reply(res, action, "");
        } else if (action == "TerminateInstanceInAutoScalingGroup") {
            auto& inst = instances.at(p["InstanceId"]);
            inst.state = "terminated";
            if (p["ShouldDecrementDesiredCapacity"] == "true") {
                --asg_desired[inst.asg];
            }
            asg_reply(res, action, "<Activity><ActivityId>a</ActivityId></Activity>");
        } else {
            BOOST_ERROR("fake_aws: unexpected action " << action);
            res.status = 400;
        }
    }
};

auto aws_config(const fake_aws& fake) -> kythira::aws_client_config {
    kythira::aws_client_config cfg;
    cfg.region = region;
    cfg.endpoint_override = fake.origin();
    cfg.api_timeout = 5s;
    cfg.credentials_provider = std::make_shared<static_credentials_chain>();
    return cfg;
}

auto ec2_config(const fake_aws& fake) -> kythira::aws_ec2_quorum_manager_config {
    kythira::aws_ec2_quorum_manager_config cfg;
    cfg.cluster_name = "ids";
    cfg.image_id = "ami-12345678";
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
    cfg.subnet_by_group["AZ1"] = "subnet-1";
    cfg.user_data_template = "node={NODE_ID} cluster={CLUSTER}";
    cfg.provision_timeout = 5s;
    cfg.poll_interval = 0s;
    cfg.aws = aws_config(fake);
    return cfg;
}

auto asg_config(const fake_aws& fake) -> kythira::aws_asg_quorum_manager_config {
    kythira::aws_asg_quorum_manager_config cfg;
    cfg.cluster_name = "ids";
    cfg.node_port = 7000;
    cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
    cfg.asg_by_group["AZ1"] = "asg-1";
    cfg.provision_timeout = 5s;
    cfg.poll_interval = 0s;
    cfg.aws = aws_config(fake);
    return cfg;
}

template<typename NodeId> auto expected_textual_id(const std::string& ec2_id) -> NodeId {
    kythira::aws_ec2_node_id id{region, ec2_id};
    if constexpr (std::same_as<NodeId, std::string>) {
        return id.to_string();
    } else {
        return id;
    }
}

template<typename NodeId>
auto placements(const std::vector<NodeId>& ids)
    -> std::vector<kythira::node_placement<NodeId, std::string>> {
    std::vector<kythira::node_placement<NodeId, std::string>> out;
    for (const auto& id : ids) {
        out.push_back({.node_id = id, .group_id = "AZ1"});
    }
    return out;
}

/// Provision three nodes (handed the awkward ids), assess them, lose one,
/// decommission one, for either manager in any id mode.
template<typename Manager, typename Config> void run_lifecycle(fake_aws& fake, const Config& cfg) {
    using NodeId = typename Manager::node_id_type;
    for (const auto& id : awkward_ids) {
        fake.next_ids.push_back(id);
    }
    Manager mgr{cfg};

    std::vector<NodeId> ids;
    for (std::size_t i = 0; i < awkward_ids.size(); ++i) {
        auto peer = std::move(mgr.provision_node("AZ1", std::nullopt)).get();
        ids.push_back(peer.node_id);
        const auto& ec2_id = awkward_ids[i];
        BOOST_CHECK_EQUAL(peer.address, fake.get(ec2_id).private_ip + ":7000");
        BOOST_CHECK_EQUAL(mgr.instance_id_of(peer.node_id).value_or("<none>"), ec2_id);
        BOOST_CHECK(mgr.node_id_of_instance(ec2_id) == std::optional<NodeId>{peer.node_id});

        auto tags = fake.get(ec2_id).tags;
        BOOST_CHECK_EQUAL(tags["kythira:node-id"],
                          kythira::node_id_traits<NodeId>::to_text(peer.node_id));
        BOOST_CHECK_EQUAL(tags["kythira:cluster"], "ids");
        if constexpr (kythira::node_id_traits<NodeId>::is_textual) {
            BOOST_CHECK(peer.node_id == expected_textual_id<NodeId>(ec2_id));
            BOOST_CHECK_EQUAL(tags["Name"], "kythira-ids-" + ec2_id);
        } else {
            BOOST_CHECK_EQUAL(peer.node_id, static_cast<NodeId>(i + 1));
            BOOST_CHECK_EQUAL(tags["Name"], "kythira-ids-" + std::to_string(i + 1));
        }
    }

    auto health = std::move(mgr.assess_quorum(placements(ids))).get();
    BOOST_CHECK(health.status == kythira::quorum_status::healthy);
    BOOST_CHECK_EQUAL(health.live_node_count, 3u);

    // An instance that stops is unreachable; the others stay live.
    fake.set_state(awkward_ids[0], "stopped");
    health = std::move(mgr.assess_quorum(placements(ids))).get();
    BOOST_CHECK(health.status == kythira::quorum_status::critical);
    BOOST_REQUIRE_EQUAL(health.unreachable_nodes.size(), 1u);
    BOOST_CHECK(health.unreachable_nodes[0] == ids[0]);

    // Decommission reaches the right instance, and only it.
    std::move(mgr.decommission_node(ids[1])).get();
    BOOST_CHECK_EQUAL(fake.get(awkward_ids[1]).state, "terminated");
    BOOST_CHECK_EQUAL(fake.get(awkward_ids[2]).state, "running");
    health = std::move(mgr.assess_quorum(placements(ids))).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 1u);
    BOOST_CHECK(health.status == kythira::quorum_status::lost);
}

}  // namespace

// ── EC2 manager ──────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(ec2_manager)

BOOST_AUTO_TEST_CASE(composite_mode_lifecycle) {
    fake_aws fake;
    run_lifecycle<kythira::aws_ec2_quorum_manager<kythira::aws_ec2_node_id>>(fake,
                                                                             ec2_config(fake));
    // The id is unknown before launch, so {NODE_ID} is left for the node.
    BOOST_CHECK_EQUAL(fake.get(awkward_ids[2]).user_data, "node={NODE_ID} cluster=ids");
}

BOOST_AUTO_TEST_CASE(string_mode_lifecycle) {
    fake_aws fake;
    run_lifecycle<kythira::aws_ec2_quorum_manager<std::string>>(fake, ec2_config(fake));
}

BOOST_AUTO_TEST_CASE(numeric_mode_lifecycle) {
    fake_aws fake;
    run_lifecycle<kythira::aws_ec2_quorum_manager<std::uint64_t>>(fake, ec2_config(fake));
    // Numeric mode knows the id before launch: tags and {NODE_ID} ride in the
    // launch request, and no follow-up CreateTags is needed.
    BOOST_CHECK_EQUAL(fake.get(awkward_ids[2]).user_data, "node=3 cluster=ids");
    BOOST_CHECK_EQUAL(fake.count("CreateTags"), 0u);
}

BOOST_AUTO_TEST_CASE(composite_mode_reports_foreign_ids_unreachable) {
    fake_aws fake;
    fake.add_instance({.id = "i-0123456789abcdef0"});
    kythira::aws_ec2_quorum_manager<kythira::aws_ec2_node_id> mgr{ec2_config(fake)};
    std::vector<kythira::aws_ec2_node_id> ids{{"us-east-1", "i-0123456789abcdef0"},
                                              {"eu-west-1", "i-0123456789abcdef0"}};
    auto health = std::move(mgr.assess_quorum(placements(ids))).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 1u);
    BOOST_REQUIRE_EQUAL(health.unreachable_nodes.size(), 1u);
    BOOST_CHECK(health.unreachable_nodes[0] == ids[1]);
    BOOST_CHECK_THROW(std::move(mgr.decommission_node(ids[1])).get(), std::exception);
    BOOST_CHECK_EQUAL(fake.get("i-0123456789abcdef0").state, "running");
}

BOOST_AUTO_TEST_CASE(string_mode_rejects_non_canonical_text) {
    fake_aws fake;
    fake.add_instance({.id = "i-0123456789abcdef0"});
    kythira::aws_ec2_quorum_manager<std::string> mgr{ec2_config(fake)};
    std::vector<std::string> ids{"aws-ec2:us-east-1:i-0123456789abcdef0", "i-0123456789abcdef0",
                                 "81985529216486895"};
    auto health = std::move(mgr.assess_quorum(placements(ids))).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 1u);
    BOOST_CHECK_EQUAL(health.unreachable_nodes.size(), 2u);
}

BOOST_AUTO_TEST_CASE(textual_modes_require_a_region) {
    fake_aws fake;
    auto cfg = ec2_config(fake);
    cfg.aws.region.clear();
    BOOST_CHECK_THROW(kythira::aws_ec2_quorum_manager<kythira::aws_ec2_node_id>{cfg},
                      std::invalid_argument);
    BOOST_CHECK_NO_THROW(kythira::aws_ec2_quorum_manager<std::uint64_t>{cfg});
}

// Numeric mode, R11.4: an instance launched by the old derivation carries the
// decimal value of its hex id in kythira:node-id and is found by that tag; if
// the best-effort tag was lost, the derivation itself is the fallback.
BOOST_AUTO_TEST_CASE(numeric_mode_finds_pre_tag_instances) {
    fake_aws fake;
    // Tagged by the old apply_identity_tags (0x1234abcd = 305441741).
    fake.add_instance({.id = "i-1234abcd",
                       .tags = {{"kythira:cluster", "ids"}, {"kythira:node-id", "305441741"}}});
    // Old derivation, tag lost: i-0000000000000002a is node 42.
    fake.add_instance({.id = "i-0000000000000002a", .tags = {{"kythira:cluster", "ids"}}});
    // Same shape but someone else's cluster: never adopted.
    fake.add_instance({.id = "i-0000000000000002b", .tags = {{"kythira:cluster", "other"}}});
    kythira::aws_ec2_quorum_manager<std::uint64_t> mgr{ec2_config(fake)};

    std::vector<std::uint64_t> ids{305441741, 42, 43, 7};
    auto health = std::move(mgr.assess_quorum(placements(ids))).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 2u);
    BOOST_CHECK_EQUAL(health.unreachable_nodes.size(), 2u);
    BOOST_CHECK_EQUAL(mgr.instance_id_of(42).value_or(""), "i-0000000000000002a");
    BOOST_CHECK(!mgr.instance_id_of(43));
    BOOST_CHECK_EQUAL(mgr.node_id_of_instance("i-0000000000000002a").value_or(0), 42u);
    BOOST_CHECK(!mgr.node_id_of_instance("i-0000000000000002b"));

    // A node with no instance at all is already gone.
    BOOST_CHECK_NO_THROW(std::move(mgr.decommission_node(7)).get());
    std::move(mgr.decommission_node(42)).get();
    BOOST_CHECK_EQUAL(fake.get("i-0000000000000002a").state, "terminated");
    BOOST_CHECK_EQUAL(fake.get("i-0000000000000002b").state, "running");
}

// Allocation is one above the highest valid tag across every state; junk
// tags are skipped, and pages beyond the first are read.
BOOST_AUTO_TEST_CASE(numeric_mode_allocates_above_every_tag) {
    fake_aws fake;
    fake.add_instance({.id = "i-00000000000000001",
                       .state = "terminated",
                       .tags = {{"kythira:cluster", "ids"}, {"kythira:node-id", "9"}}});
    fake.add_instance({.id = "i-00000000000000002",
                       .tags = {{"kythira:cluster", "ids"}, {"kythira:node-id", "-1"}}});
    fake.add_instance({.id = "i-00000000000000003",
                       .tags = {{"kythira:cluster", "ids"}, {"kythira:node-id", "4x"}}});
    fake.add_instance({.id = "i-00000000000000004",
                       .tags = {{"kythira:cluster", "other"}, {"kythira:node-id", "500"}}});
    fake.add_instance({.id = "i-00000000000000005",
                       .tags = {{"kythira:cluster", "ids"}, {"kythira:node-id", "3"}}});
    fake.next_ids.push_back("i-f0123456789abcdef");
    kythira::aws_ec2_quorum_manager<std::uint64_t> mgr{ec2_config(fake)};
    auto peer = std::move(mgr.provision_node("AZ1", std::nullopt)).get();
    BOOST_CHECK_EQUAL(peer.node_id, 10u);
}

// EC2 stops listing a terminated instance after about an hour; an id the
// caller's membership still holds must not be handed out again even then.
BOOST_AUTO_TEST_CASE(numeric_mode_never_reuses_an_assessed_id) {
    fake_aws fake;
    fake.add_instance({.id = "i-00000000000000001",
                       .tags = {{"kythira:cluster", "ids"}, {"kythira:node-id", "4"}}});
    fake.next_ids.push_back("i-f0123456789abcdef");
    fake.next_ids.push_back("i-0123456789abcdef0");
    kythira::aws_ec2_quorum_manager<std::uint64_t> mgr{ec2_config(fake)};
    // Node 50's instance is long gone from EC2, but it is still a member.
    auto health = std::move(mgr.assess_quorum(placements<std::uint64_t>({4, 50}))).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 1u);
    BOOST_CHECK_EQUAL(std::move(mgr.provision_node("AZ1", std::nullopt)).get().node_id, 51u);
    BOOST_CHECK_EQUAL(std::move(mgr.provision_node("AZ1", std::nullopt)).get().node_id, 52u);
}

BOOST_AUTO_TEST_CASE(numeric_mode_refuses_to_wrap_a_narrow_id) {
    fake_aws fake;
    fake.add_instance({.id = "i-00000000000000001",
                       .tags = {{"kythira:cluster", "ids"}, {"kythira:node-id", "255"}}});
    fake.next_ids.push_back("i-f0123456789abcdef");
    kythira::aws_ec2_quorum_manager<std::uint8_t> mgr{ec2_config(fake)};
    BOOST_CHECK_THROW(std::move(mgr.provision_node("AZ1", std::nullopt)).get(), std::exception);
    BOOST_CHECK_EQUAL(fake.count("RunInstances"), 0u);
}

BOOST_AUTO_TEST_SUITE_END()

// ── ASG manager ──────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(asg_manager)

BOOST_AUTO_TEST_CASE(composite_mode_lifecycle) {
    fake_aws fake;
    run_lifecycle<kythira::aws_asg_quorum_manager<kythira::aws_ec2_node_id>>(fake,
                                                                             asg_config(fake));
}

BOOST_AUTO_TEST_CASE(string_mode_lifecycle) {
    fake_aws fake;
    run_lifecycle<kythira::aws_asg_quorum_manager<std::string>>(fake, asg_config(fake));
}

BOOST_AUTO_TEST_CASE(numeric_mode_lifecycle) {
    fake_aws fake;
    run_lifecycle<kythira::aws_asg_quorum_manager<std::uint64_t>>(fake, asg_config(fake));
}

BOOST_AUTO_TEST_SUITE_END()

#else

BOOST_AUTO_TEST_CASE(aws_sdk_not_available) {
    BOOST_TEST_MESSAGE("AWS SDK not built; skipped");
}

#endif  // KYTHIRA_HAS_AWS_SDK

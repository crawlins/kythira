/// @file alibaba_quorum_manager_unit_test.cpp
/// @brief `alibaba_ess_quorum_manager`'s construction contract, its tag-scan
///        NodeId assignment, its cluster isolation, decommission idempotency
///        and its fault points (`.kiro/specs/alibaba-cloud-services/`,
///        Requirement 16.4).
///
/// **There is no injection seam, by design.** The manager owns an
/// `alibaba_http_client` by value, so every case here points
/// `cfg.alibaba.endpoint_override` at a local `httplib::Server` — the idiom
/// `oci_quorum_manager_unit_test.cpp` and `alibaba_http_client_unit_test.cpp`
/// already use. That the tests can reach the manager at all is itself the
/// proof that the override replaces every derived service host at once, ESS
/// and ECS together.
///
/// The mock below deliberately models **additive** `TagResources`: it merges
/// the keys it is given into the instance's existing tag map rather than
/// replacing it. A mock that modelled replacement would make
/// `tag_writes_are_additive_and_preserve_operator_tags` pass for the wrong
/// reason and would quietly invite the read-merge-write cycle the header
/// argues against (design.md Property 4's neighbourhood; the OCI provider
/// needs the opposite mock for the opposite API).
///
/// Full provision/decommission/maintain *flows* over many operations belong to
/// the signature-verifying mock tier (Requirement 16.6-16.7, task 6); this
/// file stays on the manager's own rules.

#define BOOST_TEST_MODULE alibaba_quorum_manager_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/alibaba_ess_quorum_manager.hpp>

#include "test_timeout_scale.hpp"

#include <httplib.h>
#include <boost/json.hpp>

#ifdef FIU_ENABLE
#include <fiu-control.h>
#endif

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using kythira::alibaba_ess_quorum_manager;
using kythira::alibaba_ess_quorum_manager_config;
using namespace std::chrono_literals;

namespace {

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = boost::unit_test::framework::master_test_suite().argc;
        char** argv = boost::unit_test::framework::master_test_suite().argv;
        folly::init(&argc, &argv, false);
    }
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

#ifdef FIU_ENABLE
struct FiuInitFixture {
    FiuInitFixture() { fiu_init(0); }
};
BOOST_GLOBAL_FIXTURE(FiuInitFixture);
#endif

constexpr const char* default_zone = "cn-hangzhou-b";

/// An in-memory ESS + ECS control plane on an ephemeral port.
///
/// One per case rather than one shared server: the manager's constructor makes
/// a real `DescribeScalingGroups` call, so a case that wants a different
/// starting state wants a different server, and `bind_to_any_port` makes that
/// free of the fixed-port `TIME_WAIT` races that fixed ports invite.
///
/// Signatures are **not** verified here — that is Requirement 16.6's job in
/// `tests/alibaba_mock_server.hpp`, and duplicating it would put the signing
/// scheme's golden vectors in three places.
struct EssMock {
    struct Instance {
        std::string id;
        std::string lifecycle{"InService"};  ///< ESS lifecycle state.
        std::string status{"Running"};       ///< ECS instance status.
        std::string zone{default_zone};
        std::string ip;
        std::map<std::string, std::string> tags;
    };

    httplib::Server server;
    std::thread thread;
    int port{0};

    mutable std::mutex mu;
    std::string group_id{"asg-kythira-test"};
    bool group_exists{true};
    std::int64_t desired_capacity{0};
    std::vector<Instance> instances;
    int next_seq{1};

    /// Zone the group places newly launched instances into — set differently
    /// from a caller's `target_group` to exercise Requirement 7.3.
    std::string launch_zone{default_zone};
    /// ECS status newly launched instances report. `Starting` models an
    /// instance ESS has admitted but that never comes up.
    std::string launch_status{"Running"};
    /// Tags every launched instance already carries before the manager adopts
    /// it, so an additive `TagResources` is distinguishable from a replacing
    /// one.
    std::map<std::string, std::string> launch_tags{{"owner", "platform-team"}};

    std::atomic<int> modify_calls{0};
    std::atomic<int> remove_calls{0};
    std::atomic<int> tag_calls{0};

    EssMock() { install_routes(); }

    EssMock(const EssMock&) = delete;
    auto operator=(const EssMock&) -> EssMock& = delete;
    EssMock(EssMock&&) = delete;
    auto operator=(EssMock&&) -> EssMock& = delete;

    ~EssMock() { shutdown(); }

    void start() {
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
        for (int i = 0; i < 400 && !server.is_running(); ++i) {
            std::this_thread::sleep_for(5ms);
        }
    }

    void shutdown() {
        server.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }

    [[nodiscard]] auto origin() const -> std::string {
        return "http://127.0.0.1:" + std::to_string(port);
    }

    /// Seed one instance. Returns its instance ID.
    auto add_instance(std::map<std::string, std::string> tags, std::string lifecycle = "InService",
                      std::string status = "Running", std::string zone = default_zone)
        -> std::string {
        const std::lock_guard lock(mu);
        auto id = make_instance(std::move(tags), std::move(lifecycle), std::move(status),
                                std::move(zone));
        desired_capacity = static_cast<std::int64_t>(instances.size());
        return id;
    }

    /// Tags currently on an instance, as the ECS side sees them.
    [[nodiscard]] auto tags_of(const std::string& id) const -> std::map<std::string, std::string> {
        const std::lock_guard lock(mu);
        for (const auto& inst : instances) {
            if (inst.id == id) {
                return inst.tags;
            }
        }
        return {};
    }

    [[nodiscard]] auto instance_count() const -> std::size_t {
        const std::lock_guard lock(mu);
        return instances.size();
    }

    [[nodiscard]] auto capacity() const -> std::int64_t {
        const std::lock_guard lock(mu);
        return desired_capacity;
    }

private:
    /// Caller holds `mu`.
    auto make_instance(std::map<std::string, std::string> tags, std::string lifecycle,
                       std::string status, std::string zone) -> std::string {
        const int seq = next_seq++;
        Instance inst;
        inst.id = "i-mock" + std::to_string(seq);
        inst.lifecycle = std::move(lifecycle);
        inst.status = std::move(status);
        inst.zone = std::move(zone);
        inst.ip = "192.168.0." + std::to_string(seq);
        inst.tags = std::move(tags);
        instances.push_back(inst);
        return inst.id;
    }

    static void fail(httplib::Response& res, int status, const std::string& code,
                     const std::string& message) {
        boost::json::object body;
        body["Code"] = code;
        body["Message"] = message;
        body["RequestId"] = "REQ-MOCK";
        res.status = status;
        res.set_content(boost::json::serialize(body), "application/json");
    }

    void install_routes() {
        server.Post("/", [this](const httplib::Request& req, httplib::Response& res) {
            const auto action = req.get_header_value("x-acs-action");
            const std::lock_guard lock(mu);
            boost::json::object out;
            out["RequestId"] = "REQ-MOCK";

            if (action == "DescribeScalingGroups") {
                boost::json::array groups;
                if (group_exists && req.get_param_value("ScalingGroupId.1") == group_id) {
                    boost::json::object group;
                    group["ScalingGroupId"] = group_id;
                    group["DesiredCapacity"] = desired_capacity;
                    group["TotalCapacity"] = static_cast<std::int64_t>(instances.size());
                    group["MinSize"] = 0;
                    group["MaxSize"] = 10;
                    groups.push_back(group);
                }
                boost::json::object wrapper;
                wrapper["ScalingGroup"] = groups;
                out["TotalCount"] = static_cast<std::int64_t>(groups.size());
                out["ScalingGroups"] = wrapper;
            } else if (action == "DescribeScalingInstances") {
                const auto page = to_int(req.get_param_value("PageNumber"), 1);
                const auto size = to_int(req.get_param_value("PageSize"), 50);
                boost::json::array listed;
                for (std::size_t i = static_cast<std::size_t>((page - 1) * size);
                     i < instances.size() && listed.size() < static_cast<std::size_t>(size); ++i) {
                    boost::json::object entry;
                    entry["InstanceId"] = instances[i].id;
                    entry["LifecycleState"] = instances[i].lifecycle;
                    entry["ScalingGroupId"] = group_id;
                    listed.push_back(entry);
                }
                boost::json::object wrapper;
                wrapper["ScalingInstance"] = listed;
                out["TotalCount"] = static_cast<std::int64_t>(instances.size());
                out["ScalingInstances"] = wrapper;
            } else if (action == "ModifyScalingGroup") {
                ++modify_calls;
                const auto wanted = to_int(req.get_param_value("DesiredCapacity"), -1);
                if (wanted < 0) {
                    fail(res, 400, "InvalidParameter", "DesiredCapacity is required");
                    return;
                }
                desired_capacity = wanted;
                // ESS launches to close the gap, which is what makes
                // provision_node's poll terminate.
                while (static_cast<std::int64_t>(instances.size()) < desired_capacity) {
                    (void)make_instance(launch_tags, "InService", launch_status, launch_zone);
                }
            } else if (action == "RemoveInstances") {
                ++remove_calls;
                const auto target = req.get_param_value("InstanceId.1");
                const auto before = instances.size();
                std::erase_if(instances, [&](const Instance& i) { return i.id == target; });
                if (instances.size() == before) {
                    fail(res, 404, "InvalidInstanceId.NotFound",
                         "instance " + target + " is not in scaling group " + group_id);
                    return;
                }
                if (req.get_param_value("DecreaseDesiredCapacity") != "false") {
                    desired_capacity = std::max<std::int64_t>(0, desired_capacity - 1);
                }
            } else if (action == "DescribeInstances") {
                boost::json::array described;
                for (const auto& id : requested_ids(req)) {
                    for (const auto& inst : instances) {
                        if (inst.id != id) {
                            continue;
                        }
                        described.push_back(ecs_view(inst));
                    }
                }
                boost::json::object wrapper;
                wrapper["Instance"] = described;
                out["TotalCount"] = static_cast<std::int64_t>(described.size());
                out["Instances"] = wrapper;
            } else if (action == "TagResources") {
                ++tag_calls;
                const auto target = req.get_param_value("ResourceId.1");
                Instance* found = nullptr;
                for (auto& inst : instances) {
                    if (inst.id == target) {
                        found = &inst;
                        break;
                    }
                }
                if (found == nullptr) {
                    fail(res, 404, "InvalidResourceId.NotFound", "no such instance: " + target);
                    return;
                }
                // Additive per key — the ECS contract, and the reason no
                // read-merge-write exists in the manager.
                for (int n = 1; req.has_param("Tag." + std::to_string(n) + ".Key"); ++n) {
                    found->tags.insert_or_assign(
                        req.get_param_value("Tag." + std::to_string(n) + ".Key"),
                        req.get_param_value("Tag." + std::to_string(n) + ".Value"));
                }
            } else {
                fail(res, 400, "InvalidAction.NotFound", "unmocked action: " + action);
                return;
            }

            res.set_content(boost::json::serialize(out), "application/json");
        });
    }

    [[nodiscard]] static auto ecs_view(const Instance& inst) -> boost::json::object {
        boost::json::object entry;
        entry["InstanceId"] = inst.id;
        entry["Status"] = inst.status;
        entry["ZoneId"] = inst.zone;

        boost::json::array ips;
        ips.push_back(boost::json::string(inst.ip));
        boost::json::object ip_wrapper;
        ip_wrapper["IpAddress"] = ips;
        boost::json::object vpc;
        vpc["PrivateIpAddress"] = ip_wrapper;
        entry["VpcAttributes"] = vpc;

        boost::json::array tags;
        for (const auto& [key, value] : inst.tags) {
            boost::json::object tag;
            tag["TagKey"] = key;
            tag["TagValue"] = value;
            tags.push_back(tag);
        }
        boost::json::object tag_wrapper;
        tag_wrapper["Tag"] = tags;
        entry["Tags"] = tag_wrapper;
        return entry;
    }

    /// ECS takes its ID list as a JSON array inside one query parameter.
    [[nodiscard]] static auto requested_ids(const httplib::Request& req)
        -> std::vector<std::string> {
        std::vector<std::string> ids;
        const auto raw = req.get_param_value("InstanceIds");
        if (raw.empty()) {
            return ids;
        }
        boost::json::value parsed;
        try {
            parsed = boost::json::parse(raw);
        } catch (const std::exception&) {
            return ids;
        }
        const auto* arr = parsed.if_array();
        if (arr == nullptr) {
            return ids;
        }
        for (const auto& entry : *arr) {
            if (entry.is_string()) {
                ids.emplace_back(entry.get_string());
            }
        }
        return ids;
    }

    [[nodiscard]] static auto to_int(const std::string& raw, int fallback) -> int {
        try {
            return raw.empty() ? fallback : std::stoi(raw);
        } catch (const std::exception&) {
            return fallback;
        }
    }
};

auto config_for(const EssMock& mock) -> alibaba_ess_quorum_manager_config {
    alibaba_ess_quorum_manager_config cfg;
    cfg.alibaba.region = "cn-hangzhou";
    cfg.alibaba.access_key_id = "test-ak";
    cfg.alibaba.access_key_secret = "test-secret";
    cfg.alibaba.endpoint_override = mock.origin();
    cfg.alibaba.api_timeout = 5s;
    cfg.cluster_name = "test-cluster";
    cfg.scaling_group_id = mock.group_id;
    cfg.node_port = 7000;
    cfg.poll_interval = 50ms;
    cfg.provision_timeout = 10s;
    cfg.topology.groups.push_back({.group_id = default_zone, .target_count = 3});
    return cfg;
}

/// A config that can never reach the network. Every case expecting a throw from
/// the constructor's *validation* uses it, so the test proves the ordering —
/// fields first, `DescribeScalingGroups` second — rather than assuming it.
auto unreachable_config() -> alibaba_ess_quorum_manager_config {
    alibaba_ess_quorum_manager_config cfg;
    cfg.alibaba.region = "cn-hangzhou";
    cfg.alibaba.access_key_id = "test-ak";
    cfg.alibaba.access_key_secret = "test-secret";
    cfg.alibaba.endpoint_override = "http://127.0.0.1:1";
    cfg.alibaba.api_timeout = 2s;
    cfg.cluster_name = "test-cluster";
    cfg.scaling_group_id = "asg-kythira-test";
    cfg.node_port = 7000;
    return cfg;
}

auto ours(std::uint64_t id) -> std::map<std::string, std::string> {
    return {{"kythira-cluster", "test-cluster"}, {"kythira-node-id", std::to_string(id)}};
}

}  // namespace

BOOST_AUTO_TEST_SUITE(alibaba_quorum_manager_construction)

/// Requirement 4.2. The header's `static_assert` already fails the build if
/// this regresses; the runtime case exists so the intent shows up in the test
/// output rather than only in a compile error nobody reads.
BOOST_AUTO_TEST_CASE(the_manager_satisfies_the_quorum_manager_concept) {
    static_assert(kythira::quorum_manager<alibaba_ess_quorum_manager<>, std::uint64_t, std::string,
                                          std::string>);
    static_assert(
        std::is_same_v<alibaba_ess_quorum_manager<>::placement_group_id_type, std::string>);
    BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(a_valid_config_constructs_against_an_existing_scaling_group,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    EssMock mock;
    mock.start();
    const alibaba_ess_quorum_manager<> mgr{config_for(mock)};
    BOOST_CHECK_EQUAL(mgr.topology().groups.size(), 1U);
    BOOST_CHECK_EQUAL(mgr.topology().groups.front().group_id, default_zone);
}

/// Requirement 3.2. Each field separately rather than as one "incomplete
/// config" case, because the failure this guards against is a deployment with
/// exactly one blank line in its env file.
BOOST_AUTO_TEST_CASE(an_empty_cluster_name_throws) {
    auto cfg = unreachable_config();
    cfg.cluster_name.clear();
    BOOST_CHECK_THROW((alibaba_ess_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(an_empty_scaling_group_id_throws) {
    auto cfg = unreachable_config();
    cfg.scaling_group_id.clear();
    BOOST_CHECK_THROW((alibaba_ess_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(a_zero_node_port_throws) {
    auto cfg = unreachable_config();
    cfg.node_port = 0;
    BOOST_CHECK_THROW((alibaba_ess_quorum_manager<>{cfg}), std::invalid_argument);
}

/// Requirement 3.2's parenthesis: `region` is required only when there is no
/// `endpoint_override` to replace the derived hosts. Both halves are asserted
/// because the interesting claim is the condition, not the `empty()` call.
BOOST_AUTO_TEST_CASE(an_empty_region_throws_only_without_an_endpoint_override,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    auto cfg = unreachable_config();
    cfg.alibaba.region.clear();
    cfg.alibaba.endpoint_override.clear();
    BOOST_CHECK_THROW((alibaba_ess_quorum_manager<>{cfg}), std::invalid_argument);

    EssMock mock;
    mock.start();
    auto override_cfg = config_for(mock);
    override_cfg.alibaba.region.clear();
    BOOST_CHECK_NO_THROW((alibaba_ess_quorum_manager<>{override_cfg}));
}

/// Requirement 3.2's fail-fast probe. `std::runtime_error` rather than
/// `std::invalid_argument` distinguishes "your config is malformed" from
/// "Alibaba said no", which is the distinction an operator reads first.
BOOST_AUTO_TEST_CASE(an_unknown_scaling_group_throws_at_construction,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    EssMock mock;
    mock.start();
    auto cfg = config_for(mock);
    cfg.scaling_group_id = "asg-does-not-exist";
    BOOST_CHECK_THROW((alibaba_ess_quorum_manager<>{cfg}), std::runtime_error);
}

/// A dead port stands in for every transport failure. The construction probe
/// must surface it rather than yielding a manager that fails later, at the
/// first remediation.
BOOST_AUTO_TEST_CASE(an_unreachable_endpoint_throws_at_construction,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    BOOST_CHECK_THROW((alibaba_ess_quorum_manager<>{unreachable_config()}), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(alibaba_quorum_manager_node_ids)

/// Requirement 5.3, including both boundaries: an empty scan yields 1 (not 0 —
/// a NodeId of zero would collide with the "unset" value half this tree uses),
/// and a gap in the assigned range does not lower the next id.
BOOST_AUTO_TEST_CASE(node_ids_are_tag_scan_max_plus_one,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    EssMock mock;
    mock.start();
    const alibaba_ess_quorum_manager<> empty_group{config_for(mock)};
    BOOST_CHECK_EQUAL(empty_group.next_node_id(), 1U);

    mock.add_instance(ours(3));
    mock.add_instance(ours(7));
    const alibaba_ess_quorum_manager<> mgr{config_for(mock)};
    BOOST_CHECK_EQUAL(mgr.next_node_id(), 8U);
}

/// An adopted-but-unparseable tag cannot collide with anything this manager
/// assigns, because this manager only ever writes decimal ids — so it is
/// ignored rather than treated as a scan failure.
BOOST_AUTO_TEST_CASE(an_unparseable_node_id_tag_is_ignored,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    EssMock mock;
    mock.add_instance({{"kythira-cluster", "test-cluster"}, {"kythira-node-id", "not-a-number"}});
    mock.add_instance(ours(2));
    mock.start();

    const alibaba_ess_quorum_manager<> mgr{config_for(mock)};
    BOOST_CHECK_EQUAL(mgr.next_node_id(), 3U);
}

/// design.md Property 4, on the assignment path: a co-tenant cluster's high
/// NodeId must not push this cluster's numbering, because NodeIds only need to
/// be unique within a cluster and the isolation rule is the same one every
/// other read here applies.
BOOST_AUTO_TEST_CASE(a_foreign_clusters_node_id_does_not_move_our_numbering,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    EssMock mock;
    mock.add_instance(ours(1));
    mock.add_instance({{"kythira-cluster", "someone-elses-cluster"}, {"kythira-node-id", "99"}});
    mock.add_instance({{"owner", "platform-team"}});  // never adopted at all
    mock.start();

    const alibaba_ess_quorum_manager<> mgr{config_for(mock)};
    BOOST_CHECK_EQUAL(mgr.next_node_id(), 2U);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(alibaba_quorum_manager_assessment)

/// Requirement 6.2: live is ESS `InService` **and** ECS `Running`. Each of the
/// two failing halves is represented, because a manager checking only one of
/// them passes a test that checks only the other.
BOOST_AUTO_TEST_CASE(liveness_needs_both_inservice_and_running,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    EssMock mock;
    mock.add_instance(ours(1));
    mock.add_instance(ours(2), "InService", "Stopped");
    mock.add_instance(ours(3), "Pending", "Running");
    mock.start();

    alibaba_ess_quorum_manager<> mgr{config_for(mock)};
    const std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster{
        {.node_id = 1, .group_id = default_zone},
        {.node_id = 2, .group_id = default_zone},
        {.node_id = 3, .group_id = default_zone},
    };

    const auto health = std::move(mgr.assess_quorum(cluster)).get();
    BOOST_CHECK_EQUAL(health.total_node_count, 3U);
    BOOST_CHECK_EQUAL(health.live_node_count, 1U);
    BOOST_REQUIRE_EQUAL(health.unreachable_nodes.size(), 2U);
    BOOST_CHECK_EQUAL(health.status, kythira::quorum_status::lost);
    BOOST_REQUIRE_EQUAL(health.groups.size(), 1U);
    BOOST_CHECK_EQUAL(health.groups.front().live_count, 1U);
    BOOST_CHECK_EQUAL(health.groups.front().target_count, 3U);
}

/// Requirement 6.4 / design.md Property 4. Node 2's tag value exists in the
/// group — on an instance belonging to another cluster — so a manager that
/// filtered after matching NodeIds instead of before would report it live.
BOOST_AUTO_TEST_CASE(foreign_cluster_instances_are_never_counted_as_members,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    EssMock mock;
    mock.add_instance(ours(1));
    mock.add_instance({{"kythira-cluster", "someone-elses-cluster"}, {"kythira-node-id", "2"}});
    mock.start();

    alibaba_ess_quorum_manager<> mgr{config_for(mock)};
    const std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster{
        {.node_id = 1, .group_id = default_zone},
        {.node_id = 2, .group_id = default_zone},
    };

    const auto health = std::move(mgr.assess_quorum(cluster)).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 1U);
    BOOST_REQUIRE_EQUAL(health.unreachable_nodes.size(), 1U);
    BOOST_CHECK_EQUAL(health.unreachable_nodes.front(), 2U);
}

/// Requirement 6.1's pagination and ECS batching, together: 120 members cross
/// both the 50-per-page `DescribeScalingInstances` boundary and the
/// 100-ID `DescribeInstances` batch. A manager that read only the first page
/// would report the last node unreachable.
BOOST_AUTO_TEST_CASE(listing_paginates_and_ecs_lookups_batch,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    EssMock mock;
    for (std::uint64_t i = 1; i <= 120; ++i) {
        mock.add_instance(ours(i));
    }
    mock.start();

    alibaba_ess_quorum_manager<> mgr{config_for(mock)};
    const std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster{
        {.node_id = 1, .group_id = default_zone},
        {.node_id = 51, .group_id = default_zone},
        {.node_id = 101, .group_id = default_zone},
        {.node_id = 120, .group_id = default_zone},
    };

    const auto health = std::move(mgr.assess_quorum(cluster)).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 4U);
    BOOST_CHECK(health.unreachable_nodes.empty());
    BOOST_CHECK_EQUAL(mgr.next_node_id(), 121U);
}

/// Requirement 6.5: an unreachable control plane must not be reported as a
/// dead cluster. "Alibaba is unreachable" and "every node is gone" call for
/// opposite responses, and only one of them is worth terminating instances
/// over.
BOOST_AUTO_TEST_CASE(a_transport_failure_is_an_exceptional_future_not_a_dead_cluster,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    EssMock mock;
    mock.add_instance(ours(1));
    mock.start();

    alibaba_ess_quorum_manager<> mgr{config_for(mock)};
    mock.shutdown();  // the endpoint is now a dead port

    const std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster{
        {.node_id = 1, .group_id = default_zone}};
    BOOST_CHECK_THROW(std::move(mgr.assess_quorum(cluster)).get(), std::exception);
}

/// An empty cluster is healthy by definition and costs no API call — asserted
/// against a manager whose endpoint has been taken away, which is the only way
/// to prove the short-circuit rather than the arithmetic.
BOOST_AUTO_TEST_CASE(an_empty_cluster_is_healthy_without_touching_the_api,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    EssMock mock;
    mock.start();
    alibaba_ess_quorum_manager<> mgr{config_for(mock)};
    mock.shutdown();

    const std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster;
    const auto health = std::move(mgr.assess_quorum(cluster)).get();
    BOOST_CHECK_EQUAL(health.status, kythira::quorum_status::healthy);
    BOOST_CHECK_EQUAL(health.total_node_count, 0U);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(alibaba_quorum_manager_provisioning)

/// Requirements 7.1, 7.2, 7.4: capacity+1, poll for the new InService/Running
/// instance, adopt it with the next NodeId, return `<private-ip>:<node_port>`.
BOOST_AUTO_TEST_CASE(provision_grows_the_group_and_adopts_the_new_instance,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    EssMock mock;
    mock.add_instance(ours(1));
    mock.start();

    alibaba_ess_quorum_manager<> mgr{config_for(mock)};
    const auto peer = std::move(mgr.provision_node(default_zone, std::nullopt)).get();

    BOOST_CHECK_EQUAL(peer.node_id, 2U);
    BOOST_CHECK_EQUAL(peer.address, "192.168.0.2:7000");
    BOOST_CHECK_EQUAL(mock.instance_count(), 2U);
    BOOST_CHECK_EQUAL(mock.capacity(), 2);
    BOOST_CHECK_EQUAL(mock.modify_calls.load(), 1);
}

/// Requirement 5.2 in the mock's mirror: `TagResources` is additive, so the
/// operator tag the instance launched with survives adoption. A manager that
/// copied the OCI read-merge-write shape would still pass; a manager that
/// wrote a replacing call would not, which is the regression this pins.
BOOST_AUTO_TEST_CASE(tag_writes_are_additive_and_preserve_operator_tags,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    EssMock mock;
    mock.start();

    auto cfg = config_for(mock);
    cfg.extra_tags = {{"cost-center", "42"}};
    alibaba_ess_quorum_manager<> mgr{cfg};
    const auto peer = std::move(mgr.provision_node(default_zone, std::nullopt)).get();

    const auto tags = mock.tags_of("i-mock1");
    BOOST_CHECK_EQUAL(tags.at("owner"), "platform-team");
    BOOST_CHECK_EQUAL(tags.at("cost-center"), "42");
    BOOST_CHECK_EQUAL(tags.at("kythira-cluster"), "test-cluster");
    BOOST_CHECK_EQUAL(tags.at("kythira-node-id"), "1");
    BOOST_CHECK_EQUAL(peer.node_id, 1U);
    BOOST_CHECK_EQUAL(mock.tag_calls.load(), 1);
}

/// Requirement 4.3's cousin: `extra_tags` cannot redirect a managed instance,
/// because the two well-known keys are written last.
BOOST_AUTO_TEST_CASE(extra_tags_cannot_override_the_well_known_keys,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    EssMock mock;
    mock.start();

    auto cfg = config_for(mock);
    cfg.extra_tags = {{"kythira-node-id", "9999"}, {"kythira-cluster", "hijacked"}};
    alibaba_ess_quorum_manager<> mgr{cfg};
    (void)std::move(mgr.provision_node(default_zone, std::nullopt)).get();

    const auto tags = mock.tags_of("i-mock1");
    BOOST_CHECK_EQUAL(tags.at("kythira-node-id"), "1");
    BOOST_CHECK_EQUAL(tags.at("kythira-cluster"), "test-cluster");
}

/// Requirement 7.3, the zone caveat this provider inherits from
/// `aws_asg_quorum_manager`: ESS placed the instance somewhere else, and the
/// manager proceeds rather than failing. Capacity in the wrong failure domain
/// still beats no capacity; strict placement is one group per zone.
BOOST_AUTO_TEST_CASE(a_zone_mismatch_proceeds_rather_than_failing,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    EssMock mock;
    mock.launch_zone = "cn-hangzhou-h";
    mock.start();

    alibaba_ess_quorum_manager<> mgr{config_for(mock)};
    const auto peer = std::move(mgr.provision_node("cn-hangzhou-b", std::nullopt)).get();
    BOOST_CHECK_EQUAL(peer.node_id, 1U);
    BOOST_CHECK_EQUAL(mock.instance_count(), 1U);
}

/// Requirement 7.2's timeout, with the capacity rollback the AWS and OCI
/// siblings also perform: a group left one instance larger than the cluster
/// believes turns a transient timeout into permanent capacity drift.
BOOST_AUTO_TEST_CASE(a_provision_that_never_settles_times_out_and_rolls_capacity_back,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    EssMock mock;
    // ESS admits the instance, ECS never reports it Running: the half of the
    // liveness rule a manager watching only the lifecycle state would miss,
    // and the state a stockout or a broken image actually produces.
    mock.launch_status = "Starting";
    mock.add_instance(ours(1));
    mock.start();

    auto cfg = config_for(mock);
    cfg.provision_timeout = 1s;
    alibaba_ess_quorum_manager<> mgr{cfg};

    BOOST_CHECK_THROW(std::move(mgr.provision_node(default_zone, std::nullopt)).get(),
                      std::exception);
    // Grow, then roll back — the second `ModifyScalingGroup` is the whole
    // point of the case.
    BOOST_CHECK_EQUAL(mock.modify_calls.load(), 2);
    BOOST_CHECK_EQUAL(mock.capacity(), 1);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(alibaba_quorum_manager_decommission)

/// Requirement 8.1-8.2: resolve by tag, remove (terminating and decrementing
/// in one call), then wait for the instance to leave the group listing.
BOOST_AUTO_TEST_CASE(decommission_removes_the_tagged_instance_and_shrinks_capacity,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    EssMock mock;
    const auto keep = mock.add_instance(ours(1));
    const auto drop = mock.add_instance(ours(2));
    mock.start();

    alibaba_ess_quorum_manager<> mgr{config_for(mock)};
    BOOST_CHECK_NO_THROW(std::move(mgr.decommission_node(std::uint64_t{2})).get());

    BOOST_CHECK_EQUAL(mock.remove_calls.load(), 1);
    BOOST_CHECK_EQUAL(mock.instance_count(), 1U);
    BOOST_CHECK_EQUAL(mock.capacity(), 1);
    BOOST_CHECK(!mock.tags_of(keep).empty());
    BOOST_CHECK(mock.tags_of(drop).empty());
}

/// Requirement 8.3 / design.md Property 2. The absent case resolves
/// **without** a mutating call — asserted on the call counter, because a
/// manager that issued `RemoveInstances` for an unknown id and swallowed the
/// vendor's 404 would also resolve successfully while being one blast radius
/// wider.
BOOST_AUTO_TEST_CASE(decommissioning_an_absent_node_is_idempotent_success,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    EssMock mock;
    mock.add_instance(ours(1));
    mock.start();

    alibaba_ess_quorum_manager<> mgr{config_for(mock)};
    BOOST_CHECK_NO_THROW(std::move(mgr.decommission_node(std::uint64_t{42})).get());
    BOOST_CHECK_EQUAL(mock.remove_calls.load(), 0);
    BOOST_CHECK_EQUAL(mock.instance_count(), 1U);

    // And twice over the same node: the second call is the common remediation
    // race, not an error.
    BOOST_CHECK_NO_THROW(std::move(mgr.decommission_node(std::uint64_t{1})).get());
    BOOST_CHECK_NO_THROW(std::move(mgr.decommission_node(std::uint64_t{1})).get());
    BOOST_CHECK_EQUAL(mock.remove_calls.load(), 1);
    BOOST_CHECK_EQUAL(mock.instance_count(), 0U);
}

/// Requirement 8.1 with design.md Property 4: a foreign cluster's instance
/// carrying the same NodeId tag is not this manager's to remove. Getting this
/// wrong terminates another cluster's node and looks like success.
BOOST_AUTO_TEST_CASE(decommission_never_touches_a_foreign_clusters_instance,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    EssMock mock;
    mock.add_instance({{"kythira-cluster", "someone-elses-cluster"}, {"kythira-node-id", "1"}});
    mock.start();

    alibaba_ess_quorum_manager<> mgr{config_for(mock)};
    BOOST_CHECK_NO_THROW(std::move(mgr.decommission_node(std::uint64_t{1})).get());
    BOOST_CHECK_EQUAL(mock.remove_calls.load(), 0);
    BOOST_CHECK_EQUAL(mock.instance_count(), 1U);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(alibaba_quorum_manager_maintenance)

/// Requirement 9.1: assess, decommission the unreachable, refill the deficit,
/// and return the **pre**-remediation health — answering with the post-repair
/// state would hide the fault that triggered the repair.
BOOST_AUTO_TEST_CASE(maintain_quorum_replaces_a_dead_node_and_reports_the_prior_health,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    EssMock mock;
    mock.add_instance(ours(1), "InService", "Stopped");
    mock.start();

    auto cfg = config_for(mock);
    cfg.topology.groups.clear();
    cfg.topology.groups.push_back({.group_id = default_zone, .target_count = 1});
    alibaba_ess_quorum_manager<> mgr{cfg};

    const std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster{
        {.node_id = 1, .group_id = default_zone}};
    const auto health = std::move(mgr.maintain_quorum(cluster)).get();

    BOOST_CHECK_EQUAL(health.live_node_count, 0U);
    BOOST_REQUIRE_EQUAL(health.unreachable_nodes.size(), 1U);
    BOOST_CHECK_EQUAL(health.unreachable_nodes.front(), 1U);

    BOOST_CHECK_EQUAL(mock.remove_calls.load(), 1);
    BOOST_CHECK_EQUAL(mock.instance_count(), 1U);
    BOOST_CHECK_EQUAL(mock.tags_of("i-mock2").at("kythira-node-id"), "1");
}

/// Requirement 4.3: `topology()` is a pure config read. Asserted with the
/// endpoint gone, so a future implementation that reached for the API to
/// answer it fails here rather than in production.
BOOST_AUTO_TEST_CASE(topology_is_synchronous_and_does_no_io,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    EssMock mock;
    mock.start();
    const alibaba_ess_quorum_manager<> mgr{config_for(mock)};
    mock.shutdown();

    const auto topo = mgr.topology();
    BOOST_REQUIRE_EQUAL(topo.groups.size(), 1U);
    BOOST_CHECK_EQUAL(topo.groups.front().group_id, default_zone);
    BOOST_CHECK_EQUAL(topo.total_size(), 3U);
}

BOOST_AUTO_TEST_SUITE_END()

#ifdef FIU_ENABLE

BOOST_AUTO_TEST_SUITE(alibaba_quorum_manager_fault_injection)

/// Requirement 10.1. The cluster is non-empty on purpose: `assess_quorum`
/// returns healthy without touching Alibaba for an empty one, so an empty
/// vector here would pass whether the fault point existed or not.
BOOST_AUTO_TEST_CASE(the_list_instances_fault_rejects_the_future,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    EssMock mock;
    mock.add_instance(ours(1));
    mock.start();
    alibaba_ess_quorum_manager<> mgr{config_for(mock)};
    const std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster{
        {.node_id = 1, .group_id = default_zone}};

    fiu_enable("raft/alibaba/ess/list_instances", 1, nullptr, 0);
    auto fut = mgr.assess_quorum(cluster);
    fiu_disable("raft/alibaba/ess/list_instances");

    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

/// Requirement 10.1-10.2. Also proves the fault fires *before*
/// `ModifyScalingGroup`: the group must not have grown.
BOOST_AUTO_TEST_CASE(the_modify_capacity_fault_rejects_the_future_without_growing_the_group,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    EssMock mock;
    mock.add_instance(ours(1));
    mock.start();
    alibaba_ess_quorum_manager<> mgr{config_for(mock)};

    fiu_enable("raft/alibaba/ess/modify_capacity", 1, nullptr, 0);
    auto fut = mgr.provision_node(default_zone, std::nullopt);
    fiu_disable("raft/alibaba/ess/modify_capacity");

    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
    BOOST_CHECK_EQUAL(mock.modify_calls.load(), 0);
    BOOST_CHECK_EQUAL(mock.instance_count(), 1U);
}

/// Requirement 10.1-10.2, and the same before-the-call claim: the instance is
/// still in the group afterwards.
BOOST_AUTO_TEST_CASE(the_remove_instances_fault_rejects_the_future,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    EssMock mock;
    mock.add_instance(ours(1));
    mock.start();
    alibaba_ess_quorum_manager<> mgr{config_for(mock)};

    fiu_enable("raft/alibaba/ess/remove_instances", 1, nullptr, 0);
    auto fut = mgr.decommission_node(std::uint64_t{1});
    fiu_disable("raft/alibaba/ess/remove_instances");

    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
    BOOST_CHECK_EQUAL(mock.remove_calls.load(), 0);
    BOOST_CHECK_EQUAL(mock.instance_count(), 1U);
}

/// Requirement 10.1. An empty cluster is fine here, unlike the assess case:
/// `maintain_quorum`'s fault point is checked before it delegates, so a
/// short-circuiting `assess_quorum` cannot mask it.
BOOST_AUTO_TEST_CASE(the_maintain_quorum_fault_rejects_the_future,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    EssMock mock;
    mock.start();
    alibaba_ess_quorum_manager<> mgr{config_for(mock)};
    const std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster;

    fiu_enable("raft/alibaba/ess/maintain_quorum", 1, nullptr, 0);
    auto fut = mgr.maintain_quorum(cluster);
    fiu_disable("raft/alibaba/ess/maintain_quorum");

    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

BOOST_AUTO_TEST_SUITE_END()

#endif  // FIU_ENABLE

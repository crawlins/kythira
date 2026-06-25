#define BOOST_TEST_MODULE quorum_management_test
#include <boost/test/unit_test.hpp>

#include <raft/quorum_management.hpp>

#include <folly/init/Init.h>

struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("quorum_management_test"), nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);

// Concept satisfaction (Req 10.1, Req 8.1, Req 3.3)
static_assert(
    kythira::quorum_manager<kythira::no_op_quorum_manager<std::uint64_t, std::string, std::string>,
                            std::uint64_t, std::string, std::string>,
    "no_op_quorum_manager must satisfy quorum_manager");

// placement_group_id concept checks (Req 1.2)
static_assert(kythira::placement_group_id<std::string>);
static_assert(kythira::placement_group_id<std::uint64_t>);

BOOST_AUTO_TEST_SUITE(no_op_quorum_manager_tests)

// Req 10.2 — assess_quorum returns healthy with correct live/total counts
BOOST_AUTO_TEST_CASE(assess_quorum_all_live, *boost::unit_test::timeout(5)) {
    kythira::no_op_quorum_manager<std::uint64_t, std::string, std::string> mgr;

    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster{
        {.node_id = 1, .group_id = "az-a"},
        {.node_id = 2, .group_id = "az-b"},
        {.node_id = 3, .group_id = "az-c"},
    };

    auto health = mgr.assess_quorum(cluster).get();

    BOOST_CHECK(health.status == kythira::quorum_status::healthy);
    BOOST_CHECK_EQUAL(health.live_node_count, 3u);
    BOOST_CHECK_EQUAL(health.total_node_count, 3u);
    BOOST_CHECK(health.unreachable_nodes.empty());
}

// Req 10.3 — one placement_group_health entry per distinct group_id
BOOST_AUTO_TEST_CASE(assess_quorum_per_group_breakdown, *boost::unit_test::timeout(5)) {
    kythira::desired_topology<std::string> topo{
        .groups =
            {
                {.group_id = "az-a", .target_count = 2},
                {.group_id = "az-b", .target_count = 1},
            },
    };
    kythira::no_op_quorum_manager<std::uint64_t, std::string, std::string> mgr{topo};

    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster{
        {.node_id = 1, .group_id = "az-a"},
        {.node_id = 2, .group_id = "az-a"},
        {.node_id = 3, .group_id = "az-b"},
    };

    auto health = mgr.assess_quorum(cluster).get();

    BOOST_CHECK_EQUAL(health.groups.size(), 2u);

    const auto* az_a = [&]() -> const kythira::placement_group_health<std::uint64_t, std::string>* {
        for (const auto& g : health.groups) {
            if (g.group_id == "az-a") return &g;
        }
        return nullptr;
    }();
    const auto* az_b = [&]() -> const kythira::placement_group_health<std::uint64_t, std::string>* {
        for (const auto& g : health.groups) {
            if (g.group_id == "az-b") return &g;
        }
        return nullptr;
    }();

    BOOST_REQUIRE(az_a != nullptr);
    BOOST_CHECK_EQUAL(az_a->live_count, 2u);
    BOOST_CHECK_EQUAL(az_a->target_count, 2u);
    BOOST_CHECK(az_a->unreachable_nodes.empty());

    BOOST_REQUIRE(az_b != nullptr);
    BOOST_CHECK_EQUAL(az_b->live_count, 1u);
    BOOST_CHECK_EQUAL(az_b->target_count, 1u);
    BOOST_CHECK(az_b->unreachable_nodes.empty());
}

// Req 10.4 — provision_node returns an exceptional Future
BOOST_AUTO_TEST_CASE(provision_node_exceptional, *boost::unit_test::timeout(5)) {
    kythira::no_op_quorum_manager<std::uint64_t, std::string, std::string> mgr;

    auto fut = mgr.provision_node(std::string{"az-a"}, std::nullopt);
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

// Req 10.5 — decommission_node returns a successfully-resolved Future
BOOST_AUTO_TEST_CASE(decommission_node_noop, *boost::unit_test::timeout(5)) {
    kythira::no_op_quorum_manager<std::uint64_t, std::string, std::string> mgr;

    auto fut = mgr.decommission_node(std::uint64_t{42});
    BOOST_CHECK_NO_THROW(std::move(fut).get());
}

// Req 10.6 — topology().total_size() equals sum of target_count values
BOOST_AUTO_TEST_CASE(topology_total_size, *boost::unit_test::timeout(5)) {
    kythira::desired_topology<std::string> topo{
        .groups =
            {
                {.group_id = "az-a", .target_count = 2},
                {.group_id = "az-b", .target_count = 1},
                {.group_id = "az-c", .target_count = 2},
            },
    };
    kythira::no_op_quorum_manager<std::uint64_t, std::string, std::string> mgr{topo};

    BOOST_CHECK_EQUAL(mgr.topology().total_size(), 5u);
}

// assess_quorum with single group (no explicit topology — target_count=0 fallback)
BOOST_AUTO_TEST_CASE(assess_quorum_single_group_no_topology, *boost::unit_test::timeout(5)) {
    kythira::no_op_quorum_manager<std::uint64_t, std::string, std::string> mgr;

    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster{
        {.node_id = 1, .group_id = "default"},
        {.node_id = 2, .group_id = "default"},
        {.node_id = 3, .group_id = "default"},
    };

    auto health = mgr.assess_quorum(cluster).get();
    BOOST_CHECK_EQUAL(health.groups.size(), 1u);
    BOOST_CHECK_EQUAL(health.groups[0].live_count, 3u);
    BOOST_CHECK_EQUAL(health.groups[0].target_count, 0u);  // no topology declared
}

// Empty cluster is also valid
BOOST_AUTO_TEST_CASE(assess_quorum_empty_cluster, *boost::unit_test::timeout(5)) {
    kythira::no_op_quorum_manager<std::uint64_t, std::string, std::string> mgr;

    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster;
    auto health = mgr.assess_quorum(cluster).get();

    BOOST_CHECK(health.status == kythira::quorum_status::healthy);
    BOOST_CHECK_EQUAL(health.live_node_count, 0u);
    BOOST_CHECK_EQUAL(health.total_node_count, 0u);
    BOOST_CHECK(health.groups.empty());
}

// default-constructed topology has total_size() == 0
BOOST_AUTO_TEST_CASE(default_topology_total_size_zero, *boost::unit_test::timeout(5)) {
    kythira::no_op_quorum_manager<std::uint64_t, std::string, std::string> mgr;
    BOOST_CHECK_EQUAL(mgr.topology().total_size(), 0u);
}

// operator<< for quorum_status covers all enumerators
BOOST_AUTO_TEST_CASE(quorum_status_ostream, *boost::unit_test::timeout(5)) {
    std::ostringstream ss;
    ss << kythira::quorum_status::healthy;
    BOOST_CHECK_EQUAL(ss.str(), "healthy");
    ss.str("");
    ss << kythira::quorum_status::degraded;
    BOOST_CHECK_EQUAL(ss.str(), "degraded");
    ss.str("");
    ss << kythira::quorum_status::critical;
    BOOST_CHECK_EQUAL(ss.str(), "critical");
    ss.str("");
    ss << kythira::quorum_status::lost;
    BOOST_CHECK_EQUAL(ss.str(), "lost");
}

BOOST_AUTO_TEST_SUITE_END()

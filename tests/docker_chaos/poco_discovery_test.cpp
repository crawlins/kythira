#define BOOST_TEST_MODULE poco_discovery_test
#include <boost/test/unit_test.hpp>

#include "os_faults.hpp"

#include <httplib.h>
#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace json = boost::json;

// ── Port / container layout — mirrors poco-discovery-compose.yml ──────────────

struct DiscoveryNode {
    std::string id;
    int http_port;
    std::string container;
};

static const std::vector<DiscoveryNode> k_nodes{
    {"node1", 9011, "poco-discovery-node1"},
    {"node2", 9012, "poco-discovery-node2"},
    {"node3", 9013, "poco-discovery-node3"},
};

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string compose_file() {
    const char* env = std::getenv("KYTHIRA_POCO_DISCOVERY_COMPOSE_FILE");
    if (env && *env) return env;
    return "docker/poco-discovery-compose.yml";
}

static bool is_healthy(const DiscoveryNode& n) {
    httplib::Client cli("localhost", n.http_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(3);
    auto res = cli.Get("/health");
    return res && res->status == 200;
}

// Query /peers with the given browse timeout and return the node_id strings.
static std::vector<std::string> peer_ids(const DiscoveryNode& n, int browse_ms = 3000) {
    httplib::Client cli("localhost", n.http_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(browse_ms / 1000 + 5);
    const std::string path = "/peers?timeout_ms=" + std::to_string(browse_ms);
    auto res = cli.Get(path);
    if (!res || res->status != 200) return {};
    std::vector<std::string> ids;
    for (const auto& item : json::parse(res->body).as_array()) {
        ids.emplace_back(item.as_object().at("id").as_string());
    }
    return ids;
}

static bool wait_all_healthy(std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        bool all = std::ranges::all_of(k_nodes, is_healthy);
        if (all) return true;
        std::this_thread::sleep_for(500ms);
    }
    return false;
}

// ── Fixture ───────────────────────────────────────────────────────────────────

struct DiscoveryFixture {
    std::string file{compose_file()};

    DiscoveryFixture() {
        // Tear down stale containers from a prior run, then start fresh.
        docker_chaos::os::try_exec(docker_chaos::os::real_exec,
                                   docker_chaos::os::compose_down_cmd(file));
        docker_chaos::os::checked_exec(docker_chaos::os::real_exec,
                                       docker_chaos::os::compose_up_cmd(file));
        if (!wait_all_healthy(60s)) {
            docker_chaos::os::try_exec(docker_chaos::os::real_exec,
                                       docker_chaos::os::compose_down_cmd(file));
            BOOST_FAIL("discovery nodes did not become healthy within 60 s");
        }
    }

    ~DiscoveryFixture() {
        try {
            docker_chaos::os::real_exec(docker_chaos::os::compose_down_cmd(file));
        } catch (...) {
        }
    }
};

// ── Tests ─────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(poco_discovery)

// Req: all three nodes report healthy after startup.
BOOST_FIXTURE_TEST_CASE(all_nodes_healthy, DiscoveryFixture, *boost::unit_test::timeout(90)) {
    for (const auto& n : k_nodes) {
        BOOST_TEST(is_healthy(n), "node " + n.id + " not healthy after startup");
    }
}

// Req: each node discovers both of its peers via DNS-SD.
BOOST_FIXTURE_TEST_CASE(all_nodes_discover_peers, DiscoveryFixture,
                        *boost::unit_test::timeout(90)) {
    // Allow time for all registrations to propagate via Avahi.
    std::this_thread::sleep_for(3s);

    for (const auto& querying : k_nodes) {
        auto peers = peer_ids(querying, 3000);
        for (const auto& expected : k_nodes) {
            if (expected.id == querying.id) continue;
            bool found = std::ranges::find(peers, expected.id) != peers.end();
            BOOST_TEST(found, "node " + querying.id + " did not discover peer " + expected.id);
        }
    }
}

// Req: after one node is killed, surviving nodes no longer see it once the
// DNS-SD registration is removed (immediate on D-Bus disconnect) and the
// freshness interval (20 s) has elapsed — whichever comes first.
BOOST_FIXTURE_TEST_CASE(dead_node_absent_after_kill, DiscoveryFixture,
                        *boost::unit_test::timeout(150)) {
    // Let registrations propagate, then confirm the to-be-killed node is visible.
    std::this_thread::sleep_for(3s);

    {
        auto peers = peer_ids(k_nodes[1], 3000);
        bool visible = std::ranges::find(peers, k_nodes[0].id) != peers.end();
        BOOST_REQUIRE_MESSAGE(visible, k_nodes[0].id + " must be visible before kill");
    }

    // Kill node1 with SIGKILL — no graceful Avahi goodbye.
    docker_chaos::os::checked_exec(docker_chaos::os::real_exec,
                                   docker_chaos::os::docker_kill_cmd(k_nodes[0].container));

    // Wait for the registration to disappear: either the Avahi daemon removes
    // it on D-Bus disconnect (seconds) or the freshness interval (20 s) expires.
    std::this_thread::sleep_for(25s);

    for (const auto& survivor : {k_nodes[1], k_nodes[2]}) {
        auto peers = peer_ids(survivor, 3000);
        bool still_present = std::ranges::find(peers, k_nodes[0].id) != peers.end();
        BOOST_TEST(!still_present, survivor.id + " must not see killed node " + k_nodes[0].id);
    }
}

BOOST_AUTO_TEST_SUITE_END()

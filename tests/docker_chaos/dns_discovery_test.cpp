#define BOOST_TEST_MODULE dns_discovery_test
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

// ── Port / container layout — mirrors dns-discovery-compose.yml ───────────────

struct DnsNode {
    std::string id;
    int http_port;
    std::string container;
};

static const std::vector<DnsNode> k_nodes{
    {"node1", 9021, "dns-discovery-node1"},
    {"node2", 9022, "dns-discovery-node2"},
    {"node3", 9023, "dns-discovery-node3"},
};

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string compose_file() {
    const char* env = std::getenv("KYTHIRA_DNS_DISCOVERY_COMPOSE_FILE");
    if ((env != nullptr) && (*env != 0)) {
        return env;
    }
    return "docker/dns-discovery-compose.yml";
}

static bool is_healthy(const DnsNode& n) {
    httplib::Client cli("localhost", n.http_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(3);
    auto res = cli.Get("/health");
    return res && res->status == 200;
}

static std::vector<std::string> peer_ids(const DnsNode& n, int browse_ms = 3000) {
    httplib::Client cli("localhost", n.http_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(browse_ms / 1000 + 5);
    const std::string path = "/peers?timeout_ms=" + std::to_string(browse_ms);
    auto res = cli.Get(path);
    if (!res || res->status != 200) {
        return {};
    }
    std::vector<std::string> ids;
    const json::value parsed = json::parse(res->body);
    for (const auto& item : parsed.as_array()) {
        ids.emplace_back(item.as_object().at("id").as_string());
    }
    return ids;
}

static std::size_t peer_count(const DnsNode& n, int browse_ms = 3000) {
    return peer_ids(n, browse_ms).size();
}

static bool wait_all_healthy(std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        bool all = std::ranges::all_of(k_nodes, is_healthy);
        if (all) {
            return true;
        }
        std::this_thread::sleep_for(500ms);
    }
    return false;
}

// ── Fixture ───────────────────────────────────────────────────────────────────

struct DnsFixture {
    std::string file{compose_file()};

    DnsFixture() {
        docker_chaos::os::try_exec(docker_chaos::os::real_exec,
                                   docker_chaos::os::compose_down_cmd(file));
        docker_chaos::os::checked_exec(docker_chaos::os::real_exec,
                                       docker_chaos::os::compose_up_cmd(file));
        if (!wait_all_healthy(60s)) {
            docker_chaos::os::try_exec(docker_chaos::os::real_exec,
                                       docker_chaos::os::compose_down_cmd(file));
            BOOST_FAIL("DNS discovery nodes did not become healthy within 60 s");
        }
    }

    ~DnsFixture() {
        try {
            docker_chaos::os::real_exec(docker_chaos::os::compose_down_cmd(file));
        } catch (...) {
        }
    }
};

// ── Tests ─────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(dns_discovery)

// Req: all three nodes serve a healthy HTTP control plane.
BOOST_FIXTURE_TEST_CASE(all_nodes_healthy, DnsFixture, *boost::unit_test::timeout(90)) {
    for (const auto& n : k_nodes) {
        BOOST_TEST(is_healthy(n), "node " + n.id + " not healthy after startup");
    }
}

// Req: after all nodes register, each sees both other nodes via DNS A-record lookup.
BOOST_FIXTURE_TEST_CASE(all_nodes_discover_peers, DnsFixture, *boost::unit_test::timeout(90)) {
    // Allow time for all A records to propagate.
    std::this_thread::sleep_for(3s);

    for (const auto& querying : k_nodes) {
        // rfc2136_ldns_discovery excludes self; expect 2 peers per node.
        std::size_t count = peer_count(querying, 3000);
        BOOST_TEST(count == 2u,
                   "node " + querying.id + " found " + std::to_string(count) + " peers, want 2");
    }
}

// Req: stopping a node with SIGTERM triggers clean DDNS deregistration; the
// remaining two nodes no longer see the stopped node in their peer list.
BOOST_FIXTURE_TEST_CASE(stopped_node_absent_after_deregister, DnsFixture,
                        *boost::unit_test::timeout(90)) {
    std::this_thread::sleep_for(3s);

    // Confirm node1 is visible from node2 before stopping.
    {
        std::size_t count = peer_count(k_nodes[1], 3000);
        BOOST_REQUIRE_MESSAGE(count == 2u,
                              "node2 must see 2 peers before stop; saw " + std::to_string(count));
    }

    // docker stop sends SIGTERM; the node binary catches it, lets the
    // rfc2136_ldns_discovery destructor send a DELETE UPDATE, then exits.
    docker_chaos::os::checked_exec(docker_chaos::os::real_exec,
                                   docker_chaos::os::docker_stop_cmd(k_nodes[0].container, 15));

    // Give BIND9 a moment to process the DELETE and the surviving nodes to
    // pick up the change on their next /peers call.
    std::this_thread::sleep_for(3s);

    for (const auto& survivor : {k_nodes[1], k_nodes[2]}) {
        std::size_t count = peer_count(survivor, 3000);
        BOOST_TEST(count == 1u,
                   survivor.id + " must see 1 peer after stop; saw " + std::to_string(count));
    }
}

BOOST_AUTO_TEST_SUITE_END()

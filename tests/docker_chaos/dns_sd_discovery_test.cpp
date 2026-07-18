#define BOOST_TEST_MODULE dns_sd_discovery_test
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

// ── Port / container layout — mirrors dns-sd-discovery-compose.yml ────────────

struct DnsSdNode {
    std::string id;
    int http_port;
    std::string container;
};

static const std::vector<DnsSdNode> k_nodes{
    {"node1", 9031, "dns-sd-discovery-node1"},
    {"node2", 9032, "dns-sd-discovery-node2"},
    {"node3", 9033, "dns-sd-discovery-node3"},
};

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string compose_file() {
    const char* env = std::getenv("KYTHIRA_DNS_SD_DISCOVERY_COMPOSE_FILE");
    if ((env != nullptr) && (*env != 0)) {
        return env;
    }
    return "docker/dns-sd-discovery-compose.yml";
}

static bool is_healthy(const DnsSdNode& n) {
    httplib::Client cli("localhost", n.http_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(3);
    auto res = cli.Get("/health");
    return res && res->status == 200;
}

static std::vector<std::string> peer_ids(const DnsSdNode& n, int browse_ms = 3000) {
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

struct DnsSdFixture {
    std::string file{compose_file()};

    DnsSdFixture() {
        docker_chaos::os::try_exec(docker_chaos::os::real_exec,
                                   docker_chaos::os::compose_down_cmd(file));
        docker_chaos::os::checked_exec(docker_chaos::os::real_exec,
                                       docker_chaos::os::compose_up_cmd(file));
        if (!wait_all_healthy(60s)) {
            docker_chaos::os::try_exec(docker_chaos::os::real_exec,
                                       docker_chaos::os::compose_down_cmd(file));
            BOOST_FAIL("DNS-SD discovery nodes did not become healthy within 60 s");
        }
    }

    ~DnsSdFixture() {
        try {
            docker_chaos::os::real_exec(docker_chaos::os::compose_down_cmd(file));
        } catch (...) {
        }
    }
};

// ── Tests ─────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(dns_sd_discovery)

// Req: all three nodes serve a healthy HTTP control plane.
BOOST_FIXTURE_TEST_CASE(all_nodes_healthy, DnsSdFixture, *boost::unit_test::timeout(90)) {
    for (const auto& n : k_nodes) {
        BOOST_TEST(is_healthy(n), "node " + n.id + " not healthy after startup");
    }
}

// Req: each node discovers both peers by DNS-SD node ID via PTR → SRV lookup.
BOOST_FIXTURE_TEST_CASE(all_nodes_discover_peers, DnsSdFixture, *boost::unit_test::timeout(90)) {
    // Allow time for all PTR/SRV/TXT records to propagate.
    std::this_thread::sleep_for(3s);

    for (const auto& querying : k_nodes) {
        auto peers = peer_ids(querying, 3000);
        for (const auto& expected : k_nodes) {
            if (expected.id == querying.id) {
                continue;
            }
            bool found = std::ranges::find(peers, expected.id) != peers.end();
            BOOST_TEST(found, "node " + querying.id + " did not discover peer " + expected.id);
        }
    }
}

// Req: after node1 is killed with SIGKILL (no graceful deregistration), the
// surviving nodes stop reporting it once its TXT fresh_until timestamp expires
// (freshness_interval = 20 s; we wait 25 s to give BIND9 a processing margin).
BOOST_FIXTURE_TEST_CASE(dead_node_absent_after_freshness_expiry, DnsSdFixture,
                        *boost::unit_test::timeout(150)) {
    std::this_thread::sleep_for(3s);

    // Verify node1 is currently visible from node2.
    {
        auto peers = peer_ids(k_nodes[1], 3000);
        bool visible = std::ranges::find(peers, k_nodes[0].id) != peers.end();
        BOOST_REQUIRE_MESSAGE(visible, k_nodes[0].id + " must be visible before kill");
    }

    // SIGKILL — no Avahi goodbye, no DNS DELETE UPDATE.
    docker_chaos::os::checked_exec(docker_chaos::os::real_exec,
                                   docker_chaos::os::docker_kill_cmd(k_nodes[0].container));

    // Wait for the 20 s freshness interval to expire (25 s gives margin).
    std::this_thread::sleep_for(25s);

    for (const auto& survivor : {k_nodes[1], k_nodes[2]}) {
        auto peers = peer_ids(survivor, 3000);
        bool still_present = std::ranges::find(peers, k_nodes[0].id) != peers.end();
        BOOST_TEST(!still_present, survivor.id + " must not see killed node " + k_nodes[0].id);
    }
}

BOOST_AUTO_TEST_SUITE_END()

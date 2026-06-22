#define BOOST_TEST_MODULE dns_peer_discovery_chaos_test
#include <boost/test/unit_test.hpp>

#include <fiu.h>
#include <fiu-control.h>

#include <raft/peer_discovery.hpp>
#include <raft/rfc1035_peer_discovery.hpp>
#include <raft/rfc2136_dns_sd_discovery.hpp>
#include <raft/rfc2136_ldns_discovery.hpp>

#include <folly/init/Init.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

// Fault point names — must match the fiu_do_on() strings in the headers.
static constexpr const char* k_rfc1035_fail = "raft/dns/rfc1035/find_peers/fail";
static constexpr const char* k_rfc1035_ipv4 = "raft/dns/rfc1035/find_peers/inject_ipv4";
static constexpr const char* k_rfc1035_mixed = "raft/dns/rfc1035/find_peers/inject_mixed";
static constexpr const char* k_rfc2136_update = "raft/dns/rfc2136/send_update";
static constexpr const char* k_rfc2136_update_noop = "raft/dns/rfc2136/send_update/noop";
static constexpr const char* k_dns_sd_update = "raft/dns/rfc2136_dns_sd/send_update_rr";
static constexpr const char* k_dns_sd_update_noop = "raft/dns/rfc2136_dns_sd/send_update_rr/noop";

static constexpr const char* k_all_dns_faults[] = {
    k_rfc1035_fail,        k_rfc1035_ipv4,  k_rfc1035_mixed,      k_rfc2136_update,
    k_rfc2136_update_noop, k_dns_sd_update, k_dns_sd_update_noop,
};

struct DnsChaosFixture {
    DnsChaosFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("dns_peer_discovery_chaos_test"), nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
        fiu_init(0);
    }
    ~DnsChaosFixture() {
        for (const auto* name : k_all_dns_faults) {
            fiu_disable(name);
        }
    }
    std::unique_ptr<folly::Init> _init;
};
BOOST_GLOBAL_FIXTURE(DnsChaosFixture);

static void clear_faults() {
    for (const auto* name : k_all_dns_faults) {
        fiu_disable(name);
    }
}

#ifdef KYTHIRA_HAS_LDNS

static constexpr const char* k_server = "127.0.0.1";
static constexpr uint16_t k_port = 5399;
static constexpr const char* k_shared_name = "raft.test.local.";
static constexpr const char* k_zone = "test.local.";

namespace {

kythira::rfc1035_peer_discovery::config make_rfc1035_cfg() {
    return {k_server, k_port, k_shared_name};
}

kythira::rfc2136_ldns_discovery::config make_rfc2136_cfg() {
    return {make_rfc1035_cfg(), k_zone, 30, "", "hmac-sha256.", ""};
}

kythira::rfc2136_dns_sd_discovery::config make_dns_sd_cfg() {
    kythira::rfc2136_dns_sd_discovery::config cfg;
    cfg.server = k_server;
    cfg.port = k_port;
    cfg.zone = k_zone;
    cfg.service_domain = "cluster.test.local.";
    cfg.service_type = "_kythira-test._tcp";
    cfg.ttl = 30;
    cfg.freshness_interval = std::chrono::seconds{2};  // short for fresher tests
    return cfg;
}

}  // namespace

// ── rfc1035 chaos ───────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(rfc1035_chaos_suite)

// Verify the "fail" fault point returns an empty list and does not throw.
BOOST_AUTO_TEST_CASE(find_peers_fail_returns_empty, *boost::unit_test::timeout(5)) {
    clear_faults();
    kythira::rfc1035_peer_discovery disc{make_rfc1035_cfg()};

    fiu_enable(k_rfc1035_fail, 1, nullptr, 0);
    auto peers = disc.find_peers(std::chrono::milliseconds{5000}).get();
    fiu_disable(k_rfc1035_fail);

    BOOST_TEST(peers.empty());
}

// Verify the "inject_ipv4" fault point returns exactly the two hardcoded IPv4 entries.
BOOST_AUTO_TEST_CASE(find_peers_inject_ipv4_returns_two_peers, *boost::unit_test::timeout(5)) {
    clear_faults();
    kythira::rfc1035_peer_discovery disc{make_rfc1035_cfg()};

    fiu_enable(k_rfc1035_ipv4, 1, nullptr, 0);
    auto peers = disc.find_peers(std::chrono::milliseconds{5000}).get();
    fiu_disable(k_rfc1035_ipv4);

    BOOST_REQUIRE_EQUAL(peers.size(), 2u);
    BOOST_CHECK_EQUAL(peers[0].node_id, "10.0.0.1");
    BOOST_CHECK_EQUAL(peers[0].address, "10.0.0.1");
    BOOST_CHECK_EQUAL(peers[1].node_id, "10.0.0.2");
    BOOST_CHECK_EQUAL(peers[1].address, "10.0.0.2");
}

// Verify the "inject_mixed" fault point returns one IPv4 and one IPv6 entry.
BOOST_AUTO_TEST_CASE(find_peers_inject_mixed_returns_one_ipv4_one_ipv6,
                     *boost::unit_test::timeout(5)) {
    clear_faults();
    kythira::rfc1035_peer_discovery disc{make_rfc1035_cfg()};

    fiu_enable(k_rfc1035_mixed, 1, nullptr, 0);
    auto peers = disc.find_peers(std::chrono::milliseconds{5000}).get();
    fiu_disable(k_rfc1035_mixed);

    BOOST_REQUIRE_EQUAL(peers.size(), 2u);
    BOOST_CHECK_EQUAL(peers[0].address, "10.0.0.1");
    BOOST_CHECK_EQUAL(peers[1].address, "::2");
}

// With no fault active, find_peers must not return injected data (unreachable server → empty).
BOOST_AUTO_TEST_CASE(find_peers_no_fault_does_not_inject, *boost::unit_test::timeout(10)) {
    clear_faults();
    kythira::rfc1035_peer_discovery disc{make_rfc1035_cfg()};

    auto peers = disc.find_peers(std::chrono::milliseconds{200}).get();

    BOOST_TEST(peers.empty());
}

// Enabling then disabling the fault must restore normal (empty-on-unreachable) behaviour.
BOOST_AUTO_TEST_CASE(find_peers_fault_disable_restores_normal, *boost::unit_test::timeout(10)) {
    clear_faults();
    kythira::rfc1035_peer_discovery disc{make_rfc1035_cfg()};

    fiu_enable(k_rfc1035_ipv4, 1, nullptr, 0);
    auto injected = disc.find_peers(std::chrono::milliseconds{5000}).get();
    fiu_disable(k_rfc1035_ipv4);

    BOOST_REQUIRE_EQUAL(injected.size(), 2u);

    auto after = disc.find_peers(std::chrono::milliseconds{200}).get();
    BOOST_TEST(after.empty());
}

BOOST_AUTO_TEST_SUITE_END()

// ── rfc2136 chaos ───────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(rfc2136_chaos_suite)

// register_node sets _self_address then calls send_update; "noop" skips the update
// without throwing, so register_node returns normally.  find_peers then filters out
// the entry whose address matches _self_address.
BOOST_AUTO_TEST_CASE(find_peers_self_filter_ipv4, *boost::unit_test::timeout(5)) {
    clear_faults();

    // Keep noop active through the entire scope so the dtor's deregister_self()
    // also skips the network call.
    fiu_enable(k_rfc2136_update_noop, 1, nullptr, 0);
    {
        kythira::rfc2136_ldns_discovery disc{make_rfc2136_cfg()};
        BOOST_CHECK_NO_THROW(disc.register_node("n1", "10.0.0.1").get());

        fiu_enable(k_rfc1035_ipv4, 1, nullptr, 0);
        auto peers = disc.find_peers(std::chrono::milliseconds{5000}).get();
        fiu_disable(k_rfc1035_ipv4);

        // Injected: {"10.0.0.1","10.0.0.1"}, {"10.0.0.2","10.0.0.2"}
        // Self: "10.0.0.1" → filtered out
        BOOST_REQUIRE_EQUAL(peers.size(), 1u);
        BOOST_CHECK_EQUAL(peers[0].address, "10.0.0.2");
    }
    fiu_disable(k_rfc2136_update_noop);
}

BOOST_AUTO_TEST_CASE(find_peers_self_filter_ipv6, *boost::unit_test::timeout(5)) {
    clear_faults();

    fiu_enable(k_rfc2136_update_noop, 1, nullptr, 0);
    {
        kythira::rfc2136_ldns_discovery disc{make_rfc2136_cfg()};
        BOOST_CHECK_NO_THROW(disc.register_node("n1", "::2").get());

        fiu_enable(k_rfc1035_mixed, 1, nullptr, 0);
        auto peers = disc.find_peers(std::chrono::milliseconds{5000}).get();
        fiu_disable(k_rfc1035_mixed);

        // Injected: {"10.0.0.1","10.0.0.1"}, {"::2","::2"}
        // Self: "::2" → filtered out
        BOOST_REQUIRE_EQUAL(peers.size(), 1u);
        BOOST_CHECK_EQUAL(peers[0].address, "10.0.0.1");
    }
    fiu_disable(k_rfc2136_update_noop);
}

// Without any register_node call, _self_address is empty → all injected peers returned.
BOOST_AUTO_TEST_CASE(find_peers_no_self_address_returns_all, *boost::unit_test::timeout(5)) {
    clear_faults();
    kythira::rfc2136_ldns_discovery disc{make_rfc2136_cfg()};

    fiu_enable(k_rfc1035_ipv4, 1, nullptr, 0);
    auto peers = disc.find_peers(std::chrono::milliseconds{5000}).get();
    fiu_disable(k_rfc1035_ipv4);

    BOOST_REQUIRE_EQUAL(peers.size(), 2u);
}

// "fail" fault on rfc1035 propagates through rfc2136::find_peers as an empty list.
BOOST_AUTO_TEST_CASE(find_peers_empty_when_rfc1035_faulted, *boost::unit_test::timeout(5)) {
    clear_faults();
    kythira::rfc2136_ldns_discovery disc{make_rfc2136_cfg()};

    fiu_enable(k_rfc1035_fail, 1, nullptr, 0);
    auto peers = disc.find_peers(std::chrono::milliseconds{5000}).get();
    fiu_disable(k_rfc1035_fail);

    BOOST_TEST(peers.empty());
}

// "send_update" fault causes register_node to throw std::runtime_error.
BOOST_AUTO_TEST_CASE(register_node_throws_when_send_update_faulted, *boost::unit_test::timeout(5)) {
    clear_faults();
    kythira::rfc2136_ldns_discovery disc{make_rfc2136_cfg()};

    fiu_enable(k_rfc2136_update, 1, nullptr, 0);
    BOOST_CHECK_THROW(disc.register_node("n1", "10.0.0.1"), std::runtime_error);
    fiu_disable(k_rfc2136_update);
}

// Dtor must silently absorb the exception from deregister_self when the fault is active.
// Step 1: register via "noop" fault so _self_address is set without network I/O.
// Step 2: swap to "throw" fault; dtor calls deregister_self → send_update throws,
//         but the dtor's catch(...) swallows it.
BOOST_AUTO_TEST_CASE(dtor_silent_when_deregister_faulted, *boost::unit_test::timeout(5)) {
    clear_faults();
    {
        kythira::rfc2136_ldns_discovery disc{make_rfc2136_cfg()};

        fiu_enable(k_rfc2136_update_noop, 1, nullptr, 0);
        BOOST_CHECK_NO_THROW(disc.register_node("n1", "10.0.0.1").get());
        fiu_disable(k_rfc2136_update_noop);

        // _self_address == "10.0.0.1"; activate throw fault so deregister fails.
        fiu_enable(k_rfc2136_update, 1, nullptr, 0);
        // disc destroyed here; dtor calls deregister_self → throws; catch(...) swallows.
    }
    fiu_disable(k_rfc2136_update);
    BOOST_TEST(true);
}

// Fault cleared mid-run: first call to find_peers gets injected data; second gets real
// (empty) data from the unreachable server.
BOOST_AUTO_TEST_CASE(fault_disable_mid_run_restores_normal, *boost::unit_test::timeout(10)) {
    clear_faults();
    kythira::rfc2136_ldns_discovery disc{make_rfc2136_cfg()};

    fiu_enable(k_rfc1035_ipv4, 1, nullptr, 0);
    auto injected = disc.find_peers(std::chrono::milliseconds{5000}).get();
    fiu_disable(k_rfc1035_ipv4);

    BOOST_REQUIRE_EQUAL(injected.size(), 2u);

    auto after = disc.find_peers(std::chrono::milliseconds{200}).get();
    BOOST_TEST(after.empty());
}

BOOST_AUTO_TEST_SUITE_END()

// ── rfc2136_dns_sd chaos ─────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(rfc2136_dns_sd_chaos_suite)

// "noop" fault: register_node (PTR + SRV + TXT-del + TXT-add) succeeds without network I/O.
BOOST_AUTO_TEST_CASE(register_node_noop_succeeds, *boost::unit_test::timeout(5)) {
    clear_faults();
    fiu_enable(k_dns_sd_update_noop, 1, nullptr, 0);
    {
        kythira::rfc2136_dns_sd_discovery disc{make_dns_sd_cfg()};
        BOOST_CHECK_NO_THROW(disc.register_node("n1", "host:7000").get());
    }
    fiu_disable(k_dns_sd_update_noop);
    BOOST_TEST(true);
}

// "throw" fault: register_node propagates the exception from send_update_rr.
// The fault must remain active through the dtor so send_deregister throws and
// is swallowed by the dtor's catch(...)  rather than hitting the network.
BOOST_AUTO_TEST_CASE(register_node_throws_when_faulted, *boost::unit_test::timeout(5)) {
    clear_faults();
    {
        kythira::rfc2136_dns_sd_discovery disc{make_dns_sd_cfg()};
        fiu_enable(k_dns_sd_update, 1, nullptr, 0);
        BOOST_CHECK_THROW(disc.register_node("n1", "host:7000").get(), std::runtime_error);
    }  // dtor: send_deregister throws (fault active) → caught → fast
    fiu_disable(k_dns_sd_update);
}

// After noop register, dtor absorbs the exception from deregister when the fault is active.
BOOST_AUTO_TEST_CASE(dtor_silent_when_deregister_faulted, *boost::unit_test::timeout(5)) {
    clear_faults();
    {
        kythira::rfc2136_dns_sd_discovery disc{make_dns_sd_cfg()};
        fiu_enable(k_dns_sd_update_noop, 1, nullptr, 0);
        BOOST_CHECK_NO_THROW(disc.register_node("n1", "host:7000").get());
        fiu_disable(k_dns_sd_update_noop);
        fiu_enable(k_dns_sd_update, 1, nullptr, 0);
        // dtor: stop_fresher (fast) + send_deregister → throw → caught by catch(...).
    }
    fiu_disable(k_dns_sd_update);
    BOOST_TEST(true);
}

// With a short freshness_interval and noop fault, the fresher thread fires at least
// once before destruction; stop_fresher must join the thread cleanly.
BOOST_AUTO_TEST_CASE(fresher_fires_and_stop_joins, *boost::unit_test::timeout(10)) {
    clear_faults();
    fiu_enable(k_dns_sd_update_noop, 1, nullptr, 0);
    {
        // freshness_interval=2s → fresher wakes every 1 s
        kythira::rfc2136_dns_sd_discovery disc{make_dns_sd_cfg()};
        BOOST_CHECK_NO_THROW(disc.register_node("n1", "host:7000").get());
        std::this_thread::sleep_for(std::chrono::milliseconds{1100});
        // dtor: stop_fresher joins the running fresher thread, then send_deregister noops.
    }
    fiu_disable(k_dns_sd_update_noop);
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_SUITE_END()

#else  // !KYTHIRA_HAS_LDNS

BOOST_AUTO_TEST_CASE(ldns_not_available) {
    BOOST_TEST_MESSAGE("libldns not compiled in — DNS peer discovery chaos tests skipped");
    BOOST_TEST(true);
}

#endif  // KYTHIRA_HAS_LDNS

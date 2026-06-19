#define BOOST_TEST_MODULE dns_peer_discovery_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/peer_discovery.hpp>
#include <raft/rfc1035_peer_discovery.hpp>
#include <raft/rfc2136_ldns_discovery.hpp>

#include <folly/init/Init.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("dns_peer_discovery_unit_test"), nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);

#ifdef KYTHIRA_HAS_LDNS

// 127.0.0.1:5399 — loopback closed port.  ldns UDP queries get no ICMP
// port-unreachable (Linux does not propagate it through poll POLLERR without
// IP_RECVERR), so rfc1035::find_peers relies on its explicitly-set timeout to
// return quickly.  rfc2136::send_update has no configurable timeout in the
// current implementation, so unit tests avoid triggering its network path.
static constexpr const char* k_server = "127.0.0.1";
static constexpr uint16_t k_port = 5399;
static constexpr const char* k_shared_name = "raft.test.local.";
static constexpr const char* k_zone = "test.local.";

namespace {

kythira::rfc1035_peer_discovery::config make_rfc1035_cfg() {
    return {k_server, k_port, k_shared_name};
}

kythira::rfc2136_ldns_discovery::config make_rfc2136_cfg() {
    return {make_rfc1035_cfg(),
            k_zone,
            /*ttl=*/30,
            /*tsig_key_name=*/"",
            /*tsig_algorithm=*/"hmac-sha256.",
            /*tsig_key_base64=*/""};
}

}  // namespace

// ── rfc1035_peer_discovery ──────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(rfc1035_suite)

BOOST_AUTO_TEST_CASE(concept_satisfied) {
    static_assert(
        kythira::peer_discovery<kythira::rfc1035_peer_discovery, std::string, std::string>);
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_CASE(construct_with_config) {
    BOOST_CHECK_NO_THROW((kythira::rfc1035_peer_discovery{make_rfc1035_cfg()}));
}

BOOST_AUTO_TEST_CASE(register_node_is_noop, *boost::unit_test::timeout(5)) {
    kythira::rfc1035_peer_discovery disc{make_rfc1035_cfg()};
    BOOST_CHECK_NO_THROW(disc.register_node("n1", "10.0.0.1").get());
}

// ldns_resolver_set_timeout caps each poll() to 200 ms; with default 3 retries
// the maximum wait is ~600 ms, well inside the 10 s timeout.
BOOST_AUTO_TEST_CASE(find_peers_unreachable_server_returns_empty, *boost::unit_test::timeout(10)) {
    kythira::rfc1035_peer_discovery disc{make_rfc1035_cfg()};
    auto peers = disc.find_peers(std::chrono::milliseconds{200}).get();
    BOOST_TEST(peers.empty());
}

BOOST_AUTO_TEST_CASE(find_peers_does_not_throw_on_network_failure, *boost::unit_test::timeout(10)) {
    kythira::rfc1035_peer_discovery disc{make_rfc1035_cfg()};
    BOOST_CHECK_NO_THROW(disc.find_peers(std::chrono::milliseconds{200}).get());
}

BOOST_AUTO_TEST_SUITE_END()

// ── rfc2136_ldns_discovery ──────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(rfc2136_suite)

BOOST_AUTO_TEST_CASE(concept_satisfied) {
    static_assert(
        kythira::peer_discovery<kythira::rfc2136_ldns_discovery, std::string, std::string>);
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_CASE(construct_with_config) {
    BOOST_CHECK_NO_THROW((kythira::rfc2136_ldns_discovery{make_rfc2136_cfg()}));
}

// send_update short-circuits on empty addr before any allocation or network I/O.
BOOST_AUTO_TEST_CASE(register_node_empty_address_skips_network, *boost::unit_test::timeout(5)) {
    kythira::rfc2136_ldns_discovery disc{make_rfc2136_cfg()};
    BOOST_CHECK_NO_THROW(disc.register_node("n1", "").get());
}

// "not.a.valid.ip" contains '.' → IPv4 path; ldns_str2rdf_a fails → throws
// BEFORE any network call.  register_node now sets _self_address only after
// send_update succeeds, so the dtor does not attempt deregistration.
BOOST_AUTO_TEST_CASE(register_node_invalid_ipv4_address_throws, *boost::unit_test::timeout(5)) {
    kythira::rfc2136_ldns_discovery disc{make_rfc2136_cfg()};
    BOOST_CHECK_THROW(disc.register_node("n1", "not.a.valid.ip"), std::runtime_error);
}

// "::invalid" has no '.' → IPv6 path; ldns_str2rdf_aaaa fails → throws before network.
BOOST_AUTO_TEST_CASE(register_node_invalid_ipv6_address_throws, *boost::unit_test::timeout(5)) {
    kythira::rfc2136_ldns_discovery disc{make_rfc2136_cfg()};
    BOOST_CHECK_THROW(disc.register_node("n1", "::invalid"), std::runtime_error);
}

// Empty zone string: ldns_dname_new_frm_str("") returns nullptr → throws before network.
BOOST_AUTO_TEST_CASE(register_node_empty_zone_throws, *boost::unit_test::timeout(5)) {
    auto cfg = make_rfc2136_cfg();
    cfg.zone = "";
    kythira::rfc2136_ldns_discovery disc{std::move(cfg)};
    BOOST_CHECK_THROW(disc.register_node("n1", "10.0.0.1"), std::runtime_error);
}

// No registration → _self_address is empty → deregister_self is a no-op → dtor silent.
BOOST_AUTO_TEST_CASE(dtor_silent_when_no_registration, *boost::unit_test::timeout(5)) {
    {
        kythira::rfc2136_ldns_discovery disc{make_rfc2136_cfg()};
    }
    BOOST_TEST(true);
}

// register_node with an invalid address throws; because _self_address is only
// set after a successful send_update, it stays empty and the dtor is a no-op.
BOOST_AUTO_TEST_CASE(dtor_silent_after_failed_registration, *boost::unit_test::timeout(5)) {
    {
        kythira::rfc2136_ldns_discovery disc{make_rfc2136_cfg()};
        BOOST_CHECK_THROW(disc.register_node("n1", "not.a.valid.ip"), std::runtime_error);
    }
    BOOST_TEST(true);
}

// Delegates to rfc1035 which returns empty; find_peers must not throw.
BOOST_AUTO_TEST_CASE(find_peers_returns_empty_on_unreachable_server,
                     *boost::unit_test::timeout(10)) {
    kythira::rfc2136_ldns_discovery disc{make_rfc2136_cfg()};
    BOOST_CHECK_NO_THROW(disc.find_peers(std::chrono::milliseconds{200}).get());
}

BOOST_AUTO_TEST_SUITE_END()

#else  // !KYTHIRA_HAS_LDNS

BOOST_AUTO_TEST_CASE(ldns_not_available) {
    BOOST_TEST_MESSAGE("libldns not compiled in — DNS peer discovery tests skipped");
    BOOST_TEST(true);
}

#endif  // KYTHIRA_HAS_LDNS

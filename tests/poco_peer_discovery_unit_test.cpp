#define BOOST_TEST_MODULE poco_peer_discovery_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/peer_discovery.hpp>
#include <raft/poco_peer_discovery.hpp>

#include <folly/init/Init.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("poco_peer_discovery_unit_test"), nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);

#ifdef KYTHIRA_HAS_POCO_DNSSD

static const kythira::poco_peer_discovery::config k_default_cfg{};

namespace {

kythira::poco_peer_discovery::config make_cfg() {
    return {};
}

}  // namespace

// ── poco_peer_discovery ─────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(poco_suite)

// ── Compile-time checks ─────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(concept_satisfied) {
    static_assert(kythira::peer_discovery<kythira::poco_peer_discovery, std::string, std::string>);
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_CASE(type_aliases) {
    static_assert(std::is_same_v<kythira::poco_peer_discovery::node_id_type, std::string>);
    static_assert(std::is_same_v<kythira::poco_peer_discovery::address_type, std::string>);
    BOOST_TEST(true);
}

// ── Config defaults ──────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(config_default_service_type) {
    BOOST_TEST(k_default_cfg.service_type == "_raft._tcp");
}

BOOST_AUTO_TEST_CASE(config_default_domain_empty) {
    BOOST_TEST(k_default_cfg.domain.empty());
}

BOOST_AUTO_TEST_CASE(config_custom_service_type_round_trips) {
    kythira::poco_peer_discovery::config cfg;
    cfg.service_type = "_mraft._tcp";
    BOOST_TEST(cfg.service_type == "_mraft._tcp");
}

// ── Construction ─────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(construct_default_config) {
    BOOST_CHECK_NO_THROW((kythira::poco_peer_discovery{make_cfg()}));
}

BOOST_AUTO_TEST_CASE(construct_custom_service_type) {
    kythira::poco_peer_discovery::config cfg;
    cfg.service_type = "_kythira._tcp";
    BOOST_CHECK_NO_THROW((kythira::poco_peer_discovery{std::move(cfg)}));
}

BOOST_AUTO_TEST_CASE(construct_nonempty_domain) {
    kythira::poco_peer_discovery::config cfg;
    cfg.domain = "cluster.example.com.";
    BOOST_CHECK_NO_THROW((kythira::poco_peer_discovery{std::move(cfg)}));
}

// ── register_node: argument validation (all throw before any Poco call) ──────

BOOST_AUTO_TEST_CASE(register_node_rejects_empty_address) {
    kythira::poco_peer_discovery disc{make_cfg()};
    BOOST_CHECK_THROW(disc.register_node("n1", ""), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(register_node_rejects_address_without_colon) {
    kythira::poco_peer_discovery disc{make_cfg()};
    BOOST_CHECK_THROW(disc.register_node("n1", "node1.local"), std::invalid_argument);
}

// ":" alone: host is empty (colon at position 0)
BOOST_AUTO_TEST_CASE(register_node_rejects_colon_only) {
    kythira::poco_peer_discovery disc{make_cfg()};
    BOOST_CHECK_THROW(disc.register_node("n1", ":"), std::invalid_argument);
}

// ":4001" — no host
BOOST_AUTO_TEST_CASE(register_node_rejects_colon_at_start) {
    kythira::poco_peer_discovery disc{make_cfg()};
    BOOST_CHECK_THROW(disc.register_node("n1", ":4001"), std::invalid_argument);
}

// "node1:" — no port digits
BOOST_AUTO_TEST_CASE(register_node_rejects_colon_at_end) {
    kythira::poco_peer_discovery disc{make_cfg()};
    BOOST_CHECK_THROW(disc.register_node("n1", "node1.local:"), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(register_node_rejects_port_zero) {
    kythira::poco_peer_discovery disc{make_cfg()};
    BOOST_CHECK_THROW(disc.register_node("n1", "node1.local:0"), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(register_node_rejects_port_too_large) {
    kythira::poco_peer_discovery disc{make_cfg()};
    BOOST_CHECK_THROW(disc.register_node("n1", "node1.local:65536"), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(register_node_rejects_non_numeric_port) {
    kythira::poco_peer_discovery disc{make_cfg()};
    BOOST_CHECK_THROW(disc.register_node("n1", "node1.local:abc"), std::invalid_argument);
}

// Leading-numeric port with trailing non-digits must also be rejected
BOOST_AUTO_TEST_CASE(register_node_rejects_port_with_trailing_chars) {
    kythira::poco_peer_discovery disc{make_cfg()};
    BOOST_CHECK_THROW(disc.register_node("n1", "node1.local:4001abc"), std::invalid_argument);
}

// Boundary: port 1 is valid (no throw before network path)
// Note: this may throw runtime_error if daemon is unavailable; it must NOT throw
// invalid_argument.  That alone is what this test verifies.
BOOST_AUTO_TEST_CASE(register_node_accepts_port_one_format, *boost::unit_test::timeout(15)) {
    kythira::poco_peer_discovery disc{make_cfg()};
    try {
        disc.register_node("n1", "node1.local:1").get();
    } catch (const std::invalid_argument& e) {
        BOOST_FAIL(std::string("valid address format must not throw invalid_argument: ") +
                   e.what());
    } catch (const std::runtime_error&) {
        // Daemon unavailable — acceptable
    } catch (...) {
        // Other Poco errors — acceptable
    }
    BOOST_TEST(true);
}

// Boundary: port 65535 is valid
BOOST_AUTO_TEST_CASE(register_node_accepts_port_65535_format, *boost::unit_test::timeout(15)) {
    kythira::poco_peer_discovery disc{make_cfg()};
    try {
        disc.register_node("n1", "node1.local:65535").get();
    } catch (const std::invalid_argument& e) {
        BOOST_FAIL(std::string("valid address format must not throw invalid_argument: ") +
                   e.what());
    } catch (const std::runtime_error&) {
    } catch (...) {
    }
    BOOST_TEST(true);
}

// ── Destructor safety ─────────────────────────────────────────────────────────

// Destroy without ever calling register_node.
BOOST_AUTO_TEST_CASE(dtor_silent_when_not_registered, *boost::unit_test::timeout(5)) {
    {
        kythira::poco_peer_discovery disc{make_cfg()};
    }
    BOOST_TEST(true);
}

// Failed arg validation → _self_address is NOT set → dtor must be silent.
BOOST_AUTO_TEST_CASE(dtor_silent_after_arg_validation_failure, *boost::unit_test::timeout(5)) {
    {
        kythira::poco_peer_discovery disc{make_cfg()};
        BOOST_CHECK_THROW(disc.register_node("n1", "bad"), std::invalid_argument);
    }
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_CASE(dtor_silent_after_multiple_failed_validations, *boost::unit_test::timeout(5)) {
    {
        kythira::poco_peer_discovery disc{make_cfg()};
        BOOST_CHECK_THROW(disc.register_node("n1", ""), std::invalid_argument);
        BOOST_CHECK_THROW(disc.register_node("n1", ":0"), std::invalid_argument);
        BOOST_CHECK_THROW(disc.register_node("n1", "host:65536"), std::invalid_argument);
    }
    BOOST_TEST(true);
}

// ── find_peers ────────────────────────────────────────────────────────────────

// find_peers must complete within the timeout (plus overhead) and must not throw.
// Without a running DNS-SD daemon, Poco either returns no results or throws
// internally; the implementation catches daemon errors and returns empty.
BOOST_AUTO_TEST_CASE(find_peers_completes_without_throwing, *boost::unit_test::timeout(10)) {
    kythira::poco_peer_discovery disc{make_cfg()};
    BOOST_CHECK_NO_THROW(disc.find_peers(std::chrono::milliseconds{200}).get());
}

// Results (if any) must be a vector; the return type must satisfy the concept.
BOOST_AUTO_TEST_CASE(find_peers_returns_vector, *boost::unit_test::timeout(10)) {
    kythira::poco_peer_discovery disc{make_cfg()};
    auto peers = disc.find_peers(std::chrono::milliseconds{100}).get();
    // No assertion on contents — daemon may or may not be running.
    BOOST_TEST(peers.size() >= 0u);  // trivially true; confirms return type compiles
}

// find_peers after a failed arg-validation must still not throw.
BOOST_AUTO_TEST_CASE(find_peers_after_failed_registration, *boost::unit_test::timeout(10)) {
    kythira::poco_peer_discovery disc{make_cfg()};
    BOOST_CHECK_THROW(disc.register_node("n1", "bad"), std::invalid_argument);
    BOOST_CHECK_NO_THROW(disc.find_peers(std::chrono::milliseconds{100}).get());
}

// Multiple sequential find_peers calls must all complete safely.
BOOST_AUTO_TEST_CASE(find_peers_called_twice, *boost::unit_test::timeout(15)) {
    kythira::poco_peer_discovery disc{make_cfg()};
    BOOST_CHECK_NO_THROW(disc.find_peers(std::chrono::milliseconds{100}).get());
    BOOST_CHECK_NO_THROW(disc.find_peers(std::chrono::milliseconds{100}).get());
}

BOOST_AUTO_TEST_SUITE_END()

#else  // !KYTHIRA_HAS_POCO_DNSSD

BOOST_AUTO_TEST_CASE(poco_dnssd_not_available) {
    BOOST_TEST_MESSAGE("Poco DNSSD not compiled in — poco_peer_discovery tests skipped");
    BOOST_TEST(true);
}

#endif  // KYTHIRA_HAS_POCO_DNSSD

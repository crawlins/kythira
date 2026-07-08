// Property 21 (Requirements 20.3, 20.5): IP identifiers never attempt
// dns-01. For any csr_signing_options containing at least one IP address,
// the ACME requests acme_certificate_provider sends for that identifier show
// identifier.type == "ip" and the selected challenge is always http-01 —
// regardless of challenge_type being configured to dns_01. A mixed request
// (one DNS name, one IP) validates the DNS name however challenge_type says
// to, and the IP always via http-01, in the same sign_csr() call.
//
// Property 22 (Requirements 20.2, 20.6): .local validation degrades to a
// distinguishable error, never silent DNS. On a host with no mDNS-capable
// resolver configured, requesting a certificate for a .local identifier
// fails with an error identifying the missing mDNS capability specifically.

#define BOOST_TEST_MODULE acme_identifier_type_test

#include <boost/test/unit_test.hpp>

#include "acme_test_server.hpp"

#include <raft/acme_certificate_provider.hpp>
#include <raft/acme_certificate_provider_impl.hpp>
#include <raft/certificate_provider.hpp>

#include <netdb.h>
#include <unistd.h>

using namespace raft::testing;

namespace {

// Fixed, non-overlapping http-01 validation ports per test case — same
// convention as acme_certificate_provider_test.cpp (there is no ACME wire
// mechanism for the server to discover a dynamically-chosen client port).
constexpr int k_base_port = 18800;

}  // namespace

// ── classify()/challenge_for() unit coverage ────────────────────────────────

BOOST_AUTO_TEST_CASE(classify_recognizes_ipv4_and_ipv6_literals) {
    BOOST_TEST((acme_identifier::classify("127.0.0.1") == acme_identifier::kind::ip));
    BOOST_TEST((acme_identifier::classify("10.0.1.10") == acme_identifier::kind::ip));
    BOOST_TEST((acme_identifier::classify("::1") == acme_identifier::kind::ip));
    BOOST_TEST((acme_identifier::classify("2001:db8::1") == acme_identifier::kind::ip));
}

BOOST_AUTO_TEST_CASE(classify_treats_everything_else_as_dns_including_dot_local) {
    BOOST_TEST((acme_identifier::classify("example.com") == acme_identifier::kind::dns));
    BOOST_TEST((acme_identifier::classify("localhost") == acme_identifier::kind::dns));
    // .local names are NOT a distinct ACME identifier type — mDNS resolution
    // (task 35) is a validation-time concern, not a classification concern.
    BOOST_TEST((acme_identifier::classify("node1.local") == acme_identifier::kind::dns));
    BOOST_TEST((acme_identifier::classify("999.999.999.999") ==
                acme_identifier::kind::dns));  // not a valid literal
}

BOOST_AUTO_TEST_CASE(challenge_for_ip_is_always_http01) {
    BOOST_TEST(acme_identifier::challenge_for(
                   acme_identifier::kind::ip,
                   acme_certificate_provider_config::challenge_type::http_01) == "http-01");
    BOOST_TEST(acme_identifier::challenge_for(
                   acme_identifier::kind::ip,
                   acme_certificate_provider_config::challenge_type::dns_01) == "http-01");
}

BOOST_AUTO_TEST_CASE(challenge_for_dns_follows_configured_type) {
    BOOST_TEST(acme_identifier::challenge_for(
                   acme_identifier::kind::dns,
                   acme_certificate_provider_config::challenge_type::http_01) == "http-01");
    BOOST_TEST(acme_identifier::challenge_for(
                   acme_identifier::kind::dns,
                   acme_certificate_provider_config::challenge_type::dns_01) == "dns-01");
}

// ── End-to-end: IP identifier ignores a dns_01 configuration ───────────────

BOOST_AUTO_TEST_CASE(ip_only_request_uses_http01_even_when_dns01_configured,
                     *boost::unit_test::timeout(30)) {
    acme_test_server::options server_opts;
    server_opts.http01_validation_port = k_base_port;
    acme_test_server server{server_opts};

    acme_certificate_provider_config config;
    config.directory_url = server.directory_url();
    // The core Property 21 claim: configuring dns_01 does NOT push an IP
    // identifier's challenge to dns-01 — there's no infrastructure dependency
    // in this path at all, since the IP always uses http-01 regardless.
    config.challenge = acme_certificate_provider_config::challenge_type::dns_01;
    config.http01_bind_address = "127.0.0.1:" + std::to_string(k_base_port);
    config.poll_timeout = std::chrono::seconds(10);
    config.poll_interval = std::chrono::milliseconds(200);
    acme_certificate_provider provider(config);

    leaf_certificate_options leaf_opts;
    leaf_opts.subject.common_name = "127.0.0.1";
    leaf_opts.ip_addresses = {"127.0.0.1"};
    auto csr = generate_key_and_csr(leaf_opts);

    csr_signing_options sign_opts;
    sign_opts.ip_addresses = {"127.0.0.1"};

    auto material = provider.sign_csr(csr.csr_pem, sign_opts).get();
    BOOST_TEST(!material.certificate_pem.empty());

    BOOST_TEST(server.challenge_status_for("127.0.0.1", "http-01").value_or("<none>") == "valid");
    // No dns-01 challenge exists at all for an "ip"-typed identifier
    // (acme_test_server never offers one — Requirement 20.3/20.5).
    BOOST_TEST(!server.challenge_status_for("127.0.0.1", "dns-01").has_value());
}

// ── End-to-end: mixed DNS+IP request, both identifiers dispatched
// correctly within the same sign_csr() call ────────────────────────────────
//
// With challenge_type left at its http_01 default, both the DNS name and the
// IP validate via http-01 — this is the scenario task 34's own "Verify"
// bullet calls out directly (a mixed request issues successfully), and it
// also confirms the DNS identifier is correctly typed "dns" (not defaulted
// to "ip" or vice versa) via the newOrder request.
BOOST_AUTO_TEST_CASE(mixed_dns_and_ip_request_both_validate_via_http01,
                     *boost::unit_test::timeout(30)) {
    acme_test_server::options server_opts;
    server_opts.http01_validation_port = k_base_port + 1;
    acme_test_server server{server_opts};

    acme_certificate_provider_config config;
    config.directory_url = server.directory_url();
    config.challenge = acme_certificate_provider_config::challenge_type::http_01;
    config.http01_bind_address = "127.0.0.1:" + std::to_string(k_base_port + 1);
    config.poll_timeout = std::chrono::seconds(10);
    config.poll_interval = std::chrono::milliseconds(200);
    acme_certificate_provider provider(config);

    leaf_certificate_options leaf_opts;
    leaf_opts.subject.common_name = "localhost";
    leaf_opts.dns_names = {"localhost"};
    leaf_opts.ip_addresses = {"127.0.0.1"};
    auto csr = generate_key_and_csr(leaf_opts);

    csr_signing_options sign_opts;
    sign_opts.dns_names = {"localhost"};
    sign_opts.ip_addresses = {"127.0.0.1"};

    // Both identifiers' http-01 responders share config.http01_bind_address
    // sequentially (one authorization is fully completed, including its
    // responder's RAII teardown, before the next authorization starts — see
    // the for loop over authz_urls in sign_csr()), so there's no port
    // conflict between them.
    auto material = provider.sign_csr(csr.csr_pem, sign_opts).get();
    BOOST_TEST(!material.certificate_pem.empty());

    BOOST_TEST(server.challenge_status_for("localhost", "http-01").value_or("<none>") == "valid");
    BOOST_TEST(server.challenge_status_for("127.0.0.1", "http-01").value_or("<none>") == "valid");
}

// ── Mixed request under dns_01 configuration: both identifiers dispatch to
// their correct challenge type within the SAME sign_csr() call, even though
// the overall call can't complete — the DNS identifier's dns-01 challenge
// has no real DNS infrastructure to validate against in this environment
// (same reasoning as acme_certificate_provider_test.cpp's
// dns01_against_unreachable_server_fails_closed). What's being verified here
// is dispatch, not dns-01 completion: the IP identifier's http-01 challenge
// reaches "valid" BEFORE the DNS identifier's dns-01 challenge is even
// attempted, proving IP-before-DNS ordering and that the IP never falls back
// to (or gets forced into) dns-01.
#ifdef KYTHIRA_HAS_LDNS
BOOST_AUTO_TEST_CASE(mixed_request_under_dns01_config_dispatches_ip_to_http01_first,
                     *boost::unit_test::timeout(30)) {
    acme_test_server::options server_opts;
    server_opts.http01_validation_port = k_base_port + 2;
    acme_test_server server{server_opts};

    acme_certificate_provider_config config;
    config.directory_url = server.directory_url();
    config.challenge = acme_certificate_provider_config::challenge_type::dns_01;
    config.http01_bind_address = "127.0.0.1:" + std::to_string(k_base_port + 2);
    // Unreachable (RFC 5737 TEST-NET-1) — the DNS identifier's dns-01
    // responder will fail to publish its TXT record and sign_csr() will
    // reject the future, but only AFTER the IP identifier ahead of it in
    // authorization order has already completed via http-01.
    config.dns01.server = "192.0.2.1";
    config.dns01.port = 53;
    config.dns01.zone = "example.com.";
    config.poll_timeout = std::chrono::seconds(3);
    config.poll_interval = std::chrono::milliseconds(200);
    acme_certificate_provider provider(config);

    leaf_certificate_options leaf_opts;
    leaf_opts.subject.common_name = "dns01-mixed.example.com";
    leaf_opts.dns_names = {"dns01-mixed.example.com"};
    leaf_opts.ip_addresses = {"127.0.0.1"};
    auto csr = generate_key_and_csr(leaf_opts);

    csr_signing_options sign_opts;
    sign_opts.dns_names = {"dns01-mixed.example.com"};
    sign_opts.ip_addresses = {"127.0.0.1"};

    BOOST_CHECK_THROW(provider.sign_csr(csr.csr_pem, sign_opts).get(), std::exception);

    // The IP identifier reached a *valid* http-01 challenge before the call
    // failed — proving it was dispatched to (and completed via) http-01
    // despite dns_01 being configured, within this same call.
    BOOST_TEST(server.challenge_status_for("127.0.0.1", "http-01").value_or("<none>") == "valid");
    BOOST_TEST(!server.challenge_status_for("127.0.0.1", "dns-01").has_value());

    // The DNS identifier never got anywhere near http-01 — it either never
    // started (still "pending") or was correctly routed to dns-01 and failed
    // there, but it must not show a "valid"/attempted http-01 challenge.
    auto dns_http01_status = server.challenge_status_for("dns01-mixed.example.com", "http-01");
    BOOST_TEST((!dns_http01_status.has_value() || *dns_http01_status == "pending"));
}
#endif

// ── Property 22: .local validation degrades to a distinguishable error ─────

namespace {

struct mdns_override_guard {
    explicit mdns_override_guard(std::optional<bool> value) {
        set_mdns_capability_override_for_test(value);
    }
    ~mdns_override_guard() { set_mdns_capability_override_for_test(std::nullopt); }
    mdns_override_guard(const mdns_override_guard&) = delete;
    mdns_override_guard& operator=(const mdns_override_guard&) = delete;
};

}  // namespace

BOOST_AUTO_TEST_CASE(dot_local_validation_fails_distinguishably_when_mdns_forced_unavailable,
                     *boost::unit_test::timeout(30)) {
    // Forces acme_test_server's capability probe to report "unavailable"
    // regardless of this machine's actual /etc/nsswitch.conf — the
    // test-only injection point task 35 calls for, so this test doesn't
    // depend on the CI host's real resolver configuration.
    mdns_override_guard override_guard(false);

    acme_test_server::options server_opts;
    server_opts.http01_validation_port = k_base_port + 3;
    acme_test_server server{server_opts};

    acme_certificate_provider_config config;
    config.directory_url = server.directory_url();
    config.http01_bind_address = "127.0.0.1:" + std::to_string(k_base_port + 3);
    config.poll_timeout = std::chrono::seconds(5);
    config.poll_interval = std::chrono::milliseconds(200);
    acme_certificate_provider provider(config);

    leaf_certificate_options leaf_opts;
    leaf_opts.subject.common_name = "node1.local";
    leaf_opts.dns_names = {"node1.local"};
    auto csr = generate_key_and_csr(leaf_opts);

    csr_signing_options sign_opts;
    sign_opts.dns_names = {"node1.local"};

    // Classified "dns" (not a distinct ACME identifier type — Requirement
    // 20.2) — the failure is a validation-time capability problem, not a
    // classification/dispatch problem, so it surfaces only once the server
    // actually attempts http-01 validation.
    BOOST_TEST((acme_identifier::classify("node1.local") == acme_identifier::kind::dns));

    BOOST_CHECK_THROW(provider.sign_csr(csr.csr_pem, sign_opts).get(), std::exception);

    // The distinguishable error, not a generic "challenge failed" and not a
    // silent fallthrough to public DNS (which would either time out or,
    // worse, resolve to something unrelated on split-horizon DNS).
    BOOST_TEST(server.challenge_error_for("node1.local", "http-01").value_or("<none>") ==
               "mdnsResolverUnavailable");
}

BOOST_AUTO_TEST_CASE(dot_local_validation_succeeds_on_an_mdns_capable_network,
                     *boost::unit_test::timeout(30)) {
    // No override here: exercises the REAL /etc/nsswitch.conf-based probe,
    // per Requirement 20.2's "relying on the host's own mDNS-capable
    // resolver configuration."
    mdns_override_guard override_guard(std::nullopt);

    char hostname_buf[256]{};
    if (gethostname(hostname_buf, sizeof(hostname_buf)) != 0) {
        BOOST_TEST_MESSAGE("skipping: gethostname() failed");
        return;
    }
    std::string local_identifier = std::string(hostname_buf) + ".local";

    // Live-resolution probe distinct from acme_test_server's own
    // nsswitch.conf-based capability check — just to decide whether THIS
    // test's environment can exercise the real success path at all. Per
    // Requirement 20.2/design.md: skipped, not failed, where no mDNS
    // infrastructure is available (mirrors the LocalStack tests' convention).
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    addrinfo* resolved = nullptr;
    int rc = getaddrinfo(local_identifier.c_str(), nullptr, &hints, &resolved);
    if (resolved != nullptr) freeaddrinfo(resolved);
    if (rc != 0) {
        BOOST_TEST_MESSAGE(
            "skipping: " << local_identifier
                         << " did not resolve on this network (no mDNS infrastructure available)");
        return;
    }

    acme_test_server::options server_opts;
    server_opts.http01_validation_port = k_base_port + 4;
    acme_test_server server{server_opts};

    acme_certificate_provider_config config;
    config.directory_url = server.directory_url();
    // Bind on all interfaces: the mDNS-resolved address is this machine's
    // real LAN/link-local address, not loopback.
    config.http01_bind_address = "0.0.0.0:" + std::to_string(k_base_port + 4);
    config.poll_timeout = std::chrono::seconds(10);
    config.poll_interval = std::chrono::milliseconds(200);
    acme_certificate_provider provider(config);

    leaf_certificate_options leaf_opts;
    leaf_opts.subject.common_name = local_identifier;
    leaf_opts.dns_names = {local_identifier};
    auto csr = generate_key_and_csr(leaf_opts);

    csr_signing_options sign_opts;
    sign_opts.dns_names = {local_identifier};

    // The getaddrinfo() pre-check above only proves SOME address resolved —
    // not that it's this host's own address, reachable on the http01
    // responder's bound interface. On some networks (observed on GitHub
    // Actions runners) "<hostname>.local" resolves to an address that isn't
    // actually reachable for the challenge, i.e. mDNS infrastructure isn't
    // genuinely functional even though resolution nominally succeeds. Per
    // Requirement 20.2/design.md this case is skipped, not failed — the
    // property under test is "IF a real mDNS-capable network is available,
    // THEN .local issuance succeeds," not "this specific network is
    // mDNS-capable."
    pem_material material;
    try {
        material = provider.sign_csr(csr.csr_pem, sign_opts).get();
    } catch (const std::exception& ex) {
        BOOST_TEST_MESSAGE("skipping: " << local_identifier
                                        << " resolved but did not actually validate (" << ex.what()
                                        << ") — no functional mDNS infrastructure "
                                           "on this network");
        return;
    }
    BOOST_TEST(!material.certificate_pem.empty());
    BOOST_TEST(server.challenge_status_for(local_identifier, "http-01").value_or("<none>") ==
               "valid");
}

static_assert(raft::testing::certificate_provider<raft::testing::acme_certificate_provider>);

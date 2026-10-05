// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE acme_certificate_provider_test

#include <boost/test/unit_test.hpp>

#include "acme_test_server.hpp"

#include <raft/acme_certificate_provider.hpp>
#include <raft/acme_certificate_provider_impl.hpp>
#include <raft/certificate_provider.hpp>

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

#include <unistd.h>

using namespace raft::testing;

namespace {

struct x509_deleter {
    void operator()(X509* c) const {
        if (c != nullptr) {
            X509_free(c);
        }
    }
};
using x509_ptr = std::unique_ptr<X509, x509_deleter>;

auto load_cert_pem(const std::string& pem) -> x509_ptr {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    x509_ptr cert{PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)};
    BIO_free(bio);
    return cert;
}

// A fixed, coordinated port for the http-01 exchange: acme_test_server's
// challenge validation always targets whatever host the identifier names on
// this port (there is no ACME wire mechanism for the server to discover a
// dynamically-chosen client port — real-world ACME always uses port 80; this
// override exists purely so tests don't need root to bind 80).
constexpr int k_http01_port = 18765;

}  // namespace

BOOST_AUTO_TEST_CASE(sign_csr_via_http01_chains_to_test_server_root,
                     *boost::unit_test::timeout(30)) {
    acme_test_server::options server_opts;
    server_opts.http01_validation_port = k_http01_port;
    acme_test_server server{server_opts};

    acme_certificate_provider_config config;
    config.directory_url = server.directory_url();
    config.trust_anchors_pem = server.root_certificate_pem();
    config.challenge = acme_certificate_provider_config::challenge_type::http_01;
    config.http01_bind_address = "127.0.0.1:" + std::to_string(k_http01_port);
    config.poll_timeout = std::chrono::seconds(10);
    config.poll_interval = std::chrono::milliseconds(200);

    acme_certificate_provider provider(config);

    leaf_certificate_options leaf_opts;
    leaf_opts.subject.common_name = "localhost";
    leaf_opts.dns_names = {"localhost"};
    auto csr = generate_key_and_csr(leaf_opts);

    csr_signing_options sign_opts;
    sign_opts.dns_names = {"localhost"};
    sign_opts.server_auth = true;

    auto material = provider.sign_csr(csr.csr_pem, sign_opts).get();
    BOOST_TEST(!material.certificate_pem.empty());
    BOOST_TEST(material.private_key_pem.empty());  // ACME never sees the CSR's key
    BOOST_TEST(!material.chain_pem.empty());

    auto root_pem = provider.root_certificate_pem().get();
    BOOST_TEST(root_pem == server.root_certificate_pem());

    // Property 18: the obtained certificate chain-verifies against the
    // issuing server's root, exactly like Property 1/7, now reached through
    // the ACME wire protocol.
    auto leaf = load_cert_pem(material.certificate_pem);
    auto root = load_cert_pem(root_pem);
    BOOST_REQUIRE(leaf != nullptr);
    BOOST_REQUIRE(root != nullptr);

    X509_STORE* store = X509_STORE_new();
    X509_STORE_add_cert(store, root.get());
    X509_STORE_CTX* ctx = X509_STORE_CTX_new();
    X509_STORE_CTX_init(ctx, store, leaf.get(), nullptr);
    BOOST_TEST(X509_verify_cert(ctx) == 1);
    X509_STORE_CTX_free(ctx);
    X509_STORE_free(store);

    // Property 19 (success path): the http-01 responder is torn down once
    // sign_csr()'s future settles, not left listening indefinitely.
    httplib::Client probe("127.0.0.1", k_http01_port);
    probe.set_connection_timeout(1, 0);
    auto probe_res = probe.Get("/.well-known/acme-challenge/anything");
    BOOST_TEST(!probe_res);
}

BOOST_AUTO_TEST_CASE(sign_csr_reuses_account_across_calls, *boost::unit_test::timeout(30)) {
    acme_test_server::options server_opts;
    server_opts.http01_validation_port = k_http01_port + 1;
    acme_test_server server{server_opts};

    acme_certificate_provider_config config;
    config.directory_url = server.directory_url();
    config.trust_anchors_pem = server.root_certificate_pem();
    config.http01_bind_address = "127.0.0.1:" + std::to_string(k_http01_port + 1);
    config.poll_timeout = std::chrono::seconds(10);
    config.poll_interval = std::chrono::milliseconds(200);
    acme_certificate_provider provider(config);

    for (const std::string& name : {"localhost", "localhost"}) {
        leaf_certificate_options leaf_opts;
        leaf_opts.subject.common_name = name;
        leaf_opts.dns_names = {name};
        auto csr = generate_key_and_csr(leaf_opts);

        csr_signing_options sign_opts;
        sign_opts.dns_names = {name};
        auto material = provider.sign_csr(csr.csr_pem, sign_opts).get();
        BOOST_TEST(!material.certificate_pem.empty());
    }
}

BOOST_AUTO_TEST_CASE(challenge_failure_rejects_future_and_tears_down_responder,
                     *boost::unit_test::timeout(30)) {
    acme_test_server::options server_opts;
    server_opts.http01_validation_port = k_http01_port + 2;
    acme_test_server server{server_opts};

    acme_certificate_provider_config config;
    config.directory_url = server.directory_url();
    config.trust_anchors_pem = server.root_certificate_pem();
    // Deliberately wrong: the responder binds a DIFFERENT port than the one
    // the server will validate against, so the challenge can never succeed.
    config.http01_bind_address = "127.0.0.1:" + std::to_string(k_http01_port + 3);
    config.poll_timeout = std::chrono::seconds(5);
    config.poll_interval = std::chrono::milliseconds(200);
    acme_certificate_provider provider(config);

    leaf_certificate_options leaf_opts;
    leaf_opts.subject.common_name = "localhost";
    leaf_opts.dns_names = {"localhost"};
    auto csr = generate_key_and_csr(leaf_opts);
    csr_signing_options sign_opts;
    sign_opts.dns_names = {"localhost"};

    BOOST_CHECK_THROW(provider.sign_csr(csr.csr_pem, sign_opts).get(), std::exception);

    // Property 19: the responder is torn down regardless of outcome — the
    // well-known path is no longer reachable shortly after the future settled.
    httplib::Client probe("127.0.0.1", k_http01_port + 3);
    probe.set_connection_timeout(1, 0);
    auto res = probe.Get("/.well-known/acme-challenge/anything");
    BOOST_TEST(!res);  // connection refused — nothing is listening anymore
}

// Negative path: finalizing an order before any of its authorizations have
// validated. Bypasses acme_certificate_provider entirely (it would never do
// this itself) to drive acme_test_server directly with a hand-built JWS —
// the "simulated via a test-only hook" tampered-client scenario task 30
// calls for, applied to premature finalization specifically.
BOOST_AUTO_TEST_CASE(finalize_before_ready_rejected_with_order_not_ready,
                     *boost::unit_test::timeout(15)) {
    acme_test_server server;
    auto key = acme_jws::generate_p256_key();

    auto post_jws = [&](const std::string& url, const std::string& payload) -> httplib::Result {
        auto colon_scheme = url.find("://");
        auto path_start = url.find('/', colon_scheme + 3);
        std::string origin = url.substr(0, path_start);
        std::string path = url.substr(path_start);

        httplib::Client nonce_client(origin);
        auto nonce_res = nonce_client.Get("/new-nonce");
        std::string nonce = nonce_res->get_header_value("Replay-Nonce");

        boost::json::object header;
        header["nonce"] = nonce;
        header["url"] = url;
        static std::optional<std::string> kid;
        if (kid.has_value()) {
            header["kid"] = *kid;
        } else {
            header["jwk"] = acme_jws::jwk_from_public_key(key.get());
        }
        auto compact = acme_jws::sign(payload, header, key.get());
        auto dot1 = compact.find('.');
        auto dot2 = compact.find('.', dot1 + 1);
        boost::json::object flattened;
        flattened["protected"] = compact.substr(0, dot1);
        flattened["payload"] = compact.substr(dot1 + 1, dot2 - dot1 - 1);
        flattened["signature"] = compact.substr(dot2 + 1);

        httplib::Client client(origin);
        auto res = client.Post(path, boost::json::serialize(flattened), "application/json");
        if (res && res->status < 300 && !kid.has_value()) {
            auto loc = res->get_header_value("Location");
            if (!loc.empty() && url.find("/new-account") != std::string::npos) {
                kid = loc;
            }
        }
        return res;
    };

    post_jws(server.base_url() + "/new-account",
             boost::json::serialize(boost::json::object{{"termsOfServiceAgreed", true}}));

    auto order_res = post_jws(server.base_url() + "/new-order",
                              boost::json::serialize(boost::json::object{
                                  {"identifiers", boost::json::array{boost::json::object{
                                                      {"type", "dns"}, {"value", "localhost"}}}}}));
    BOOST_REQUIRE(order_res);
    BOOST_TEST(order_res->status == 201);
    auto order_body = boost::json::parse(order_res->body).as_object();
    std::string finalize_url = std::string(order_body.at("finalize").as_string());

    // No authorization was ever validated — finalize must be rejected.
    auto finalize_res =
        post_jws(finalize_url, boost::json::serialize(boost::json::object{{"csr", "irrelevant"}}));
    BOOST_REQUIRE(finalize_res);
    BOOST_TEST(finalize_res->status == 403);
    auto problem = boost::json::parse(finalize_res->body).as_object();
    BOOST_TEST(problem.at("type").as_string() == "urn:ietf:params:acme:error:orderNotReady");
}

// Negative path: dns-01 against an unreachable DNS server fails closed
// (rejects the future) rather than hanging or silently proceeding as if
// validated. Doesn't require any real DNS infrastructure — the point is
// that a genuinely-unreachable server surfaces as an error.
#ifdef KYTHIRA_HAS_LDNS
BOOST_AUTO_TEST_CASE(dns01_against_unreachable_server_fails_closed,
                     *boost::unit_test::timeout(30)) {
    acme_test_server server;

    acme_certificate_provider_config config;
    config.directory_url = server.directory_url();
    config.trust_anchors_pem = server.root_certificate_pem();
    config.challenge = acme_certificate_provider_config::challenge_type::dns_01;
    config.dns01.server =
        "192.0.2.1";  // TEST-NET-1 (RFC 5737) — guaranteed unreachable, never routed
    config.dns01.port = 53;
    config.dns01.zone = "example.com.";
    config.poll_timeout = std::chrono::seconds(3);
    config.poll_interval = std::chrono::milliseconds(200);

    acme_certificate_provider provider(config);
    leaf_certificate_options leaf_opts;
    leaf_opts.subject.common_name = "dns01-client.example.com";
    leaf_opts.dns_names = {"dns01-client.example.com"};
    auto csr = generate_key_and_csr(leaf_opts);
    csr_signing_options sign_opts;
    sign_opts.dns_names = {"dns01-client.example.com"};

    BOOST_CHECK_THROW(provider.sign_csr(csr.csr_pem, sign_opts).get(), std::exception);
}
#endif

// The trust anchor comes from configuration, never from the server's chain:
// a CA whose root the operator did not configure is refused, even though its
// chain is internally consistent and the order otherwise completes.
BOOST_AUTO_TEST_CASE(chain_to_unconfigured_root_is_rejected, *boost::unit_test::timeout(30)) {
    acme_test_server::options server_opts;
    server_opts.http01_validation_port = k_http01_port + 5;
    acme_test_server server{server_opts};
    acme_test_server other_ca;

    acme_certificate_provider_config config;
    config.directory_url = server.directory_url();
    config.trust_anchors_pem = other_ca.root_certificate_pem();
    config.http01_bind_address = "127.0.0.1:" + std::to_string(k_http01_port + 5);
    config.poll_timeout = std::chrono::seconds(10);
    config.poll_interval = std::chrono::milliseconds(200);
    acme_certificate_provider provider(config);

    BOOST_TEST(provider.root_certificate_pem().get() == other_ca.root_certificate_pem());

    leaf_certificate_options leaf_opts;
    leaf_opts.dns_names = {"localhost"};
    auto csr = generate_key_and_csr(leaf_opts);
    csr_signing_options sign_opts;
    sign_opts.dns_names = {"localhost"};

    BOOST_CHECK_EXCEPTION(provider.sign_csr(csr.csr_pem, sign_opts).get(), std::runtime_error,
                          [](const std::runtime_error& e) {
                              return std::string(e.what()).find("trust anchors") !=
                                     std::string::npos;
                          });
}

BOOST_AUTO_TEST_CASE(leaf_key_not_matching_csr_is_rejected, *boost::unit_test::timeout(30)) {
    acme_test_server::options server_opts;
    server_opts.http01_validation_port = k_http01_port + 6;
    server_opts.substitute_leaf_key = true;
    acme_test_server server{server_opts};

    acme_certificate_provider_config config;
    config.directory_url = server.directory_url();
    config.trust_anchors_pem = server.root_certificate_pem();
    config.http01_bind_address = "127.0.0.1:" + std::to_string(k_http01_port + 6);
    config.poll_timeout = std::chrono::seconds(10);
    config.poll_interval = std::chrono::milliseconds(200);
    acme_certificate_provider provider(config);

    leaf_certificate_options leaf_opts;
    leaf_opts.dns_names = {"localhost"};
    auto csr = generate_key_and_csr(leaf_opts);
    csr_signing_options sign_opts;
    sign_opts.dns_names = {"localhost"};

    BOOST_CHECK_EXCEPTION(provider.sign_csr(csr.csr_pem, sign_opts).get(), std::runtime_error,
                          [](const std::runtime_error& e) {
                              return std::string(e.what()).find("does not match the CSR") !=
                                     std::string::npos;
                          });
}

BOOST_AUTO_TEST_CASE(construction_requires_trust_anchors) {
    acme_test_server server;
    acme_certificate_provider_config config;
    config.directory_url = server.directory_url();
    BOOST_CHECK_THROW(acme_certificate_provider{config}, std::invalid_argument);
    config.trust_anchors_pem = "not a certificate";
    BOOST_CHECK_THROW(acme_certificate_provider{config}, std::invalid_argument);
}

// TLS is verified on every https connection, so the only way to skip it is
// plain http, and that is refused off loopback.
BOOST_AUTO_TEST_CASE(plain_http_is_refused_off_loopback) {
    acme_test_server server;
    acme_certificate_provider_config config;
    config.trust_anchors_pem = server.root_certificate_pem();

    for (const char* url : {"http://acme.example.com/directory", "http://10.0.0.1:14000/dir",
                            "http://[2001:db8::1]:80/dir", "ftp://127.0.0.1/dir"}) {
        config.directory_url = url;
        BOOST_CHECK_THROW(acme_certificate_provider{config}, std::invalid_argument);
    }
    for (const char* url :
         {"https://acme.example.com/directory", "http://127.0.0.1:1/dir", "http://127.8.9.10/dir",
          "http://localhost:1/dir", "http://[::1]:1/dir"}) {
        config.directory_url = url;
        BOOST_CHECK_NO_THROW(acme_certificate_provider{config});
    }
}

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT  // needs httplib::SSLServer
// An https ACME endpoint whose certificate the client cannot verify is not
// contacted further: the self-signed listener below would be accepted by the
// old verification-disabled client.
BOOST_AUTO_TEST_CASE(unverifiable_https_server_is_refused, *boost::unit_test::timeout(30)) {
    certificate_authority tls_ca;
    leaf_certificate_options tls_opts;
    tls_opts.subject.common_name = "localhost";
    tls_opts.dns_names = {"localhost"};
    tls_opts.ip_addresses = {"127.0.0.1"};
    auto tls_material = tls_ca.issue(tls_opts);

    auto cert_path = std::filesystem::temp_directory_path() /
                     ("acme_tls_cert_" + std::to_string(::getpid()) + ".pem");
    auto key_path = std::filesystem::temp_directory_path() /
                    ("acme_tls_key_" + std::to_string(::getpid()) + ".pem");
    std::ofstream(cert_path) << tls_material.certificate_pem;
    std::ofstream(key_path) << tls_material.private_key_pem;

    httplib::SSLServer tls_server(cert_path.c_str(), key_path.c_str());
    std::filesystem::remove(cert_path);
    std::filesystem::remove(key_path);
    BOOST_REQUIRE(tls_server.is_valid());
    tls_server.Get("/directory", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"newNonce":"x","newAccount":"x","newOrder":"x"})", "application/json");
    });
    int port = tls_server.bind_to_any_port("127.0.0.1");
    BOOST_REQUIRE(port > 0);
    std::jthread listener([&] { tls_server.listen_after_bind(); });
    tls_server.wait_until_ready();

    acme_test_server issuing;
    acme_certificate_provider_config config;
    config.directory_url = "https://127.0.0.1:" + std::to_string(port) + "/directory";
    config.trust_anchors_pem = issuing.root_certificate_pem();
    acme_certificate_provider provider(config);

    leaf_certificate_options leaf_opts;
    leaf_opts.dns_names = {"localhost"};
    auto csr = generate_key_and_csr(leaf_opts);
    csr_signing_options sign_opts;
    sign_opts.dns_names = {"localhost"};

    // Untrusted TLS root: the directory GET fails before any ACME exchange.
    BOOST_CHECK_EXCEPTION(provider.sign_csr(csr.csr_pem, sign_opts).get(), std::runtime_error,
                          [](const std::runtime_error& e) {
                              return std::string(e.what()).find("GET directory failed") !=
                                     std::string::npos;
                          });

    // Trusting the listener's CA explicitly gets past the handshake, onto the
    // stub directory's bogus newNonce URL.
    config.server_ca_bundle_pem = tls_ca.root_certificate_pem();
    acme_certificate_provider trusting(config);
    BOOST_CHECK_EXCEPTION(trusting.sign_csr(csr.csr_pem, sign_opts).get(), std::exception,
                          [](const std::exception& e) {
                              return std::string(e.what()).find("GET directory failed") ==
                                     std::string::npos;
                          });

    tls_server.stop();
}
#endif  // CPPHTTPLIB_OPENSSL_SUPPORT

// The http-01 responder defaults to "*:80": a CA validates over whichever
// of the identifier's A/AAAA records it picks, so it must answer on IPv4 and
// IPv6. "*:0" exercises the same path on an ephemeral port.
BOOST_AUTO_TEST_CASE(http01_responder_star_bind_answers_on_ipv4) {
    BOOST_TEST(raft::testing::acme_certificate_provider_config{}.http01_bind_address == "*:80");
    raft::testing::acme_detail::http01_responder responder("*:0", "tok123", "tok123.thumb");
    BOOST_REQUIRE(responder.port() > 0);
    httplib::Client client("127.0.0.1", responder.port());
    auto res = client.Get("/.well-known/acme-challenge/tok123");
    BOOST_REQUIRE(res);
    BOOST_TEST(res->status == 200);
    BOOST_TEST(res->body == "tok123.thumb");
}

// A responder host that /etc/hosts doesn't list fails loudly rather than
// being looked up in DNS.
BOOST_AUTO_TEST_CASE(http01_responder_refuses_unlisted_host) {
    BOOST_CHECK_THROW(
        raft::testing::acme_detail::http01_responder("kythira-test.invalid:0", "t", "t.k"),
        std::runtime_error);
}

// static_assert already lives in acme_certificate_provider.hpp; this
// exercises it via the concept directly too, for a readable failure message
// if it ever regresses.
static_assert(raft::testing::certificate_provider<raft::testing::acme_certificate_provider>);

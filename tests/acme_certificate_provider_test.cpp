#define BOOST_TEST_MODULE acme_certificate_provider_test

#include <boost/test/unit_test.hpp>

#include "acme_test_server.hpp"

#include <raft/acme_certificate_provider.hpp>
#include <raft/acme_certificate_provider_impl.hpp>
#include <raft/certificate_provider.hpp>

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <memory>

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

// static_assert already lives in acme_certificate_provider.hpp; this
// exercises it via the concept directly too, for a readable failure message
// if it ever regresses.
static_assert(raft::testing::certificate_provider<raft::testing::acme_certificate_provider>);

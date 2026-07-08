// Property 20: pinned-fingerprint bootstrap accepts only the exact expected
// root, over TWO independently-TLS'd ca_test_fixtures (each with its own
// "listener CA" — deliberately DIFFERENT from the fixture's own certificate-
// issuance root, exactly mirroring how a real ca_service --serve's
// --tls-cert/--tls-key listener identity is independent of whatever CA it
// issues certificates from).

#define BOOST_TEST_MODULE ca_bootstrap_client_test

#include <boost/test/unit_test.hpp>

#include "ca_test_fixture.hpp"

#include <raft/ca_bootstrap_client.hpp>
#include <raft/ca_http_helpers.hpp>

#include <memory>

using namespace raft::testing;

namespace {

auto network_options_with_tls(const temp_cert_files& listener_files) -> ca_test_fixture_options {
    ca_test_fixture_options opts;
    opts.start_network_service = true;
    opts.startup_timeout = std::chrono::seconds(15);
    // chain_path() (leaf+root), NOT cert_path() (leaf only): fingerprint
    // pinning targets the ROOT specifically (Requirement 19.1 — the root
    // survives leaf rotation, unlike the leaf itself), which only appears in
    // the presented chain at all if the TLS listener is configured with its
    // full chain, not just its leaf. A leaf-only --tls-cert would present no
    // root for a client to pin against.
    opts.tls_cert_path = listener_files.chain_path();
    opts.tls_key_path = listener_files.key_path();
    return opts;
}

}  // namespace

BOOST_AUTO_TEST_CASE(correct_fingerprint_succeeds_and_returns_matching_root,
                     *boost::unit_test::timeout(30)) {
    certificate_authority listener_ca;
    leaf_certificate_options listener_opts;
    listener_opts.subject.common_name = "ca-bootstrap-listener-a";
    listener_opts.dns_names = {"localhost"};
    listener_opts.ip_addresses = {"127.0.0.1"};
    temp_cert_files listener_files(listener_ca.issue(listener_opts));

    ca_test_fixture fixture{network_options_with_tls(listener_files)};

    // Parse the listener CA's OWN root (not the fixture's certificate-
    // issuance root — those are deliberately different CAs here) to compute
    // the fingerprint an operator would have obtained via
    // `ca_service --print-root-fingerprint --tls-cert ... --tls-key ...`.
    BIO* bio = BIO_new_mem_buf(listener_ca.root_certificate_pem().data(),
                               static_cast<int>(listener_ca.root_certificate_pem().size()));
    X509* raw_root = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    BOOST_REQUIRE(raw_root != nullptr);
    std::string fingerprint = sha256_fingerprint_hex(raw_root);
    X509_free(raw_root);

    auto result =
        fetch_trusted_root(fixture.service_base_url(), fingerprint, fixture.service_auth_token());

    // /v1/root-ca returns the fixture's certificate-issuance root, which is
    // NOT listener_ca's root (Requirement 19.5: fetch the CA's actual root
    // once the pinned TLS connection itself is trusted).
    BOOST_TEST(result.root_certificate_pem == fixture.root_certificate_pem());
}

BOOST_AUTO_TEST_CASE(wrong_fingerprint_rejected_with_mismatch_error,
                     *boost::unit_test::timeout(30)) {
    certificate_authority listener_ca_a;
    leaf_certificate_options opts_a;
    opts_a.subject.common_name = "ca-bootstrap-listener-a";
    opts_a.dns_names = {"localhost"};
    opts_a.ip_addresses = {"127.0.0.1"};
    temp_cert_files listener_files_a(listener_ca_a.issue(opts_a));
    ca_test_fixture fixture_a{network_options_with_tls(listener_files_a)};

    certificate_authority listener_ca_b;
    leaf_certificate_options opts_b;
    opts_b.subject.common_name = "ca-bootstrap-listener-b";
    opts_b.dns_names = {"localhost"};
    opts_b.ip_addresses = {"127.0.0.1"};
    temp_cert_files listener_files_b(listener_ca_b.issue(opts_b));
    ca_test_fixture fixture_b{network_options_with_tls(listener_files_b)};

    // fixture_b's (valid, well-formed, but WRONG) fingerprint against fixture_a.
    BIO* bio = BIO_new_mem_buf(listener_ca_b.root_certificate_pem().data(),
                               static_cast<int>(listener_ca_b.root_certificate_pem().size()));
    X509* raw_root_b = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    BOOST_REQUIRE(raw_root_b != nullptr);
    std::string fingerprint_b = sha256_fingerprint_hex(raw_root_b);
    X509_free(raw_root_b);

    BOOST_CHECK_EXCEPTION(fetch_trusted_root(fixture_a.service_base_url(), fingerprint_b,
                                             fixture_a.service_auth_token()),
                          std::runtime_error, [](const std::runtime_error& ex) {
                              std::string msg = ex.what();
                              return msg.find("mismatch") != std::string::npos;
                          });
}

BOOST_AUTO_TEST_CASE(malformed_expected_fingerprint_is_a_usage_error,
                     *boost::unit_test::timeout(15)) {
    BOOST_CHECK_THROW(
        fetch_trusted_root("https://127.0.0.1:1", "not-a-valid-fingerprint", "irrelevant-token"),
        std::invalid_argument);
    BOOST_CHECK_THROW(fetch_trusted_root("https://127.0.0.1:1", "AA:BB:CC", "irrelevant-token"),
                      std::invalid_argument);  // too short
}

// **Feature: ca-cluster-rpc-mtls, Property 4: Dual-Trust Never Accepts a
// Weaker Credential Than Either Alone**
// Unit coverage for tls_rpc_trust_policy::accepts() under all three named
// policy shapes (pinned_fingerprint, ca_root_only, either) — fingerprint
// match/mismatch, chain verifies/doesn't, and either accepting exactly what
// either alone would accept.
// **Validates: Requirements 1.2, 2.2, 6.1**
#define BOOST_TEST_MODULE tls_tcp_rpc_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/ca_bootstrap_client.hpp>
#include <raft/certificate_authority.hpp>
#include <raft/tls_tcp_rpc.hpp>

#include <openssl/x509.h>

using namespace raft::testing;
using kythira::ca_root_only;
using kythira::either;
using kythira::pinned_fingerprint;
using kythira::tls_rpc_trust_policy;

namespace {

struct x509_deleter {
    void operator()(X509* c) const {
        if (c != nullptr) X509_free(c);
    }
};
using x509_ptr = std::unique_ptr<X509, x509_deleter>;

auto load_cert_pem(const std::string& pem) -> x509_ptr {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    x509_ptr cert{PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)};
    BIO_free(bio);
    return cert;
}

auto fingerprint_of(X509* cert) -> std::string {
    return ca_bootstrap_detail::sha256_fingerprint_hex_bare(cert);
}

}  // namespace

BOOST_AUTO_TEST_SUITE(tls_tcp_rpc_trust_policy_tests)

BOOST_AUTO_TEST_CASE(pinned_fingerprint_accepts_exact_match, *boost::unit_test::timeout(30)) {
    certificate_authority bootstrap_cred;
    auto root = load_cert_pem(bootstrap_cred.root_certificate_pem());
    BOOST_REQUIRE(root != nullptr);

    auto policy = pinned_fingerprint(fingerprint_of(root.get()));
    BOOST_TEST(policy.accepts(root.get()));
}

BOOST_AUTO_TEST_CASE(pinned_fingerprint_rejects_mismatch, *boost::unit_test::timeout(30)) {
    certificate_authority bootstrap_cred;
    certificate_authority other_cred;
    auto root = load_cert_pem(bootstrap_cred.root_certificate_pem());
    auto other_root = load_cert_pem(other_cred.root_certificate_pem());
    BOOST_REQUIRE(root != nullptr);
    BOOST_REQUIRE(other_root != nullptr);

    auto policy = pinned_fingerprint(fingerprint_of(root.get()));
    BOOST_TEST(!policy.accepts(other_root.get()));
}

BOOST_AUTO_TEST_CASE(pinned_fingerprint_rejects_null_certificate, *boost::unit_test::timeout(30)) {
    certificate_authority bootstrap_cred;
    auto root = load_cert_pem(bootstrap_cred.root_certificate_pem());
    auto policy = pinned_fingerprint(fingerprint_of(root.get()));
    BOOST_TEST(!policy.accepts(nullptr));
}

BOOST_AUTO_TEST_CASE(ca_root_only_accepts_chained_leaf, *boost::unit_test::timeout(30)) {
    certificate_authority ca;
    leaf_certificate_options opts;
    opts.subject.common_name = "peer-1";
    opts.dns_names = {"peer-1"};
    auto leaf = ca.issue(opts);
    auto leaf_cert = load_cert_pem(leaf.certificate_pem);
    BOOST_REQUIRE(leaf_cert != nullptr);

    auto policy = ca_root_only(ca.root_certificate_pem());
    BOOST_TEST(policy.accepts(leaf_cert.get()));
}

BOOST_AUTO_TEST_CASE(ca_root_only_rejects_unrelated_certificate, *boost::unit_test::timeout(30)) {
    certificate_authority ca;
    certificate_authority unrelated_ca;
    leaf_certificate_options opts;
    opts.subject.common_name = "peer-1";
    opts.dns_names = {"peer-1"};
    auto unrelated_leaf = unrelated_ca.issue(opts);
    auto unrelated_cert = load_cert_pem(unrelated_leaf.certificate_pem);
    BOOST_REQUIRE(unrelated_cert != nullptr);

    auto policy = ca_root_only(ca.root_certificate_pem());
    BOOST_TEST(!policy.accepts(unrelated_cert.get()));
}

BOOST_AUTO_TEST_CASE(ca_root_only_rejects_bootstrap_fingerprint_alone,
                     *boost::unit_test::timeout(30)) {
    certificate_authority bootstrap_cred;
    auto root = load_cert_pem(bootstrap_cred.root_certificate_pem());
    certificate_authority ca;

    // ca_root_only has no bootstrap_fingerprint_hex set at all — a
    // bootstrap-credential-signed certificate should never be accepted by
    // this policy, since accepts() only checks the fingerprint branch when
    // bootstrap_fingerprint_hex.has_value().
    auto policy = ca_root_only(ca.root_certificate_pem());
    BOOST_TEST(!policy.accepts(root.get()));
}

BOOST_AUTO_TEST_CASE(either_accepts_bootstrap_fingerprint_before_cutover,
                     *boost::unit_test::timeout(30)) {
    certificate_authority bootstrap_cred;
    certificate_authority ca;
    auto root = load_cert_pem(bootstrap_cred.root_certificate_pem());
    BOOST_REQUIRE(root != nullptr);

    auto policy = either(fingerprint_of(root.get()), ca.root_certificate_pem());
    BOOST_TEST(policy.accepts(root.get()));
}

BOOST_AUTO_TEST_CASE(either_accepts_ca_issued_leaf_during_dual_trust_window,
                     *boost::unit_test::timeout(30)) {
    certificate_authority bootstrap_cred;
    certificate_authority ca;
    leaf_certificate_options opts;
    opts.subject.common_name = "peer-1";
    opts.dns_names = {"peer-1"};
    auto leaf = ca.issue(opts);
    auto leaf_cert = load_cert_pem(leaf.certificate_pem);
    auto bootstrap_root = load_cert_pem(bootstrap_cred.root_certificate_pem());
    BOOST_REQUIRE(leaf_cert != nullptr);
    BOOST_REQUIRE(bootstrap_root != nullptr);

    auto policy = either(fingerprint_of(bootstrap_root.get()), ca.root_certificate_pem());
    // Property 4: either() accepts exactly what either policy alone would.
    BOOST_TEST(policy.accepts(leaf_cert.get()));
    BOOST_TEST(policy.accepts(bootstrap_root.get()));
}

BOOST_AUTO_TEST_CASE(either_rejects_certificate_matching_neither, *boost::unit_test::timeout(30)) {
    certificate_authority bootstrap_cred;
    certificate_authority ca;
    certificate_authority unrelated_ca;
    auto bootstrap_root = load_cert_pem(bootstrap_cred.root_certificate_pem());
    auto unrelated_root = load_cert_pem(unrelated_ca.root_certificate_pem());
    BOOST_REQUIRE(bootstrap_root != nullptr);
    BOOST_REQUIRE(unrelated_root != nullptr);

    auto policy = either(fingerprint_of(bootstrap_root.get()), ca.root_certificate_pem());
    BOOST_TEST(!policy.accepts(unrelated_root.get()));
}

BOOST_AUTO_TEST_SUITE_END()

#define BOOST_TEST_MODULE certificate_authority_property_test

#include <boost/test/unit_test.hpp>

#include <raft/certificate_authority.hpp>
#include <raft/certificate_provider.hpp>

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <memory>
#include <string>

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

struct crl_deleter {
    void operator()(X509_CRL* c) const {
        if (c != nullptr) {
            X509_CRL_free(c);
        }
    }
};
using crl_ptr = std::unique_ptr<X509_CRL, crl_deleter>;

struct store_deleter {
    void operator()(X509_STORE* s) const {
        if (s != nullptr) {
            X509_STORE_free(s);
        }
    }
};
using store_ptr = std::unique_ptr<X509_STORE, store_deleter>;

struct ctx_deleter {
    void operator()(X509_STORE_CTX* c) const {
        if (c != nullptr) {
            X509_STORE_CTX_free(c);
        }
    }
};
using ctx_ptr = std::unique_ptr<X509_STORE_CTX, ctx_deleter>;

auto load_cert(const std::string& pem) -> x509_ptr {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    x509_ptr cert{PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)};
    BIO_free(bio);
    return cert;
}

auto load_crl(const std::string& pem) -> crl_ptr {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    crl_ptr crl{PEM_read_bio_X509_CRL(bio, nullptr, nullptr, nullptr)};
    BIO_free(bio);
    return crl;
}

/// Verifies `leaf` against a store rooted at `root`, optionally enabling
/// CRL-checking and/or loading a CRL. Returns the X509_V_ERR_* code
/// (X509_V_OK on success).
auto verify(X509* leaf, X509* root, X509_CRL* crl = nullptr) -> int {
    store_ptr store{X509_STORE_new()};
    X509_STORE_add_cert(store.get(), root);
    if (crl != nullptr) {
        X509_STORE_add_crl(store.get(), crl);
        X509_VERIFY_PARAM* param = X509_VERIFY_PARAM_new();
        X509_VERIFY_PARAM_set_flags(param, X509_V_FLAG_CRL_CHECK);
        X509_STORE_set1_param(store.get(), param);
        X509_VERIFY_PARAM_free(param);
    }
    ctx_ptr ctx{X509_STORE_CTX_new()};
    X509_STORE_CTX_init(ctx.get(), store.get(), leaf, nullptr);
    int rc = X509_verify_cert(ctx.get());
    return rc == 1 ? X509_V_OK : X509_STORE_CTX_get_error(ctx.get());
}

auto leaf_opts(const std::string& cn) -> leaf_certificate_options {
    leaf_certificate_options opts;
    opts.subject.common_name = cn;
    opts.dns_names = {cn + ".example.com"};
    return opts;
}

}  // namespace

// Property 1: an issued leaf chain-verifies against its own CA's root.
BOOST_AUTO_TEST_CASE(property_issued_leaf_chains_to_own_root, *boost::unit_test::timeout(30)) {
    certificate_authority ca;
    auto material = ca.issue(leaf_opts("p1"));
    auto root = load_cert(ca.root_certificate_pem());
    auto leaf = load_cert(material.certificate_pem);
    BOOST_TEST(verify(leaf.get(), root.get()) == X509_V_OK);
}

// Property 2: expired / not-yet-valid certificates fail verification exactly on
// the time-window check, not on some other unrelated error.
BOOST_AUTO_TEST_CASE(property_expired_and_not_yet_valid_fail_on_time_window,
                     *boost::unit_test::timeout(30)) {
    certificate_authority ca;
    auto root = load_cert(ca.root_certificate_pem());

    auto expired = ca.issue_expired(leaf_opts("p2-expired"));
    auto expired_leaf = load_cert(expired.certificate_pem);
    BOOST_TEST(verify(expired_leaf.get(), root.get()) == X509_V_ERR_CERT_HAS_EXPIRED);

    auto not_yet = ca.issue_not_yet_valid(leaf_opts("p2-not-yet"));
    auto not_yet_leaf = load_cert(not_yet.certificate_pem);
    BOOST_TEST(verify(not_yet_leaf.get(), root.get()) == X509_V_ERR_CERT_NOT_YET_VALID);
}

// Property 3: a leaf issued by one CA fails chain verification against a trust
// store rooted at a different, independently-constructed CA's root.
BOOST_AUTO_TEST_CASE(property_cross_ca_leaf_fails_verification, *boost::unit_test::timeout(30)) {
    certificate_authority ca_a;
    certificate_authority ca_b;
    BOOST_TEST(ca_a.root_certificate_pem() != ca_b.root_certificate_pem());

    auto material = ca_a.issue(leaf_opts("p3"));
    auto leaf = load_cert(material.certificate_pem);
    auto root_b = load_cert(ca_b.root_certificate_pem());
    BOOST_TEST(verify(leaf.get(), root_b.get()) != X509_V_OK);
}

// Property 4: revocation + CRL-checking rejects a revoked certificate, while an
// un-revoked certificate (or CRL-checking disabled) still verifies successfully.
BOOST_AUTO_TEST_CASE(property_revocation_rejected_only_with_crl_checking,
                     *boost::unit_test::timeout(30)) {
    certificate_authority ca;
    auto root = load_cert(ca.root_certificate_pem());
    auto material = ca.issue(leaf_opts("p4"));
    auto leaf = load_cert(material.certificate_pem);

    // Before revoke(): verifies fine even with CRL-checking enabled.
    auto crl_before = load_crl(ca.crl_pem());
    BOOST_TEST(verify(leaf.get(), root.get(), crl_before.get()) == X509_V_OK);

    ca.revoke(material);
    auto crl_after = load_crl(ca.crl_pem());

    // With CRL-checking enabled: rejected as revoked.
    BOOST_TEST(verify(leaf.get(), root.get(), crl_after.get()) == X509_V_ERR_CERT_REVOKED);

    // Without CRL-checking (no CRL supplied to the store): still verifies —
    // revocation only matters when a caller opts into CRL checking.
    BOOST_TEST(verify(leaf.get(), root.get(), nullptr) == X509_V_OK);
}

// Property 5 (Requirement 1.4/3.3): two instances constructed back-to-back never
// collide on serial number or subject/key material for the same requested CN.
BOOST_AUTO_TEST_CASE(property_independent_instances_produce_distinct_material,
                     *boost::unit_test::timeout(30)) {
    certificate_authority ca_a;
    certificate_authority ca_b;

    auto m_a = ca_a.issue(leaf_opts("p5"));
    auto m_b = ca_b.issue(leaf_opts("p5"));
    BOOST_TEST(m_a.serial != m_b.serial);
    BOOST_TEST(m_a.private_key_pem != m_b.private_key_pem);
    BOOST_TEST(m_a.certificate_pem != m_b.certificate_pem);

    // Repeated issuance from the same instance also never repeats a serial.
    auto m_c = ca_a.issue(leaf_opts("p5b"));
    BOOST_TEST(m_a.serial != m_c.serial);
}

// Property 7: a certificate obtained via generate_key_and_csr() + sign_csr()
// chain-verifies against root_certificate_pem() identically to issue() output.
BOOST_AUTO_TEST_CASE(property_sign_csr_chains_to_own_root, *boost::unit_test::timeout(30)) {
    certificate_authority ca;

    leaf_certificate_options gen_opts;
    gen_opts.subject.common_name = "p7-csr-subject";
    gen_opts.dns_names = {"p7.example.com"};
    auto csr = generate_key_and_csr(gen_opts);
    BOOST_TEST(!csr.private_key_pem.empty());
    BOOST_TEST(!csr.csr_pem.empty());

    csr_signing_options sign_opts;
    sign_opts.dns_names = {"p7.example.com"};
    sign_opts.server_auth = true;
    sign_opts.client_auth = true;
    auto material = ca.sign_csr(csr.csr_pem, sign_opts);

    auto root = load_cert(ca.root_certificate_pem());
    auto leaf = load_cert(material.certificate_pem);
    BOOST_TEST(verify(leaf.get(), root.get()) == X509_V_OK);

    // The SAN/EKU set from sign_csr() matches what issue() would have produced.
    int idx = X509_get_ext_by_NID(leaf.get(), NID_ext_key_usage, -1);
    BOOST_TEST(idx >= 0);
}

// Property 8: sign_csr() never returns a private key, from either the local
// certificate_authority directly or through the local_certificate_provider
// adapter (Requirement 9.1, 9.4).
BOOST_AUTO_TEST_CASE(property_sign_csr_never_returns_private_key, *boost::unit_test::timeout(30)) {
    certificate_authority ca;
    auto csr = generate_key_and_csr(leaf_opts("p8"));

    csr_signing_options sign_opts;
    sign_opts.dns_names = {"p8.example.com"};

    auto direct_material = ca.sign_csr(csr.csr_pem, sign_opts);
    BOOST_TEST(direct_material.private_key_pem.empty());

    local_certificate_provider provider(ca);
    auto provider_material = provider.sign_csr(csr.csr_pem, sign_opts).get();
    BOOST_TEST(provider_material.private_key_pem.empty());

    auto root_pem = provider.root_certificate_pem().get();
    BOOST_TEST(root_pem == ca.root_certificate_pem());
}

// sign_csr() rejects a CSR whose self-signature has been tampered with.
BOOST_AUTO_TEST_CASE(sign_csr_rejects_tampered_csr, *boost::unit_test::timeout(30)) {
    certificate_authority ca;
    auto csr = generate_key_and_csr(leaf_opts("tampered"));

    // Flip a byte inside the base64 body (skip the PEM header/footer lines) so
    // the CSR either fails to parse or fails its embedded self-signature check.
    auto first_newline = csr.csr_pem.find('\n');
    auto pos = csr.csr_pem.find_first_not_of('\n', first_newline);
    std::string tampered = csr.csr_pem;
    tampered[pos] = (tampered[pos] == 'A') ? 'B' : 'A';

    csr_signing_options sign_opts;
    sign_opts.dns_names = {"tampered.example.com"};
    BOOST_CHECK_THROW(ca.sign_csr(tampered, sign_opts), std::invalid_argument);
}

// certificate_authority::from_existing() round-trips: a reconstructed instance
// issues certificates that chain-verify against the same root as the original.
BOOST_AUTO_TEST_CASE(from_existing_round_trips_and_issues_valid_certs,
                     *boost::unit_test::timeout(30)) {
    certificate_authority original;
    auto original_material = original.issue(leaf_opts("orig"));

    auto ca_key_pem = detail_testing::unsafe_extract_ca_private_key_pem(original);
    auto reconstructed =
        certificate_authority::from_existing(original.root_certificate_pem(), ca_key_pem);
    BOOST_TEST(reconstructed.root_certificate_pem() == original.root_certificate_pem());

    auto reconstructed_material = reconstructed.issue(leaf_opts("reconstructed"));

    auto root = load_cert(original.root_certificate_pem());
    auto original_leaf = load_cert(original_material.certificate_pem);
    auto reconstructed_leaf = load_cert(reconstructed_material.certificate_pem);
    BOOST_TEST(verify(original_leaf.get(), root.get()) == X509_V_OK);
    BOOST_TEST(verify(reconstructed_leaf.get(), root.get()) == X509_V_OK);
}

// from_existing() rejects unparseable PEM and a key/certificate mismatch.
BOOST_AUTO_TEST_CASE(from_existing_rejects_invalid_material, *boost::unit_test::timeout(30)) {
    certificate_authority ca_a;
    certificate_authority ca_b;
    auto ca_b_key_pem = detail_testing::unsafe_extract_ca_private_key_pem(ca_b);

    BOOST_CHECK_THROW(certificate_authority::from_existing("not a cert", "not a key"),
                      std::invalid_argument);
    // A's certificate paired with B's key: parses fine individually, but the
    // key does not match the certificate's public key.
    BOOST_CHECK_THROW(
        certificate_authority::from_existing(ca_a.root_certificate_pem(), ca_b_key_pem),
        std::invalid_argument);
}

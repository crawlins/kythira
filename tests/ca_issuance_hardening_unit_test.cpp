// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE ca_issuance_hardening_unit_test
#include <boost/test/unit_test.hpp>

// Regression tests for the OpenBao/Vault-style issuance chain analysed for
// Kythira: caller-controlled SAN smuggling, renew re-subjecting, unbounded
// validity, revocation bypass on renew, and Raft peer identities obtainable
// with only the client bearer token.

#include <raft/ca_http_helpers.hpp>
#include <raft/certificate_authority.hpp>
#include <raft/certificate_provider.hpp>
#include <raft/csr_policy.hpp>
#include <raft/tls_tcp_rpc.hpp>

#include <boost/json.hpp>

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <memory>
#include <utility>
#include <set>
#include <string>
#include <vector>

using namespace raft::testing;

namespace {

struct x509_free_t {
    void operator()(X509* c) const noexcept { X509_free(c); }
};
using cert_ptr = std::unique_ptr<X509, x509_free_t>;

auto load(const std::string& pem) -> cert_ptr {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    X509* c = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    BOOST_REQUIRE(c != nullptr);
    return cert_ptr{c};
}

// Every SAN in `cert` as (GEN_* type, value) pairs.
auto san_types(X509* cert) -> std::vector<int> {
    std::vector<int> types;
    auto* sans =
        static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
    if (sans == nullptr) return types;
    for (int i = 0; i < sk_GENERAL_NAME_num(sans); ++i) {
        types.push_back(sk_GENERAL_NAME_value(sans, i)->type);
    }
    GENERAL_NAMES_free(sans);
    return types;
}

auto csr_for(const std::string& cn, const std::string& dns) -> csr_material {
    leaf_certificate_options opts;
    opts.subject.common_name = cn;
    opts.dns_names = {dns};
    return generate_key_and_csr(opts);
}

}  // namespace

// ── SAN handling ────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(sign_csr_rejects_comma_smuggled_uri_san, *boost::unit_test::timeout(30)) {
    certificate_authority ca;
    auto csr = csr_for("victim", "victim.example.com");
    csr_signing_options opts;
    opts.dns_names = {"x.example.com,URI:spiffe://prod/admin"};
    BOOST_CHECK_THROW((void)ca.sign_csr(csr.csr_pem, opts), std::invalid_argument);

    opts.dns_names = {"x.example.com"};
    opts.ip_addresses = {"10.0.0.1,email:root@corp"};
    BOOST_CHECK_THROW((void)ca.sign_csr(csr.csr_pem, opts), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(issue_rejects_malformed_names, *boost::unit_test::timeout(30)) {
    certificate_authority ca;
    for (const char* bad : {"", "-leading.example", "trailing-.example", "a..b", "sp ace",
                            "otherName:1.2.3;UTF8:x", "bad/char"}) {
        leaf_certificate_options opts;
        opts.dns_names = {bad};
        BOOST_CHECK_THROW((void)ca.issue(opts), std::invalid_argument);
    }
    leaf_certificate_options ip_opts;
    ip_opts.ip_addresses = {"999.1.1.1"};
    BOOST_CHECK_THROW((void)ca.issue(ip_opts), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(issued_cert_carries_only_requested_dns_and_ip,
                     *boost::unit_test::timeout(30)) {
    certificate_authority ca;
    auto csr = csr_for("svc", "svc.example.com");
    csr_signing_options opts;
    opts.dns_names = {"svc.example.com", "*.svc.example.com", "localhost"};
    opts.ip_addresses = {"127.0.0.1", "::1"};
    auto material = ca.sign_csr(csr.csr_pem, opts);
    auto cert = load(material.certificate_pem);
    auto types = san_types(cert.get());
    BOOST_REQUIRE(types.size() == 5);
    for (int t : types) {
        BOOST_TEST((t == GEN_DNS || t == GEN_IPADD));
    }
    BOOST_TEST(cert_has_dns_san_in(cert.get(), {"*.svc.example.com"}));
}

// ── validity_days ────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(validity_days_is_bounded, *boost::unit_test::timeout(30)) {
    auto parse = [](const char* json) {
        return parse_validity_days(boost::json::parse(json).as_object());
    };
    BOOST_TEST(!parse("{}").has_value());
    BOOST_TEST(parse(R"({"validity_days":30})").value() == std::chrono::hours(24 * 30));
    BOOST_TEST(parse(R"({"validity_days":825})").has_value());
    BOOST_CHECK_THROW(parse(R"({"validity_days":0})"), std::invalid_argument);
    BOOST_CHECK_THROW(parse(R"({"validity_days":-5})"), std::invalid_argument);
    BOOST_CHECK_THROW(parse(R"({"validity_days":826})"), std::invalid_argument);
    BOOST_CHECK_THROW(parse(R"({"validity_days":100000000000})"), std::invalid_argument);
    BOOST_CHECK_THROW(parse(R"({"validity_days":18446744073709551615})"), std::invalid_argument);
    BOOST_CHECK_THROW(parse(R"({"validity_days":1.5})"), std::invalid_argument);
    BOOST_CHECK_THROW(parse(R"({"validity_days":"30"})"), std::invalid_argument);
}

// ── renew helpers ───────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(renew_csr_subject_must_match_presented, *boost::unit_test::timeout(30)) {
    certificate_authority ca;
    leaf_certificate_options opts;
    opts.subject.common_name = "low-value-client";
    opts.dns_names = {"client.example.com"};
    auto presented = load(ca.issue(opts).certificate_pem);

    BOOST_TEST(csr_subject_matches_cert(presented.get(),
                                        csr_for("low-value-client", "x.example").csr_pem));
    BOOST_TEST(!csr_subject_matches_cert(presented.get(), csr_for("admin", "x.example").csr_pem));
    BOOST_TEST(!csr_subject_matches_cert(presented.get(), "not a csr"));
}

BOOST_AUTO_TEST_CASE(revoked_cert_is_found_in_crl, *boost::unit_test::timeout(30)) {
    certificate_authority ca;
    leaf_certificate_options opts;
    opts.dns_names = {"a.example.com"};
    auto revoked = ca.issue(opts);
    auto kept = ca.issue(opts);
    ca.revoke(revoked);
    auto crl = ca.crl_pem();

    BOOST_TEST(cert_revoked_in_crl(load(revoked.certificate_pem).get(), crl));
    BOOST_TEST(!cert_revoked_in_crl(load(kept.certificate_pem).get(), crl));
    // Fail closed on an unusable CRL.
    BOOST_TEST(cert_revoked_in_crl(load(kept.certificate_pem).get(), "garbage"));
    BOOST_TEST(cert_serial_u64(load(kept.certificate_pem).get()).value() == kept.serial);
}

// ── Raft peer enrollment ─────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(reserved_peer_names_are_case_insensitive, *boost::unit_test::timeout(30)) {
    BOOST_TEST(is_reserved_peer_dns_name("ca-cluster-node-1"));
    BOOST_TEST(is_reserved_peer_dns_name("CA-Cluster-Node-99"));
    BOOST_TEST(!is_reserved_peer_dns_name("ca-cluster-node"));
    BOOST_TEST(!is_reserved_peer_dns_name("my-ca-cluster-node-1"));
    BOOST_TEST(peer_identity_dns_name(7) == "ca-cluster-node-7");
}

BOOST_AUTO_TEST_CASE(peer_enrollment_requires_mac, *boost::unit_test::timeout(30)) {
    const std::set<std::uint64_t> ids{1, 2, 3};
    const auto key = derive_peer_enrollment_key("unseal-passphrase");
    BOOST_TEST(key != "unseal-passphrase");
    const std::string csr = "-----BEGIN CERTIFICATE REQUEST-----\nabc\n";

    csr_signing_options client;
    client.dns_names = {"client.example.com"};
    // Ordinary issuance: untouched.
    BOOST_TEST(classify_peer_enrollment(client, std::nullopt, "", ids, key, csr).allowed);

    csr_signing_options peer;
    peer.dns_names = {"ca-cluster-node-2"};
    auto good_mac = peer_enrollment_mac(key, 2, csr);

    // Bearer-token-only caller asking for a peer name: refused.
    BOOST_TEST(!classify_peer_enrollment(peer, std::nullopt, "", ids, key, csr).allowed);
    BOOST_TEST(!classify_peer_enrollment(peer, 2, "", ids, key, csr).allowed);
    BOOST_TEST(!classify_peer_enrollment(peer, 2, std::string(64, '0'), ids, key, csr).allowed);
    // MAC for a different CSR / node / key: refused.
    BOOST_TEST(
        !classify_peer_enrollment(peer, 2, peer_enrollment_mac(key, 2, csr + "x"), ids, key, csr)
             .allowed);
    BOOST_TEST(!classify_peer_enrollment(
                    peer, 2, peer_enrollment_mac(derive_peer_enrollment_key("other"), 2, csr), ids,
                    key, csr)
                    .allowed);
    // Correct MAC but wrong / extra names, or a non-member node id: refused.
    csr_signing_options other_node;
    other_node.dns_names = {"ca-cluster-node-3"};
    BOOST_TEST(!classify_peer_enrollment(other_node, 2, good_mac, ids, key, csr).allowed);
    csr_signing_options extra = peer;
    extra.dns_names.push_back("admin.example.com");
    BOOST_TEST(!classify_peer_enrollment(extra, 2, good_mac, ids, key, csr).allowed);
    csr_signing_options outsider;
    outsider.dns_names = {"ca-cluster-node-9"};
    BOOST_TEST(
        !classify_peer_enrollment(outsider, 9, peer_enrollment_mac(key, 9, csr), ids, key, csr)
             .allowed);
    // rpc_tls_ready_node_id on an ordinary request also needs the MAC.
    BOOST_TEST(!classify_peer_enrollment(client, 2, "", ids, key, csr).allowed);

    auto ok = classify_peer_enrollment(peer, 2, good_mac, ids, key, csr);
    BOOST_TEST(ok.allowed);
    BOOST_TEST(ok.node_id.value() == 2u);
}

BOOST_AUTO_TEST_CASE(root_mac_binds_nonce_and_body, *boost::unit_test::timeout(30)) {
    const auto key = derive_peer_enrollment_key("unseal-passphrase");
    auto nonce = random_nonce_hex();
    BOOST_TEST(is_well_formed_nonce(nonce));
    BOOST_TEST(nonce != random_nonce_hex());
    BOOST_TEST(!is_well_formed_nonce(""));
    BOOST_TEST(!is_well_formed_nonce("zz\nroot"));
    auto mac = peer_root_mac(key, nonce, "root-pem");
    BOOST_TEST(constant_time_equals(mac, peer_root_mac(key, nonce, "root-pem")));
    BOOST_TEST(!constant_time_equals(mac, peer_root_mac(key, nonce, "attacker-root")));
    BOOST_TEST(!constant_time_equals(mac, peer_root_mac(key, random_nonce_hex(), "root-pem")));
}

BOOST_AUTO_TEST_CASE(rpc_trust_requires_peer_name_for_ca_certs, *boost::unit_test::timeout(30)) {
    certificate_authority ca;
    leaf_certificate_options client_opts;
    client_opts.dns_names = {"client.example.com"};
    auto client = load(ca.issue(client_opts).certificate_pem);
    leaf_certificate_options peer_opts;
    peer_opts.dns_names = {peer_identity_dns_name(2)};
    auto peer = load(ca.issue(peer_opts).certificate_pem);

    auto unrestricted = kythira::ca_root_only(ca.root_certificate_pem());
    BOOST_TEST(unrestricted.accepts(client.get()));  // historical behaviour

    auto restricted = unrestricted.requiring_peer_names(
        {peer_identity_dns_name(1), peer_identity_dns_name(2), peer_identity_dns_name(3)});
    BOOST_TEST(!restricted.accepts(client.get()));
    BOOST_TEST(restricted.accepts(peer.get()));

    certificate_authority other_ca;
    auto foreign_peer = load(other_ca.issue(peer_opts).certificate_pem);
    BOOST_TEST(!restricted.accepts(foreign_peer.get()));
}

// ── CSR policy for CSR-authoritative providers (AWS/GCP/OCI) ────────────────

namespace {

// A CSR requesting exactly `exts` (OpenSSL config-syntax values; test input
// only), signed with a fresh P-256 key.
auto csr_with_extensions(const std::vector<std::pair<int, std::string>>& exts) -> std::string {
    EVP_PKEY* key = EVP_EC_gen("P-256");
    BOOST_REQUIRE(key != nullptr);
    X509_REQ* req = X509_REQ_new();
    X509_NAME* name = X509_NAME_new();
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("csr-policy"), -1, -1, 0);
    X509_REQ_set_subject_name(req, name);
    X509_NAME_free(name);
    X509_REQ_set_pubkey(req, key);
    if (!exts.empty()) {
        STACK_OF(X509_EXTENSION)* stack = sk_X509_EXTENSION_new_null();
        X509V3_CTX ctx;
        X509V3_set_ctx_nodb(&ctx);
        X509V3_set_ctx(&ctx, nullptr, nullptr, req, nullptr, 0);
        for (const auto& [nid, value] : exts) {
            X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value.c_str());
            BOOST_REQUIRE(ext != nullptr);
            sk_X509_EXTENSION_push(stack, ext);
        }
        X509_REQ_add_extensions(req, stack);
        sk_X509_EXTENSION_pop_free(stack, X509_EXTENSION_free);
    }
    X509_REQ_sign(req, key, EVP_sha256());
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509_REQ(bio, req);
    char* data = nullptr;
    long len = BIO_get_mem_data(bio, &data);  // NOLINT(google-runtime-int)
    std::string pem(data, static_cast<std::size_t>(len));
    BIO_free(bio);
    X509_REQ_free(req);
    EVP_PKEY_free(key);
    return pem;
}

auto approved() -> csr_signing_options {
    csr_signing_options o;
    o.dns_names = {"svc.example.com", "alt.example.com"};
    o.ip_addresses = {"10.0.0.7"};
    o.server_auth = true;
    o.client_auth = false;
    return o;
}

}  // namespace

BOOST_AUTO_TEST_CASE(csr_policy_accepts_approved_requests, *boost::unit_test::timeout(30)) {
    BOOST_CHECK_NO_THROW(enforce_csr_matches_options(csr_with_extensions({}), approved()));
    BOOST_CHECK_NO_THROW(enforce_csr_matches_options(
        csr_with_extensions({{NID_subject_alt_name, "DNS:SVC.example.com,IP:10.0.0.7"},
                             {NID_basic_constraints, "CA:FALSE"},
                             {NID_key_usage, "digitalSignature,keyEncipherment"},
                             {NID_ext_key_usage, "serverAuth"}}),
        approved()));
}

BOOST_AUTO_TEST_CASE(csr_policy_rejects_unapproved_requests, *boost::unit_test::timeout(30)) {
    const std::vector<std::vector<std::pair<int, std::string>>> bad = {
        {{NID_subject_alt_name, "DNS:svc.example.com,DNS:admin.example.com"}},
        {{NID_subject_alt_name, "URI:spiffe://prod/admin"}},
        {{NID_subject_alt_name, "email:root@example.com"}},
        {{NID_subject_alt_name, "IP:10.0.0.8"}},
        {{NID_basic_constraints, "critical,CA:TRUE"}},
        {{NID_key_usage, "keyCertSign"}},
        {{NID_ext_key_usage, "clientAuth"}},  // not approved: client_auth=false
        {{NID_ext_key_usage, "codeSigning"}},
        {{NID_name_constraints, "permitted;DNS:example.com"}},
    };
    for (const auto& exts : bad) {
        BOOST_CHECK_THROW(enforce_csr_matches_options(csr_with_extensions(exts), approved()),
                          std::invalid_argument);
    }
    BOOST_CHECK_THROW(enforce_csr_matches_options("not a csr", approved()), std::invalid_argument);
}

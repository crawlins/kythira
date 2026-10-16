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

#include "wide_serial_test_support.hpp"

#include <boost/json.hpp>

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace raft::testing;

// Optionals compared in BOOST_TEST have no operator<<.
BOOST_TEST_DONT_PRINT_LOG_VALUE(std::optional<raft::testing::certificate_serial>)
BOOST_TEST_DONT_PRINT_LOG_VALUE(std::optional<std::uint64_t>)

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
    if (sans == nullptr) {
        return types;
    }
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
    BOOST_TEST(cert_serial_u64(load(kept.certificate_pem).get()).value() ==
               kept.serial.to_u64().value());
    BOOST_TEST(serial_of(load(kept.certificate_pem).get()) == kept.serial);
}

// ── Wide serials (.kiro/specs/cloud-composite-node-ids/ Requirement 12.5) ────

// The renew handlers' CRL check and serial lookup work at full width: a
// cloud CA's 20-octet serial, revoked through the ledger replay path, is
// found in the CRL, and is read back whole rather than as "does not fit".
BOOST_AUTO_TEST_CASE(twenty_octet_serial_is_revocable_and_found_in_crl,
                     *boost::unit_test::timeout(30)) {
    certificate_authority ca;
    csr_signing_options signing;
    signing.dns_names = {"wide.example.com"};
    const auto wide = kythira::testing::twenty_octet_serial();
    auto material = kythira::testing::sign_csr_with_serial(
        ca, csr_for("wide", "wide.example.com").csr_pem, signing, wide);
    auto cert = load(material.certificate_pem);

    BOOST_TEST(serial_of(cert.get()) == wide);
    BOOST_TEST(!cert_serial_u64(cert.get()).has_value());
    BOOST_TEST(!cert_revoked_in_crl(cert.get(), ca.crl_pem()));
    // The JSON a client gets back names the serial as "serial_hex", the
    // field revoke takes for that encoding; a body built from it revokes
    // this certificate.
    auto response = pem_material_to_json(material);
    BOOST_TEST(!response.contains("serial"));
    BOOST_TEST(response.at("serial_hex").as_string() == wide.to_hex());
    boost::json::object body;
    body["serial_hex"] = response.at("serial_hex");
    const auto parsed = parse_revoke_serial(body);
    BOOST_TEST(parsed == wide);
    ca.mark_revoked_externally(parsed, std::chrono::system_clock::now());
    BOOST_TEST(cert_revoked_in_crl(cert.get(), ca.crl_pem()));
}

namespace {

auto revoke_body(std::string_view json) -> boost::json::object {
    return boost::json::parse(json).as_object();
}

}  // namespace

// "serial" keeps exactly the meaning it had before serial_hex existed: a
// non-negative integer, or a string of decimal digits that fits 64 bits.
BOOST_AUTO_TEST_CASE(revoke_serial_keeps_its_decimal_meaning, *boost::unit_test::timeout(30)) {
    BOOST_TEST(parse_revoke_serial(revoke_body(R"({"serial":42})")) ==
               certificate_serial::from_u64(42));
    BOOST_TEST(parse_revoke_serial(revoke_body(R"({"serial":"42"})")) ==
               certificate_serial::from_u64(42));
    BOOST_TEST(parse_revoke_serial(revoke_body(R"({"serial":"0042"})")) ==
               certificate_serial::from_u64(42));
    BOOST_TEST(parse_revoke_serial(revoke_body(R"({"serial":18446744073709551615})")) ==
               certificate_serial::from_u64(std::numeric_limits<std::uint64_t>::max()));
    BOOST_TEST(parse_revoke_serial(revoke_body(R"({"serial":"18446744073709551615"})")) ==
               certificate_serial::from_u64(std::numeric_limits<std::uint64_t>::max()));

    // Hex in "serial" is refused, never reinterpreted: "0a1b" is an error,
    // not some other certificate's serial. So are signs, whitespace,
    // overflow and anything that is not an integer.
    for (const char* bad :
         {R"({"serial":"0a1b"})", R"({"serial":"2a"})", R"({"serial":"0A:1B"})", R"({"serial":""})",
          R"({"serial":"-1"})", R"({"serial":"+1"})", R"({"serial":" 1"})", R"({"serial":"1 "})",
          R"({"serial":"18446744073709551616"})", R"({"serial":-1})", R"({"serial":1.5})",
          R"({"serial":true})", R"({"serial":null})", R"({"serial":[]})"}) {
        BOOST_CHECK_THROW((void)parse_revoke_serial(revoke_body(bad)), std::invalid_argument);
    }
}

BOOST_AUTO_TEST_CASE(revoke_serial_hex_takes_any_width, *boost::unit_test::timeout(30)) {
    BOOST_TEST(parse_revoke_serial(revoke_body(R"({"serial_hex":"0a1b"})")) ==
               certificate_serial::from_u64(0x0a1b));
    BOOST_TEST(parse_revoke_serial(revoke_body(R"({"serial_hex":"0A:1B:2C"})")) ==
               certificate_serial::from_u64(0x0a1b2c));
    BOOST_TEST(parse_revoke_serial(revoke_body(R"({"serial_hex":"0a 1b 2c"})")) ==
               certificate_serial::from_u64(0x0a1b2c));
    boost::json::object wide;
    wide["serial_hex"] = kythira::testing::twenty_octet_serial().to_hex();
    BOOST_TEST(parse_revoke_serial(wide) == kythira::testing::twenty_octet_serial());

    for (const char* bad :
         {R"({"serial_hex":""})", R"({"serial_hex":"abc"})", R"({"serial_hex":"zz"})",
          R"({"serial_hex":"0x2a"})", R"({"serial_hex":"2a:"})", R"({"serial_hex":"2a:1b 3c"})",
          R"({"serial_hex":"ff00000000000000000000000000000000000000ff"})", R"({"serial_hex":42})",
          R"({"serial_hex":null})"}) {
        BOOST_CHECK_THROW((void)parse_revoke_serial(revoke_body(bad)), std::invalid_argument);
    }
}

BOOST_AUTO_TEST_CASE(revoke_serial_needs_exactly_one_field, *boost::unit_test::timeout(30)) {
    BOOST_CHECK_THROW((void)parse_revoke_serial(revoke_body(R"({"serial":42,"serial_hex":"2a"})")),
                      std::invalid_argument);
    BOOST_CHECK_THROW((void)parse_revoke_serial(revoke_body(R"({})")), std::invalid_argument);
    BOOST_CHECK_THROW((void)parse_revoke_serial(revoke_body(R"({"reason":"x"})")),
                      std::invalid_argument);
}

// ca_service's aws-acm-pca path always took "serial" as a hex string, which
// it passed to ACM Private CA; it keeps that meaning.
BOOST_AUTO_TEST_CASE(revoke_serial_keeps_its_hex_meaning_on_acm, *boost::unit_test::timeout(30)) {
    constexpr auto hex = legacy_revoke_serial::hex;
    BOOST_TEST(parse_revoke_serial(revoke_body(R"({"serial":"0a:1b"})"), hex) ==
               certificate_serial::from_u64(0x0a1b));
    BOOST_TEST(parse_revoke_serial(revoke_body(R"({"serial":"42"})"), hex) ==
               certificate_serial::from_u64(0x42));
    BOOST_TEST(parse_revoke_serial(revoke_body(R"({"serial_hex":"42"})"), hex) ==
               certificate_serial::from_u64(0x42));
    for (const char* bad : {R"({"serial":42})", R"({"serial":""})", R"({"serial":"zz"})",
                            R"({"serial":"2a","serial_hex":"2a"})", R"({})"}) {
        BOOST_CHECK_THROW((void)parse_revoke_serial(revoke_body(bad), hex), std::invalid_argument);
    }
}

// A peer certificate always comes from the in-house CA and so has a 64-bit
// serial; once any peer serial is revoked, a presented certificate whose
// serial does not fit is refused rather than waved through.
BOOST_AUTO_TEST_CASE(rpc_trust_refuses_wide_serial_peer_once_revocations_exist,
                     *boost::unit_test::timeout(30)) {
    certificate_authority ca;
    csr_signing_options signing;
    signing.dns_names = {peer_identity_dns_name(2)};
    auto wide = load(kythira::testing::sign_csr_with_serial(
                         ca, csr_for("peer", peer_identity_dns_name(2)).csr_pem, signing,
                         kythira::testing::twenty_octet_serial())
                         .certificate_pem);
    auto policy = kythira::ca_root_only(ca.root_certificate_pem())
                      .binding_peer_node_ids({{peer_identity_dns_name(2), 2}});
    BOOST_TEST(policy.accepts(wide.get()));
    BOOST_TEST(!policy.revoking_serials({1}).accepts(wide.get()));
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

BOOST_AUTO_TEST_CASE(trust_mac_binds_nonce_and_body, *boost::unit_test::timeout(30)) {
    const auto key = derive_peer_enrollment_key("unseal-passphrase");
    auto nonce = random_nonce_hex();
    BOOST_TEST(is_well_formed_nonce(nonce));
    BOOST_TEST(nonce != random_nonce_hex());
    BOOST_TEST(!is_well_formed_nonce(""));
    BOOST_TEST(!is_well_formed_nonce("zz\nroot"));
    // The request MAC (sent instead of the bearer token) is key- and
    // nonce-bound, and distinct from the response MAC.
    auto req_mac = peer_trust_request_mac(key, nonce);
    BOOST_TEST(constant_time_equals(req_mac, peer_trust_request_mac(key, nonce)));
    BOOST_TEST(!constant_time_equals(
        req_mac, peer_trust_request_mac(derive_peer_enrollment_key("other"), nonce)));
    BOOST_TEST(!constant_time_equals(req_mac, peer_trust_mac(key, nonce, "")));
    auto mac = peer_trust_mac(key, nonce, "state");
    BOOST_TEST(constant_time_equals(mac, peer_trust_mac(key, nonce, "state")));
    BOOST_TEST(!constant_time_equals(mac, peer_trust_mac(key, nonce, "attacker-state")));
    BOOST_TEST(!constant_time_equals(mac, peer_trust_mac(key, random_nonce_hex(), "state")));
}

// The enrollment key is a slow KDF of the passphrase, not one HMAC: a MAC
// captured off the wire must cost as much per passphrase guess as the
// encrypted CA key at rest.
BOOST_AUTO_TEST_CASE(enrollment_key_is_a_slow_kdf_of_the_passphrase,
                     *boost::unit_test::timeout(30)) {
    const auto key = derive_peer_enrollment_key("unseal-passphrase");
    BOOST_TEST(key.size() == 64u);
    BOOST_TEST(key == derive_peer_enrollment_key("unseal-passphrase"));
    BOOST_TEST(key != derive_peer_enrollment_key("unseal-passphrasf"));
    BOOST_TEST(key !=
               hmac_sha256_hex("unseal-passphrase", "kythira ca_cluster_node peer-enrollment v1"));
    BOOST_TEST(k_peer_enrollment_kdf_iterations >= 200000);
    BOOST_CHECK_THROW((void)derive_peer_enrollment_key(""), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(rpc_trust_state_round_trips, *boost::unit_test::timeout(30)) {
    peer_rpc_trust_state state;
    state.root_pem = "-----BEGIN CERTIFICATE-----\nroot\n-----END CERTIFICATE-----\n";
    state.cutover_complete = true;
    state.revoked_peer_serials = {1, 18446744073709551615ULL};
    BOOST_TEST((decode_peer_rpc_trust_state(encode_peer_rpc_trust_state(state)) == state));

    BOOST_CHECK_THROW((void)decode_peer_rpc_trust_state("not json"), std::invalid_argument);
    BOOST_CHECK_THROW((void)decode_peer_rpc_trust_state(R"({"root_pem":"x"})"),
                      std::invalid_argument);
    BOOST_CHECK_THROW((void)decode_peer_rpc_trust_state(
                          R"({"root_pem":"","cutover_complete":false,"revoked_peer_serials":[]})"),
                      std::invalid_argument);
    BOOST_CHECK_THROW(
        (void)decode_peer_rpc_trust_state(
            R"({"root_pem":"x","cutover_complete":false,"revoked_peer_serials":["nope"]})"),
        std::invalid_argument);
}

// A revoked peer certificate stops being a Raft peer, whichever way the
// policy would otherwise have accepted it through the CA path.
BOOST_AUTO_TEST_CASE(rpc_trust_refuses_revoked_peer_certs, *boost::unit_test::timeout(30)) {
    certificate_authority ca;
    leaf_certificate_options peer_opts;
    peer_opts.dns_names = {peer_identity_dns_name(2)};
    auto issued = ca.issue(peer_opts);
    auto peer = load(issued.certificate_pem);
    auto other = load(ca.issue(peer_opts).certificate_pem);

    auto policy = kythira::ca_root_only(ca.root_certificate_pem())
                      .binding_peer_node_ids({{peer_identity_dns_name(2), 2}});
    BOOST_TEST(policy.accepts(peer.get()));
    BOOST_TEST(policy.binds(peer.get(), 2));

    auto revoking = policy.revoking_serials({issued.serial.to_u64().value()});
    BOOST_TEST(!revoking.accepts(peer.get()));
    BOOST_TEST(!revoking.binds(peer.get(), 2));
    BOOST_TEST(!revoking.authenticated_node_id(peer.get()).has_value());
    // A different (unrevoked) certificate for the same node still works.
    BOOST_TEST(revoking.accepts(other.get()));
    BOOST_TEST(revoking.binds(other.get(), 2));
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

// ── Textual node ids (.kiro/specs/cloud-composite-node-ids/ Requirement 12) ──

BOOST_AUTO_TEST_CASE(csr_policy_matches_uri_names_exactly, *boost::unit_test::timeout(30)) {
    auto opts = approved();
    opts.uri_names = {"urn:kythira:node:docker:c1:n1"};
    BOOST_CHECK_NO_THROW(enforce_csr_matches_options(
        csr_with_extensions({{NID_subject_alt_name, "URI:urn:kythira:node:docker:c1:n1"}}), opts));
    BOOST_CHECK_THROW(
        enforce_csr_matches_options(
            csr_with_extensions({{NID_subject_alt_name, "URI:urn:kythira:node:docker:c1:n2"}}),
            opts),
        std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(textual_peer_name_is_a_reserved_uri, *boost::unit_test::timeout(30)) {
    // Canonical composite text keeps its ':' separators; '%' (from the
    // composite escaping of ':' and '%') and anything outside RFC 3986's
    // unreserved set is percent-encoded, so the URI decodes back exactly.
    BOOST_TEST(peer_identity_uri("aws-ec2:us-east-1:i-0abc") ==
               "urn:kythira:node:aws-ec2:us-east-1:i-0abc");
    BOOST_TEST(peer_identity_uri("docker:a%3Ab:n 1/x") ==
               "urn:kythira:node:docker:a%253Ab:n%201%2Fx");
    BOOST_TEST(peer_identity_name<std::string>("gcp:p:z:n") == "urn:kythira:node:gcp:p:z:n");
    BOOST_TEST(peer_identity_name<std::uint64_t>(7) == "ca-cluster-node-7");
    BOOST_TEST(peer_identity_is_uri<kythira::aws_ec2_node_id>);
    BOOST_TEST(!peer_identity_is_uri<std::uint32_t>);

    BOOST_TEST(is_reserved_peer_uri("urn:kythira:node:x"));
    BOOST_TEST(is_reserved_peer_uri("urn:kythira:node:"));
    // Scheme and namespace id fold case; escaped unreserved characters are
    // the characters themselves.
    BOOST_TEST(is_reserved_peer_uri("URN:Kythira:NODE:x"));
    BOOST_TEST(is_reserved_peer_uri("urn:kythira:%6Eode:x"));
    BOOST_TEST(is_reserved_peer_uri("urn%3Akythira:node:x"));
    BOOST_TEST(!is_reserved_peer_uri("urn:kythira:nodes:x"));
    BOOST_TEST(!is_reserved_peer_uri("urn:kythira:node"));
    BOOST_TEST(!is_reserved_peer_uri("spiffe://kythira/node/x"));
    BOOST_TEST(!is_reserved_peer_uri("urn:kythira:%6"));
}

BOOST_AUTO_TEST_CASE(issued_cert_carries_requested_uri_names, *boost::unit_test::timeout(30)) {
    certificate_authority ca;
    leaf_certificate_options opts;
    opts.uri_names = {peer_identity_uri("docker:c1:n1")};
    auto cert = load(ca.issue(opts).certificate_pem);
    BOOST_TEST((san_types(cert.get()) == std::vector<int>{GEN_URI}));
    BOOST_TEST(cert_has_uri_san_in(cert.get(), {"urn:kythira:node:docker:c1:n1"}));
    // The same text is not a DNS SAN, and a URI is only ever matched as one.
    BOOST_TEST(!cert_has_dns_san_in(cert.get(), {"urn:kythira:node:docker:c1:n1"}));
    BOOST_TEST(cert_has_peer_name_in<std::string>(cert.get(), {"urn:kythira:node:docker:c1:n1"}));
    BOOST_TEST(
        !cert_has_peer_name_in<std::uint64_t>(cert.get(), {"urn:kythira:node:docker:c1:n1"}));
    // Renewal copies URI SANs from the presented certificate.
    BOOST_TEST((options_from_presented_cert(cert.get()).uri_names == opts.uri_names));

    for (const char* bad : {"", "no-scheme", ":empty-scheme", "1x:digit-first", "x:", "x:sp ace",
                            "x:new\nline", "x:quote\"", "x:a,URI:b<"}) {
        leaf_certificate_options bad_opts;
        bad_opts.uri_names = {bad};
        BOOST_CHECK_THROW((void)ca.issue(bad_opts), std::invalid_argument);
    }
}

BOOST_AUTO_TEST_CASE(textual_peer_enrollment, *boost::unit_test::timeout(30)) {
    const std::string a = "docker:c1:a";
    const std::string b = "docker:c1:b";
    const std::set<std::string> ids{a, b};
    const auto key = derive_peer_enrollment_key("unseal-passphrase");
    const std::string csr = "-----BEGIN CERTIFICATE REQUEST-----\nabc\n";

    // Requirement 12.3: the MAC is over the canonical text, and the numeric
    // form is unchanged, so an existing numeric enrollment stays valid.
    BOOST_TEST(peer_enrollment_mac(key, a, csr) ==
               hmac_sha256_hex(key, "enroll\n" + a + "\n" + csr));
    BOOST_TEST(peer_enrollment_mac(key, std::uint64_t{2}, csr) ==
               hmac_sha256_hex(key, "enroll\n2\n" + csr));

    csr_signing_options peer;
    peer.uri_names = {peer_identity_name(a)};
    auto good_mac = peer_enrollment_mac(key, a, csr);
    auto ok = classify_peer_enrollment<std::string>(peer, a, good_mac, ids, key, csr);
    BOOST_TEST(ok.allowed);
    BOOST_TEST(ok.node_id.value() == a);

    // A client asking for a reserved URI without enrolling: refused, by a
    // textual cluster and a numeric one alike.
    BOOST_TEST(
        !classify_peer_enrollment<std::string>(peer, std::nullopt, "", ids, key, csr).allowed);
    BOOST_TEST(
        !classify_peer_enrollment<std::uint64_t>(peer, std::nullopt, "", {1, 2}, key, csr).allowed);
    csr_signing_options sneaky;
    sneaky.uri_names = {"URN:kythira:node:docker:c1:a"};
    BOOST_TEST(
        !classify_peer_enrollment<std::string>(sneaky, std::nullopt, "", ids, key, csr).allowed);
    // An ordinary URI is untouched.
    csr_signing_options client;
    client.uri_names = {"spiffe://example.org/svc"};
    BOOST_TEST(
        classify_peer_enrollment<std::string>(client, std::nullopt, "", ids, key, csr).allowed);

    // Wrong node's name, the DNS form, an extra name, a non-member, or a bad
    // MAC: refused.
    csr_signing_options other;
    other.uri_names = {peer_identity_name(b)};
    BOOST_TEST(!classify_peer_enrollment<std::string>(other, a, good_mac, ids, key, csr).allowed);
    csr_signing_options both = peer;
    both.dns_names = {"ca-cluster-node-1"};
    BOOST_TEST(!classify_peer_enrollment<std::string>(both, a, good_mac, ids, key, csr).allowed);
    csr_signing_options extra = peer;
    extra.uri_names.push_back("spiffe://example.org/admin");
    BOOST_TEST(!classify_peer_enrollment<std::string>(extra, a, good_mac, ids, key, csr).allowed);
    csr_signing_options outsider;
    outsider.uri_names = {peer_identity_name(std::string("docker:c1:z"))};
    BOOST_TEST(!classify_peer_enrollment<std::string>(
                    outsider, "docker:c1:z",
                    peer_enrollment_mac(key, std::string("docker:c1:z"), csr), ids, key, csr)
                    .allowed);
    BOOST_TEST(!classify_peer_enrollment<std::string>(peer, a, peer_enrollment_mac(key, b, csr),
                                                      ids, key, csr)
                    .allowed);
    // A numeric cluster refuses the URI form even with a valid numeric MAC.
    csr_signing_options numeric_as_uri;
    numeric_as_uri.uri_names = {peer_identity_uri("2")};
    BOOST_TEST(!classify_peer_enrollment<std::uint64_t>(
                    numeric_as_uri, 2, peer_enrollment_mac(key, 2, csr), {1, 2}, key, csr)
                    .allowed);
}

// Requirement 12.2: a textual cluster's trust policy accepts and binds peers
// by their URI SAN, and only by it.
BOOST_AUTO_TEST_CASE(textual_rpc_trust_binds_uri_peer_names, *boost::unit_test::timeout(30)) {
    certificate_authority ca;
    const std::string a = "docker:c1:a";
    const std::string b = "docker:c1:b";
    leaf_certificate_options a_opts;
    a_opts.uri_names = {peer_identity_name(a)};
    auto a_cert = load(ca.issue(a_opts).certificate_pem);
    leaf_certificate_options dns_opts;
    dns_opts.dns_names = {peer_identity_dns_name(1)};
    auto dns_cert = load(ca.issue(dns_opts).certificate_pem);

    auto policy =
        kythira::ca_root_only<std::string>(ca.root_certificate_pem())
            .binding_peer_node_ids({{peer_identity_name(a), a}, {peer_identity_name(b), b}});
    BOOST_TEST(policy.accepts(a_cert.get()));
    BOOST_TEST(policy.binds(a_cert.get(), a));
    BOOST_TEST(!policy.binds(a_cert.get(), b));
    BOOST_TEST(policy.authenticated_node_id(a_cert.get()).value() == a);
    // A CA-issued certificate naming no textual peer is not a peer, even
    // one carrying a numeric peer name.
    BOOST_TEST(!policy.accepts(dns_cert.get()));

    // Even a map entry spelled as the DNS name does not make a DNS SAN count
    // for a textual policy, nor a URI SAN for a numeric one.
    auto odd_textual = kythira::ca_root_only<std::string>(ca.root_certificate_pem())
                           .binding_peer_node_ids({{peer_identity_dns_name(1), a}});
    BOOST_TEST(!odd_textual.accepts(dns_cert.get()));
    auto odd_numeric = kythira::ca_root_only(ca.root_certificate_pem())
                           .binding_peer_node_ids({{peer_identity_name(a), 1}});
    BOOST_TEST(!odd_numeric.accepts(a_cert.get()));
}

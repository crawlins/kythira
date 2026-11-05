// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// OIDC token endpoint decisions (`.kiro/specs/iam-compatible-auth/` task 2,
// Requirements 1.6-1.8): body parsing, subject derivation from real
// CA-issued certificates, every refusal the task names (wrong CA, revoked
// certificate, unlisted audience, a lifetime past the certificate, a
// workload SAN outside the prefix), the claims of a granted token, and a
// minted token verifying under the JWKS the issuer publishes.

#define BOOST_TEST_MODULE oidc_token_unit_test

#include <boost/test/unit_test.hpp>

#include <raft/certificate_authority.hpp>
#include <raft/certificate_serial_x509.hpp>
#include <raft/jwt.hpp>
#include <raft/oidc_issuer.hpp>
#include <raft/oidc_token.hpp>

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

using kythira::oidc::decide_token;
using kythira::oidc::token_error;
using kythira::oidc::token_evidence;
using kythira::oidc::token_grant;
using kythira::oidc::token_policy;
using kythira::oidc::token_request;

namespace {

constexpr const char* k_issuer = "https://issuer.kythira.test";
constexpr const char* k_prefix = "spiffe://kythira.test/wl/";

using x509_ptr = std::unique_ptr<X509, decltype(&X509_free)>;

auto parse_cert(const std::string& pem) -> x509_ptr {
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(
        BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), &BIO_free);
    return {PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), &X509_free};
}

auto chains_to(X509* cert, const std::string& root_pem) -> bool {
    auto root = parse_cert(root_pem);
    std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)> store(X509_STORE_new(),
                                                                  &X509_STORE_free);
    X509_STORE_add_cert(store.get(), root.get());
    std::unique_ptr<X509_STORE_CTX, decltype(&X509_STORE_CTX_free)> ctx(X509_STORE_CTX_new(),
                                                                        &X509_STORE_CTX_free);
    X509_STORE_CTX_init(ctx.get(), store.get(), cert, nullptr);
    return X509_verify_cert(ctx.get()) == 1;
}

/// What ca_cluster_node gathers for a presented certificate.
auto evidence_for(const raft::testing::pem_material& leaf, const std::string& cluster_root_pem,
                  bool revoked = false) -> token_evidence {
    auto cert = parse_cert(leaf.certificate_pem);
    BOOST_REQUIRE(cert);
    auto serial = raft::testing::serial_of(cert.get());
    BOOST_REQUIRE(serial.has_value());
    token_evidence ev;
    ev.cert = kythira::oidc::cert_facts_of(cert.get(), serial->to_hex());
    ev.chains_to_ca = chains_to(cert.get(), cluster_root_pem);
    ev.revoked = revoked;
    return ev;
}

auto client_options() -> raft::testing::leaf_certificate_options {
    raft::testing::leaf_certificate_options o;
    o.subject.common_name = "client";
    o.server_auth = false;
    o.client_auth = true;
    return o;
}

auto node_cert(raft::testing::certificate_authority& ca, std::uint64_t id)
    -> raft::testing::pem_material {
    auto o = client_options();
    o.dns_names = {"ca-cluster-node-" + std::to_string(id)};
    return ca.issue(o);
}

auto workload_cert(raft::testing::certificate_authority& ca, std::vector<std::string> uris)
    -> raft::testing::pem_material {
    auto o = client_options();
    o.uri_names = std::move(uris);
    return ca.issue(o);
}

auto policy() -> token_policy {
    token_policy p;
    p.audiences = {"sts.amazonaws.com", "api://AzureADTokenExchange"};
    p.workload_uri_prefix = k_prefix;
    return p;
}

auto peer_names() -> std::map<std::string, std::string> {
    return {{"ca-cluster-node-1", "1"}, {"ca-cluster-node-2", "2"}, {"ca-cluster-node-3", "3"}};
}

auto now_secs() -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

auto grant_of(const std::variant<token_grant, token_error>& r) -> token_grant {
    if (const auto* e = std::get_if<token_error>(&r)) {
        BOOST_FAIL("expected a grant, got " << e->status << " " << e->code << ": " << e->message);
    }
    return std::get<token_grant>(r);
}

auto error_of(const std::variant<token_grant, token_error>& r) -> token_error {
    BOOST_REQUIRE_MESSAGE(std::holds_alternative<token_error>(r), "expected a refusal");
    return std::get<token_error>(r);
}

}  // namespace

BOOST_AUTO_TEST_SUITE(request_body)

BOOST_AUTO_TEST_CASE(parses_audience_and_lifetime) {
    auto r = kythira::oidc::parse_token_request(
        "audience=%2F%2Fiam.googleapis.com%2Fprojects%2F1%2Fpool&lifetime=900&grant_type=x");
    BOOST_REQUIRE(std::holds_alternative<token_request>(r));
    const auto& req = std::get<token_request>(r);
    BOOST_TEST(req.audience == "//iam.googleapis.com/projects/1/pool");
    BOOST_REQUIRE(req.lifetime_secs.has_value());
    BOOST_TEST(*req.lifetime_secs == 900);
}

BOOST_AUTO_TEST_CASE(plus_is_a_space_and_lifetime_is_optional) {
    auto r = kythira::oidc::parse_token_request("audience=a+b");
    BOOST_REQUIRE(std::holds_alternative<token_request>(r));
    BOOST_TEST(std::get<token_request>(r).audience == "a b");
    BOOST_TEST(!std::get<token_request>(r).lifetime_secs.has_value());
}

BOOST_AUTO_TEST_CASE(refuses_malformed_bodies) {
    for (const char* body :
         {"", "lifetime=60", "audience=", "audience=a&audience=b", "audience=a&lifetime=-5",
          "audience=a&lifetime=1e3", "audience=a&lifetime=9999999999", "audience=%zz",
          "audience=a%2", "audience=a&lifetime=1&lifetime=2"}) {
        auto r = kythira::oidc::parse_token_request(body);
        BOOST_TEST_INFO("body: " << body);
        BOOST_REQUIRE(std::holds_alternative<token_error>(r));
        BOOST_TEST(std::get<token_error>(r).status == 400);
    }
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(configuration)

BOOST_AUTO_TEST_CASE(workload_prefix_rules) {
    BOOST_TEST(!kythira::oidc::workload_prefix_error("spiffe://kythira.test/wl/").has_value());
    BOOST_TEST(!kythira::oidc::workload_prefix_error("urn:example:wl:").has_value());
    BOOST_TEST(kythira::oidc::workload_prefix_error("").has_value());
    BOOST_TEST(kythira::oidc::workload_prefix_error("spiffe://kythira.test/wl").has_value());
    BOOST_TEST(kythira::oidc::workload_prefix_error("/wl/").has_value());
    BOOST_TEST(kythira::oidc::workload_prefix_error("urn:").has_value());
    BOOST_TEST(kythira::oidc::workload_prefix_error("urn:kythira:node:").has_value());
    BOOST_TEST(kythira::oidc::workload_prefix_error("urn:kythira:node:x/").has_value());
    BOOST_TEST(kythira::oidc::workload_prefix_error("spiffe://a b/").has_value());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(decisions)

BOOST_AUTO_TEST_CASE(node_certificate_gets_a_node_token) {
    raft::testing::certificate_authority ca;
    auto leaf = node_cert(ca, 2);
    const auto now = now_secs();
    auto r = decide_token(evidence_for(leaf, ca.root_certificate_pem()),
                          "audience=sts.amazonaws.com", k_issuer, policy(), peer_names(), now);
    const auto& g = grant_of(r);
    const auto& c = g.claims;
    BOOST_TEST(c.at("iss").as_string() == k_issuer);
    BOOST_TEST(c.at("sub").as_string() == "node:2");
    BOOST_TEST(c.at("aud").as_string() == "sts.amazonaws.com");
    BOOST_TEST(c.at("kythira:node_id").as_string() == "2");
    BOOST_TEST(!c.contains("kythira:workload"));
    BOOST_TEST(c.at("kythira:cert_serial").as_string() == leaf.serial.to_hex());
    BOOST_TEST(c.at("iat").as_int64() == now);
    BOOST_TEST(c.at("nbf").as_int64() == now);
    BOOST_TEST(c.at("exp").as_int64() == now + 3600);  // 1 hour by default (1.8)
    BOOST_TEST(g.expires_at == now + 3600);
    BOOST_TEST(!c.contains("jti"));  // the caller draws it
    // Every claim is one the discovery document advertises.
    for (const auto& [name, value] : c) {
        BOOST_TEST_INFO("claim " << name);
        BOOST_TEST(std::ranges::find(kythira::oidc::k_claims_supported, std::string_view(name)) !=
                   kythira::oidc::k_claims_supported.end());
    }
}

BOOST_AUTO_TEST_CASE(workload_certificate_gets_a_workload_token) {
    raft::testing::certificate_authority ca;
    auto leaf = workload_cert(ca, {std::string(k_prefix) + "billing/api"});
    const auto now = now_secs();
    auto r = decide_token(evidence_for(leaf, ca.root_certificate_pem()),
                          "audience=api%3A%2F%2FAzureADTokenExchange&lifetime=600", k_issuer,
                          policy(), peer_names(), now);
    const auto c = grant_of(r).claims;
    BOOST_TEST(c.at("sub").as_string() == std::string("workload:") + k_prefix + "billing/api");
    BOOST_TEST(c.at("kythira:workload").as_string() == std::string(k_prefix) + "billing/api");
    BOOST_TEST(!c.contains("kythira:node_id"));
    BOOST_TEST(c.at("aud").as_string() == "api://AzureADTokenExchange");
    BOOST_TEST(c.at("exp").as_int64() == now + 600);
}

BOOST_AUTO_TEST_CASE(no_certificate_is_401) {
    auto r = decide_token(token_evidence{}, "audience=sts.amazonaws.com", k_issuer, policy(),
                          peer_names(), now_secs());
    BOOST_TEST(error_of(r).status == 401);
}

BOOST_AUTO_TEST_CASE(wrong_ca_is_401) {
    raft::testing::certificate_authority cluster_ca;
    raft::testing::certificate_authority other_ca;
    auto leaf = node_cert(other_ca, 1);  // the right name from the wrong CA
    auto r =
        decide_token(evidence_for(leaf, cluster_ca.root_certificate_pem()),
                     "audience=sts.amazonaws.com", k_issuer, policy(), peer_names(), now_secs());
    auto e = error_of(r);
    BOOST_TEST(e.status == 401);
    BOOST_TEST(e.message.find("chain") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(revoked_certificate_is_401) {
    raft::testing::certificate_authority ca;
    auto leaf = node_cert(ca, 1);
    auto r =
        decide_token(evidence_for(leaf, ca.root_certificate_pem(), /*revoked=*/true),
                     "audience=sts.amazonaws.com", k_issuer, policy(), peer_names(), now_secs());
    auto e = error_of(r);
    BOOST_TEST(e.status == 401);
    BOOST_TEST(e.message.find("revoked") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(certificate_outside_validity_is_401) {
    raft::testing::certificate_authority ca;
    auto leaf = node_cert(ca, 1);
    auto ev = evidence_for(leaf, ca.root_certificate_pem());
    BOOST_TEST(error_of(decide_token(ev, "audience=sts.amazonaws.com", k_issuer, policy(),
                                     peer_names(), ev.cert->not_after))
                   .status == 401);
    BOOST_TEST(error_of(decide_token(ev, "audience=sts.amazonaws.com", k_issuer, policy(),
                                     peer_names(), ev.cert->not_before - 1))
                   .status == 401);
}

BOOST_AUTO_TEST_CASE(unlisted_audience_is_400) {
    raft::testing::certificate_authority ca;
    auto leaf = node_cert(ca, 1);
    auto r = decide_token(evidence_for(leaf, ca.root_certificate_pem()),
                          "audience=sts.amazonaws.com.evil.example", k_issuer, policy(),
                          peer_names(), now_secs());
    auto e = error_of(r);
    BOOST_TEST(e.status == 400);
    BOOST_TEST(e.code == "invalid_target");
}

BOOST_AUTO_TEST_CASE(empty_allowlist_refuses_everything) {
    raft::testing::certificate_authority ca;
    auto leaf = node_cert(ca, 1);
    auto p = policy();
    p.audiences.clear();
    auto r = decide_token(evidence_for(leaf, ca.root_certificate_pem()),
                          "audience=sts.amazonaws.com", k_issuer, p, peer_names(), now_secs());
    BOOST_TEST(error_of(r).status == 400);
}

BOOST_AUTO_TEST_CASE(lifetime_past_the_certificate_is_400) {
    raft::testing::certificate_authority ca;
    auto o = client_options();
    o.dns_names = {"ca-cluster-node-1"};
    o.validity = std::chrono::minutes(30);
    auto leaf = ca.issue(o);
    auto ev = evidence_for(leaf, ca.root_certificate_pem());
    const auto now = ev.cert->not_before + 60;  // 29 minutes of validity left

    auto e = error_of(decide_token(ev, "audience=sts.amazonaws.com&lifetime=3600", k_issuer,
                                   policy(), peer_names(), now));
    BOOST_TEST(e.status == 400);
    BOOST_TEST(e.message.find("outlast") != std::string::npos);

    // With no lifetime asked for, the 1-hour default is cut to the
    // certificate's remaining validity rather than refused.
    const auto& g = grant_of(
        decide_token(ev, "audience=sts.amazonaws.com", k_issuer, policy(), peer_names(), now));
    BOOST_TEST(g.expires_at == ev.cert->not_after);
}

BOOST_AUTO_TEST_CASE(lifetime_bounds_are_400) {
    raft::testing::certificate_authority ca;
    auto leaf = node_cert(ca, 1);  // 30 days of validity
    auto ev = evidence_for(leaf, ca.root_certificate_pem());
    const auto now = now_secs();
    for (const char* body :
         {"audience=sts.amazonaws.com&lifetime=59", "audience=sts.amazonaws.com&lifetime=43201"}) {
        BOOST_TEST_INFO("body: " << body);
        BOOST_TEST(error_of(decide_token(ev, body, k_issuer, policy(), peer_names(), now)).status ==
                   400);
    }
    BOOST_TEST(grant_of(decide_token(ev, "audience=sts.amazonaws.com&lifetime=43200", k_issuer,
                                     policy(), peer_names(), now))
                   .expires_at == now + 43200);

    // A tighter --oidc-max-token-lifetime-secs lowers both the cap and the
    // default.
    auto p = policy();
    p.max_lifetime = std::chrono::minutes(10);
    BOOST_TEST(error_of(decide_token(ev, "audience=sts.amazonaws.com&lifetime=601", k_issuer, p,
                                     peer_names(), now))
                   .status == 400);
    BOOST_TEST(
        grant_of(decide_token(ev, "audience=sts.amazonaws.com", k_issuer, p, peer_names(), now))
            .expires_at == now + 600);
}

BOOST_AUTO_TEST_CASE(workload_outside_the_prefix_is_403) {
    raft::testing::certificate_authority ca;
    const auto root = ca.root_certificate_pem();
    const auto now = now_secs();
    for (const std::vector<std::string>& uris : std::vector<std::vector<std::string>>{
             {"spiffe://kythira.test/other/api"},
             {"spiffe://kythira.test/wlx/api"},      // shares the prefix's text only
             {"spiffe://kythira.test/wl/"},          // the bare prefix names nobody
             {"spiffe://kythira.test/wl/../admin"},  // dot segment
             {"spiffe://kythira.test/wl/a/./b"},     // dot segment
             {"spiffe://kythira.test/wl/a?b"},       // query
             {std::string(k_prefix) + "a", std::string(k_prefix) + "b"},  // ambiguous
         }) {
        BOOST_TEST_INFO("uris[0]: " << uris.front());
        auto r = decide_token(evidence_for(workload_cert(ca, uris), root),
                              "audience=sts.amazonaws.com", k_issuer, policy(), peer_names(), now);
        BOOST_TEST(error_of(r).status == 403);
    }
}

BOOST_AUTO_TEST_CASE(ordinary_client_certificates_are_403) {
    raft::testing::certificate_authority ca;
    auto o = client_options();
    o.dns_names = {"app.example.com", "ca-cluster-node-9"};  // 9 is not a member
    auto r =
        decide_token(evidence_for(ca.issue(o), ca.root_certificate_pem()),
                     "audience=sts.amazonaws.com", k_issuer, policy(), peer_names(), now_secs());
    BOOST_TEST(error_of(r).status == 403);

    // With no prefix configured a workload certificate is just a client.
    auto p = policy();
    p.workload_uri_prefix.clear();
    r = decide_token(
        evidence_for(workload_cert(ca, {std::string(k_prefix) + "api"}), ca.root_certificate_pem()),
        "audience=sts.amazonaws.com", k_issuer, p, peer_names(), now_secs());
    BOOST_TEST(error_of(r).status == 403);
}

BOOST_AUTO_TEST_CASE(two_members_in_one_certificate_is_403) {
    raft::testing::certificate_authority ca;
    auto o = client_options();
    o.dns_names = {"ca-cluster-node-1", "ca-cluster-node-2"};
    auto r =
        decide_token(evidence_for(ca.issue(o), ca.root_certificate_pem()),
                     "audience=sts.amazonaws.com", k_issuer, policy(), peer_names(), now_secs());
    BOOST_TEST(error_of(r).status == 403);
}

BOOST_AUTO_TEST_CASE(textual_node_ids_use_the_uri_peer_name) {
    raft::testing::certificate_authority ca;
    auto leaf = workload_cert(ca, {"urn:kythira:node:aws:us-east-1:i-0abc"});
    std::map<std::string, std::string> names{
        {"urn:kythira:node:aws:us-east-1:i-0abc", "aws:us-east-1:i-0abc"}};
    auto r = decide_token(evidence_for(leaf, ca.root_certificate_pem()),
                          "audience=sts.amazonaws.com", k_issuer, policy(), names, now_secs());
    BOOST_TEST(grant_of(r).claims.at("sub").as_string() == "node:aws:us-east-1:i-0abc");
}

BOOST_AUTO_TEST_CASE(refusal_order_puts_authentication_first) {
    // A bad body from an unauthenticated caller reveals nothing about the
    // body rules: it is 401, not 400.
    raft::testing::certificate_authority cluster_ca;
    raft::testing::certificate_authority other_ca;
    auto r = decide_token(evidence_for(node_cert(other_ca, 1), cluster_ca.root_certificate_pem()),
                          "garbage", k_issuer, policy(), peer_names(), now_secs());
    BOOST_TEST(error_of(r).status == 401);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(minting)

BOOST_AUTO_TEST_CASE(minted_token_verifies_under_the_published_jwks) {
    for (auto alg : {kythira::jose::jws_alg::rs256, kythira::jose::jws_alg::es256}) {
        raft::testing::certificate_authority ca;
        auto leaf = node_cert(ca, 3);
        const auto now = now_secs();
        auto grant = grant_of(decide_token(evidence_for(leaf, ca.root_certificate_pem()),
                                           "audience=sts.amazonaws.com", k_issuer, policy(),
                                           peer_names(), now));

        auto key = kythira::jose::generate_signing_key(alg);
        kythira::oidc::oidc_key_record rec;
        rec.alg = alg;
        rec.public_jwk = kythira::jose::public_jwk(key.get());
        rec.kid = kythira::jose::jwk_thumbprint(rec.public_jwk);
        rec.sealed_private_key = "unused";
        rec.publish_after = now - 10;
        rec.sign_after = now - 10;

        auto body = kythira::oidc::mint_token(grant, "jti-123", rec, key.get(), now);
        BOOST_TEST(body.at("token_type").as_string() == "urn:ietf:params:oauth:token-type:jwt");
        BOOST_TEST(body.at("expires_in").as_int64() == 3600);
        BOOST_TEST(body.at("expires_at").as_string() == kythira::oidc::rfc3339_utc(now + 3600));
        const std::string token(body.at("token").as_string());

        // Verify the way a relying party does: pick the JWKS entry by kid,
        // import it, check the signature under the advertised alg.
        auto parts = kythira::jose::parse_compact(token);
        BOOST_REQUIRE(parts.has_value());
        BOOST_TEST(parts->header.at("kid").as_string() == rec.kid);
        BOOST_TEST(parts->header.at("typ").as_string() == "JWT");
        BOOST_TEST(parts->header.at("alg").as_string() == kythira::jose::alg_name(alg));
        const auto jwks = kythira::oidc::jwks_document({rec}, now);
        const auto& jwk = jwks.at("keys").as_array().at(0).as_object();
        auto pub = kythira::jose::public_key_from_jwk(jwk);
        auto payload = kythira::jose::verify_compact(token, pub.get(), alg);
        BOOST_REQUIRE(payload.has_value());
        auto claims = boost::json::parse(payload->payload).as_object();
        BOOST_TEST(claims.at("jti").as_string() == "jti-123");
        BOOST_TEST(claims.at("sub").as_string() == "node:3");
        BOOST_TEST(claims.at("iss").as_string() == k_issuer);
    }
}

BOOST_AUTO_TEST_CASE(rfc3339_formatting) {
    BOOST_TEST(kythira::oidc::rfc3339_utc(0) == "1970-01-01T00:00:00Z");
    BOOST_TEST(kythira::oidc::rfc3339_utc(1'800'000'000) == "2027-01-15T08:00:00Z");
}

BOOST_AUTO_TEST_SUITE_END()

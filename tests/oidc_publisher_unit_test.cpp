// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Publishing the OIDC documents to a bucket (`.kiro/specs/iam-compatible-auth/`
// task 2, Requirement 1.9): --oidc-publish-url parsing, object keys, the
// documents uploaded (byte-identical to what the issuer serves), upload
// order, and when the leader uploads, retries and refreshes.

#define BOOST_TEST_MODULE oidc_publisher_unit_test

#include <boost/test/unit_test.hpp>

#include <raft/jwt.hpp>
#include <raft/oidc_issuer.hpp>
#include <raft/oidc_publisher.hpp>

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using kythira::oidc::documents_to_publish;
using kythira::oidc::oidc_key_record;
using kythira::oidc::parse_publish_url;
using kythira::oidc::publish_schedule;
using kythira::oidc::publish_target;
using kythira::oidc::published_documents;

namespace {

constexpr const char* k_issuer = "https://kythira-oidc.s3.us-east-1.amazonaws.com/prod";
constexpr std::int64_t t0 = 1'800'000'000;

/// Records puts in order; optionally fails every put.
struct recording_store {
    mutable std::vector<std::pair<std::string, std::string>> puts;  // bucket/key, body
    bool fail = false;
    void put_object(const std::string& bucket, const std::string& key,
                    std::string_view body) const {
        if (fail) {
            throw std::runtime_error("store unavailable");
        }
        puts.emplace_back(bucket + "/" + key, std::string(body));
    }
};

auto real_key(std::int64_t publish_after, std::int64_t sign_after) -> oidc_key_record {
    auto key = kythira::jose::generate_signing_key(kythira::jose::jws_alg::es256);
    oidc_key_record rec;
    rec.alg = kythira::jose::jws_alg::es256;
    rec.public_jwk = kythira::jose::public_jwk(key.get());
    rec.kid = kythira::jose::jwk_thumbprint(rec.public_jwk);
    rec.sealed_private_key = "sealed";
    rec.publish_after = publish_after;
    rec.sign_after = sign_after;
    return rec;
}

auto target_of(std::string_view url) -> publish_target {
    auto r = parse_publish_url(url);
    if (const auto* err = std::get_if<std::string>(&r)) {
        BOOST_FAIL("parse_publish_url(" << url << "): " << *err);
    }
    return std::get<publish_target>(r);
}

}  // namespace

BOOST_AUTO_TEST_CASE(parses_every_scheme) {
    const std::map<std::string, publish_target> cases{
        {"s3://kythira-oidc/prod", {"s3", "kythira-oidc", "prod"}},
        {"gs://kythira-oidc", {"gcs", "kythira-oidc", ""}},
        {"az://oidc/issuers/prod/", {"azure-blob", "oidc", "issuers/prod"}},
        {"oci://bucket/a", {"oci-objectstorage", "bucket", "a"}},
        {"oss://bucket/", {"oss", "bucket", ""}},
    };
    for (const auto& [url, want] : cases) {
        BOOST_TEST_INFO("url: " << url);
        BOOST_TEST((target_of(url) == want));
    }
}

BOOST_AUTO_TEST_CASE(refuses_bad_urls) {
    for (const char* url :
         {"kythira-oidc/prod", "http://bucket/x", "s3:///prod", "s3://", "s3://b/a//c", "s3://b//a",
          "s3://b/../x", "s3://b/a/./c", "s3://b/a?x", "s3://b/a#x", "s3://b/a b"}) {
        BOOST_TEST_INFO("url: " << url);
        BOOST_TEST(std::holds_alternative<std::string>(parse_publish_url(url)));
    }
}

BOOST_AUTO_TEST_CASE(object_keys_follow_the_issuer_paths) {
    BOOST_TEST(kythira::oidc::publish_key(target_of("s3://b/prod"), kythira::oidc::k_jwks_path) ==
               "prod/.well-known/jwks.json");
    BOOST_TEST(kythira::oidc::publish_key(target_of("s3://b"), kythira::oidc::k_discovery_path) ==
               ".well-known/openid-configuration");
}

BOOST_AUTO_TEST_CASE(documents_match_what_the_issuer_serves) {
    const auto k = real_key(t0, t0);
    BOOST_TEST(!documents_to_publish({k}, t0 - 1, k_issuer, k.alg).has_value());  // empty JWKS

    auto docs = documents_to_publish({k}, t0, k_issuer, k.alg);
    BOOST_REQUIRE(docs.has_value());
    BOOST_TEST(docs->jwks == boost::json::serialize(kythira::oidc::jwks_document({k}, t0)));
    auto disc = boost::json::parse(docs->discovery).as_object();
    BOOST_TEST(disc.at("issuer").as_string() == k_issuer);
    BOOST_TEST(disc.at("jwks_uri").as_string() == std::string(k_issuer) + "/.well-known/jwks.json");
}

BOOST_AUTO_TEST_CASE(uploads_jwks_before_discovery) {
    const auto k = real_key(t0, t0);
    auto docs = *documents_to_publish({k}, t0, k_issuer, k.alg);
    recording_store store;
    kythira::oidc::publish_documents(store, target_of("s3://kythira-oidc/prod"), docs);
    BOOST_REQUIRE_EQUAL(store.puts.size(), 2u);
    BOOST_TEST(store.puts[0].first == "kythira-oidc/prod/.well-known/jwks.json");
    BOOST_TEST(store.puts[0].second == docs.jwks);
    BOOST_TEST(store.puts[1].first == "kythira-oidc/prod/.well-known/openid-configuration");
    BOOST_TEST(store.puts[1].second == docs.discovery);

    recording_store broken;
    broken.fail = true;
    BOOST_CHECK_THROW(kythira::oidc::publish_documents(broken, target_of("s3://b"), docs),
                      std::runtime_error);
}

BOOST_AUTO_TEST_CASE(republishes_when_the_jwks_changes_without_a_command) {
    // A successor recorded at t0 + 89 days is published then, and the old
    // key drops out at its retire_after: both change the JWKS with no new
    // command, and both must reach the bucket.
    constexpr std::int64_t day = 86400;
    auto old_key = real_key(t0, t0);
    auto next_key = real_key(t0 + 89 * day, t0 + 90 * day);
    publish_schedule sched;
    std::int64_t steady = 1000;

    auto at = [&](std::int64_t now, std::vector<oidc_key_record> keys) {
        return *documents_to_publish(keys, now, k_issuer, old_key.alg);
    };
    auto d1 = at(t0, {old_key, next_key});
    BOOST_TEST(sched.due(d1, steady));
    sched.succeeded(d1, steady);
    BOOST_TEST(!sched.due(at(t0 + day, {old_key, next_key}), steady + 60));

    auto d2 = at(t0 + 89 * day, {old_key, next_key});  // successor now published
    BOOST_TEST(d2.jwks != d1.jwks);
    BOOST_TEST(sched.due(d2, steady + 120));
    sched.succeeded(d2, steady + 120);

    old_key.retire_after = t0 + 91 * day;
    auto d3 = at(t0 + 91 * day, {old_key, next_key});  // old key unpublished
    BOOST_TEST(boost::json::parse(d3.jwks).at("keys").as_array().size() == 1u);
    BOOST_TEST(sched.due(d3, steady + 180));
}

BOOST_AUTO_TEST_CASE(failures_back_off_and_refresh_restores_deleted_objects) {
    const auto k = real_key(t0, t0);
    auto docs = *documents_to_publish({k}, t0, k_issuer, k.alg);
    publish_schedule sched;  // 5 s first retry, 5 min cap, 6 h refresh

    sched.failed(0);
    BOOST_TEST(!sched.due(docs, 4));
    BOOST_TEST(sched.due(docs, 5));
    sched.failed(5);  // 10 s
    BOOST_TEST(*sched.retry_at() == 15);
    sched.failed(15);  // 20 s
    BOOST_TEST(*sched.retry_at() == 35);
    for (int i = 0; i < 10; ++i) {
        sched.failed(100);
    }
    BOOST_TEST(*sched.retry_at() == 400);  // capped at 5 minutes
    BOOST_TEST(sched.consecutive_failures() == 13);

    sched.succeeded(docs, 1000);
    BOOST_TEST(sched.consecutive_failures() == 0);
    BOOST_TEST(!sched.due(docs, 1000 + 6 * 3600 - 1));
    BOOST_TEST(sched.due(docs, 1000 + 6 * 3600));

    sched.reset();  // a new leadership term uploads at once
    BOOST_TEST(sched.due(docs, 1001));
}

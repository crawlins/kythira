// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// SigV4 verification (`.kiro/specs/iam-compatible-auth/` task 6,
// Requirements 3.1-3.3 and 3.5-3.7). Every case of AWS's signing test suite
// (tests/sigv4_suite/v4, vendored) in both its header and presigned forms,
// then the checks the suite does not cover: scope, skew, expiry, unsigned
// headers, payload hashes, error mapping and the signing-key cache.

#define BOOST_TEST_MODULE sigv4_verifier_test

#include <boost/test/unit_test.hpp>

#include <raft/sigv4_verifier.hpp>

#include <boost/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#ifndef SIGV4_SUITE_DIR
#error "SIGV4_SUITE_DIR must name tests/sigv4_suite/v4"
#endif

namespace fs = std::filesystem;
namespace sv4 = kythira::sigv4;

namespace {

auto slurp(const fs::path& p) -> std::string {
    std::ifstream f(p, std::ios::binary);
    BOOST_REQUIRE_MESSAGE(f, "cannot read " << p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// A parsed suite request: the strings the request views point into.
struct http_request {
    std::string method;
    std::string path;
    std::string query;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    [[nodiscard]] auto view() const -> sv4::request { return {method, path, query, headers, body}; }
};

// The suite's request format: "METHOD target HTTP/1.1", "Name:value" lines
// (a line starting with whitespace continues the previous value), a blank
// line, then the body. The target may contain raw spaces.
auto parse_http(const std::string& text) -> http_request {
    http_request r;
    std::size_t pos = 0;
    auto next_line = [&]() -> std::optional<std::string> {
        if (pos >= text.size()) {
            return std::nullopt;
        }
        auto end = text.find('\n', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        return line;
    };
    auto first = next_line();
    BOOST_REQUIRE(first.has_value());
    const auto sp = first->find(' ');
    const auto http = first->rfind(" HTTP/");
    BOOST_REQUIRE(sp != std::string::npos && http != std::string::npos && http > sp);
    r.method = first->substr(0, sp);
    const auto target = first->substr(sp + 1, http - sp - 1);
    const auto q = target.find('?');
    r.path = target.substr(0, q);
    if (q != std::string::npos) {
        r.query = target.substr(q + 1);
    }
    while (auto line = next_line()) {
        if (line->empty()) {
            break;
        }
        if ((line->front() == ' ' || line->front() == '\t') && !r.headers.empty()) {
            r.headers.back().second += ' ';
            r.headers.back().second += *line;
            continue;
        }
        const auto colon = line->find(':');
        BOOST_REQUIRE(colon != std::string::npos);
        r.headers.emplace_back(line->substr(0, colon), line->substr(colon + 1));
    }
    if (pos < text.size()) {
        r.body = text.substr(pos);
    }
    return r;
}

auto trim_newlines(std::string s) -> std::string {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
    return s;
}

auto parse_iso(const std::string& s) -> std::chrono::sys_seconds {
    // "2015-08-30T12:36:00Z" -> the amz basic form the verifier parses.
    std::string basic;
    for (char c : s) {
        if (c != '-' && c != ':') {
            basic += c;
        }
    }
    auto t = sv4::detail::parse_amz_date(basic);
    BOOST_REQUIRE(t.has_value());
    return *t;
}

constexpr std::string_view k_akid = "AKIDEXAMPLE";
constexpr std::string_view k_secret = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";

auto rules_for(std::string service, bool normalize = true) -> sv4::service_rules {
    sv4::service_rules r;
    r.service = std::move(service);
    r.regions = {"us-east-1"};
    r.s3_path_rules = !normalize;
    return r;
}

const auto k_now = sv4::detail::parse_amz_date("20150830T123600Z").value();

// get-vanilla's header form, as a starting point to mutate.
auto vanilla() -> http_request {
    return parse_http(
        slurp(fs::path(SIGV4_SUITE_DIR) / "get-vanilla" / "header-signed-request.txt"));
}

auto key_for(const sv4::claim& c) -> sv4::signing_key {
    return sv4::derive_signing_key(k_secret, c.scope);
}

// Signs `r` (header form) the way an SDK would, using the verifier's own
// canonicalisation, for tests that need a request the suite does not have.
void sign_header_form(http_request& r, const sv4::service_rules& rules,
                      std::string_view amz_date = "20150830T123600Z",
                      std::string_view region = "us-east-1") {
    std::erase_if(r.headers,
                  [](const auto& h) { return sv4::detail::lower(h.first) == "authorization"; });
    std::vector<std::string> names;
    for (const auto& [n, v] : r.headers) {
        names.push_back(sv4::detail::lower(n));
    }
    std::ranges::sort(names);
    names.erase(std::unique(names.begin(), names.end()), names.end());
    std::string signed_headers;
    for (const auto& n : names) {
        signed_headers += (signed_headers.empty() ? "" : ";") + n;
    }
    const sv4::credential_scope scope{std::string(amz_date.substr(0, 8)), std::string(region),
                                      rules.service};
    // A placeholder signature lets parse() build the canonical request.
    r.headers.emplace_back("Authorization", "AWS4-HMAC-SHA256 Credential=" + std::string(k_akid) +
                                                "/" + scope.text() +
                                                ", SignedHeaders=" + signed_headers +
                                                ", Signature=" + std::string(64, '0'));
    const auto parsed = sv4::parse(r.view(), rules, k_now);
    BOOST_REQUIRE_MESSAGE(parsed.has_value(), "sign_header_form: " << parsed.error().message);
    const auto sig = sv4::compute_signature(*parsed, sv4::derive_signing_key(k_secret, scope));
    r.headers.back().second.replace(r.headers.back().second.size() - 64, 64, sig);
}

}  // namespace

// ---------------------------------------------------------------------------
// The AWS suite
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(aws_signing_test_suite_v4) {
    int cases = 0;
    std::vector<fs::path> dirs;
    for (const auto& entry : fs::directory_iterator(SIGV4_SUITE_DIR)) {
        if (entry.is_directory()) {
            dirs.push_back(entry.path());
        }
    }
    std::ranges::sort(dirs);
    for (const auto& dir : dirs) {
        const auto ctx = boost::json::parse(slurp(dir / "context.json")).as_object();
        const auto& creds = ctx.at("credentials").as_object();
        BOOST_REQUIRE(creds.at("secret_access_key").as_string() == k_secret);
        auto rules =
            rules_for(std::string(ctx.at("service").as_string()), ctx.at("normalize").as_bool());
        rules.regions = {std::string(ctx.at("region").as_string())};
        const auto now = parse_iso(std::string(ctx.at("timestamp").as_string()));

        for (std::string form : {"header", "query"}) {
            BOOST_TEST_CONTEXT(dir.filename().string() << " (" << form << ")") {
                const auto req = parse_http(slurp(dir / (form + "-signed-request.txt")));
                const auto parsed = sv4::parse(req.view(), rules, now);
                BOOST_REQUIRE_MESSAGE(parsed.has_value(),
                                      parsed.error().code << ": " << parsed.error().message);
                BOOST_TEST(parsed->presigned == (form == "query"));
                BOOST_TEST(parsed->access_key_id == creds.at("access_key_id").as_string());
                const auto expected_sts =
                    trim_newlines(slurp(dir / (form + "-string-to-sign.txt")));
                const auto expected_sig = trim_newlines(slurp(dir / (form + "-signature.txt")));
                const auto key = key_for(*parsed);
                if (dir.filename() == "post-sts-header-after" && form == "query") {
                    // The token was appended after signing: it matches the
                    // token-less alternative, which verify() also accepts.
                    BOOST_REQUIRE(parsed->string_to_sign_without_token.has_value());
                    BOOST_TEST(*parsed->string_to_sign_without_token == expected_sts);
                } else {
                    BOOST_TEST(parsed->canonical_request ==
                               trim_newlines(slurp(dir / (form + "-canonical-request.txt"))));
                    BOOST_TEST(parsed->string_to_sign == expected_sts);
                    BOOST_TEST(sv4::compute_signature(*parsed, key) == expected_sig);
                }
                BOOST_TEST(parsed->signature == expected_sig);
                BOOST_TEST(sv4::verify(*parsed, key));
                if (const auto* token = creds.if_contains("token")) {
                    BOOST_TEST(parsed->security_token.value_or("") != "");
                    if (form == "query") {
                        BOOST_TEST(*parsed->security_token == token->as_string());
                    }
                }
                ++cases;
            }
        }
    }
    // 38 cases in the vendored copy, each in two forms.
    BOOST_TEST(cases == 76);
}

// ---------------------------------------------------------------------------
// Canonicalisation details
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(path_rules_double_encode_except_s3) {
    // Most services encode the wire path again; S3 keeps escapes as sent.
    BOOST_TEST(sv4::canonical_uri("/a%20b/", false) == "/a%2520b/");
    BOOST_TEST(sv4::canonical_uri("/a%20b/", true) == "/a%20b/");
    BOOST_TEST(sv4::canonical_uri("/a%7Eb", true) == "/a%7Eb");  // not rewritten to ~
    BOOST_TEST(sv4::canonical_uri("/a%7eb", true) == "/a%7Eb");  // hex case normalised
    BOOST_TEST(sv4::canonical_uri("/a b", true) == "/a%20b");
    BOOST_TEST(sv4::canonical_uri("/100%", true) == "/100%25");                // stray '%'
    BOOST_TEST(sv4::canonical_uri("/a+b=c,d:e@f!", true) == "/a+b=c,d:e@f!");  // pchar kept
    BOOST_TEST(sv4::canonical_uri("/a+b", false) == "/a%2Bb");
    BOOST_TEST(sv4::canonical_uri("/a/./b/../c//d/", false) == "/a/c/d/");
    BOOST_TEST(sv4::canonical_uri("/a/./b/../c//d/", true) == "/a/./b/../c//d/");
    BOOST_TEST(sv4::canonical_uri("", false) == "/");
    BOOST_TEST(sv4::canonical_uri("/..", false) == "/");
}

BOOST_AUTO_TEST_CASE(query_rules) {
    BOOST_TEST(sv4::canonical_query("b=2&a=1&a=0", false) == "a=0&a=1&b=2");
    BOOST_TEST(sv4::canonical_query("k", false) == "k=");
    BOOST_TEST(sv4::canonical_query("a+b=c d", false) == "a%2Bb=c%20d");  // '+' is literal
    BOOST_TEST(sv4::canonical_query("x=%7e", false) == "x=~");
    BOOST_TEST(sv4::canonical_query("X-Amz-Signature=abc&a=1", true) == "a=1");
    BOOST_TEST(sv4::canonical_query("X-Amz-Signature=abc&a=1", false) == "X-Amz-Signature=abc&a=1");
}

// ---------------------------------------------------------------------------
// Rejections
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(tampering_breaks_the_signature) {
    auto r = vanilla();
    const auto rules = rules_for("service");
    auto parsed = sv4::parse(r.view(), rules, k_now);
    BOOST_REQUIRE(parsed.has_value());
    BOOST_TEST(sv4::verify(*parsed, key_for(*parsed)));

    // Another path, method, query or secret: parses, does not verify.
    for (const auto& mutate : std::vector<std::function<void(http_request&)>>{
             [](http_request& x) { x.path = "/other"; },
             [](http_request& x) { x.method = "DELETE"; }, [](http_request& x) { x.query = "a=1"; },
             [](http_request& x) {
                 for (auto& [n, v] : x.headers) {
                     if (n == "Host") {
                         v = "evil.example.com";
                     }
                 }
             }}) {
        auto t = r;
        mutate(t);
        auto p = sv4::parse(t.view(), rules, k_now);
        BOOST_REQUIRE(p.has_value());
        BOOST_TEST(!sv4::verify(*p, key_for(*p)));
    }
    BOOST_TEST(!sv4::verify(*parsed, sv4::derive_signing_key("not-the-secret", parsed->scope)));
}

BOOST_AUTO_TEST_CASE(scope_is_checked) {
    auto r = vanilla();
    auto wrong_service = sv4::parse(r.view(), rules_for("iam"), k_now);
    BOOST_REQUIRE(!wrong_service.has_value());
    BOOST_TEST(wrong_service.error().code == "SignatureDoesNotMatch");
    BOOST_TEST(wrong_service.error().message.find("correct service") != std::string::npos);

    auto rules = rules_for("service");
    rules.regions = {"eu-west-1"};
    auto wrong_region = sv4::parse(r.view(), rules, k_now);
    BOOST_REQUIRE(!wrong_region.has_value());
    BOOST_TEST(wrong_region.error().message.find("valid region") != std::string::npos);
    rules.regions.clear();  // any region
    BOOST_TEST(sv4::parse(r.view(), rules, k_now).has_value());

    // Scope date that disagrees with X-Amz-Date.
    auto bad_date = r;
    for (auto& [n, v] : bad_date.headers) {
        if (n == "Authorization") {
            v.replace(v.find("20150830"), 8, "20150829");
        }
    }
    auto p = sv4::parse(bad_date.view(), rules_for("service"), k_now);
    BOOST_REQUIRE(!p.has_value());
    BOOST_TEST(p.error().message.find("valid date") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(clock_skew_window) {
    const auto r = vanilla();
    const auto rules = rules_for("service");
    using std::chrono::minutes;
    BOOST_TEST(sv4::parse(r.view(), rules, k_now + minutes(15)).has_value());
    BOOST_TEST(sv4::parse(r.view(), rules, k_now - minutes(15)).has_value());

    auto late = sv4::parse(r.view(), rules, k_now + minutes(16));
    BOOST_REQUIRE(!late.has_value());
    BOOST_TEST(late.error().code == "SignatureDoesNotMatch");
    BOOST_TEST(late.error().message ==
               "Signature expired: 20150830T123600Z is now earlier than 20150830T123700Z "
               "(20150830T125200Z - 15 min.)");
    auto early = sv4::parse(r.view(), rules, k_now - minutes(16));
    BOOST_REQUIRE(!early.has_value());
    BOOST_TEST(early.error().message.starts_with("Signature not yet current: 20150830T123600Z"));

    auto s3 = rules;
    s3.protocol = sv4::error_protocol::s3;
    auto s3_late = sv4::parse(r.view(), s3, k_now + minutes(16));
    BOOST_REQUIRE(!s3_late.has_value());
    BOOST_TEST(s3_late.error().code == "RequestTimeTooSkewed");
}

BOOST_AUTO_TEST_CASE(presigned_expiry_and_bounds) {
    const auto dir = fs::path(SIGV4_SUITE_DIR) / "get-vanilla";
    const auto r = parse_http(slurp(dir / "query-signed-request.txt"));
    const auto rules = rules_for("service");
    using std::chrono::seconds;
    BOOST_TEST(sv4::parse(r.view(), rules, k_now + seconds(3600)).has_value());
    auto expired = sv4::parse(r.view(), rules, k_now + seconds(3601));
    BOOST_REQUIRE(!expired.has_value());
    BOOST_TEST(expired.error().code == "RequestExpired");
    BOOST_TEST(expired.error().message == "Request has expired.");
    // A presigned URL dated well in the future is not valid yet.
    BOOST_TEST(!sv4::parse(r.view(), rules, k_now - seconds(16 * 60)).has_value());

    for (std::string bad : {"0", "604801", "-5", "", "abc", "99999999999999999999"}) {
        auto t = r;
        auto at = t.query.find("X-Amz-Expires=3600");
        BOOST_REQUIRE(at != std::string::npos);
        t.query.replace(at, std::string("X-Amz-Expires=3600").size(), "X-Amz-Expires=" + bad);
        auto p = sv4::parse(t.view(), rules, k_now);
        BOOST_TEST_CONTEXT("X-Amz-Expires=" << bad) {
            BOOST_REQUIRE(!p.has_value());
            BOOST_TEST(p.error().code == "IncompleteSignature");
        }
    }
    auto max = r;
    max.query.replace(max.query.find("X-Amz-Expires=3600"), 18, "X-Amz-Expires=604800");
    BOOST_TEST(sv4::parse(max.view(), rules, k_now).has_value());
}

BOOST_AUTO_TEST_CASE(host_and_amz_headers_must_be_signed) {
    const auto rules = rules_for("service");
    // Host left out of SignedHeaders.
    auto r = vanilla();
    for (auto& [n, v] : r.headers) {
        if (n == "Authorization") {
            v.replace(v.find("SignedHeaders=host;x-amz-date"), 29, "SignedHeaders=x-amz-date");
        }
    }
    auto p = sv4::parse(r.view(), rules, k_now);
    BOOST_REQUIRE(!p.has_value());
    BOOST_TEST(p.error().message.find("'Host' must be a 'SignedHeader'") != std::string::npos);

    // An x-amz-* header present but unsigned.
    auto extra = vanilla();
    extra.headers.emplace_back("X-Amz-Target", "Kinesis_20131202.PutRecord");
    p = sv4::parse(extra.view(), rules, k_now);
    BOOST_REQUIRE(!p.has_value());
    BOOST_TEST(p.error().message.find("not signed: x-amz-target") != std::string::npos);

    // The session token may be unsigned outside S3 (post-sts-header-after),
    // but not under the S3 protocol.
    auto token = vanilla();
    token.headers.emplace_back("X-Amz-Security-Token", "tok");
    BOOST_TEST(sv4::parse(token.view(), rules, k_now).has_value());
    auto s3 = rules;
    s3.protocol = sv4::error_protocol::s3;
    token.headers.emplace_back("x-amz-content-sha256", std::string(sv4::k_empty_sha256));
    sign_header_form(token, rules);  // signs everything, token included
    BOOST_TEST(sv4::parse(token.view(), s3, k_now).has_value());

    // A signed header that is not in the request.
    auto missing = vanilla();
    for (auto& [n, v] : missing.headers) {
        if (n == "Authorization") {
            v.replace(v.find("host;x-amz-date"), 15, "host;x-amz-date;x-foo");
        }
    }
    BOOST_TEST(!sv4::parse(missing.view(), rules, k_now).has_value());
}

BOOST_AUTO_TEST_CASE(malformed_authorization_is_incomplete_signature) {
    const auto rules = rules_for("service");
    auto with_auth = [&](std::string value) {
        auto r = vanilla();
        for (auto& [n, v] : r.headers) {
            if (n == "Authorization") {
                v = value;
            }
        }
        return sv4::parse(r.view(), rules, k_now);
    };
    auto p = with_auth("AWS4-HMAC-SHA256 SignedHeaders=host;x-amz-date, Signature=" +
                       std::string(64, 'a'));
    BOOST_REQUIRE(!p.has_value());
    BOOST_TEST(p.error().code == "IncompleteSignature");
    BOOST_TEST(p.error().message == "Authorization header requires 'Credential' parameter.");
    p = with_auth("AWS4-HMAC-SHA1 Credential=x");
    BOOST_REQUIRE(!p.has_value());
    BOOST_TEST(p.error().code == "IncompleteSignature");
    p = with_auth(
        "AWS4-HMAC-SHA256 Credential=AKID/20150830/us-east-1/service, "
        "SignedHeaders=host;x-amz-date, Signature=" +
        std::string(64, 'a'));
    BOOST_TEST(!p.has_value());
    // Upper-case or short signatures never verify.
    p = with_auth(
        "AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20150830/us-east-1/service/"
        "aws4_request, SignedHeaders=host;x-amz-date, Signature=" +
        std::string(64, 'A'));
    BOOST_REQUIRE(!p.has_value());
    BOOST_TEST(p.error().code == "SignatureDoesNotMatch");

    // Unsigned request, and both mechanisms at once.
    auto bare = vanilla();
    std::erase_if(bare.headers, [](const auto& h) { return h.first == "Authorization"; });
    p = sv4::parse(bare.view(), rules, k_now);
    BOOST_REQUIRE(!p.has_value());
    BOOST_TEST(p.error().message == "Request is missing Authentication Token");
    auto both = vanilla();
    both.query = "X-Amz-Algorithm=AWS4-HMAC-SHA256";
    p = sv4::parse(both.view(), rules, k_now);
    BOOST_REQUIRE(!p.has_value());
    BOOST_TEST(p.error().message.starts_with("Only one auth mechanism allowed"));
}

BOOST_AUTO_TEST_CASE(payload_hash_modes) {
    auto rules = rules_for("service");
    auto r = vanilla();
    r.method = "POST";
    r.body = "Action=ListUsers";
    r.headers.emplace_back("X-Amz-Content-Sha256", sv4::detail::sha256_hex(r.body));
    sign_header_form(r, rules);
    BOOST_TEST(sv4::parse(r.view(), rules, k_now).has_value());

    // A body that does not match its declared hash.
    auto wrong = r;
    wrong.body = "Action=DeleteUser";
    auto p = sv4::parse(wrong.view(), rules, k_now);
    BOOST_REQUIRE(!p.has_value());
    BOOST_TEST(p.error().message.find("does not match what was computed") != std::string::npos);

    // Streaming forms only where the front end declares them.
    auto streaming = vanilla();
    streaming.headers.emplace_back("X-Amz-Content-Sha256", "STREAMING-AWS4-HMAC-SHA256-PAYLOAD");
    sign_header_form(streaming, [] {
        auto s = rules_for("service");
        s.payload_modes.set(static_cast<std::size_t>(sv4::payload_mode::streaming_signed));
        return s;
    }());
    BOOST_TEST(!sv4::parse(streaming.view(), rules, k_now).has_value());
    rules.payload_modes.set(static_cast<std::size_t>(sv4::payload_mode::streaming_signed));
    auto ok = sv4::parse(streaming.view(), rules, k_now);
    BOOST_REQUIRE(ok.has_value());
    BOOST_TEST((ok->payload == sv4::payload_mode::streaming_signed));
    BOOST_TEST(ok->canonical_request.ends_with("\nSTREAMING-AWS4-HMAC-SHA256-PAYLOAD"));

    // S3 requires the header on header-signed requests, and presigned URLs
    // default to UNSIGNED-PAYLOAD.
    auto s3 = rules_for("service", false);
    s3.protocol = sv4::error_protocol::s3;
    p = sv4::parse(vanilla().view(), s3, k_now);
    BOOST_REQUIRE(!p.has_value());
    BOOST_TEST(p.error().message.find("x-amz-content-sha256") != std::string::npos);
    const auto q =
        parse_http(slurp(fs::path(SIGV4_SUITE_DIR) / "get-vanilla" / "query-signed-request.txt"));
    auto presigned = sv4::parse(q.view(), s3, k_now);
    BOOST_REQUIRE(presigned.has_value());
    BOOST_TEST(presigned->payload_hash == "UNSIGNED-PAYLOAD");
}

BOOST_AUTO_TEST_CASE(error_mapping_per_protocol) {
    using sv4::error_protocol;
    BOOST_TEST(sv4::signature_mismatch(error_protocol::query).code == "SignatureDoesNotMatch");
    BOOST_TEST(sv4::signature_mismatch(error_protocol::json).code == "InvalidSignatureException");
    BOOST_TEST(sv4::invalid_client_token_id(error_protocol::query).code == "InvalidClientTokenId");
    BOOST_TEST(sv4::invalid_client_token_id(error_protocol::query).message ==
               "The security token included in the request is invalid.");
    BOOST_TEST(sv4::invalid_client_token_id(error_protocol::json).code ==
               "UnrecognizedClientException");
    BOOST_TEST(sv4::invalid_client_token_id(error_protocol::s3).code == "InvalidAccessKeyId");
    BOOST_TEST(sv4::expired_token(error_protocol::query).code == "ExpiredToken");
    BOOST_TEST(sv4::request_expired(error_protocol::query).code == "RequestExpired");
    BOOST_TEST(sv4::request_expired(error_protocol::s3).code == "AccessDenied");
    BOOST_TEST(sv4::access_denied(error_protocol::json, "no").code == "AccessDeniedException");
    BOOST_TEST(sv4::access_denied(error_protocol::query, "no").code == "AccessDenied");
    BOOST_TEST(sv4::incomplete_signature(error_protocol::json, "x").code ==
               "IncompleteSignatureException");
}

// ---------------------------------------------------------------------------
// Signing-key cache
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(signing_key_cache_is_bounded_and_invalidates) {
    // AWS's documented example key derivation (SigV4 docs, "iam" in
    // us-east-1 on 2015-08-30) for the suite's example secret.
    const sv4::credential_scope scope{"20150830", "us-east-1", "iam"};
    const auto derived = sv4::derive_signing_key(k_secret, scope);

    sv4::signing_key_cache cache(2);
    BOOST_TEST((cache.get("A", k_secret, scope) == derived));
    // A hit returns the cached key even if a caller passes another secret,
    // which is why invalidate() must run when a secret changes.
    BOOST_TEST((cache.get("A", "rotated", scope) == derived));
    cache.invalidate("A");
    BOOST_TEST(cache.size() == 0u);
    BOOST_TEST((cache.get("A", "rotated", scope) != derived));

    // Bounded: the least recently used entry goes first.
    cache.invalidate("A");
    (void)cache.get("A", k_secret, scope);
    (void)cache.get("B", k_secret, scope);
    (void)cache.get("A", k_secret, scope);  // A is now the most recent
    (void)cache.get("C", k_secret, scope);  // evicts B
    BOOST_TEST(cache.size() == 2u);
    BOOST_TEST((cache.get("A", "other", scope) == derived));  // still cached
    BOOST_TEST((cache.get("B", "other", scope) != derived));  // re-derived
    // Different scope parts are different entries.
    const sv4::credential_scope sts{"20150830", "us-east-1", "sts"};
    BOOST_TEST((cache.get("A", k_secret, sts) != derived));
    BOOST_CHECK_THROW(sv4::signing_key_cache(0), std::invalid_argument);
}

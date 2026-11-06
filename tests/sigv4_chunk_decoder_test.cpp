// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Streaming SigV4 payloads and failure rate limiting
// (`.kiro/specs/iam-compatible-auth/` task 7, Requirements 3.4 and 3.8).
//
// The vectors under sigv4_chunked/ are signed by a pinned botocore
// (gen_vectors.py): every case must verify and decode to its payload
// however the body is split across feed() calls, and every truncation,
// corruption, reordering or wrong key must fail without releasing an
// unverified byte.

#define BOOST_TEST_MODULE sigv4_chunk_decoder_test

#include <boost/test/unit_test.hpp>

#include <raft/sigv4_chunk_decoder.hpp>
#include <raft/sigv4_failure_limiter.hpp>

#include <boost/json.hpp>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <ostream>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace sigv4 = kythira::sigv4;

namespace {

auto load() -> const boost::json::object& {
    static const boost::json::object doc = [] {
        std::ifstream in(SIGV4_CHUNKED_VECTORS, std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(in),
                               std::istreambuf_iterator<char>()};
        return boost::json::parse(text).as_object();
    }();
    return doc;
}

auto str(const boost::json::object& o, std::string_view k) -> std::string {
    return std::string(o.at(k).as_string());
}

auto unhex(std::string_view h) -> std::string {
    std::string out;
    out.reserve(h.size() / 2);
    for (std::size_t i = 0; i + 1 < h.size(); i += 2) {
        out.push_back(static_cast<char>(std::stoi(std::string(h.substr(i, 2)), nullptr, 16)));
    }
    return out;
}

auto now() -> std::chrono::sys_seconds {
    // The vectors' signing time, 20261106T120000Z.
    using namespace std::chrono;
    return sys_days{year{2026} / November / 6} + hours{12};
}

auto rules() -> sigv4::service_rules {
    sigv4::service_rules r;
    r.service = "s3";
    r.s3_path_rules = true;
    r.protocol = sigv4::error_protocol::s3;
    r.payload_modes.set();  // every form
    return r;
}

/// One vector: its request, payload, trailers, and the bytes on the wire.
struct vector_case {
    std::string name;
    std::string mode;
    std::string method;
    std::string path;
    std::string query;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::string payload;
    std::vector<std::pair<std::string, std::string>> trailers;

    [[nodiscard]] auto request() const -> sigv4::request {
        return {method, path, query, headers, std::nullopt};
    }
};

auto cases() -> std::vector<vector_case> {
    std::vector<vector_case> out;
    for (const auto& v : load().at("cases").as_array()) {
        const auto& o = v.as_object();
        vector_case c{str(o, "name"),
                      str(o, "mode"),
                      str(o, "method"),
                      str(o, "path"),
                      str(o, "query"),
                      {},
                      unhex(str(o, "body_hex")),
                      unhex(str(o, "payload_hex")),
                      {}};
        for (const auto& h : o.at("headers").as_array()) {
            c.headers.emplace_back(h.as_array()[0].as_string(), h.as_array()[1].as_string());
        }
        for (const auto& t : o.at("trailers").as_array()) {
            c.trailers.emplace_back(t.as_array()[0].as_string(), t.as_array()[1].as_string());
        }
        out.push_back(std::move(c));
    }
    return out;
}

auto chunked_cases() -> std::vector<vector_case> {
    auto all = cases();
    std::erase_if(all, [](const vector_case& c) { return c.mode == "presigned"; });
    return all;
}

auto key_for(const sigv4::claim& c, std::string_view secret = {}) -> sigv4::signing_key {
    return sigv4::derive_signing_key(
        secret.empty() ? str(load(), "secret_key") : std::string(secret), c.scope);
}

struct decoded {
    sigv4::chunk_decoder::status status;
    std::string out;
    sigv4::aws_error error;
    std::vector<std::pair<std::string, std::string>> trailers;
};

/// Feeds `body` in pieces cut at `cuts`, then finish().
auto decode(const vector_case& vc, std::string_view body, const std::vector<std::size_t>& cuts = {},
            std::string_view secret = {}, sigv4::chunk_limits limits = {}) -> decoded {
    const auto req = vc.request();
    const auto parsed = sigv4::parse(req, rules(), now());
    BOOST_TEST_REQUIRE(parsed.has_value(), vc.name << ": " << parsed.error().message);
    const auto key = key_for(*parsed, secret);
    sigv4::chunk_decoder d(*parsed, &key, req, sigv4::error_protocol::s3, limits);
    decoded r{d.state(), {}, {}, {}};
    std::size_t at = 0;
    for (auto cut : cuts) {
        if (cut > at && cut < body.size()) {
            d.feed(body.substr(at, cut - at), r.out);
            at = cut;
        }
    }
    d.feed(body.substr(at), r.out);
    r.status = d.finish();
    r.error = d.error();
    r.trailers = d.trailers();
    return r;
}

using status = sigv4::chunk_decoder::status;

}  // namespace

namespace kythira::sigv4 {
// For BOOST_TEST's failure messages.
inline auto operator<<(std::ostream& os, chunk_decoder::status s) -> std::ostream& {
    return os << (s == chunk_decoder::status::done     ? "done"
                  : s == chunk_decoder::status::failed ? "failed"
                                                       : "need_more");
}
inline auto operator<<(std::ostream& os, failure_limiter::verdict v) -> std::ostream& {
    return os << static_cast<int>(v);
}
}  // namespace kythira::sigv4

BOOST_AUTO_TEST_SUITE(vectors)

BOOST_AUTO_TEST_CASE(generated_by_pinned_botocore) {
    BOOST_TEST(str(load(), "botocore") == "1.43.100");
    BOOST_TEST(cases().size() == 9U);
}

BOOST_AUTO_TEST_CASE(seed_signatures_verify) {
    for (const auto& vc : cases()) {
        BOOST_TEST_CONTEXT(vc.name) {
            const auto parsed = sigv4::parse(vc.request(), rules(), now());
            BOOST_TEST_REQUIRE(parsed.has_value(), parsed.error().message);
            BOOST_TEST(sigv4::verify(*parsed, key_for(*parsed)));
            BOOST_TEST(!sigv4::verify(*parsed, key_for(*parsed, "another-secret")));
        }
    }
}

BOOST_AUTO_TEST_CASE(presigned_token_and_path) {
    for (const auto& vc : cases()) {
        if (vc.mode != "presigned") {
            continue;
        }
        BOOST_TEST_CONTEXT(vc.name) {
            const auto parsed = sigv4::parse(vc.request(), rules(), now());
            BOOST_TEST_REQUIRE(parsed.has_value());
            BOOST_TEST(parsed->presigned);
            if (vc.name.ends_with("token")) {
                BOOST_TEST(parsed->security_token.value_or("") == str(load(), "session_token"));
            }
            auto moved = vc;
            moved.path += "x";
            const auto other = sigv4::parse(moved.request(), rules(), now());
            BOOST_TEST_REQUIRE(other.has_value());
            BOOST_TEST(!sigv4::verify(*other, key_for(*other)));
        }
    }
}

BOOST_AUTO_TEST_CASE(decode_whole_and_split) {
    std::mt19937 rng(7);
    for (const auto& vc : chunked_cases()) {
        BOOST_TEST_CONTEXT(vc.name) {
            const auto whole = decode(vc, vc.body);
            BOOST_TEST_REQUIRE(whole.status == status::done, whole.error.message);
            BOOST_TEST(whole.out == vc.payload);
            BOOST_TEST(whole.trailers == vc.trailers);

            // One byte at a time.
            std::vector<std::size_t> every(vc.body.size());
            for (std::size_t i = 0; i < every.size(); ++i) {
                every[i] = i;
            }
            const auto bytewise = decode(vc, vc.body, every);
            BOOST_TEST_REQUIRE(bytewise.status == status::done, bytewise.error.message);
            BOOST_TEST(bytewise.out == vc.payload);

            // Random cuts.
            for (int round = 0; round < 20; ++round) {
                std::vector<std::size_t> cuts;
                std::uniform_int_distribution<std::size_t> d(0, vc.body.size());
                for (int i = 0; i < 6; ++i) {
                    cuts.push_back(d(rng));
                }
                std::ranges::sort(cuts);
                const auto r = decode(vc, vc.body, cuts);
                BOOST_TEST_REQUIRE(r.status == status::done, r.error.message);
                BOOST_TEST_REQUIRE(r.out == vc.payload);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(every_truncation_fails) {
    for (const auto& vc : chunked_cases()) {
        BOOST_TEST_CONTEXT(vc.name) {
            for (std::size_t n = 0; n < vc.body.size(); ++n) {
                const auto r = decode(vc, std::string_view(vc.body).substr(0, n));
                BOOST_TEST_REQUIRE(r.status == status::failed, "prefix " << n);
                // Whatever was released is a prefix of the payload.
                BOOST_TEST_REQUIRE(vc.payload.starts_with(r.out));
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(trailing_garbage_fails) {
    for (const auto& vc : chunked_cases()) {
        BOOST_TEST_CONTEXT(vc.name) {
            BOOST_TEST(decode(vc, vc.body + "x").status == status::failed);
            BOOST_TEST(decode(vc, vc.body + "\r\n").status == status::failed);
        }
    }
}

BOOST_AUTO_TEST_CASE(signed_corruption_fails_before_release) {
    for (const auto& vc : chunked_cases()) {
        if (vc.mode == "streaming_unsigned_trailer") {
            continue;
        }
        BOOST_TEST_CONTEXT(vc.name) {
            const auto first_crlf = vc.body.find("\r\n");
            const auto first_chunk_size =
                std::stoul(vc.body.substr(0, vc.body.find(';')), nullptr, 16);
            for (std::size_t i = 0; i < vc.body.size(); ++i) {
                auto bad = vc.body;
                bad[i] = static_cast<char>(bad[i] ^ 0x01);
                const auto r = decode(vc, bad);
                BOOST_TEST_REQUIRE(r.status == status::failed, "byte " << i);
                // A flip inside the first chunk (header or data) releases
                // nothing at all.
                if (i < first_crlf + 2 + first_chunk_size + 2) {
                    BOOST_TEST_REQUIRE(r.out.empty(), "byte " << i);
                }
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(wrong_key_releases_nothing) {
    for (const auto& vc : chunked_cases()) {
        if (vc.mode == "streaming_unsigned_trailer") {
            continue;
        }
        BOOST_TEST_CONTEXT(vc.name) {
            const auto r = decode(vc, vc.body, {}, "not-the-secret");
            BOOST_TEST(r.status == status::failed);
            BOOST_TEST(r.out.empty());
            BOOST_TEST(r.error.code == "SignatureDoesNotMatch");
        }
    }
}

BOOST_AUTO_TEST_CASE(chunks_cannot_be_reordered_or_dropped) {
    const auto all = cases();
    const auto& vc = all.front();  // signed-three-chunks: 400, 400, 200, 0
    BOOST_TEST_REQUIRE(vc.name == "signed-three-chunks");
    std::vector<std::string> chunks;
    std::size_t at = 0;
    while (at < vc.body.size()) {
        const auto eol = vc.body.find("\r\n", at);
        const auto size = std::stoul(vc.body.substr(at, vc.body.find(';', at) - at), nullptr, 16);
        const auto end = eol + 2 + size + 2;
        chunks.push_back(vc.body.substr(at, end - at));
        at = end;
    }
    BOOST_TEST_REQUIRE(chunks.size() == 4U);
    // The two 400-byte chunks swapped: same sizes, wrong chain.
    const auto swapped = chunks[1] + chunks[0] + chunks[2] + chunks[3];
    const auto r1 = decode(vc, swapped);
    BOOST_TEST(r1.status == status::failed);
    BOOST_TEST(r1.out.empty());
    // A chunk dropped (the decoded length would catch it too; the chain
    // fails first).
    const auto r2 = decode(vc, chunks[0] + chunks[2] + chunks[3]);
    BOOST_TEST(r2.status == status::failed);
    BOOST_TEST(r2.out == vc.payload.substr(0, 400));
}

BOOST_AUTO_TEST_CASE(limits) {
    for (const auto& vc : chunked_cases()) {
        BOOST_TEST_CONTEXT(vc.name) {
            if (!vc.payload.empty()) {
                const auto r = decode(vc, vc.body, {}, {}, {.max_chunk_size = 100});
                BOOST_TEST(r.status == status::failed);
                BOOST_TEST(r.out.empty());
            }
            const auto lines = decode(vc, vc.body, {}, {}, {.max_line = 2});
            BOOST_TEST(lines.status == status::failed);
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(headers)

auto find_case(std::string_view name) -> vector_case {
    for (auto& c : cases()) {
        if (c.name == name) {
            return c;
        }
    }
    BOOST_FAIL("no case " << name);
    return {};
}

void set_header(vector_case& vc, std::string_view name, std::string value) {
    for (auto& [k, v] : vc.headers) {
        if (k == name) {
            v = std::move(value);
            return;
        }
    }
    vc.headers.emplace_back(name, std::move(value));
}

BOOST_AUTO_TEST_CASE(decoded_length_enforced) {
    // Unsigned trailer: the headers are signed, so changing them breaks the
    // seed, but the decoder itself does not re-verify the seed. Check it
    // directly against an altered length.
    auto vc = find_case("unsigned-trailer-no-checksum");
    set_header(vc, "x-amz-decoded-content-length", "699");
    auto r = decode(vc, vc.body);
    BOOST_TEST(r.status == status::failed);
    BOOST_TEST(r.error.code == "IncompleteBody");
    set_header(vc, "x-amz-decoded-content-length", "701");
    r = decode(vc, vc.body);
    BOOST_TEST(r.status == status::failed);
    BOOST_TEST(r.error.code == "IncompleteBody");
    set_header(vc, "x-amz-decoded-content-length", "7x");
    BOOST_TEST(decode(vc, vc.body).status == status::failed);
}

BOOST_AUTO_TEST_CASE(trailers_must_be_declared) {
    auto vc = find_case("unsigned-trailer-crc32");
    set_header(vc, "x-amz-trailer", "x-amz-checksum-sha256");
    auto r = decode(vc, vc.body);
    BOOST_TEST(r.status == status::failed);  // crc32 undeclared
    set_header(vc, "x-amz-trailer", "x-amz-checksum-crc32,x-amz-checksum-sha256");
    r = decode(vc, vc.body);
    BOOST_TEST(r.status == status::failed);  // sha256 missing
    set_header(vc, "x-amz-trailer", "x-amz-trailer-signature");
    BOOST_TEST(decode(vc, vc.body).status == status::failed);
}

BOOST_AUTO_TEST_CASE(signed_trailer_needs_its_signature) {
    const auto vc = find_case("signed-trailer-crc32");
    auto body = vc.body;
    const auto sig = body.find("x-amz-trailer-signature:");
    BOOST_TEST_REQUIRE(sig != std::string::npos);
    const auto unsigned_body = body.substr(0, sig) + "\r\n";
    const auto r = decode(vc, unsigned_body);
    BOOST_TEST(r.status == status::failed);
    BOOST_TEST(r.error.code == "SignatureDoesNotMatch");
    // The payload was verified chunk by chunk, so it was released; the
    // stream as a whole still fails and the front end discards it.
    BOOST_TEST(r.out == vc.payload);
}

BOOST_AUTO_TEST_CASE(non_streaming_claims_are_refused) {
    const auto vc = find_case("presigned-get");
    const auto req = vc.request();
    const auto parsed = sigv4::parse(req, rules(), now());
    BOOST_TEST_REQUIRE(parsed.has_value());
    const auto key = key_for(*parsed);
    sigv4::chunk_decoder d(*parsed, &key, req, sigv4::error_protocol::s3);
    BOOST_TEST(d.state() == status::failed);

    const auto signed_vc = find_case("signed-three-chunks");
    const auto sreq = signed_vc.request();
    const auto sparsed = sigv4::parse(sreq, rules(), now());
    sigv4::chunk_decoder no_key(*sparsed, nullptr, sreq, sigv4::error_protocol::s3);
    BOOST_TEST(no_key.state() == status::failed);
}

BOOST_AUTO_TEST_SUITE_END()

// ── failure limiter ─────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(limiter)

BOOST_AUTO_TEST_CASE(source_keys) {
    using L = sigv4::failure_limiter;
    BOOST_TEST(L::source_key("192.0.2.7") == "192.0.2.7");
    BOOST_TEST(L::source_key("::ffff:192.0.2.7") == "192.0.2.7");
    BOOST_TEST(L::source_key("2001:db8:1:2:3:4:5:6") == "2001:db8:1:2::/64");
    BOOST_TEST(L::source_key("[2001:db8:1:2:ffff::1]") == "2001:db8:1:2::/64");
    BOOST_TEST(L::source_key("not-an-address") == "not-an-address");
}

BOOST_AUTO_TEST_CASE(per_source_budget_and_window) {
    sigv4::failure_limiter lim({.per_source = 3, .per_key = 0, .window = std::chrono::seconds(60)});
    const auto t0 = sigv4::failure_limiter::clock::time_point{} + std::chrono::hours(1);
    const auto src = sigv4::failure_limiter::source_key("2001:db8::1");
    for (int i = 0; i < 3; ++i) {
        BOOST_TEST((lim.check(src, "k" + std::to_string(i), t0) ==
                    sigv4::failure_limiter::verdict::allowed));
        lim.record_failure(src, "k" + std::to_string(i), t0);
    }
    // Another address in the same /64 shares the budget.
    const auto neighbour = sigv4::failure_limiter::source_key("2001:db8::ffff");
    BOOST_TEST((lim.check(neighbour, "k9", t0) == sigv4::failure_limiter::verdict::source_limited));
    BOOST_TEST((lim.check("192.0.2.1", "k9", t0) == sigv4::failure_limiter::verdict::allowed));
    BOOST_TEST((lim.check(src, "k9", t0 + std::chrono::seconds(60)) ==
                sigv4::failure_limiter::verdict::allowed));
    BOOST_TEST(lim.tracked_keys() == 0U);
}

BOOST_AUTO_TEST_CASE(per_key_budget) {
    sigv4::failure_limiter lim({.per_source = 0, .per_key = 2, .window = std::chrono::seconds(60)});
    const auto t0 = sigv4::failure_limiter::clock::time_point{} + std::chrono::hours(1);
    lim.record_failure("192.0.2.1", "AKID", t0);
    lim.record_failure("192.0.2.2", "AKID", t0);
    BOOST_TEST(
        (lim.check("192.0.2.3", "AKID", t0) == sigv4::failure_limiter::verdict::key_limited));
    BOOST_TEST((lim.check("192.0.2.3", "OTHER", t0) == sigv4::failure_limiter::verdict::allowed));
    BOOST_TEST(lim.tracked_sources() == 0U);
}

BOOST_AUTO_TEST_CASE(table_is_bounded_and_keeps_the_charged_key) {
    sigv4::failure_limiter lim(
        {.per_source = 2, .per_key = 0, .window = std::chrono::seconds(60), .max_tracked = 8});
    const auto t0 = sigv4::failure_limiter::clock::time_point{} + std::chrono::hours(1);
    for (int i = 0; i < 100; ++i) {
        // Every source here is older than the one being charged, so each
        // eviction has a candidate; the charged one must always survive.
        const auto src = "cycle-" + std::to_string(i);
        const auto at = t0 + std::chrono::milliseconds(i);
        lim.record_failure(src, "", at);
        lim.record_failure(src, "", at);
        BOOST_TEST_REQUIRE(lim.tracked_sources() <= 8U);
        BOOST_TEST_REQUIRE(
            (lim.check(src, "", at) == sigv4::failure_limiter::verdict::source_limited));
    }
    // Expired windows go before any live one.
    const auto later = t0 + std::chrono::seconds(120);
    lim.record_failure("fresh", "", later);
    BOOST_TEST(lim.tracked_sources() == 1U);
}

BOOST_AUTO_TEST_CASE(throttle_errors) {
    BOOST_TEST(sigv4::throttled(sigv4::error_protocol::query).code == "Throttling");
    BOOST_TEST(sigv4::throttled(sigv4::error_protocol::json).code == "ThrottlingException");
    BOOST_TEST(sigv4::throttled(sigv4::error_protocol::s3).code == "SlowDown");
    BOOST_TEST(sigv4::throttled(sigv4::error_protocol::s3).http_status == 503);
}

BOOST_AUTO_TEST_SUITE_END()

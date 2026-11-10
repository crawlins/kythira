// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Kinesis spec task 2 (.kiro/specs/kinesis-compatible-streams/ Requirements
// 4.1 and 4.3): the CBOR side of the shared AWS JSON codec. RFC 8949
// Appendix A's vectors through the decoder, malformed bodies, the
// shape-guided mapping (blobs as byte strings, timestamps as epoch
// milliseconds under tag 1), Jackson-style indefinite-length bodies, and the
// codec and dispatcher answering each request in its own encoding.

#define BOOST_TEST_MODULE aws_json_cbor_test

#include <boost/test/unit_test.hpp>

#include <raft/aws_json_protocol.hpp>

#include "aws_json_protocol/fixture_model.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace aj = kythira::aws_json;
namespace fx = kythira::aws_json_fixture;

namespace {

auto unhex(std::string_view h) -> std::string {
    std::string out;
    for (std::size_t i = 0; i + 1 < h.size(); i += 2) {
        out.push_back(static_cast<char>(std::stoi(std::string(h.substr(i, 2)), nullptr, 16)));
    }
    return out;
}

auto hex(std::string_view s) -> std::string {
    static constexpr std::string_view digits = "0123456789abcdef";
    std::string out;
    for (const char c : s) {
        const auto b = static_cast<unsigned char>(c);
        out.push_back(digits[b >> 4U]);
        out.push_back(digits[b & 0xFU]);
    }
    return out;
}

/// Decode with no shape guidance.
auto decode(std::string_view hexed) -> aj::result<boost::json::value> {
    return aj::cbor_to_json(fx::k_model, aj::no_shape, unhex(hexed));
}

auto decodes_to(std::string_view hexed, std::string_view json) -> bool {
    auto r = decode(hexed);
    if (!r.has_value()) {
        BOOST_TEST_MESSAGE(std::string(hexed) << ": " << r.error().message.value_or("?"));
        return false;
    }
    const auto want = boost::json::parse(json);
    if (*r != want) {
        BOOST_TEST_MESSAGE(std::string(hexed) << " gave " << boost::json::serialize(*r));
        return false;
    }
    return true;
}

auto shape(std::string_view name) -> std::uint32_t {
    const auto idx = fx::k_model.find_shape(name);
    BOOST_REQUIRE(idx != aj::no_shape);
    return idx;
}

auto cbor_codec(bool accept = true) -> aj::codec {
    aj::codec_options o;
    o.style = aj::error_style::bare();
    o.ids = aj::request_id_style::uuid;
    o.accept_cbor = accept;
    return aj::codec(fx::k_model, std::move(o));
}

auto cbor_request(std::string_view target, std::string_view body) -> aj::http_request_view {
    return aj::http_request_view{"POST", "/", "application/x-amz-cbor-1.1", target, body};
}

}  // namespace

BOOST_AUTO_TEST_SUITE(rfc8949_vectors)

BOOST_AUTO_TEST_CASE(integers) {
    BOOST_TEST(decodes_to("00", "0"));
    BOOST_TEST(decodes_to("01", "1"));
    BOOST_TEST(decodes_to("0a", "10"));
    BOOST_TEST(decodes_to("17", "23"));
    BOOST_TEST(decodes_to("1818", "24"));
    BOOST_TEST(decodes_to("1819", "25"));
    BOOST_TEST(decodes_to("1864", "100"));
    BOOST_TEST(decodes_to("1903e8", "1000"));
    BOOST_TEST(decodes_to("1a000f4240", "1000000"));
    BOOST_TEST(decodes_to("1b000000e8d4a51000", "1000000000000"));
    BOOST_TEST(decodes_to("1bffffffffffffffff", "18446744073709551615"));
    BOOST_TEST(decodes_to("20", "-1"));
    BOOST_TEST(decodes_to("29", "-10"));
    BOOST_TEST(decodes_to("3863", "-100"));
    BOOST_TEST(decodes_to("3903e7", "-1000"));
    BOOST_TEST(decodes_to("3b7fffffffffffffff", "-9223372036854775808"));
    // -18446744073709551616 does not fit a JSON integer here.
    BOOST_TEST(!decode("3bffffffffffffffff").has_value());
}

BOOST_AUTO_TEST_CASE(floats) {
    BOOST_TEST(decodes_to("f90000", "0.0"));
    BOOST_TEST(decodes_to("f93c00", "1.0"));
    BOOST_TEST(decodes_to("f93e00", "1.5"));
    BOOST_TEST(decodes_to("f97bff", "65504.0"));
    BOOST_TEST(decodes_to("fa47c35000", "100000.0"));
    auto fmax = decode("fa7f7fffff");
    BOOST_REQUIRE(fmax.has_value());
    BOOST_TEST(fmax->as_double() == static_cast<double>(std::numeric_limits<float>::max()));
    BOOST_TEST(decodes_to("fb3ff199999999999a", "1.1"));
    BOOST_TEST(decodes_to("fbc010666666666666", "-4.1"));
    BOOST_TEST(decodes_to("f90001", "5.960464477539063e-08"));
    BOOST_TEST(decodes_to("f9c400", "-4.0"));
}

BOOST_AUTO_TEST_CASE(simple_values) {
    BOOST_TEST(decodes_to("f4", "false"));
    BOOST_TEST(decodes_to("f5", "true"));
    BOOST_TEST(decodes_to("f6", "null"));
    BOOST_TEST(decodes_to("f7", "null"));
    BOOST_TEST(!decode("f0").has_value());    // simple(16)
    BOOST_TEST(!decode("f818").has_value());  // simple(24)
}

BOOST_AUTO_TEST_CASE(strings) {
    BOOST_TEST(decodes_to("40", R"("")"));
    BOOST_TEST(decodes_to("4401020304", R"("AQIDBA==")"));
    BOOST_TEST(decodes_to("60", R"("")"));
    BOOST_TEST(decodes_to("6161", R"("a")"));
    BOOST_TEST(decodes_to("6449455446", R"("IETF")"));
    BOOST_TEST(decodes_to("62225c", R"("\"\\")"));
    BOOST_TEST(decodes_to("62c3bc", "\"\xc3\xbc\""));
    BOOST_TEST(decodes_to("63e6b0b4", "\"\xe6\xb0\xb4\""));
    BOOST_TEST(decodes_to("64f0908591", "\"\xf0\x90\x85\x91\""));
}

BOOST_AUTO_TEST_CASE(containers) {
    BOOST_TEST(decodes_to("80", "[]"));
    BOOST_TEST(decodes_to("83010203", "[1,2,3]"));
    BOOST_TEST(decodes_to("8301820203820405", "[1,[2,3],[4,5]]"));
    BOOST_TEST(decodes_to("98190102030405060708090a0b0c0d0e0f101112131415161718181819",
                          "[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25]"));
    BOOST_TEST(decodes_to("a0", "{}"));
    BOOST_TEST(decodes_to("a26161016162820203", R"({"a":1,"b":[2,3]})"));
    BOOST_TEST(decodes_to("826161a161626163", R"(["a",{"b":"c"}])"));
}

BOOST_AUTO_TEST_CASE(indefinite_lengths) {
    BOOST_TEST(decodes_to("5f42010243030405ff", R"("AQIDBAU=")"));
    BOOST_TEST(decodes_to("7f657374726561646d696e67ff", R"("streaming")"));
    BOOST_TEST(decodes_to("9fff", "[]"));
    BOOST_TEST(decodes_to("9f018202039f0405ffff", "[1,[2,3],[4,5]]"));
    BOOST_TEST(decodes_to("9f01820203820405ff", "[1,[2,3],[4,5]]"));
    BOOST_TEST(decodes_to("83018202039f0405ff", "[1,[2,3],[4,5]]"));
    BOOST_TEST(decodes_to("bf61610161629f0203ffff", R"({"a":1,"b":[2,3]})"));
    BOOST_TEST(decodes_to("826161bf61626163ff", R"(["a",{"b":"c"}])"));
    BOOST_TEST(decodes_to("bf6346756ef563416d7421ff", R"({"Fun":true,"Amt":-2})"));
}

BOOST_AUTO_TEST_CASE(tags_without_a_shape_keep_their_value) {
    BOOST_TEST(decodes_to("c11a514b67b0", "1363896240"));
    BOOST_TEST(decodes_to("c1fb41d452d9ec200000", "1363896240.5"));
    BOOST_TEST(decodes_to("d74401020304", R"("AQIDBA==")"));
    BOOST_TEST(decodes_to("d82076687474703a2f2f7777772e6578616d706c652e636f6d",
                          R"("http://www.example.com")"));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(malformed)

BOOST_AUTO_TEST_CASE(refused_with_a_reason) {
    for (const auto* bad : {
             "",                    // nothing at all
             "18",                  // argument missing
             "1a0000",              // argument short
             "1c",                  // reserved additional info
             "ff",                  // break outside a container
             "62c3",                // text shorter than declared
             "61ff",                // invalid UTF-8
             "62c0af",              // overlong encoding
             "63eda080",            // a surrogate
             "a1016161",            // map key not text
             "a2616101616102",      // duplicate key
             "0000",                // trailing bytes
             "5f4101",              // unterminated chunked string
             "5f6161ff",            // text chunk in a byte string
             "5f5f4101ffff",        // nested indefinite chunk
             "9f01",                // unterminated array
             "bf616101",            // unterminated map
             "9bffffffffffffffff",  // a huge count
             "5bffffffffffffffff",  // a huge length
             "3f",                  // indefinite negative integer
             "c1",                  // a tag with no value
         }) {
        auto r = decode(bad);
        BOOST_TEST(!r.has_value(), "accepted " << bad);
        if (!r.has_value()) {
            BOOST_TEST(r.error().message.has_value());
        }
    }
}

BOOST_AUTO_TEST_CASE(depth_is_bounded) {
    std::string deep(200, static_cast<char>(0x81));
    deep.push_back(0x00);
    auto r = aj::cbor_to_json(fx::k_model, aj::no_shape, deep, 128);
    BOOST_TEST(!r.has_value());
    std::string ok(100, static_cast<char>(0x81));
    ok.push_back(0x00);
    BOOST_TEST(aj::cbor_to_json(fx::k_model, aj::no_shape, ok, 128).has_value());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(shape_guided)

// A blob is a byte string in CBOR and base64 in the JSON form; a timestamp
// is epoch milliseconds in CBOR and epoch seconds in the JSON form.
BOOST_AUTO_TEST_CASE(request_blobs_and_timestamps) {
    const auto input = shape("PutThingInput");
    // {"Payload": h'01020304', "CreatedAt": 1(1700000000123)}
    auto r = aj::cbor_to_json(fx::k_model, input,
                              unhex("a2675061796c6f61644401020304694372656174656441"
                                    "74c11b0000018bcfe56800"));
    BOOST_REQUIRE(r.has_value());
    const auto& o = r->as_object();
    BOOST_TEST(o.at("Payload").as_string() == "AQIDBA==");
    BOOST_TEST(o.at("CreatedAt").as_int64() == 1'700'000'000);

    // Fractional milliseconds and an untagged number read the same way.
    auto frac =
        aj::cbor_to_json(fx::k_model, input, unhex("a169437265617465644174c11b0000018bcfe5687b"));
    BOOST_REQUIRE(frac.has_value());
    BOOST_TEST(frac->as_object().at("CreatedAt").as_double() == 1'700'000'000.123);
    auto untagged = aj::cbor_to_json(fx::k_model, input,
                                     unhex("a169437265617465644174"
                                           "1b0000018bcfe56800"));
    BOOST_REQUIRE(untagged.has_value());
    BOOST_TEST(untagged->as_object().at("CreatedAt").as_int64() == 1'700'000'000);

    // The seconds unit leaves the number alone.
    auto secs = aj::cbor_to_json(fx::k_model, input, unhex("a169437265617465644174c11a6553f100"),
                                 128, aj::cbor_timestamp_unit::seconds);
    BOOST_REQUIRE(secs.has_value());
    BOOST_TEST(secs->as_object().at("CreatedAt").as_int64() == 1'700'000'000);
}

BOOST_AUTO_TEST_CASE(responses_use_byte_strings_and_tag_1) {
    const auto input = shape("PutThingInput");
    const auto json = boost::json::parse(
        R"({"Payload":"AQIDBA==","CreatedAt":1700000000.123,"ThingName":"abc"})");
    const auto bytes = aj::json_to_cbor(fx::k_model, input, json);
    const auto h = hex(bytes);
    // h'01020304' and 1(1700000000123).
    BOOST_TEST(h.find("4401020304") != std::string::npos);
    BOOST_TEST(h.find("c11b0000018bcfe5687b") != std::string::npos);
    // And back again.
    auto back = aj::cbor_to_json(fx::k_model, input, bytes);
    BOOST_REQUIRE(back.has_value());
    BOOST_TEST(back->as_object().at("Payload").as_string() == "AQIDBA==");
    BOOST_TEST(back->as_object().at("CreatedAt").as_double() == 1'700'000'000.123);
    BOOST_TEST(back->as_object().at("ThingName").as_string() == "abc");

    // A list of structures: element shapes are followed.
    const auto out = shape("ListThingsOutput");
    const auto names = boost::json::parse(R"({"Names":["abc","def"]})");
    auto names_back = aj::cbor_to_json(fx::k_model, out, aj::json_to_cbor(fx::k_model, out, names));
    BOOST_REQUIRE(names_back.has_value());
    BOOST_TEST((*names_back == names));

    // Without a shape, a string stays text.
    const auto plain = aj::json_to_cbor(fx::k_model, aj::no_shape, boost::json::value("AQIDBA=="));
    BOOST_TEST(hex(plain) == "684151494442413d3d");
}

// Any JSON value survives CBOR and back when no shape changes its form.
BOOST_AUTO_TEST_CASE(generic_round_trip) {
    std::mt19937_64 rng(7);
    std::function<boost::json::value(int)> gen = [&](int depth) -> boost::json::value {
        switch (rng() % (depth > 3 ? 6 : 8)) {
            case 0:
                return nullptr;
            case 1:
                return (rng() % 2) == 0;
            case 2:
                return static_cast<std::int64_t>(rng());
            case 3:
                return rng();
            case 4:
                return std::bit_cast<double>(rng() & 0x7FEFFFFFFFFFFFFFULL);
            case 5: {
                std::string s;
                for (std::size_t i = rng() % 12; i > 0; --i) {
                    s.push_back(static_cast<char>('a' + (rng() % 26)));
                }
                return boost::json::string(s);
            }
            case 6: {
                boost::json::array a;
                for (std::size_t i = rng() % 5; i > 0; --i) {
                    a.push_back(gen(depth + 1));
                }
                return a;
            }
            default: {
                boost::json::object o;
                for (std::size_t i = rng() % 5; i > 0; --i) {
                    o["k" + std::to_string(rng() % 100)] = gen(depth + 1);
                }
                return o;
            }
        }
    };
    for (int i = 0; i < 300; ++i) {
        const auto v = gen(0);
        auto back = aj::cbor_to_json(fx::k_model, aj::no_shape,
                                     aj::json_to_cbor(fx::k_model, aj::no_shape, v));
        BOOST_REQUIRE(back.has_value());
        // uint64 values that fit int64 come back as int64; compare by text.
        BOOST_TEST(boost::json::serialize(*back) == boost::json::serialize(v));
    }
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(codec_and_dispatch)

// Jackson writes an indefinite-length map: the Java SDK's bodies look like
// this one (bf ... ff).
BOOST_AUTO_TEST_CASE(a_cbor_request_parses_like_its_json_twin) {
    const auto c = cbor_codec();
    const std::string body = unhex(
        "bf"
        "695468696e674e616d6563616263"  // "ThingName": "abc"
        "644b696e6465534d414c4c"        // "Kind": "SMALL"
        "65436f756e7405"                // "Count": 5
        "675061796c6f6164430a0b0c"      // "Payload": h'0a0b0c'
        "ff");
    auto p = c.parse(cbor_request("Fixture_20261109.PutThing", body));
    BOOST_REQUIRE(p.has_value());
    BOOST_TEST((p->encoding == aj::body_encoding::cbor));
    BOOST_TEST(p->input.at("ThingName").as_string() == "abc");
    BOOST_TEST(p->input.at("Count").as_int64() == 5);
    BOOST_TEST(p->input.at("Payload").as_string() == "CgsM");

    // The same body as JSON gives the same input.
    auto j = c.parse(aj::http_request_view{
        "POST", "/", "application/x-amz-json-1.0", "Fixture_20261109.PutThing",
        R"({"ThingName":"abc","Kind":"SMALL","Count":5,"Payload":"CgsM"})"});
    BOOST_REQUIRE(j.has_value());
    BOOST_TEST((j->input == p->input));
    BOOST_TEST((j->encoding == aj::body_encoding::json));
}

BOOST_AUTO_TEST_CASE(cbor_bodies_are_validated_and_faults_typed) {
    const auto c = cbor_codec();
    // Missing required Kind, and Count out of range: one ValidationException.
    auto v = c.parse(cbor_request("Fixture_20261109.PutThing",
                                  unhex("a2695468696e674e616d656361626365436f756e741903e8")));
    BOOST_REQUIRE(!v.has_value());
    BOOST_TEST(v.error().type == "ValidationException");
    // A malformed body is a SerializationException with a reason.
    auto s = c.parse(cbor_request("Fixture_20261109.PutThing", unhex("bf6161")));
    BOOST_REQUIRE(!s.has_value());
    BOOST_TEST(s.error().type == "SerializationException");
    BOOST_TEST(s.error().message.has_value());
    // A blob over its max length counts its bytes, not its base64.
    auto big = c.parse(cbor_request(
        "Fixture_20261109.PutThing",
        unhex("a3695468696e674e616d6563616263644b696e6465534d414c4c675061796c6f6164450102030405")));
    BOOST_REQUIRE(!big.has_value());
    BOOST_TEST(big.error().type == "ValidationException");
}

BOOST_AUTO_TEST_CASE(cbor_is_off_unless_accepted) {
    const auto c = cbor_codec(false);
    BOOST_TEST(c.resolve_target(cbor_request("Fixture_20261109.Ping", "")) == nullptr);
    BOOST_TEST(!c.encoding_of("application/x-amz-cbor-1.1").has_value());
    const auto on = cbor_codec();
    BOOST_TEST((on.encoding_of("Application/X-Amz-Cbor-1.1; charset=utf-8") ==
                std::optional{aj::body_encoding::cbor}));
    BOOST_TEST(
        (on.encoding_of("application/x-amz-json-1.0") == std::optional{aj::body_encoding::json}));
    BOOST_TEST(!on.encoding_of("application/json").has_value());
}

BOOST_AUTO_TEST_CASE(the_dispatcher_answers_in_the_request_encoding) {
    const auto c = cbor_codec();
    aj::dispatcher d(c);
    d.on("ListThings", [](aj::request_context&, boost::json::object&) -> aj::handler_result {
        return boost::json::object{{"Names", boost::json::array{"abc", "def"}}};
    });
    d.on("PutThing", [](aj::request_context&, boost::json::object&) -> aj::handler_result {
        return aj::error_style::bare().modeled("ThingNotFoundException", "no such thing");
    });

    auto ok = d.handle(cbor_request("Fixture_20261109.ListThings", unhex("a0")));
    BOOST_TEST(ok.status == 200U);
    BOOST_TEST(ok.header("Content-Type").value_or("") == "application/x-amz-cbor-1.1");
    BOOST_TEST(ok.header("x-amz-crc32").value_or("") == aj::crc32_header(ok.body));
    auto body = aj::cbor_to_json(fx::k_model, aj::no_shape, ok.body);
    BOOST_REQUIRE(body.has_value());
    BOOST_TEST(boost::json::serialize(*body) == R"({"Names":["abc","def"]})");

    // A modeled error, in CBOR, with __type and message.
    auto err =
        d.handle(cbor_request("Fixture_20261109.PutThing",
                              unhex("a2695468696e674e616d6563616263644b696e6465534d414c4c")));
    BOOST_TEST(err.status == 400U);
    BOOST_TEST(err.header("Content-Type").value_or("") == "application/x-amz-cbor-1.1");
    auto eb = aj::cbor_to_json(fx::k_model, aj::no_shape, err.body);
    BOOST_REQUIRE(eb.has_value());
    BOOST_TEST(eb->as_object().at("__type").as_string() == "ThingNotFoundException");
    BOOST_TEST(eb->as_object().at("message").as_string() == "no such thing");

    // An operation without a handler, and a framing fault, answer in CBOR too.
    auto unknown = d.handle(cbor_request("Fixture_20261109.Ping", ""));
    auto ub = aj::cbor_to_json(fx::k_model, aj::no_shape, unknown.body);
    BOOST_REQUIRE(ub.has_value());
    BOOST_TEST(ub->as_object().at("__type").as_string() == "UnknownOperationException");
    auto bad = d.handle(cbor_request("Fixture_20261109.ListThings", unhex("ff")));
    auto bb = aj::cbor_to_json(fx::k_model, aj::no_shape, bad.body);
    BOOST_REQUIRE(bb.has_value());
    BOOST_TEST(bb->as_object().at("__type").as_string() == "SerializationException");

    // JSON requests still get JSON.
    auto json = d.handle(aj::http_request_view{"POST", "/", "application/x-amz-json-1.0",
                                               "Fixture_20261109.ListThings", "{}"});
    BOOST_TEST(json.header("Content-Type").value_or("") == "application/x-amz-json-1.0");
    BOOST_TEST(json.body == R"({"Names":["abc","def"]})");
}

BOOST_AUTO_TEST_SUITE_END()

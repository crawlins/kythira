// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Kinesis spec task 2 (.kiro/specs/kinesis-compatible-streams/ Requirement
// 4.2): event-stream framing. The empty-message vector is the one AWS's
// event-stream libraries publish; the others were encoded by an independent
// Python implementation and checked by botocore's EventStreamBuffer
// (botocore 1.43), so encoder and decoder are held to bytes AWS's own parser
// accepts.

#define BOOST_TEST_MODULE aws_eventstream_test

#include <boost/test/unit_test.hpp>

#include <raft/aws_eventstream.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace es = kythira::aws_eventstream;

BOOST_TEST_DONT_PRINT_LOG_VALUE(std::vector<std::byte>)
BOOST_TEST_DONT_PRINT_LOG_VALUE(es::message)

namespace {

auto unhex(std::string_view h) -> std::vector<std::byte> {
    std::vector<std::byte> out;
    for (std::size_t i = 0; i + 1 < h.size(); i += 2) {
        out.push_back(static_cast<std::byte>(std::stoi(std::string(h.substr(i, 2)), nullptr, 16)));
    }
    return out;
}

auto bytes(std::string_view s) -> std::vector<std::byte> {
    std::vector<std::byte> out;
    for (const char c : s) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

const std::string_view k_empty = "000000100000000005c248eb7d98c8ff";
const std::string_view k_payload_only =
    "0000001d00000000fd528c5a7b22666f6f223a22626172227d3e9c25b4";
const std::string_view k_all_types =
    "0000006800000055d761469e017400016601016202f9017303fed401690400009c40016c05fffffffed5fa"
    "0e00017906000200ff0373747207000668c3a96c6c6f027473080000018bcfe5687b02696409000102030405"
    "060708090a0b0c0d0e0f61626381dcdfb8";
const std::string_view k_event =
    "000000b10000005bb43c76b10d3a6d6573736167652d747970650700056576656e740b3a6576656e742d7479"
    "7065070015537562736372696265546f53686172644576656e740d3a636f6e74656e742d7479706507001061"
    "70706c69636174696f6e2f6a736f6e7b22436f6e74696e756174696f6e53657175656e63654e756d62657222"
    "3a2231222c224d696c6c6973426568696e644c6174657374223a302c225265636f726473223a5b5d7d676c10"
    "b3";
const std::string_view k_error =
    "0000005a0000004a8188a08d0d3a6d6573736167652d747970650700056572726f720b3a6572726f722d636f"
    "646507000f496e7465726e616c4661696c7572650e3a6572726f722d6d657373616765070004626f6f6dd78e"
    "0703";

auto all_types_message() -> es::message {
    es::uuid id{};
    for (std::size_t i = 0; i < id.size(); ++i) {
        id[i] = static_cast<std::byte>(i);
    }
    es::message m;
    m.headers = {
        {"t", true},
        {"f", false},
        {"b", std::int8_t{-7}},
        {"s", std::int16_t{-300}},
        {"i", std::int32_t{40000}},
        {"l", std::int64_t{-5'000'000'000}},
        {"y", std::vector<std::byte>{std::byte{0x00}, std::byte{0xff}}},
        {"str", std::string("h\xc3\xa9llo")},
        {"ts", es::timestamp_ms{1'700'000'000'123}},
        {"id", id},
    };
    m.payload = bytes("abc");
    return m;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(vectors)

BOOST_AUTO_TEST_CASE(encode_matches_and_decode_reads_back) {
    struct vec {
        std::string_view hex;
        es::message msg;
    };
    const std::vector<vec> vecs = {
        {k_empty, {}},
        {k_payload_only, {{}, bytes(R"({"foo":"bar"})")}},
        {k_all_types, all_types_message()},
        {k_event, es::make_event("SubscribeToShardEvent", "application/json",
                                 bytes(R"({"ContinuationSequenceNumber":"1",)"
                                       R"("MillisBehindLatest":0,"Records":[]})"))},
        {k_error, es::make_error("InternalFailure", "boom")},
    };
    for (const auto& v : vecs) {
        BOOST_TEST((es::encode(v.msg) == unhex(v.hex)), "encoding " << v.hex);
        BOOST_TEST((es::decode(unhex(v.hex)) == v.msg), "decoding " << v.hex);
    }
}

BOOST_AUTO_TEST_CASE(helpers_name_the_message_kind) {
    auto ev = es::make_event("SubscribeToShardEvent", "application/x-amz-cbor-1.1", {});
    BOOST_TEST(ev.string_header(":message-type").value_or("") == "event");
    BOOST_TEST(ev.string_header(":event-type").value_or("") == "SubscribeToShardEvent");
    auto ex = es::make_exception("ResourceNotFoundException", "application/json",
                                 bytes(R"({"message":"gone"})"));
    BOOST_TEST(ex.string_header(":message-type").value_or("") == "exception");
    BOOST_TEST(ex.string_header(":exception-type").value_or("") == "ResourceNotFoundException");
    auto er = es::make_error("InternalFailure", "boom");
    BOOST_TEST(er.string_header(":error-message").value_or("") == "boom");
    BOOST_TEST(!er.string_header(":nope").has_value());
    BOOST_TEST(er.find(":nope") == nullptr);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(streaming)

// Messages arrive split anywhere; the decoder hands each over once whole.
BOOST_AUTO_TEST_CASE(byte_by_byte_and_back_to_back) {
    std::vector<std::byte> stream;
    for (const auto h : {k_event, k_empty, k_all_types, k_error}) {
        const auto b = unhex(h);
        stream.insert(stream.end(), b.begin(), b.end());
    }
    es::decoder d;
    std::vector<es::message> got;
    for (const auto b : stream) {
        d.feed(std::span(&b, 1));
        while (auto m = d.next()) {
            got.push_back(std::move(*m));
        }
    }
    BOOST_TEST_REQUIRE(got.size() == 4U);
    BOOST_TEST(got[0].string_header(":event-type").value_or("") == "SubscribeToShardEvent");
    BOOST_TEST(got[1].headers.empty());
    BOOST_TEST((got[2] == all_types_message()));
    BOOST_TEST(got[3].string_header(":error-code").value_or("") == "InternalFailure");
    BOOST_TEST(d.pending_bytes() == 0U);

    // All at once.
    es::decoder whole;
    whole.feed(stream);
    int n = 0;
    while (whole.next()) {
        ++n;
    }
    BOOST_TEST(n == 4);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(faults)

// The two CRCs cover every byte: flipping any bit of a message is caught.
BOOST_AUTO_TEST_CASE(every_corruption_is_caught) {
    const auto good = unhex(k_all_types);
    for (std::size_t i = 0; i < good.size(); ++i) {
        for (const unsigned bit : {0U, 7U}) {
            auto bad = good;
            bad[i] ^= static_cast<std::byte>(1U << bit);
            es::decoder d;
            d.feed(bad);
            bool caught = false;
            try {
                // A corrupted length can make the decoder wait for more
                // bytes; it must never hand over a message.
                caught = !d.next().has_value();
            } catch (const es::eventstream_error&) {
                caught = true;
            }
            BOOST_TEST(caught, "byte " << i << " bit " << bit);
        }
    }
}

BOOST_AUTO_TEST_CASE(a_failed_decoder_stays_failed) {
    auto bad = unhex(k_empty);
    bad.back() ^= std::byte{1};
    es::decoder d;
    d.feed(bad);
    BOOST_CHECK_THROW((void)d.next(), es::eventstream_error);
    BOOST_CHECK_THROW((void)d.next(), es::eventstream_error);
    BOOST_CHECK_THROW(d.feed(unhex(k_empty)), es::eventstream_error);
}

BOOST_AUTO_TEST_CASE(lengths_out_of_range_are_refused_before_buffering) {
    // A prelude announcing more than 16 MiB, with a valid prelude CRC.
    es::message big;
    big.payload.assign(1000, std::byte{0});
    auto bytes_ = es::encode(big);
    // Rewrite the total length to 17 MiB and fix the prelude CRC.
    const std::uint32_t total = 17U * 1024U * 1024U;
    for (int i = 0; i < 4; ++i) {
        bytes_[static_cast<std::size_t>(i)] =
            static_cast<std::byte>((total >> (static_cast<unsigned>(3 - i) * 8U)) & 0xFFU);
    }
    const auto crc = kythira::crc32(std::span(bytes_).first(8));
    for (int i = 0; i < 4; ++i) {
        bytes_[8 + static_cast<std::size_t>(i)] =
            static_cast<std::byte>((crc >> (static_cast<unsigned>(3 - i) * 8U)) & 0xFFU);
    }
    es::decoder d;
    d.feed(std::span(bytes_).first(12));
    BOOST_CHECK_THROW((void)d.next(), es::eventstream_error);

    // A truncated buffer is not a message.
    const auto event = unhex(k_event);
    BOOST_CHECK_THROW((void)es::decode(std::span(event).first(30)), es::eventstream_error);
    // Nor are two.
    auto two = unhex(k_empty);
    const auto again = unhex(k_empty);
    two.insert(two.end(), again.begin(), again.end());
    BOOST_CHECK_THROW((void)es::decode(two), es::eventstream_error);
}

BOOST_AUTO_TEST_CASE(the_encoder_enforces_limits) {
    es::message m;
    m.headers.push_back({std::string(256, 'n'), true});
    BOOST_CHECK_THROW((void)es::encode(m), es::eventstream_error);
    m.headers = {{"", true}};
    BOOST_CHECK_THROW((void)es::encode(m), es::eventstream_error);
    m.headers = {{"v", std::string(32768, 'x')}};
    BOOST_CHECK_THROW((void)es::encode(m), es::eventstream_error);
    m.headers = {{"v", std::string(32767, 'x')}};
    BOOST_CHECK_NO_THROW((void)es::encode(m));
    m.headers.clear();
    m.payload.assign(es::k_max_message_bytes - es::k_min_message_bytes + 1, std::byte{0});
    BOOST_CHECK_THROW((void)es::encode(m), es::eventstream_error);
    m.payload.pop_back();
    BOOST_CHECK_NO_THROW((void)es::encode(m));
}

BOOST_AUTO_TEST_CASE(malformed_headers_are_refused) {
    // Build messages with hand-made header blocks and valid CRCs.
    const auto frame = [](const std::vector<std::byte>& headers) {
        std::vector<std::byte> out;
        const auto total = 12 + headers.size() + 4;
        for (const auto& [v, n] : {std::pair{total, 4}, std::pair{headers.size(), 4}}) {
            for (int i = n - 1; i >= 0; --i) {
                out.push_back(
                    static_cast<std::byte>((v >> (static_cast<unsigned>(i) * 8U)) & 0xFF));
            }
        }
        const auto pc = kythira::crc32(out);
        for (int i = 3; i >= 0; --i) {
            out.push_back(static_cast<std::byte>((pc >> (static_cast<unsigned>(i) * 8U)) & 0xFF));
        }
        out.insert(out.end(), headers.begin(), headers.end());
        const auto mc = kythira::crc32(out);
        for (int i = 3; i >= 0; --i) {
            out.push_back(static_cast<std::byte>((mc >> (static_cast<unsigned>(i) * 8U)) & 0xFF));
        }
        return out;
    };
    for (const auto* h : {
             "00",                // empty name
             "0161",              // name with no type
             "01610a",            // unknown type 10
             "016104000000",      // int32 cut short
             "0161070005616263",  // string shorter than declared
             "05616263",          // name longer than the block
         }) {
        BOOST_CHECK_THROW((void)es::decode(frame(unhex(h))), es::eventstream_error);
    }
    // And a good one through the same framing.
    auto ok = es::decode(frame(unhex("01610401020304")));
    BOOST_TEST(std::get<std::int32_t>(ok.headers.at(0).value) == 0x01020304);
}

BOOST_AUTO_TEST_SUITE_END()

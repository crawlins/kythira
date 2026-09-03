// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file redis_kv_commands_unit_test.cpp
/// @brief Binary log-entry codec tests (.kiro/specs/redis-compatible-kv/
///        task 2, Requirements 2.6, 8.2).

#define BOOST_TEST_MODULE redis_kv_commands_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/redis_kv_commands.hpp>

#include <cstddef>
#include <string>
#include <vector>

using namespace kythira;

namespace {

auto bytes_of(const std::string& s) -> std::vector<std::byte> {
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        out[i] = static_cast<std::byte>(s[i]);
    }
    return out;
}

}  // namespace

BOOST_AUTO_TEST_CASE(set_round_trip_and_layout, *boost::unit_test::timeout(10)) {
    redis_kv_set_command c{"ab", bytes_of("xyz"), 0x0102030405060708ull};
    auto encoded = encode_redis_kv_command(c);
    // version, opcode, keylen(4), key(2), expire(8), vlen(4), value(3)
    BOOST_REQUIRE_EQUAL(encoded.size(), 1u + 1u + 4u + 2u + 8u + 4u + 3u);
    BOOST_CHECK_EQUAL(static_cast<int>(encoded[0]), 0x01);
    BOOST_CHECK_EQUAL(static_cast<int>(encoded[1]), 0x01);
    BOOST_CHECK_EQUAL(static_cast<int>(encoded[5]), 2);     // key length, big-endian low byte
    BOOST_CHECK_EQUAL(static_cast<int>(encoded[8]), 0x01);  // expire high byte first
    BOOST_CHECK_EQUAL(static_cast<int>(encoded[15]), 0x08);
    BOOST_CHECK_EQUAL(static_cast<int>(encoded[19]), 3);

    auto decoded = decode_redis_kv_command(encoded);
    auto* s = std::get_if<redis_kv_set_command>(&decoded);
    BOOST_REQUIRE(s != nullptr);
    BOOST_CHECK_EQUAL(s->_key, "ab");
    BOOST_CHECK(s->_value == bytes_of("xyz"));
    BOOST_CHECK_EQUAL(s->_expire_at_ms, 0x0102030405060708ull);
    BOOST_CHECK(peek_redis_kv_opcode(encoded) == redis_kv_opcode::set);
}

BOOST_AUTO_TEST_CASE(del_round_trip, *boost::unit_test::timeout(10)) {
    auto encoded = encode_redis_kv_command(redis_kv_del_command{"key"});
    BOOST_REQUIRE_EQUAL(encoded.size(), 1u + 1u + 4u + 3u);
    BOOST_CHECK_EQUAL(static_cast<int>(encoded[1]), 0x02);
    auto decoded = decode_redis_kv_command(encoded);
    auto* d = std::get_if<redis_kv_del_command>(&decoded);
    BOOST_REQUIRE(d != nullptr);
    BOOST_CHECK_EQUAL(d->_key, "key");
}

BOOST_AUTO_TEST_CASE(sweep_round_trip, *boost::unit_test::timeout(10)) {
    redis_kv_sweep_command c;
    c._entries.push_back({"a", 10});
    c._entries.push_back({"bb", 20});
    auto encoded = encode_redis_kv_command(c);
    auto decoded = decode_redis_kv_command(encoded);
    auto* s = std::get_if<redis_kv_sweep_command>(&decoded);
    BOOST_REQUIRE(s != nullptr);
    BOOST_REQUIRE_EQUAL(s->_entries.size(), 2u);
    BOOST_CHECK_EQUAL(s->_entries[0]._key, "a");
    BOOST_CHECK_EQUAL(s->_entries[0]._expire_at_ms, 10u);
    BOOST_CHECK_EQUAL(s->_entries[1]._key, "bb");
    BOOST_CHECK_EQUAL(s->_entries[1]._expire_at_ms, 20u);
}

BOOST_AUTO_TEST_CASE(evict_round_trip, *boost::unit_test::timeout(10)) {
    redis_kv_evict_command c{{"x", "", "yz"}};
    auto decoded = decode_redis_kv_command(encode_redis_kv_command(c));
    auto* e = std::get_if<redis_kv_evict_command>(&decoded);
    BOOST_REQUIRE(e != nullptr);
    BOOST_CHECK(e->_keys == (std::vector<std::string>{"x", "", "yz"}));
}

BOOST_AUTO_TEST_CASE(variant_round_trip, *boost::unit_test::timeout(10)) {
    redis_kv_command v = redis_kv_del_command{"q"};
    auto decoded = decode_redis_kv_command(encode_redis_kv_command(v));
    BOOST_CHECK(std::holds_alternative<redis_kv_del_command>(decoded));
}

BOOST_AUTO_TEST_CASE(rejects_every_truncation, *boost::unit_test::timeout(10)) {
    auto full = encode_redis_kv_command(redis_kv_set_command{"key", bytes_of("value"), 42});
    for (std::size_t n = 0; n < full.size(); ++n) {
        std::vector<std::byte> cut(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(n));
        BOOST_CHECK_THROW(decode_redis_kv_command(cut), redis_kv_codec_error);
    }
}

BOOST_AUTO_TEST_CASE(rejects_trailing_bytes, *boost::unit_test::timeout(10)) {
    auto full = encode_redis_kv_command(redis_kv_del_command{"key"});
    full.push_back(std::byte{0});
    BOOST_CHECK_THROW(decode_redis_kv_command(full), redis_kv_codec_error);
}

BOOST_AUTO_TEST_CASE(rejects_unknown_version_and_opcode, *boost::unit_test::timeout(10)) {
    auto full = encode_redis_kv_command(redis_kv_del_command{"key"});
    auto bad_version = full;
    bad_version[0] = std::byte{0x02};
    BOOST_CHECK_THROW(decode_redis_kv_command(bad_version), redis_kv_codec_error);
    BOOST_CHECK(!peek_redis_kv_opcode(bad_version).has_value());
    auto bad_opcode = full;
    bad_opcode[1] = std::byte{0x09};
    BOOST_CHECK_THROW(decode_redis_kv_command(bad_opcode), redis_kv_codec_error);
    BOOST_CHECK(!peek_redis_kv_opcode(bad_opcode).has_value());
}

BOOST_AUTO_TEST_CASE(rejects_absurd_counts_without_allocating, *boost::unit_test::timeout(10)) {
    // A sweep claiming 2^32-1 entries in a 10-byte buffer must fail fast.
    std::vector<std::byte> buf = {
        std::byte{0x01}, std::byte{0x03}, std::byte{0},    std::byte{0},    std::byte{0},
        std::byte{0},    std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}};
    BOOST_CHECK_THROW(decode_redis_kv_command(buf), redis_kv_codec_error);
    buf[1] = std::byte{0x04};
    BOOST_CHECK_THROW(decode_redis_kv_command(buf), redis_kv_codec_error);
}

BOOST_AUTO_TEST_CASE(sweep_with_key_is_rejected, *boost::unit_test::timeout(10)) {
    // Hand-build a sweep record whose header carries a key.
    std::vector<std::byte> buf = {std::byte{0x01}, std::byte{0x03}, std::byte{0},   std::byte{0},
                                  std::byte{0},    std::byte{1},    std::byte{'k'}, std::byte{0},
                                  std::byte{0},    std::byte{0},    std::byte{0}};
    BOOST_CHECK_THROW(decode_redis_kv_command(buf), redis_kv_codec_error);
}

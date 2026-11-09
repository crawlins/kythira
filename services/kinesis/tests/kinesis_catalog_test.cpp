// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Task 5: hash keys, sequence numbers, shard ids, routing keys and the
// catalog record codec (.kiro/specs/kinesis-compatible-streams/
// Requirements 6.2, 6.3, 7.1, 8.1, 8.2).

#define BOOST_TEST_MODULE kinesis_catalog_test

#include <boost/test/unit_test.hpp>

#include <kythira/kinesis/kinesis_catalog.hpp>
#include <kythira/kinesis/kinesis_hash.hpp>
#include <kythira/kinesis/kinesis_sequence.hpp>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace k = kythira::kinesis;

namespace {
const std::string test_account = "111122223333";
}  // namespace

BOOST_TEST_DONT_PRINT_LOG_VALUE(k::hash_key)
BOOST_TEST_DONT_PRINT_LOG_VALUE(std::optional<k::hash_key>)
BOOST_TEST_DONT_PRINT_LOG_VALUE(std::optional<k::sequence_number>)
BOOST_TEST_DONT_PRINT_LOG_VALUE(std::optional<k::routing_key_parts>)
BOOST_TEST_DONT_PRINT_LOG_VALUE(k::stream_record)

namespace {
const std::string max_decimal = "340282366920938463463374607431768211455";
}

BOOST_AUTO_TEST_SUITE(hash_keys)

BOOST_AUTO_TEST_CASE(decimal_round_trip_and_bounds) {
    BOOST_TEST(k::to_decimal(0) == "0");
    BOOST_TEST(k::to_decimal(k::max_hash_key) == max_decimal);
    BOOST_TEST((k::parse_hash_key(max_decimal) == std::optional{k::max_hash_key}));
    BOOST_TEST(!k::parse_hash_key("340282366920938463463374607431768211456").has_value());
    BOOST_TEST(!k::parse_hash_key("").has_value());
    BOOST_TEST(!k::parse_hash_key("12a").has_value());
    BOOST_TEST(!k::parse_hash_key("-1").has_value());
    BOOST_TEST((k::parse_hash_key("007") == std::optional<k::hash_key>{7}));
    std::mt19937_64 rng(5);
    for (int i = 0; i < 200; ++i) {
        const k::hash_key v = (static_cast<k::hash_key>(rng()) << 64U) | rng();
        BOOST_TEST((k::parse_hash_key(k::to_decimal(v)) == std::optional{v}));
    }
}

// Known MD5 values: AWS routes a record by MD5(partition key) read as a
// big-endian 128-bit integer.
BOOST_AUTO_TEST_CASE(partition_key_md5) {
    // md5("") = d41d8cd98f00b204e9800998ecf8427e
    BOOST_TEST(k::to_decimal(k::partition_key_hash("")) ==
               "281949768489412648962353822266799178366");
    // md5("a") = 0cc175b9c0f1b6a831c399e269772661
    BOOST_TEST(k::to_decimal(k::partition_key_hash("a")) ==
               "16955237001963240173058271559858726497");
}

BOOST_AUTO_TEST_CASE(even_ranges_match_aws) {
    // One shard covers everything.
    auto one = k::even_ranges(1);
    BOOST_TEST(one.size() == 1U);
    BOOST_TEST((one[0] == k::hash_range{0, k::max_hash_key}));

    // Two shards: AWS's CreateStream gives 2^127 as the second start.
    auto two = k::even_ranges(2);
    BOOST_TEST(k::to_decimal(two[1]._start) == "170141183460469231731687303715884105728");
    BOOST_TEST(two[0]._end + 1 == two[1]._start);

    // Three: AWS prints 113427455640312821154458202477256070485 as shard 1's
    // start (floor(2^128 / 3)).
    auto three = k::even_ranges(3);
    BOOST_TEST(k::to_decimal(three[1]._start) == "113427455640312821154458202477256070485");
    BOOST_TEST(three[2]._end == k::max_hash_key);

    for (const std::uint32_t n : {1U, 2U, 3U, 4U, 5U, 7U, 8U, 64U, 500U}) {
        auto r = k::even_ranges(n);
        BOOST_TEST_REQUIRE(r.size() == n);
        BOOST_TEST(r.front()._start == 0);
        BOOST_TEST(r.back()._end == k::max_hash_key);
        for (std::size_t i = 1; i < r.size(); ++i) {
            BOOST_TEST(r[i - 1]._end + 1 == r[i]._start);
        }
    }
    BOOST_CHECK_THROW((void)k::even_ranges(0), std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(sequence_numbers)

BOOST_AUTO_TEST_CASE(aws_profile_is_56_digits_and_round_trips) {
    const k::sequence_number s{._ordinal = 3, ._counter = 123456789, ._sub = 7};
    const auto text = k::format_sequence(s);
    BOOST_TEST(text.size() == k::aws_sequence_digits);
    BOOST_TEST(text.starts_with("49"));
    BOOST_TEST((k::parse_sequence(text) == std::optional{s}));
    const k::sequence_number top{
        ._ordinal = UINT32_MAX, ._counter = UINT64_MAX, ._sub = k::max_sub_index};
    BOOST_TEST((k::parse_sequence(k::format_sequence(top)) == std::optional{top}));
}

// Requirement 8.2: within a shard, string order is integer order is record
// order.
BOOST_AUTO_TEST_CASE(aws_profile_orders_as_integers) {
    std::mt19937_64 rng(11);
    std::vector<std::uint64_t> counters{0, 1, 9, 10, 99, 100, UINT64_MAX};
    for (int i = 0; i < 100; ++i) {
        counters.push_back(rng() >> (rng() % 64));
    }
    for (const auto a : counters) {
        for (const auto b : {counters[3], counters[10], counters[50]}) {
            const auto sa = k::format_sequence({._ordinal = 4, ._counter = a});
            const auto sb = k::format_sequence({._ordinal = 4, ._counter = b});
            BOOST_TEST((sa < sb) == (a < b));
        }
    }
}

BOOST_AUTO_TEST_CASE(foreign_and_malformed_numbers_are_refused) {
    // A real AWS sequence number: right length, but its reserved tail is
    // not zero, so it is not misread as a position here.
    BOOST_TEST(
        !k::parse_sequence("49590338271490256608559692538361571095921575989136588898").has_value());
    BOOST_TEST(!k::parse_sequence("").has_value());
    BOOST_TEST(!k::parse_sequence("123").has_value());
    auto good = k::format_sequence({._counter = 5});
    auto bad = good;
    bad[20] = 'x';
    BOOST_TEST(!k::parse_sequence(bad).has_value());
    bad = good;
    bad[0] = '5';
    BOOST_TEST(!k::parse_sequence(bad).has_value());
    // Ordinal above 32 bits.
    bad = good;
    bad.replace(2, 10, "9999999999");
    BOOST_TEST(!k::parse_sequence(bad).has_value());
}

BOOST_AUTO_TEST_CASE(compact_profile_is_the_counter) {
    BOOST_TEST(k::format_sequence({._ordinal = 9, ._counter = 42}, k::sequence_profile::compact) ==
               "42");
    BOOST_TEST((k::parse_sequence("42", k::sequence_profile::compact) ==
                std::optional{k::sequence_number{._counter = 42}}));
    BOOST_TEST(
        !k::parse_sequence("18446744073709551616", k::sequence_profile::compact).has_value());
    BOOST_TEST(
        !k::parse_sequence(k::format_sequence({._counter = 42}), k::sequence_profile::compact)
             .has_value());
}

BOOST_AUTO_TEST_CASE(shard_ids) {
    BOOST_TEST(k::shard_id_of(0) == "shardId-000000000000");
    BOOST_TEST(k::shard_id_of(17) == "shardId-000000000017");
    BOOST_TEST((k::parse_shard_id("shardId-000000000017") == std::optional<std::uint32_t>{17}));
    BOOST_TEST((k::parse_shard_id("17") == std::optional<std::uint32_t>{17}));
    BOOST_TEST(!k::parse_shard_id("shardId-").has_value());
    BOOST_TEST(!k::parse_shard_id("shard-1").has_value());
    BOOST_TEST(!k::parse_shard_id("4294967296").has_value());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(catalog)

BOOST_AUTO_TEST_CASE(keys) {
    BOOST_TEST(k::catalog_prefix().size() == 14U);
    BOOST_TEST(k::catalog_prefix()[0] == '\0');
    BOOST_TEST(k::stream_key(test_account, "orders").starts_with(k::catalog_prefix()));
    BOOST_TEST(k::next_id_key().starts_with(k::catalog_prefix()));

    const auto rk = k::routing_key(0x1234, 7);
    BOOST_TEST(rk == "k/0000000000001234/00000007");
    BOOST_TEST(rk.size() == k::routing_key_size);
    BOOST_TEST((k::parse_routing_key(rk) == std::optional{k::routing_key_parts{0x1234, 7}}));
    BOOST_TEST(rk.starts_with(k::routing_prefix(0x1234)));
    BOOST_TEST(!k::parse_routing_key(rk + "/x").has_value());
    BOOST_TEST(!k::parse_routing_key("k/000000000000123G/00000007").has_value());
    BOOST_TEST(!k::parse_routing_key("k/0000000000001234-00000007").has_value());

    // Byte order of routing keys is (id, ordinal) order.
    BOOST_TEST(k::routing_key(1, 0xFFFFFFFF) < k::routing_key(2, 0));
    BOOST_TEST(k::routing_key(2, 9) < k::routing_key(2, 10));
    // Every catalog key sorts before every routing key.
    // (Double parentheses: Boost.Test compares strings as C strings, and a
    // catalog key starts with a NUL.)
    BOOST_TEST((k::stream_key(test_account, "zzz") < k::routing_key(0, 0)));
}

BOOST_AUTO_TEST_CASE(stream_names) {
    BOOST_TEST(k::valid_stream_name("a"));
    BOOST_TEST(k::valid_stream_name("my-stream_1.v2"));
    BOOST_TEST(k::valid_stream_name(std::string(128, 'x')));
    BOOST_TEST(!k::valid_stream_name(""));
    BOOST_TEST(!k::valid_stream_name(std::string(129, 'x')));
    BOOST_TEST(!k::valid_stream_name("a/b"));
    BOOST_TEST(!k::valid_stream_name("a b"));
}

BOOST_AUTO_TEST_CASE(status_transitions) {
    using enum k::stream_status;
    BOOST_TEST(k::status_transition_allowed(creating, active));
    BOOST_TEST(k::status_transition_allowed(active, updating));
    BOOST_TEST(k::status_transition_allowed(updating, active));
    BOOST_TEST(k::status_transition_allowed(creating, deleting));
    BOOST_TEST(k::status_transition_allowed(active, deleting));
    BOOST_TEST(k::status_transition_allowed(updating, deleting));
    BOOST_TEST(!k::status_transition_allowed(deleting, active));
    BOOST_TEST(!k::status_transition_allowed(deleting, deleting));
    BOOST_TEST(!k::status_transition_allowed(active, creating));
    BOOST_TEST(!k::status_transition_allowed(creating, updating));
    BOOST_TEST(k::to_string(updating) == "UPDATING");
}

BOOST_AUTO_TEST_CASE(stream_record_round_trip) {
    k::stream_record s;
    s._id = 77;
    s._account = "111122223333";
    s._name = "orders";
    s._status = k::stream_status::updating;
    s._mode = k::stream_mode::on_demand;
    s._retention_hours = 168;
    s._created_ms = 1'762'700'000'000;
    s._shards = k::initial_shards(3);
    s._shards[1]._ending_counter = 99;
    s._shards.push_back({._ordinal = 3, ._parent = 1, ._range = s._shards[1]._range});
    s._shards.push_back({._ordinal = 4,
                         ._parent = 0,
                         ._adjacent_parent = 2,
                         ._range = {0, 5},
                         ._starting_counter = 12});
    s._next_ordinal = 5;
    s._tags = {{"team", "data"}, {"env", "prod"}};
    s._encryption = k::encryption_type::kms;
    s._key_id = "alias/streams";

    const auto bytes = k::encode_stream_record(s);
    BOOST_TEST((k::decode_stream_record(bytes) == s));
    BOOST_TEST(s.open_shard_count() == 4U);

    // Truncation anywhere is an error, never a partial record.
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        BOOST_CHECK_THROW((void)k::decode_stream_record(std::span(bytes).first(n)),
                          k::kinesis_codec_error);
    }
    auto longer = bytes;
    longer.push_back(std::byte{0});
    BOOST_CHECK_THROW((void)k::decode_stream_record(longer), k::kinesis_codec_error);
}

BOOST_AUTO_TEST_CASE(routing_by_hash_picks_the_open_shard) {
    k::stream_record s;
    s._shards = k::initial_shards(4);
    for (const auto& pk : {"a", "b", "user-17", "xyz", ""}) {
        const auto h = k::partition_key_hash(pk);
        const auto* sh = s.shard_for(h);
        BOOST_TEST_REQUIRE(sh != nullptr);
        BOOST_TEST(sh->_range.contains(h));
    }
    BOOST_TEST(s.shard_for(0)->_ordinal == 0U);
    BOOST_TEST(s.shard_for(k::max_hash_key)->_ordinal == 3U);
    // Close shard 0 and give its range to a child: routing follows.
    s._shards[0]._ending_counter = 10;
    s._shards.push_back({._ordinal = 4, ._parent = 0, ._range = s._shards[0]._range});
    BOOST_TEST(s.shard_for(0)->_ordinal == 4U);
    BOOST_TEST(s.find_shard(0) != nullptr);
    BOOST_TEST(s.find_shard(9) == nullptr);
}

BOOST_AUTO_TEST_SUITE_END()

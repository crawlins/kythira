// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file shard_commands_unit_test.cpp
/// @brief The wire format of the administration-entry payloads (task 17 of
///        `.kiro/specs/multi-raft/`).
///
/// These bytes live inside a Raft log, which outlives the process that wrote
/// them and is replayed on every replica. So the decoder is tested against the
/// two things that actually go wrong with such a format: a payload from a
/// different version, and a payload that has been truncated. A decoder that
/// trusted its own length fields would turn one corrupt entry into a read out
/// of bounds on every replica in the group at once.
///
/// The round trip is checked field by field rather than by comparing encodings,
/// because a codec that dropped a field symmetrically would pass an
/// encode-decode-encode comparison and lose the field anyway.

#define BOOST_TEST_MODULE shard_commands_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/shard_commands.hpp>

#include <cstdint>
#include <string>
#include <vector>

using kythira::decode_split_command;
using kythira::default_shard_key_codec;
using kythira::encode_split_command;
using kythira::serialization_exception;
using kythira::shard_descriptor;
using kythira::shard_epoch;
using kythira::shard_range;
using kythira::split_command;
using kythira::split_reason;

namespace {

using key_type = std::string;
using group_type = std::uint64_t;
using node_type = std::uint64_t;
using command_type = split_command<group_type, key_type, node_type>;
using descriptor_type = shard_descriptor<group_type, key_type, node_type>;

auto range_of(std::optional<key_type> start, std::optional<key_type> end) -> shard_range<key_type> {
    return shard_range<key_type>{._start = std::move(start), ._end = std::move(end)};
}

/// A three-way split of `(-inf, +inf)` at {"f", "m"}, leftmost derived.
auto sample() -> command_type {
    return command_type{
        ._parent_group = 1,
        ._parent_epoch = shard_epoch{._version = 4, ._conf_version = 2},
        ._at_keys = {"f", "m"},
        ._children = {descriptor_type{._group_id = 1,
                                      ._range = range_of(std::nullopt, key_type{"f"}),
                                      ._epoch = shard_epoch{._version = 7, ._conf_version = 2},
                                      ._voters = {1, 2, 3},
                                      ._learners = {4},
                                      ._leader_hint = 2},
                      descriptor_type{._group_id = 8,
                                      ._range = range_of(key_type{"f"}, key_type{"m"}),
                                      ._epoch = shard_epoch{._version = 7, ._conf_version = 2},
                                      ._voters = {1, 2, 3},
                                      ._learners = {4},
                                      ._leader_hint = std::nullopt},
                      descriptor_type{._group_id = 9,
                                      ._range = range_of(key_type{"m"}, std::nullopt),
                                      ._epoch = shard_epoch{._version = 7, ._conf_version = 2},
                                      ._voters = {1, 2, 3},
                                      ._learners = {4},
                                      ._leader_hint = std::nullopt}},
        ._right_derive = false,
        ._reason = split_reason::write_load,
        ._pd_operation_id = 4242};
}

auto round_trip(const command_type& cmd) -> command_type {
    return decode_split_command<group_type, key_type, node_type>(
        encode_split_command<group_type, key_type, node_type>(cmd));
}

}  // namespace

BOOST_AUTO_TEST_SUITE(shard_commands_unit)

// ── the key codec ────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(the_default_key_codec_round_trips_strings_and_integers) {
    const default_shard_key_codec<std::string> text;
    for (const std::string& k : {std::string{}, std::string{"a"}, std::string{"tenant/shard-04"},
                                 std::string{"\x00\xff", 2}}) {
        BOOST_CHECK_EQUAL(text.decode(text.encode(k)), k);
    }

    const default_shard_key_codec<std::uint64_t> numeric;
    for (std::uint64_t k : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{255},
                            std::uint64_t{0xDEADBEEF}, ~std::uint64_t{0}}) {
        BOOST_CHECK_EQUAL(numeric.decode(numeric.encode(k)), k);
    }
}

BOOST_AUTO_TEST_CASE(integer_keys_encode_big_endian_so_order_survives_the_wire) {
    // Big-endian, not native: an entry written on one machine is decoded on
    // another, and a native-endian encoding would have the split cut somewhere
    // else on a differently-ordered peer.
    const default_shard_key_codec<std::uint64_t> codec;
    const auto one = codec.encode(1);
    BOOST_REQUIRE_EQUAL(one.size(), 8u);
    BOOST_CHECK(one[0] == std::byte{0});
    BOOST_CHECK(one[7] == std::byte{1});

    // And the byte order matches the numeric order, which is what makes a
    // lexicographic store agree with a numeric key space.
    BOOST_CHECK(codec.encode(1) < codec.encode(2));
    BOOST_CHECK(codec.encode(255) < codec.encode(256));
}

// ── the split payload ────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_split_command_round_trips_field_for_field) {
    const auto original = sample();
    const auto decoded = round_trip(original);

    BOOST_CHECK_EQUAL(decoded._parent_group, original._parent_group);
    BOOST_CHECK(decoded._parent_epoch == original._parent_epoch);
    BOOST_CHECK(decoded._at_keys == original._at_keys);
    BOOST_CHECK_EQUAL(decoded._right_derive, original._right_derive);
    BOOST_CHECK(decoded._reason == original._reason);
    BOOST_REQUIRE(decoded._pd_operation_id.has_value());
    BOOST_CHECK_EQUAL(*decoded._pd_operation_id, 4242u);

    BOOST_REQUIRE_EQUAL(decoded._children.size(), original._children.size());
    for (std::size_t i = 0; i < decoded._children.size(); ++i) {
        const auto& a = decoded._children[i];
        const auto& b = original._children[i];
        BOOST_CHECK_EQUAL(a._group_id, b._group_id);
        BOOST_CHECK(a._range == b._range);
        BOOST_CHECK(a._epoch == b._epoch);
        BOOST_CHECK(a._voters == b._voters);
        BOOST_CHECK(a._learners == b._learners);
        BOOST_CHECK(a._leader_hint == b._leader_hint);
    }
    BOOST_CHECK(decoded == original);
}

BOOST_AUTO_TEST_CASE(unbounded_range_ends_survive_the_round_trip) {
    // The whole reason the bounds are optionals: the first split of
    // `(-inf, +inf)` has an unbounded end on each side, and a codec that turned
    // an absent bound into an empty key would silently change what the child
    // owns.
    const auto decoded = round_trip(sample());
    BOOST_CHECK(!decoded._children.front()._range._start.has_value());
    BOOST_CHECK(decoded._children.front()._range._end.has_value());
    BOOST_CHECK(decoded._children.back()._range._start.has_value());
    BOOST_CHECK(!decoded._children.back()._range._end.has_value());
}

BOOST_AUTO_TEST_CASE(an_absent_optional_stays_absent) {
    auto cmd = sample();
    cmd._pd_operation_id = std::nullopt;
    cmd._children[0]._leader_hint = std::nullopt;
    cmd._children[0]._learners.clear();

    const auto decoded = round_trip(cmd);
    BOOST_CHECK(!decoded._pd_operation_id.has_value());
    BOOST_CHECK(!decoded._children[0]._leader_hint.has_value());
    BOOST_CHECK(decoded._children[0]._learners.empty());
}

BOOST_AUTO_TEST_CASE(a_single_key_split_round_trips) {
    auto cmd = sample();
    cmd._at_keys = {"m"};
    cmd._children.resize(2);
    cmd._children[0]._range = range_of(std::nullopt, key_type{"m"});
    cmd._children[1]._range = range_of(key_type{"m"}, std::nullopt);
    const auto decoded = round_trip(cmd);
    BOOST_CHECK(decoded == cmd);
}

BOOST_AUTO_TEST_CASE(string_group_and_node_ids_round_trip) {
    // `raft_group_id` and `node_id` both admit std::string, and the encoder's
    // textual branch is otherwise never exercised.
    using text_command = split_command<std::string, key_type, std::string>;
    using text_descriptor = shard_descriptor<std::string, key_type, std::string>;

    const text_command cmd{
        ._parent_group = "tenant-a/shard-00",
        ._parent_epoch = shard_epoch{._version = 1, ._conf_version = 0},
        ._at_keys = {"m"},
        ._children = {text_descriptor{._group_id = "tenant-a/shard-00",
                                      ._range = range_of(std::nullopt, key_type{"m"}),
                                      ._epoch = shard_epoch{._version = 3, ._conf_version = 0},
                                      ._voters = {"node-1", "node-2"},
                                      ._learners = {},
                                      ._leader_hint = std::string{"node-1"}},
                      text_descriptor{._group_id = "tenant-a/shard-01",
                                      ._range = range_of(key_type{"m"}, std::nullopt),
                                      ._epoch = shard_epoch{._version = 3, ._conf_version = 0},
                                      ._voters = {"node-1", "node-2"},
                                      ._learners = {},
                                      ._leader_hint = std::nullopt}},
        ._right_derive = true,
        ._reason = split_reason::admin,
        ._pd_operation_id = std::nullopt};

    const auto decoded = decode_split_command<std::string, key_type, std::string>(
        encode_split_command<std::string, key_type, std::string>(cmd));
    BOOST_CHECK(decoded == cmd);
}

BOOST_AUTO_TEST_CASE(the_derived_child_is_the_one_reusing_the_parents_group_id) {
    const auto cmd = sample();
    const auto derived = cmd.derived_child();
    BOOST_REQUIRE(derived.has_value());
    BOOST_CHECK_EQUAL(derived->_group_id, cmd._parent_group);

    auto orphaned = cmd;
    orphaned._children[0]._group_id = 99;
    BOOST_CHECK(!orphaned.derived_child().has_value());
}

// ── the two things that actually go wrong ────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_payload_from_an_unknown_version_is_refused) {
    // A node that starts on a newer binary and replays an older log has to be
    // able to say "I do not understand this entry" rather than misread it.
    auto bytes = encode_split_command<group_type, key_type, node_type>(sample());
    BOOST_REQUIRE(!bytes.empty());
    bytes[0] = std::byte{99};
    BOOST_CHECK_THROW((decode_split_command<group_type, key_type, node_type>(bytes)),
                      serialization_exception);
}

BOOST_AUTO_TEST_CASE(a_truncated_payload_is_refused_at_every_length) {
    // Every prefix, not just a convenient one: a decoder that checked its
    // bounds in most places and not one would still read out of bounds on
    // exactly the corrupt entry that reached it.
    const auto bytes = encode_split_command<group_type, key_type, node_type>(sample());
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        const std::vector<std::byte> truncated(bytes.begin(),
                                               bytes.begin() + static_cast<std::ptrdiff_t>(n));
        bool threw = false;
        try {
            std::ignore = decode_split_command<group_type, key_type, node_type>(truncated);
        } catch (const std::exception&) {
            threw = true;
        }
        BOOST_CHECK_MESSAGE(threw, "decoding a " << n << "-byte prefix did not throw");
    }
}

BOOST_AUTO_TEST_CASE(an_absurd_length_field_is_refused_rather_than_allocated) {
    // A corrupt length must not become a multi-gigabyte allocation, let alone a
    // read past the end.
    auto bytes = encode_split_command<group_type, key_type, node_type>(sample());
    // The key count is the first u64 after the version byte, the group id and
    // the two epoch words.
    const std::size_t key_count_offset = 1 + 8 + 8 + 8;
    BOOST_REQUIRE_GT(bytes.size(), key_count_offset + 8);
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[key_count_offset + i] = std::byte{0xFF};
    }
    BOOST_CHECK_THROW((decode_split_command<group_type, key_type, node_type>(bytes)),
                      std::exception);
}

BOOST_AUTO_TEST_SUITE_END()

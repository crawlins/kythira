// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file shard_group_id_wire_property_test.cpp
/// @brief The multi-Raft `group_id` field round-trips through every serializer,
///        and a payload recorded *before* the field existed still decodes
///        (task 5 of `.kiro/specs/multi-raft/`).
///
/// The second half is the load-bearing one. Backward compatibility here is by
/// **default value**, not by a wire-format version: the field was appended with
/// a default, so an old payload has no key and decodes to `GroupId{}`, which is
/// exactly what "the single group" has always meant. A format version would
/// have forced a coordinated upgrade of five serializers and two `.proto` files
/// at once.
///
/// The legacy payloads below are constructed the way the pre-change encoder
/// constructed them — a JSON object without the key, a CBOR map whose pair
/// count does not include it — rather than by taking a current payload and
/// deleting something. A test that asked the new encoder to produce "old"
/// bytes would be asserting against itself.
///
/// The protobuf and Ion halves of the same guarantee live in
/// `shard_group_id_protobuf_wire_property_test.cpp` and
/// `shard_group_id_ion_wire_property_test.cpp`, which are only built when
/// those optional dependencies are present.

#define BOOST_TEST_MODULE shard_group_id_wire_property_test
#include <boost/test/unit_test.hpp>

#include <raft/cbor_serializer.hpp>
#include <raft/json_serializer.hpp>
#include <raft/types.hpp>

#include <cstdint>
#include <string>
#include <vector>

using data_type = std::vector<std::byte>;
using kythira::append_entries_request;
using kythira::append_entries_response;
using kythira::cbor_rpc_serializer;
using kythira::entry_type;
using kythira::fetch_log_entries_request;
using kythira::fetch_log_entries_response;
using kythira::install_snapshot_request;
using kythira::install_snapshot_response;
using kythira::json_rpc_serializer;
using kythira::log_entry;
using kythira::request_pre_vote_request;
using kythira::request_pre_vote_response;
using kythira::request_vote_request;
using kythira::request_vote_response;

namespace {

constexpr std::uint64_t kGroup = 0xDEADBEEFull;

auto bytes_of(const std::string& s) -> data_type {
    data_type out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(s[i]));
    }
    return out;
}

// ── the pre-change CBOR encoder, reproduced ─────────────────────────────────
//
// Only as much of it as the legacy payloads below need. This is deliberately a
// separate implementation from cbor_serializer.hpp's: its job is to emit bytes
// the *old* code would have emitted, and sharing the production writer would
// make the map's pair count — the one thing that changed — impossible to get
// wrong on purpose.

auto legacy_head(data_type& out, std::uint8_t major, std::uint64_t value) -> void {
    const auto major_bits = static_cast<std::uint8_t>(major << 5U);
    if (value < 24) {
        out.push_back(static_cast<std::byte>(major_bits | static_cast<std::uint8_t>(value)));
    } else if (value <= 0xFF) {
        out.push_back(static_cast<std::byte>(major_bits | 24U));
        out.push_back(static_cast<std::byte>(value));
    } else {
        // Nothing in these fixtures needs a wider head.
        BOOST_FAIL("legacy CBOR fixture needs a wider integer head");
    }
}

auto legacy_text(data_type& out, const std::string& s) -> void {
    legacy_head(out, 3, s.size());
    for (char c : s) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
}

auto legacy_uint(data_type& out, std::uint64_t v) -> void {
    legacy_head(out, 0, v);
}
auto legacy_map(data_type& out, std::uint64_t pairs) -> void {
    legacy_head(out, 5, pairs);
}
auto legacy_bool(data_type& out, bool b) -> void {
    out.push_back(static_cast<std::byte>(b ? 0xF5U : 0xF4U));
}

}  // namespace

BOOST_AUTO_TEST_SUITE(shard_group_id_wire)

// ─────────────────────────────────────────────────────────────────────────────
// JSON
// ─────────────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(json_round_trips_a_non_zero_group_id_on_every_message) {
    const json_rpc_serializer<data_type> s;

    {
        const request_vote_request<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t> in{
            ._term = 7,
            ._candidate_id = 3,
            ._last_log_index = 11,
            ._last_log_term = 5,
            ._group_id = kGroup};
        const auto out = s.deserialize_request_vote_request(s.serialize(in));
        BOOST_CHECK_EQUAL(out.group_id(), kGroup);
        BOOST_CHECK_EQUAL(out.term(), 7u);
        BOOST_CHECK_EQUAL(out.candidate_id(), 3u);
    }
    {
        const request_vote_response<std::uint64_t, std::uint64_t> in{
            ._term = 7, ._vote_granted = true, ._group_id = kGroup};
        const auto out = s.deserialize_request_vote_response(s.serialize(in));
        BOOST_CHECK_EQUAL(out.group_id(), kGroup);
        BOOST_CHECK(out.vote_granted());
    }
    {
        const request_pre_vote_request<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t>
            in{._term = 9,
               ._candidate_id = 1,
               ._last_log_index = 2,
               ._last_log_term = 1,
               ._group_id = kGroup};
        const auto out = s.deserialize_request_pre_vote_request(s.serialize(in));
        BOOST_CHECK_EQUAL(out.group_id(), kGroup);
    }
    {
        const request_pre_vote_response<std::uint64_t, std::uint64_t> in{
            ._term = 9, ._vote_granted = false, ._group_id = kGroup};
        const auto out = s.deserialize_request_pre_vote_response(s.serialize(in));
        BOOST_CHECK_EQUAL(out.group_id(), kGroup);
    }
    {
        append_entries_request<std::uint64_t, std::uint64_t, std::uint64_t, log_entry<>,
                               std::uint64_t>
            in{._term = 4,
               ._leader_id = 2,
               ._prev_log_index = 1,
               ._prev_log_term = 3,
               ._entries = {log_entry<>{
                   ._term = 4, ._index = 2, ._command = {}, ._type = entry_type::normal}},
               ._leader_commit = 1,
               ._group_id = kGroup};
        const auto out = s.deserialize_append_entries_request(s.serialize(in));
        BOOST_CHECK_EQUAL(out.group_id(), kGroup);
        BOOST_CHECK_EQUAL(out.entries().size(), 1u);
    }
    {
        const append_entries_response<std::uint64_t, std::uint64_t, std::uint64_t> in{
            ._term = 4,
            ._success = true,
            ._conflict_index = std::nullopt,
            ._conflict_term = std::nullopt,
            ._group_id = kGroup};
        const auto out = s.deserialize_append_entries_response(s.serialize(in));
        BOOST_CHECK_EQUAL(out.group_id(), kGroup);
    }
    {
        const install_snapshot_request<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t>
            in{._term = 6,
               ._leader_id = 1,
               ._last_included_index = 10,
               ._last_included_term = 5,
               ._offset = 0,
               ._data = {std::byte{1}, std::byte{2}},
               ._done = true,
               ._group_id = kGroup};
        const auto out = s.deserialize_install_snapshot_request(s.serialize(in));
        BOOST_CHECK_EQUAL(out.group_id(), kGroup);
        BOOST_CHECK_EQUAL(out.data().size(), 2u);
    }
    {
        const install_snapshot_response<std::uint64_t, std::uint64_t> in{._term = 6,
                                                                         ._group_id = kGroup};
        const auto out = s.deserialize_install_snapshot_response(s.serialize(in));
        BOOST_CHECK_EQUAL(out.group_id(), kGroup);
    }
    {
        const fetch_log_entries_request<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t>
            in{._requester_id = 3, ._from_index = 2, ._to_index = 9, ._group_id = kGroup};
        const auto out = s.deserialize_fetch_log_entries_request(s.serialize(in));
        BOOST_CHECK_EQUAL(out.group_id(), kGroup);
    }
    {
        const fetch_log_entries_response<std::uint64_t, std::uint64_t, log_entry<>, std::uint64_t>
            in{._responder_id = 3,
               ._available = true,
               ._prev_log_term = 4,
               ._entries = {},
               ._group_id = kGroup};
        const auto out = s.deserialize_fetch_log_entries_response(s.serialize(in));
        BOOST_CHECK_EQUAL(out.group_id(), kGroup);
    }
}

BOOST_AUTO_TEST_CASE(json_decodes_a_pre_multiraft_payload_to_the_default_group) {
    // Byte-for-byte what json_rpc_serializer emitted before the field existed.
    const json_rpc_serializer<data_type> s;

    const auto legacy_rv = bytes_of(R"({"type":"request_vote_request","term":7,"candidate_id":3,)"
                                    R"("last_log_index":11,"last_log_term":5})");
    const auto rv = s.deserialize_request_vote_request(legacy_rv);
    BOOST_CHECK_EQUAL(rv.group_id(), 0u);
    BOOST_CHECK_EQUAL(rv.term(), 7u);
    BOOST_CHECK_EQUAL(rv.candidate_id(), 3u);
    BOOST_CHECK_EQUAL(rv.last_log_index(), 11u);

    const auto legacy_ae =
        bytes_of(R"({"type":"append_entries_request","term":4,"leader_id":2,"prev_log_index":1,)"
                 R"("prev_log_term":3,"leader_commit":1,"entries":[]})");
    const auto ae = s.deserialize_append_entries_request(legacy_ae);
    BOOST_CHECK_EQUAL(ae.group_id(), 0u);
    BOOST_CHECK_EQUAL(ae.leader_id(), 2u);

    const auto legacy_is = bytes_of(R"({"type":"install_snapshot_response","term":6})");
    const auto is = s.deserialize_install_snapshot_response(legacy_is);
    BOOST_CHECK_EQUAL(is.group_id(), 0u);
    BOOST_CHECK_EQUAL(is.term(), 6u);
}

BOOST_AUTO_TEST_CASE(json_round_trips_a_string_group_id) {
    // `raft_group_id` admits std::string, and the encoder's `if constexpr`
    // branch for it is otherwise never exercised.
    const json_rpc_serializer<data_type> s;
    const request_vote_response<std::uint64_t, std::string> in{
        ._term = 3, ._vote_granted = true, ._group_id = "tenant-a/shard-04"};
    const auto out =
        s.deserialize_request_vote_response<std::uint64_t, std::string>(s.serialize(in));
    BOOST_CHECK_EQUAL(out.group_id(), "tenant-a/shard-04");
}

// ─────────────────────────────────────────────────────────────────────────────
// CBOR
// ─────────────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(cbor_round_trips_a_non_zero_group_id) {
    const cbor_rpc_serializer<data_type> s;

    {
        const request_vote_request<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t> in{
            ._term = 7,
            ._candidate_id = 3,
            ._last_log_index = 11,
            ._last_log_term = 5,
            ._group_id = kGroup};
        const auto out = s.deserialize_request_vote_request(s.serialize(in));
        BOOST_CHECK_EQUAL(out.group_id(), kGroup);
        BOOST_CHECK_EQUAL(out.last_log_term(), 5u);
    }
    {
        const append_entries_response<std::uint64_t, std::uint64_t, std::uint64_t> in{
            ._term = 4,
            ._success = false,
            ._conflict_index = 3,
            ._conflict_term = 2,
            ._group_id = kGroup};
        const auto out = s.deserialize_append_entries_response(s.serialize(in));
        BOOST_CHECK_EQUAL(out.group_id(), kGroup);
        // The map's pair count is computed, not literal, on this message —
        // getting the +1 wrong here would truncate the optionals.
        BOOST_REQUIRE(out.conflict_index().has_value());
        BOOST_CHECK_EQUAL(*out.conflict_index(), 3u);
        BOOST_REQUIRE(out.conflict_term().has_value());
        BOOST_CHECK_EQUAL(*out.conflict_term(), 2u);
    }
    {
        const install_snapshot_response<std::uint64_t, std::uint64_t> in{._term = 6,
                                                                         ._group_id = kGroup};
        const auto out = s.deserialize_install_snapshot_response(s.serialize(in));
        BOOST_CHECK_EQUAL(out.group_id(), kGroup);
    }
    {
        const fetch_log_entries_request<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t>
            in{._requester_id = 3, ._from_index = 2, ._to_index = 9, ._group_id = kGroup};
        const auto out = s.deserialize_fetch_log_entries_request(s.serialize(in));
        BOOST_CHECK_EQUAL(out.group_id(), kGroup);
        BOOST_CHECK_EQUAL(out.to_index(), 9u);
    }
}

BOOST_AUTO_TEST_CASE(cbor_decodes_a_pre_multiraft_payload_to_the_default_group) {
    const cbor_rpc_serializer<data_type> s;

    // map(3) { "type": "request_vote_response", "term": 7, "vote_granted": true }
    // — three pairs, not four: the pair count is what the old encoder wrote.
    data_type legacy;
    legacy_map(legacy, 3);
    legacy_text(legacy, "type");
    legacy_text(legacy, "request_vote_response");
    legacy_text(legacy, "term");
    legacy_uint(legacy, 7);
    legacy_text(legacy, "vote_granted");
    legacy_bool(legacy, true);

    const auto out = s.deserialize_request_vote_response(legacy);
    BOOST_CHECK_EQUAL(out.group_id(), 0u);
    BOOST_CHECK_EQUAL(out.term(), 7u);
    BOOST_CHECK(out.vote_granted());
}

BOOST_AUTO_TEST_CASE(cbor_decodes_a_pre_multiraft_request_vote_request) {
    const cbor_rpc_serializer<data_type> s;

    data_type legacy;
    legacy_map(legacy, 5);
    legacy_text(legacy, "type");
    legacy_text(legacy, "request_vote_request");
    legacy_text(legacy, "term");
    legacy_uint(legacy, 7);
    legacy_text(legacy, "candidate_id");
    legacy_uint(legacy, 3);
    legacy_text(legacy, "last_log_index");
    legacy_uint(legacy, 11);
    legacy_text(legacy, "last_log_term");
    legacy_uint(legacy, 5);

    const auto out = s.deserialize_request_vote_request(legacy);
    BOOST_CHECK_EQUAL(out.group_id(), 0u);
    BOOST_CHECK_EQUAL(out.candidate_id(), 3u);
    BOOST_CHECK_EQUAL(out.last_log_index(), 11u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Admin entry types
// ─────────────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(the_five_admin_entry_types_round_trip_through_a_log_entry) {
    // entry_type gained five values; a serializer that clamped or rejected them
    // would make the split and merge entries undeliverable.
    const json_rpc_serializer<data_type> json;
    const cbor_rpc_serializer<data_type> cbor;

    for (auto t : {entry_type::split, entry_type::merge_prepare, entry_type::merge_commit,
                   entry_type::merge_rollback, entry_type::merge_abandoned}) {
        BOOST_CHECK(kythira::is_admin_entry_type(t));

        append_entries_request<std::uint64_t, std::uint64_t, std::uint64_t, log_entry<>,
                               std::uint64_t>
            in{._term = 4,
               ._leader_id = 2,
               ._prev_log_index = 1,
               ._prev_log_term = 3,
               ._entries = {log_entry<>{
                   ._term = 4, ._index = 2, ._command = {std::byte{9}}, ._type = t}},
               ._leader_commit = 1,
               ._group_id = kGroup};

        BOOST_CHECK(
            json.deserialize_append_entries_request(json.serialize(in)).entries().at(0).type() ==
            t);
        BOOST_CHECK(
            cbor.deserialize_append_entries_request(cbor.serialize(in)).entries().at(0).type() ==
            t);
    }

    // And the three pre-existing types are still not admin entries.
    BOOST_CHECK(!kythira::is_admin_entry_type(entry_type::normal));
    BOOST_CHECK(!kythira::is_admin_entry_type(entry_type::configuration));
    BOOST_CHECK(!kythira::is_admin_entry_type(entry_type::no_op));
}

BOOST_AUTO_TEST_SUITE_END()

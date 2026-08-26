// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file shard_group_id_ion_wire_property_test.cpp
/// @brief The Amazon Ion half of the multi-Raft `group_id` wire guarantee
///        (task 5 of `.kiro/specs/multi-raft/`).
///
/// Split out of `shard_group_id_wire_property_test.cpp` because `ionc` is an
/// optional dependency: gating the JSON and CBOR halves on it would have
/// quietly stopped testing them in every build without Ion, which is the
/// default one.
///
/// The legacy payload here is produced through the JSON serializer's Ion
/// counterpart rather than by hand: Ion's binary encoding carries a symbol
/// table, so a hand-written byte fixture would be asserting against a symbol
/// assignment rather than against the field's absence. Instead the old shape is
/// reproduced by encoding through `ion_json_equivalence`-style text Ion, which
/// the reader accepts and which simply has no `group_id` field.

#define BOOST_TEST_MODULE shard_group_id_ion_wire_property_test
#include <boost/test/unit_test.hpp>

#include <raft/ion_serializer.hpp>
#include <raft/types.hpp>

#include <cstdint>
#include <string>
#include <vector>

using data_type = std::vector<std::byte>;
using kythira::append_entries_request;
using kythira::append_entries_response;
using kythira::entry_type;
using kythira::fetch_log_entries_request;
using kythira::install_snapshot_response;
using kythira::ion_rpc_serializer;
using kythira::log_entry;
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

}  // namespace

BOOST_AUTO_TEST_SUITE(shard_group_id_ion_wire)

BOOST_AUTO_TEST_CASE(ion_round_trips_a_non_zero_group_id) {
    const ion_rpc_serializer<data_type> s;

    {
        const request_vote_request<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t> in{
            ._term = 7,
            ._candidate_id = 3,
            ._last_log_index = 11,
            ._last_log_term = 5,
            ._group_id = kGroup};
        const auto out = s.deserialize_request_vote_request(s.serialize(in));
        BOOST_CHECK_EQUAL(out.group_id(), kGroup);
        BOOST_CHECK_EQUAL(out.candidate_id(), 3u);
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
        BOOST_REQUIRE(out.conflict_index().has_value());
        BOOST_CHECK_EQUAL(*out.conflict_index(), 3u);
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

BOOST_AUTO_TEST_CASE(ion_decodes_a_pre_multiraft_payload_to_the_default_group) {
    // Text Ion with the annotation and the four fields the pre-change encoder
    // wrote — and no `group_id`. The reader accepts text and binary alike, so
    // this is the old shape without depending on a binary symbol assignment.
    const ion_rpc_serializer<data_type> s;

    const auto legacy = bytes_of(
        "request_vote_request::{term: 7, candidate_id: 3, "
        "last_log_index: 11, last_log_term: 5}");
    const auto out = s.deserialize_request_vote_request(legacy);
    BOOST_CHECK_EQUAL(out.group_id(), 0u);
    BOOST_CHECK_EQUAL(out.term(), 7u);
    BOOST_CHECK_EQUAL(out.candidate_id(), 3u);
    BOOST_CHECK_EQUAL(out.last_log_index(), 11u);
    BOOST_CHECK_EQUAL(out.last_log_term(), 5u);
}

BOOST_AUTO_TEST_CASE(ion_round_trips_a_string_group_id) {
    const ion_rpc_serializer<data_type> s;
    const request_vote_response<std::uint64_t, std::string> in{
        ._term = 3, ._vote_granted = true, ._group_id = "tenant-a/shard-04"};
    const auto out =
        s.deserialize_request_vote_response<std::uint64_t, std::string>(s.serialize(in));
    BOOST_CHECK_EQUAL(out.group_id(), "tenant-a/shard-04");
}

BOOST_AUTO_TEST_CASE(ion_round_trips_the_five_admin_entry_types) {
    const ion_rpc_serializer<data_type> s;
    for (auto t : {entry_type::split, entry_type::merge_prepare, entry_type::merge_commit,
                   entry_type::merge_rollback, entry_type::merge_abandoned}) {
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
        const auto out = s.deserialize_append_entries_request(s.serialize(in));
        BOOST_CHECK(out.entries().at(0).type() == t);
        BOOST_CHECK_EQUAL(out.group_id(), kGroup);
    }
}

BOOST_AUTO_TEST_SUITE_END()

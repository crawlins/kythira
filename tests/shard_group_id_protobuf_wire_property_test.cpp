// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file shard_group_id_protobuf_wire_property_test.cpp
/// @brief The protobuf half of the multi-Raft `group_id` wire guarantee
///        (task 5 of `.kiro/specs/multi-raft/`).
///
/// Split out of `shard_group_id_wire_property_test.cpp` because
/// `raft_protobuf_serializer` is an optional build target: the JSON and CBOR
/// halves must run in every configuration, and gating all three on protobuf
/// would have quietly stopped testing them wherever protoc is absent.
///
/// See that file's header for why backward compatibility here is by default
/// value rather than by a wire-format version.

#define BOOST_TEST_MODULE shard_group_id_protobuf_wire_property_test
#include <boost/test/unit_test.hpp>

#include <raft/protobuf_serializer.hpp>
#include <raft/types.hpp>

#include <cstdint>
#include <string>
#include <vector>

using data_type = std::vector<std::byte>;
using kythira::append_entries_request;
using kythira::append_entries_response;
using kythira::entry_type;
using kythira::fetch_log_entries_response;
using kythira::log_entry;
using kythira::protobuf_rpc_serializer;
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

BOOST_AUTO_TEST_SUITE(shard_group_id_protobuf_wire)

BOOST_AUTO_TEST_CASE(protobuf_round_trips_a_non_zero_group_id) {
    const protobuf_rpc_serializer<data_type> s;

    {
        const request_vote_request<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t> in{
            ._term = 7,
            ._candidate_id = 3,
            ._last_log_index = 11,
            ._last_log_term = 5,
            ._group_id = kGroup};
        const auto out = s.deserialize_request_vote_request(s.serialize(in));
        BOOST_CHECK_EQUAL(out.group_id(), kGroup);
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
        BOOST_CHECK(!out.conflict_index().has_value());
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

BOOST_AUTO_TEST_CASE(protobuf_round_trips_a_string_group_id) {
    const protobuf_rpc_serializer<data_type> s;
    const request_vote_response<std::uint64_t, std::string> in{
        ._term = 3, ._vote_granted = true, ._group_id = "tenant-a/shard-04"};
    const auto out =
        s.deserialize_request_vote_response<std::uint64_t, std::string>(s.serialize(in));
    BOOST_CHECK_EQUAL(out.group_id(), "tenant-a/shard-04");
}

BOOST_AUTO_TEST_CASE(protobuf_decodes_a_pre_multiraft_payload_to_the_default_group) {
    // An old encoder never wrote field 5, so the submessage arrives with no
    // oneof case set. Building the proto directly and leaving group_id unset is
    // exactly that payload — no hand-encoding of varints needed, because
    // "field absent" is the same bytes either way.
    namespace raft_pb = ::kythira::raft::serializer::v1;

    raft_pb::RequestVoteRequest msg;
    msg.set_term(7);
    msg.mutable_candidate_id()->set_numeric(3);
    msg.set_last_log_index(11);
    msg.set_last_log_term(5);
    BOOST_REQUIRE(!msg.has_group_id());

    std::string payload;
    payload.push_back(static_cast<char>(0));  // message_tag::request_vote_request
    payload.append(msg.SerializeAsString());

    const protobuf_rpc_serializer<data_type> s;
    const auto out = s.deserialize_request_vote_request(bytes_of(payload));
    BOOST_CHECK_EQUAL(out.group_id(), 0u);
    BOOST_CHECK_EQUAL(out.term(), 7u);
    BOOST_CHECK_EQUAL(out.candidate_id(), 3u);
}

BOOST_AUTO_TEST_CASE(protobuf_rejects_a_group_id_of_the_wrong_type) {
    // An UNSET oneof is an old payload and decodes to the default; a *populated*
    // case of the wrong type is a genuine type mismatch and must not be
    // quietly coerced.
    namespace raft_pb = ::kythira::raft::serializer::v1;

    raft_pb::RequestVoteResponse msg;
    msg.set_term(3);
    msg.set_vote_granted(true);
    msg.mutable_group_id()->set_text("tenant-a");

    std::string payload;
    payload.push_back(static_cast<char>(1));  // message_tag::request_vote_response
    payload.append(msg.SerializeAsString());

    const protobuf_rpc_serializer<data_type> s;
    BOOST_CHECK_THROW(s.deserialize_request_vote_response(bytes_of(payload)),
                      kythira::serialization_exception);
}

BOOST_AUTO_TEST_CASE(protobuf_round_trips_the_five_admin_entry_types) {
    const protobuf_rpc_serializer<data_type> pb;
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
        const auto out = pb.deserialize_append_entries_request(pb.serialize(in));
        BOOST_CHECK(out.entries().at(0).type() == t);
        BOOST_CHECK_EQUAL(out.group_id(), kGroup);
    }
}

BOOST_AUTO_TEST_SUITE_END()

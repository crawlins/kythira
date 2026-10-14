// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Integer range coverage for protobuf_rpc_serializer, the protobuf counterpart
// of json_serializer_range_property_test: golden vectors pin the encoder's
// bytes, every uint64 value round-trips, and a wire value the receiver's
// field type cannot hold fails the decode instead of being truncated.

#define BOOST_TEST_MODULE ProtobufSerializerRangePropertyTest
#include <boost/test/unit_test.hpp>

#include <raft/exceptions.hpp>
#include <raft/protobuf_serializer.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace {

using serializer_t = kythira::protobuf_rpc_serializer<std::vector<std::byte>>;
namespace raft_pb = kythira::raft_pb;

constexpr std::size_t property_test_iterations = 200;

// The sample messages and hex helper are shared verbatim with
// ion_serializer_range_property_test.cpp.
auto sample_bytes(std::size_t n) -> std::vector<std::byte> {
    std::vector<std::byte> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(static_cast<std::byte>((i * 37 + 11) & 0xFF));
    }
    return out;
}

auto sample_request_vote() -> kythira::request_vote_request<> {
    kythira::request_vote_request<> m{};
    m._group_id = 3;
    m._term = 5;
    m._candidate_id = 2;
    m._last_log_index = 300;
    m._last_log_term = 4;
    return m;
}

auto sample_append_entries() -> kythira::append_entries_request<> {
    kythira::append_entries_request<> m{};
    m._group_id = 0;
    m._term = 7;
    m._leader_id = 1;
    m._prev_log_index = 40;
    m._prev_log_term = 6;
    m._leader_commit = 39;
    kythira::log_entry<> a{};
    a._term = 7;
    a._index = 41;
    a._command = sample_bytes(5);
    a._type = kythira::entry_type::configuration;
    kythira::log_entry<> b{};
    b._term = 7;
    b._index = 70000;
    b._command = {};
    b._type = kythira::entry_type::no_op;
    m._entries = {a, b};
    return m;
}

auto sample_append_entries_response() -> kythira::append_entries_response<> {
    kythira::append_entries_response<> m{};
    m._group_id = 9;
    m._term = 12;
    m._success = false;
    m._conflict_index = 1234567;
    m._conflict_term = 11;
    return m;
}

auto sample_install_snapshot() -> kythira::install_snapshot_request<> {
    kythira::install_snapshot_request<> m{};
    m._group_id = 0;
    m._term = 9;
    m._leader_id = 3;
    m._last_included_index = 100;
    m._last_included_term = 8;
    m._offset = 4096;
    m._data = sample_bytes(16);
    m._done = true;
    return m;
}

auto sample_fetch_response() -> kythira::fetch_log_entries_response<> {
    kythira::fetch_log_entries_response<> m{};
    m._group_id = 2;
    m._responder_id = 4;
    m._available = true;
    m._prev_log_term = 6;
    kythira::log_entry<> e{};
    e._term = 6;
    e._index = 51;
    e._command = sample_bytes(3);
    m._entries = {e};
    return m;
}

auto to_hex(const std::vector<std::byte>& data) -> std::string {
    static constexpr std::array<char, 16> digits = {'0', '1', '2', '3', '4', '5', '6', '7',
                                                    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string out;
    out.reserve(data.size() * 2);
    for (auto b : data) {
        const auto v = std::to_integer<unsigned>(b);
        out.push_back(digits.at(v >> 4U));
        out.push_back(digits.at(v & 0xFU));
    }
    return out;
}

// Re-frame a hand-built protobuf body with the tag byte the serializer put on
// `framed`, the encoding of a real message of the same type.
auto reframe(const std::vector<std::byte>& framed, const google::protobuf::Message& body)
    -> std::vector<std::byte> {
    std::vector<std::byte> out{framed.front()};
    for (char c : body.SerializeAsString()) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

auto u64_samples(std::mt19937_64& rng) -> std::vector<std::uint64_t> {
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    std::vector<std::uint64_t> v = {0,
                                    1,
                                    (std::uint64_t{1} << 32) - 1,
                                    std::uint64_t{1} << 32,
                                    (std::uint64_t{1} << 63) - 1,
                                    std::uint64_t{1} << 63,
                                    max - 1,
                                    max};
    std::uniform_int_distribution<std::uint64_t> dist;
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        v.push_back(dist(rng));
    }
    return v;
}

}  // namespace

/**
 * The checked conversions do not change a single output byte (captured from
 * 0f60f34, before the change).
 */
BOOST_AUTO_TEST_CASE(golden_vectors_unchanged) {
    const serializer_t s;
    BOOST_TEST(to_hex(s.serialize(sample_request_vote())) == "0008051202080218ac0220042a020803");
    BOOST_TEST(
        to_hex(s.serialize(sample_append_entries())) ==
        "04080712020801182820062a0d080710291a050b30557a9f20012a08080710f0a204200230273a020800");
    BOOST_TEST(to_hex(s.serialize(sample_append_entries_response())) ==
               "05080c1887ad4b200b2a020809");
    BOOST_TEST(to_hex(s.serialize(sample_install_snapshot())) ==
               "060809120208031864200828802032100b30557a9fc4e90e33587da2c7ec1136380142020800");
    BOOST_TEST(to_hex(s.serialize(sample_fetch_response())) ==
               "0d0804100118062209080610331a030b30552a02080232020804");
}

/**
 * Every std::uint64_t value round-trips through the integer fields, including
 * the numeric node and group id oneofs and the legacy responder_id.
 */
BOOST_AUTO_TEST_CASE(property_full_range_u64_round_trip) {
    std::mt19937_64 rng(std::random_device{}());
    const serializer_t s;
    for (const auto v : u64_samples(rng)) {
        BOOST_TEST_CONTEXT("value " << v) {
            kythira::append_entries_request<> ae{};
            ae._group_id = v;
            ae._term = v;
            ae._leader_id = v;
            ae._prev_log_index = v;
            ae._prev_log_term = v;
            ae._leader_commit = v;
            kythira::log_entry<> e{};
            e._term = v;
            e._index = v;
            ae._entries.push_back(e);
            const auto ae2 = s.deserialize_append_entries_request(s.serialize(ae));
            BOOST_TEST(ae2.group_id() == v);
            BOOST_TEST(ae2.term() == v);
            BOOST_TEST(ae2.leader_id() == v);
            BOOST_TEST(ae2.prev_log_index() == v);
            BOOST_TEST(ae2.prev_log_term() == v);
            BOOST_TEST(ae2.leader_commit() == v);
            BOOST_TEST(ae2.entries().at(0).term() == v);
            BOOST_TEST(ae2.entries().at(0).index() == v);

            kythira::append_entries_response<> aer{};
            aer._term = v;
            aer._conflict_index = v;
            aer._conflict_term = v;
            const auto aer2 = s.deserialize_append_entries_response(s.serialize(aer));
            BOOST_TEST(aer2.conflict_index().value_or(0) == v);
            BOOST_TEST(aer2.conflict_index().has_value());
            BOOST_TEST(aer2.conflict_term().value_or(0) == v);
            BOOST_TEST(aer2.conflict_term().has_value());

            kythira::install_snapshot_request<> is{};
            is._term = v;
            is._leader_id = v;
            is._last_included_index = v;
            is._last_included_term = v;
            is._offset = static_cast<std::size_t>(v);
            const auto is2 = s.deserialize_install_snapshot_request(s.serialize(is));
            BOOST_TEST(is2.last_included_index() == v);
            BOOST_TEST(is2.offset() == static_cast<std::size_t>(v));

            kythira::fetch_log_entries_response<> fl{};
            fl._responder_id = v;
            fl._prev_log_term = v;
            const auto fl2 = s.deserialize_fetch_log_entries_response(s.serialize(fl));
            BOOST_TEST(fl2.responder_id() == v);
            BOOST_TEST(fl2.prev_log_term() == v);
        }
    }
}

/**
 * A receiver whose TermId, LogIndex, NodeId or GroupId is 32 bits wide
 * rejects a wire value past UINT32_MAX in every kind of integer field: plain
 * uint64s, log entry fields, the NodeIdValue and GroupIdValue oneofs, and the
 * legacy responder_id fallback.
 */
BOOST_AUTO_TEST_CASE(narrow_targets_reject_out_of_range_values) {
    using u32 = std::uint32_t;
    constexpr std::uint64_t fits = std::numeric_limits<u32>::max();
    constexpr std::uint64_t too_big = fits + 1;
    const serializer_t s;

    const auto decode_ae = [&](const std::vector<std::byte>& data) {
        return s
            .deserialize_append_entries_request<u32, u32, u32, kythira::log_entry<u32, u32>, u32>(
                data);
    };
    const auto decode_fl = [&](const std::vector<std::byte>& data) {
        return s.deserialize_fetch_log_entries_response<u32, u32, u32, kythira::log_entry<u32, u32>,
                                                        u32>(data);
    };

    auto ok = sample_append_entries();
    ok._group_id = fits;
    ok._term = fits;
    ok._leader_id = fits;
    ok._entries.at(0)._index = fits;
    const auto decoded = decode_ae(s.serialize(ok));
    BOOST_TEST(decoded.term() == fits);
    BOOST_TEST(decoded.leader_id() == fits);
    BOOST_TEST(decoded.group_id() == fits);
    BOOST_TEST(decoded.entries().at(0).index() == fits);

    for (int field = 0; field < 7; ++field) {
        BOOST_TEST_CONTEXT("field " << field) {
            auto bad = sample_append_entries();
            switch (field) {
                case 0:
                    bad._group_id = too_big;
                    break;
                case 1:
                    bad._term = too_big;
                    break;
                case 2:
                    bad._leader_id = too_big;
                    break;
                case 3:
                    bad._prev_log_index = too_big;
                    break;
                case 4:
                    bad._leader_commit = too_big;
                    break;
                case 5:
                    bad._entries.at(0)._term = too_big;
                    break;
                default:
                    bad._entries.at(1)._index = too_big;
                    break;
            }
            BOOST_CHECK_THROW(static_cast<void>(decode_ae(s.serialize(bad))),
                              kythira::serialization_exception);
        }
    }

    // Legacy responder_id only (an older peer): the fallback path is checked too.
    raft_pb::FetchLogEntriesResponse legacy;
    legacy.set_responder_id(too_big);
    legacy.set_available(true);
    const auto framed = s.serialize(sample_fetch_response());
    BOOST_CHECK_THROW(static_cast<void>(decode_fl(reframe(framed, legacy))),
                      kythira::serialization_exception);
    legacy.set_responder_id(fits);
    BOOST_TEST(decode_fl(reframe(framed, legacy)).responder_id() == fits);
}

/**
 * proto3 enums are open, so the wire can carry any int32 as an entry type.
 * One outside entry_type's uint8 range fails the decode rather than wrapping
 * into an unrelated entry type.
 */
BOOST_AUTO_TEST_CASE(entry_type_outside_uint8_is_rejected) {
    const serializer_t s;
    const auto framed = s.serialize(sample_append_entries());
    const auto with_type = [](int type) {
        raft_pb::AppendEntriesRequest body;
        body.set_term(1);
        body.mutable_leader_id()->set_numeric(1);
        auto* entry = body.add_entries();
        entry->set_term(1);
        entry->set_index(1);
        entry->set_type(static_cast<raft_pb::EntryType>(type));
        return body;
    };
    // Control: the same hand-built body with a valid type decodes.
    BOOST_TEST(static_cast<int>(s.deserialize_append_entries_request(reframe(framed, with_type(2)))
                                    .entries()
                                    .at(0)
                                    .type()) == 2);
    for (const int type : {256, 300, -1}) {
        BOOST_TEST_CONTEXT("type " << type) {
            const auto body = with_type(type);
            BOOST_CHECK_THROW(
                static_cast<void>(s.deserialize_append_entries_request(reframe(framed, body))),
                kythira::serialization_exception);
        }
    }
}

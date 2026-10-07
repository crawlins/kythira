// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Integer range coverage for ion_rpc_serializer, the Ion counterpart of
// json_serializer_range_property_test: golden vectors pin the encoder's bytes
// for values that fit int64, every uint64 value round-trips through every
// integer field (Ion ints are arbitrary precision, so values at or above 2^63
// are written as positive ints, never as negative int64s), and values a
// target type cannot hold fail the decode instead of being truncated.

#define BOOST_TEST_MODULE IonSerializerRangePropertyTest
#include <boost/test/unit_test.hpp>

#include <raft/exceptions.hpp>
#include <raft/ion_serializer.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace {

using serializer_t = kythira::ion_rpc_serializer<std::vector<std::byte>>;

constexpr std::size_t property_test_iterations = 200;

// The sample messages and hex helper are shared verbatim with
// protobuf_serializer_range_property_test.cpp.
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

auto from_text(const std::string& text) -> std::vector<std::byte> {
    std::vector<std::byte> out;
    out.reserve(text.size());
    for (char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

auto as_text(const std::vector<std::byte>& data) -> std::string {
    std::string out;
    out.reserve(data.size());
    for (auto b : data) {
        out.push_back(static_cast<char>(b));
    }
    return out;
}

// Fixed boundary values plus random ones, as in the JSON range test.
auto u64_samples(std::mt19937_64& rng) -> std::vector<std::uint64_t> {
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    std::vector<std::uint64_t> v = {0,
                                    1,
                                    255,
                                    256,
                                    (std::uint64_t{1} << 32) - 1,
                                    std::uint64_t{1} << 32,
                                    (std::uint64_t{1} << 63) - 1,
                                    std::uint64_t{1} << 63,
                                    (std::uint64_t{1} << 63) + 1,
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
 * Binary Ion output for values that fit int64 is byte-for-byte what the
 * encoder produced before uint64 support (captured from 0f60f34), so peers
 * running the old encoder and the new one interoperate.
 */
BOOST_AUTO_TEST_CASE(golden_vectors_binary_unchanged) {
    const serializer_t s;
    BOOST_TEST(to_hex(s.serialize(sample_request_vote())) ==
               "e00100eaeed68183ded287becf8e94726571756573745f766f74655f726571756573748867726f75"
               "705f6964847465726d8c63616e6469646174655f69648e8e6c6173745f6c6f675f696e6465788d6c"
               "6173745f6c6f675f7465726dee94818ade908b21038c21058d21028e22012c8f2104");
    BOOST_TEST(to_hex(s.serialize(sample_append_entries())) ==
               "e00100eaee01858183de018087befd8e96617070656e645f656e74726965735f7265717565737488"
               "67726f75705f6964847465726d896c65616465725f69648e8e707265765f6c6f675f696e6465788d"
               "707265765f6c6f675f7465726d8d6c65616465725f636f6d6d697487656e747269657385696e6465"
               "7887636f6d6d616e648a656e7472795f74797065eeb8818adeb48b208c21078d21018e21288f2106"
               "90212791bea0de908c210792212993a50b30557a9f942101dd8c2107922301117093a0942102");
    BOOST_TEST(to_hex(s.serialize(sample_append_entries_response())) ==
               "e00100eaeed48183ded087becd8e97617070656e645f656e74726965735f726573706f6e73658867"
               "726f75705f6964847465726d87737563636573738e8e636f6e666c6963745f696e6465788d636f6e"
               "666c6963745f7465726dee94818ade908b21098c210c8d108e2312d6878f210b");
    BOOST_TEST(to_hex(s.serialize(sample_install_snapshot())) ==
               "e00100eaeef38183deef87beec8e98696e7374616c6c5f736e617073686f745f7265717565737488"
               "67726f75705f6964847465726d896c65616465725f69648e936c6173745f696e636c756465645f69"
               "6e6465788e926c6173745f696e636c756465645f7465726d866f6666736574846461746184646f6e"
               "65eeab818adea78b208c21098d21038e21648f21089022100091ae900b30557a9fc4e90e33587da2"
               "c7ec11369211");
    BOOST_TEST(to_hex(s.serialize(sample_fetch_response())) ==
               "e00100eaeef78183def387bef08e9a66657463685f6c6f675f656e74726965735f726573706f6e73"
               "658867726f75705f69648c726573706f6e6465725f696489617661696c61626c658d707265765f6c"
               "6f675f7465726d87656e7472696573847465726d85696e64657887636f6d6d616e648a656e747279"
               "5f74797065eea0818ade9c8b21028c21048d118e21068fbe8edd90210691213392a30b30559320");
}

/**
 * A value at or above 2^63 is written as the positive Ion int it is. Text Ion
 * makes that readable without our decoder; the old encoder wrote -1 here.
 */
BOOST_AUTO_TEST_CASE(large_values_are_written_as_positive_ints) {
    const serializer_t text{kythira::ion_encoding::text};
    auto m = sample_request_vote();
    m._term = std::numeric_limits<std::uint64_t>::max();
    m._last_log_index = std::uint64_t{1} << 63;
    const auto out = as_text(text.serialize(m));
    BOOST_TEST(out.find("term:18446744073709551615") != std::string::npos, out);
    BOOST_TEST(out.find("last_log_index:9223372036854775808") != std::string::npos, out);
    BOOST_TEST(out.find('-') == std::string::npos, out);
}

/**
 * Every std::uint64_t value round-trips through every integer field of every
 * message, in both encodings.
 */
BOOST_AUTO_TEST_CASE(property_full_range_u64_round_trip) {
    std::mt19937_64 rng(std::random_device{}());
    const std::array<serializer_t, 2> serializers = {serializer_t{kythira::ion_encoding::binary},
                                                     serializer_t{kythira::ion_encoding::text}};

    for (const auto& s : serializers) {
        for (const auto v : u64_samples(rng)) {
            BOOST_TEST_CONTEXT("value " << v << " encoding " << s.name()) {
                kythira::request_vote_request<> rv{};
                rv._group_id = v;
                rv._term = v;
                rv._candidate_id = v;
                rv._last_log_index = v;
                rv._last_log_term = v;
                const auto rv2 = s.deserialize_request_vote_request(s.serialize(rv));
                BOOST_TEST(rv2.group_id() == v);
                BOOST_TEST(rv2.term() == v);
                BOOST_TEST(rv2.candidate_id() == v);
                BOOST_TEST(rv2.last_log_index() == v);
                BOOST_TEST(rv2.last_log_term() == v);

                kythira::request_pre_vote_request<> pv{};
                pv._group_id = v;
                pv._term = v;
                pv._candidate_id = v;
                pv._last_log_index = v;
                pv._last_log_term = v;
                const auto pv2 = s.deserialize_request_pre_vote_request(s.serialize(pv));
                BOOST_TEST(pv2.term() == v);
                BOOST_TEST(pv2.candidate_id() == v);
                BOOST_TEST(pv2.last_log_index() == v);
                BOOST_TEST(pv2.last_log_term() == v);

                kythira::request_vote_response<> rvr{};
                rvr._group_id = v;
                rvr._term = v;
                rvr._vote_granted = true;
                const auto rvr2 = s.deserialize_request_vote_response(s.serialize(rvr));
                BOOST_TEST(rvr2.group_id() == v);
                BOOST_TEST(rvr2.term() == v);

                kythira::timeout_now_request<> tn{};
                tn._group_id = v;
                tn._term = v;
                tn._leader_id = v;
                tn._last_log_index = v;
                const auto tn2 = s.deserialize_timeout_now_request(s.serialize(tn));
                BOOST_TEST(tn2.term() == v);
                BOOST_TEST(tn2.leader_id() == v);
                BOOST_TEST(tn2.last_log_index() == v);

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
                e._command = sample_bytes(4);
                ae._entries.push_back(e);
                const auto ae2 = s.deserialize_append_entries_request(s.serialize(ae));
                BOOST_TEST(ae2.group_id() == v);
                BOOST_TEST(ae2.term() == v);
                BOOST_TEST(ae2.leader_id() == v);
                BOOST_TEST(ae2.prev_log_index() == v);
                BOOST_TEST(ae2.prev_log_term() == v);
                BOOST_TEST(ae2.leader_commit() == v);
                BOOST_TEST_REQUIRE(ae2.entries().size() == 1U);
                BOOST_TEST(ae2.entries().at(0).term() == v);
                BOOST_TEST(ae2.entries().at(0).index() == v);

                kythira::append_entries_response<> aer{};
                aer._group_id = v;
                aer._term = v;
                aer._success = false;
                aer._conflict_index = v;
                aer._conflict_term = v;
                const auto aer2 = s.deserialize_append_entries_response(s.serialize(aer));
                BOOST_TEST(aer2.term() == v);
                BOOST_TEST(aer2.conflict_index().value_or(0) == v);
                BOOST_TEST(aer2.conflict_index().has_value());
                BOOST_TEST(aer2.conflict_term().value_or(0) == v);
                BOOST_TEST(aer2.conflict_term().has_value());

                kythira::install_snapshot_request<> is{};
                is._group_id = v;
                is._term = v;
                is._leader_id = v;
                is._last_included_index = v;
                is._last_included_term = v;
                is._offset = static_cast<std::size_t>(v);
                is._data = sample_bytes(8);
                is._done = false;
                const auto is2 = s.deserialize_install_snapshot_request(s.serialize(is));
                BOOST_TEST(is2.term() == v);
                BOOST_TEST(is2.leader_id() == v);
                BOOST_TEST(is2.last_included_index() == v);
                BOOST_TEST(is2.last_included_term() == v);
                BOOST_TEST(is2.offset() == static_cast<std::size_t>(v));

                kythira::fetch_log_entries_request<> fr{};
                fr._group_id = v;
                fr._requester_id = v;
                fr._from_index = v;
                fr._to_index = v;
                const auto fr2 = s.deserialize_fetch_log_entries_request(s.serialize(fr));
                BOOST_TEST(fr2.requester_id() == v);
                BOOST_TEST(fr2.from_index() == v);
                BOOST_TEST(fr2.to_index() == v);

                kythira::fetch_log_entries_response<> fl{};
                fl._group_id = v;
                fl._responder_id = v;
                fl._available = true;
                fl._prev_log_term = v;
                fl._entries.push_back(e);
                const auto fl2 = s.deserialize_fetch_log_entries_response(s.serialize(fl));
                BOOST_TEST(fl2.responder_id() == v);
                BOOST_TEST(fl2.prev_log_term() == v);
                BOOST_TEST(fl2.entries().at(0).index() == v);

                kythira::cluster_join_response<> cj{};
                cj.accepted = false;
                cj.redirect = kythira::peer_info<std::uint64_t, std::string>{v, "10.0.0.1:7000"};
                const auto cj2 = s.deserialize_cluster_join_response(s.serialize(cj));
                BOOST_TEST_REQUIRE(cj2.redirect.has_value());
                BOOST_TEST(cj2.redirect->node_id == v);
            }
        }
    }
}

/**
 * A receiver whose TermId, LogIndex, NodeId or GroupId is narrower than 64
 * bits rejects a value it cannot hold, and accepts the largest one it can.
 */
BOOST_AUTO_TEST_CASE(narrow_targets_reject_out_of_range_values) {
    using narrow_rv =
        kythira::request_vote_request<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>;
    constexpr std::uint64_t fits = std::numeric_limits<std::uint32_t>::max();
    constexpr std::uint64_t too_big = fits + 1;
    const serializer_t s;

    const auto decode_narrow = [&](const kythira::request_vote_request<>& m) {
        return s.deserialize_request_vote_request<std::uint32_t, std::uint32_t, std::uint32_t,
                                                  std::uint32_t>(s.serialize(m));
    };

    auto ok = sample_request_vote();
    ok._group_id = fits;
    ok._term = fits;
    ok._candidate_id = fits;
    ok._last_log_index = fits;
    ok._last_log_term = fits;
    const narrow_rv decoded = decode_narrow(ok);
    BOOST_TEST(decoded.term() == fits);
    BOOST_TEST(decoded.candidate_id() == fits);
    BOOST_TEST(decoded.group_id() == fits);

    for (int field = 0; field < 5; ++field) {
        BOOST_TEST_CONTEXT("field " << field) {
            auto bad = sample_request_vote();
            switch (field) {
                case 0:
                    bad._group_id = too_big;
                    break;
                case 1:
                    bad._term = too_big;
                    break;
                case 2:
                    bad._candidate_id = too_big;
                    break;
                case 3:
                    bad._last_log_index = too_big;
                    break;
                default:
                    bad._last_log_term = too_big;
                    break;
            }
            BOOST_CHECK_THROW(static_cast<void>(decode_narrow(bad)),
                              kythira::serialization_exception);
        }
    }

    // A 64-bit value at or above 2^63 into a 32-bit field takes the
    // arbitrary-precision read path and must still be rejected.
    auto huge = sample_request_vote();
    huge._term = std::numeric_limits<std::uint64_t>::max();
    BOOST_CHECK_THROW(static_cast<void>(decode_narrow(huge)), kythira::serialization_exception);
}

/**
 * Hand-written payloads with integers no peer of ours emits: negative values,
 * values past 2^64, null ints and an entry type past its uint8 range.
 */
BOOST_AUTO_TEST_CASE(hostile_integers_are_rejected) {
    const serializer_t s;
    const std::string rest = ",candidate_id:1,last_log_index:1,last_log_term:1}";
    for (const std::string term : {"-1", "-9223372036854775808", "18446744073709551616",
                                   "340282366920938463463374607431768211456", "null.int"}) {
        BOOST_TEST_CONTEXT("term " << term) {
            const auto payload = from_text("request_vote_request::{term:" + term + rest);
            BOOST_CHECK_THROW(static_cast<void>(s.deserialize_request_vote_request(payload)),
                              kythira::serialization_exception);
        }
    }

    // The largest valid value written by hand decodes, so the cases above fail
    // on range rather than on syntax.
    const auto max_payload = from_text("request_vote_request::{term:18446744073709551615" + rest);
    BOOST_TEST(s.deserialize_request_vote_request(max_payload).term() ==
               std::numeric_limits<std::uint64_t>::max());

    const auto good_type = from_text(
        "append_entries_request::{term:1,leader_id:1,prev_log_index:0,prev_log_term:0,"
        "leader_commit:0,entries:[{term:1,index:1,command:{{}},entry_type:255}]}");
    BOOST_TEST(static_cast<int>(
                   s.deserialize_append_entries_request(good_type).entries().at(0).type()) == 255);

    const auto bad_type = from_text(
        "append_entries_request::{term:1,leader_id:1,prev_log_index:0,prev_log_term:0,"
        "leader_commit:0,entries:[{term:1,index:1,command:{{}},entry_type:256}]}");
    BOOST_CHECK_THROW(static_cast<void>(s.deserialize_append_entries_request(bad_type)),
                      kythira::serialization_exception);
}

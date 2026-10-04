// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Range, round-trip and golden-vector coverage for json_rpc_serializer
// (.kiro/specs/json-serializer-input-validation, Requirements 1, 2, 4, 5, 6).

#define BOOST_TEST_MODULE JsonSerializerRangePropertyTest
#include <boost/test/unit_test.hpp>

#include <raft/json_serializer.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace {

using serializer_t = kythira::json_rpc_serializer<std::vector<std::byte>>;

constexpr std::size_t property_test_iterations = 200;
constexpr std::size_t max_random_bytes = 4096;

// Deterministic payload used for every golden vector: byte i is (37 * i + 11)
// mod 256, so every byte value appears and no two neighbours repeat.
auto pattern_bytes(std::size_t n) -> std::vector<std::byte> {
    std::vector<std::byte> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(static_cast<std::byte>((i * 37 + 11) & 0xFF));
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

// Captured from the encoder on main (c1a774c) before the decoder rewrite:
// base64 of pattern_bytes(n) for each golden length.
const char* const golden_base64_1024 =
    "CzBVep/E6Q4zWH2ix+wRNluApcrvFDleg6jN8hc8YYar0PUaP2SJrtP4HUJnjLHW+yBFao+02f4j"
    "SG2St9wBJktwlbrfBClOc5i94gcsUXabwOUKL1R5nsPoDTJXfKHG6xA1Wn+kye4TOF2Cp8zxFjtg"
    "harP9Bk+Y4it0vccQWaLsNX6H0RpjrPY/SJHbJG22wAlSm+Uud4DKE1yl7zhBitQdZq/5AkuU3id"
    "wucMMVZ7oMXqDzRZfqPI7RI3XIGmy/AVOl+Eqc7zGD1ih6zR9htAZYqv1PkeQ2iNstf8IUZrkLXa"
    "/yRJbpO43QInTHGWu+AFKk90mb7jCC1Sd5zB5gswVXqfxOkOM1h9osfsETZbgKXK7xQ5XoOozfIX"
    "PGGGq9D1Gj9kia7T+B1CZ4yx1vsgRWqPtNn+I0htkrfcASZLcJW63wQpTnOYveIHLFF2m8DlCi9U"
    "eZ7D6A0yV3yhxusQNVp/pMnuEzhdgqfM8RY7YIWqz/QZPmOIrdL3HEFmi7DV+h9EaY6z2P0iR2yR"
    "ttsAJUpvlLneAyhNcpe84QYrUHWav+QJLlN4ncLnDDFWe6DF6g80WX6jyO0SN1yBpsvwFTpfhKnO"
    "8xg9Yoes0fYbQGWKr9T5HkNojbLX/CFGa5C12v8kSW6TuN0CJ0xxlrvgBSpPdJm+4wgtUnecweYL"
    "MFV6n8TpDjNYfaLH7BE2W4Clyu8UOV6DqM3yFzxhhqvQ9Ro/ZImu0/gdQmeMsdb7IEVqj7TZ/iNI"
    "bZK33AEmS3CVut8EKU5zmL3iByxRdpvA5QovVHmew+gNMld8ocbrEDVaf6TJ7hM4XYKnzPEWO2CF"
    "qs/0GT5jiK3S9xxBZouw1fofRGmOs9j9IkdskbbbACVKb5S53gMoTXKXvOEGK1B1mr/kCS5TeJ3C"
    "5wwxVnugxeoPNFl+o8jtEjdcgabL8BU6X4SpzvMYPWKHrNH2G0Bliq/U+R5DaI2y1/whRmuQtdr/"
    "JEluk7jdAidMcZa74AUqT3SZvuMILVJ3nMHmCzBVep/E6Q4zWH2ix+wRNluApcrvFDleg6jN8hc8"
    "YYar0PUaP2SJrtP4HUJnjLHW+yBFao+02f4jSG2St9wBJktwlbrfBClOc5i94gcsUXabwOUKL1R5"
    "nsPoDTJXfKHG6xA1Wn+kye4TOF2Cp8zxFjtgharP9Bk+Y4it0vccQWaLsNX6H0RpjrPY/SJHbJG2"
    "2wAlSm+Uud4DKE1yl7zhBitQdZq/5AkuU3idwucMMVZ7oMXqDzRZfqPI7RI3XIGmy/AVOl+Eqc7z"
    "GD1ih6zR9htAZYqv1PkeQ2iNstf8IUZrkLXa/yRJbpO43QInTHGWu+AFKk90mb7jCC1Sd5zB5g==";

struct golden_case {
    std::size_t length;
    std::string base64;
};

auto golden_cases() -> std::vector<golden_case> {
    return {
        {0, ""},
        {1, "Cw=="},
        {2, "CzA="},
        {3, "CzBV"},
        {4, "CzBVeg=="},
        {5, "CzBVep8="},
        {1024, golden_base64_1024},
    };
}

auto random_u64(std::mt19937_64& rng) -> std::uint64_t {
    return std::uniform_int_distribution<std::uint64_t>{}(rng);
}

auto random_bytes(std::mt19937_64& rng, std::size_t n) -> std::vector<std::byte> {
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::vector<std::byte> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(static_cast<std::byte>(byte_dist(rng)));
    }
    return out;
}

// Fixed boundary values plus random ones, per Requirement 6.4.
auto u64_samples(std::mt19937_64& rng) -> std::vector<std::uint64_t> {
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    std::vector<std::uint64_t> v = {
        0, 1, (std::uint64_t{1} << 63) - 1, std::uint64_t{1} << 63, max - 1, max};
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        v.push_back(random_u64(rng));
    }
    return v;
}

}  // namespace

/**
 * Requirements 4.2, 5.1: the encoder's bytes are unchanged. Each expected
 * string was captured from the encoder before the base64 accumulator change.
 */
BOOST_AUTO_TEST_CASE(golden_vectors_append_entries_and_install_snapshot) {
    serializer_t s;
    for (const auto& gc : golden_cases()) {
        kythira::append_entries_request<> ae;
        ae._group_id = 0;
        ae._term = 7;
        ae._leader_id = 2;
        ae._prev_log_index = 40;
        ae._prev_log_term = 6;
        ae._leader_commit = 39;
        kythira::log_entry<> e;
        e._term = 7;
        e._index = 41;
        e._command = pattern_bytes(gc.length);
        e._type = kythira::entry_type::configuration;
        ae._entries.push_back(e);

        const std::string expected_ae =
            R"({"type":"append_entries_request","group_id":0,"term":7,"leader_id":2,)"
            R"("prev_log_index":40,"prev_log_term":6,"leader_commit":39,)"
            R"("entries":[{"term":7,"index":41,"command":")" +
            gc.base64 + R"(","entry_type":1}]})";
        BOOST_TEST(as_text(s.serialize(ae)) == expected_ae, "AppendEntries length " << gc.length);

        kythira::install_snapshot_request<> is;
        is._group_id = 0;
        is._term = 9;
        is._leader_id = 3;
        is._last_included_index = 100;
        is._last_included_term = 8;
        is._offset = 4096;
        is._data = pattern_bytes(gc.length);
        is._done = true;

        const std::string expected_is =
            R"({"type":"install_snapshot_request","group_id":0,"term":9,"leader_id":3,)"
            R"("last_included_index":100,"last_included_term":8,"offset":4096,"data":")" +
            gc.base64 + R"(","done":true})";
        BOOST_TEST(as_text(s.serialize(is)) == expected_is, "InstallSnapshot length " << gc.length);

        // And the golden encoding decodes back to the original bytes.
        BOOST_TEST(
            s.deserialize_append_entries_request(s.serialize(ae)).entries().at(0).command() ==
            pattern_bytes(gc.length));
        BOOST_TEST(s.deserialize_install_snapshot_request(s.serialize(is)).data() ==
                   pattern_bytes(gc.length));
    }
}

/**
 * Requirements 1.4, 5.2, 6.4: every std::uint64_t value, including those at or
 * above 2^63 that Boost.JSON parses as kind::uint64, round-trips through every
 * integer field of the main RPCs.
 */
BOOST_AUTO_TEST_CASE(property_full_range_u64_round_trip) {
    std::mt19937_64 rng(std::random_device{}());
    serializer_t s;

    for (const auto v : u64_samples(rng)) {
        BOOST_TEST_CONTEXT("value " << v) {
            kythira::request_vote_request<> rv;
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

            kythira::append_entries_request<> ae;
            ae._group_id = v;
            ae._term = v;
            ae._leader_id = v;
            ae._prev_log_index = v;
            ae._prev_log_term = v;
            ae._leader_commit = v;
            kythira::log_entry<> e;
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
            BOOST_REQUIRE_EQUAL(ae2.entries().size(), 1U);
            BOOST_TEST(ae2.entries()[0].term() == v);
            BOOST_TEST(ae2.entries()[0].index() == v);

            kythira::append_entries_response<> aer;
            aer._group_id = v;
            aer._term = v;
            aer._conflict_index = v;
            aer._conflict_term = v;
            aer._success = true;
            const auto aer2 = s.deserialize_append_entries_response(s.serialize(aer));
            BOOST_TEST(aer2.term() == v);
            BOOST_TEST(aer2.conflict_index().value() == v);
            BOOST_TEST(aer2.conflict_term().value() == v);

            kythira::install_snapshot_request<> is;
            is._group_id = v;
            is._term = v;
            is._leader_id = v;
            is._last_included_index = v;
            is._last_included_term = v;
            is._offset = static_cast<std::size_t>(v);
            is._done = false;
            const auto is2 = s.deserialize_install_snapshot_request(s.serialize(is));
            BOOST_TEST(is2.group_id() == v);
            BOOST_TEST(is2.term() == v);
            BOOST_TEST(is2.leader_id() == v);
            BOOST_TEST(is2.last_included_index() == v);
            BOOST_TEST(is2.last_included_term() == v);
            BOOST_TEST(is2.offset() == static_cast<std::size_t>(v));

            kythira::fetch_log_entries_response<> fr;
            fr._group_id = v;
            fr._responder_id = v;
            fr._prev_log_term = v;
            fr._available = true;
            kythira::log_entry<> fe;
            fe._term = v;
            fe._index = v;
            fr._entries.push_back(fe);
            const auto fr2 = s.deserialize_fetch_log_entries_response(s.serialize(fr));
            BOOST_TEST(fr2.group_id() == v);
            BOOST_TEST(fr2.responder_id() == v);
            BOOST_TEST(fr2.prev_log_term() == v);
            BOOST_REQUIRE_EQUAL(fr2.entries().size(), 1U);
            BOOST_TEST(fr2.entries()[0].term() == v);
            BOOST_TEST(fr2.entries()[0].index() == v);
        }
    }
}

/**
 * Requirements 2.6, 6.5: random byte strings round-trip through `command` and
 * `data`, and the strict decoder accepts everything the encoder emits.
 */
BOOST_AUTO_TEST_CASE(property_random_bytes_round_trip) {
    std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<std::size_t> len_dist(0, max_random_bytes);
    serializer_t s;

    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        const auto payload = random_bytes(rng, len_dist(rng));
        BOOST_TEST_CONTEXT("length " << payload.size()) {
            kythira::append_entries_request<> ae;
            kythira::log_entry<> e;
            e._command = payload;
            ae._entries.push_back(e);
            const auto ae2 = s.deserialize_append_entries_request(s.serialize(ae));
            BOOST_REQUIRE_EQUAL(ae2.entries().size(), 1U);
            BOOST_TEST(ae2.entries()[0].command() == payload);

            kythira::install_snapshot_request<> is;
            is._data = payload;
            is._done = true;
            BOOST_TEST(s.deserialize_install_snapshot_request(s.serialize(is)).data() == payload);
        }
    }
}

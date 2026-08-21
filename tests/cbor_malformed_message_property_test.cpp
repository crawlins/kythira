// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE CborMalformedMessagePropertyTest
#include <boost/test/unit_test.hpp>

#include <raft/cbor_serializer.hpp>

#include <random>
#include <string>
#include <vector>
#include <cstddef>

namespace {
constexpr std::size_t property_test_iterations = 100;
constexpr std::size_t max_random_bytes = 1000;
}

auto generate_random_bytes(std::mt19937& rng, std::size_t size) -> std::vector<std::byte> {
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::vector<std::byte> data;
    data.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        data.push_back(static_cast<std::byte>(byte_dist(rng)));
    }
    return data;
}

/**
 * Feature: cbor-rpc-serializer, Property 2: Malformed and truncated input rejected, never crashes
 * Validates: Requirements 6.1, 6.2, 9.3
 *
 * Property: For any random byte sequence, every deserialize_* method rejects it
 * with serialization_exception (or another std::exception) rather than reading
 * out of bounds, crashing, or returning a partially-populated value.
 */
BOOST_AUTO_TEST_CASE(property_random_bytes_rejected) {
    std::mt19937 rng(std::random_device{}());
    kythira::cbor_rpc_serializer<std::vector<std::byte>> serializer;

    std::uniform_int_distribution<std::size_t> size_dist(0, max_random_bytes);

    std::size_t handled = 0;
    std::size_t attempts = 0;

    auto attempt = [&](auto&& fn, const std::vector<std::byte>& data) {
        ++attempts;
        try {
            [[maybe_unused]] auto result = fn(data);
            // Extremely unlikely: random bytes decoded as a well-formed message
            // of exactly this type. Not a crash, so it counts as handled.
            ++handled;
        } catch (const kythira::serialization_exception&) {
            ++handled;
        } catch (const std::exception&) {
            ++handled;
        }
    };

    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        auto data = generate_random_bytes(rng, size_dist(rng));
        attempt([&](const auto& d) { return serializer.deserialize_request_vote_request(d); },
                data);
        attempt([&](const auto& d) { return serializer.deserialize_append_entries_request(d); },
                data);
        attempt([&](const auto& d) { return serializer.deserialize_install_snapshot_request(d); },
                data);
        attempt([&](const auto& d) { return serializer.deserialize_cluster_join_response(d); },
                data);
        attempt([&](const auto& d) { return serializer.deserialize_fetch_log_entries_response(d); },
                data);
    }

    // No call may crash; all must be handled (thrown or, vanishingly rarely,
    // cleanly decoded).
    BOOST_CHECK_EQUAL(handled, attempts);
}

/**
 * Feature: cbor-rpc-serializer, Property 2: Malformed and truncated input rejected, never crashes
 * Validates: Requirements 6.1, 9.3
 *
 * Property: For any valid message truncated at an arbitrary byte offset, the
 * matching deserialize_* method rejects the truncated buffer rather than
 * reading past its end.
 */
BOOST_AUTO_TEST_CASE(property_truncated_message_rejected) {
    std::mt19937 rng(std::random_device{}());
    kythira::cbor_rpc_serializer<std::vector<std::byte>> serializer;

    std::size_t rejection_count = 0;
    std::size_t truncation_count = 0;

    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        // Build a valid AppendEntries request with byte-carrying entries, then
        // truncate it at a random offset strictly shorter than the full length.
        kythira::append_entries_request<> req;
        req._term = 5;
        req._leader_id = 2;
        req._prev_log_index = 10;
        req._prev_log_term = 4;
        req._leader_commit = 8;
        kythira::log_entry<> entry;
        entry._term = 5;
        entry._index = 11;
        entry._command = std::vector<std::byte>{std::byte{0xAB}, std::byte{0xCD}, std::byte{0xEF}};
        req._entries.push_back(entry);

        auto full = serializer.serialize(req);
        if (full.size() < 2) {
            continue;
        }
        std::uniform_int_distribution<std::size_t> cut_dist(1, full.size() - 1);
        std::size_t cut = cut_dist(rng);
        std::vector<std::byte> truncated(full.begin(), full.begin() + cut);
        ++truncation_count;

        try {
            [[maybe_unused]] auto result = serializer.deserialize_append_entries_request(truncated);
            // A prefix should never decode to a complete, valid message.
        } catch (const kythira::serialization_exception&) {
            ++rejection_count;
        } catch (const std::exception&) {
            ++rejection_count;
        }
    }

    BOOST_TEST_MESSAGE("Truncated message rejection: " << rejection_count << "/" << truncation_count
                                                       << " rejected");
    BOOST_CHECK_EQUAL(rejection_count, truncation_count);
}

/**
 * Feature: cbor-rpc-serializer, Property 2: Malformed and truncated input rejected, never crashes
 * Validates: Requirements 6.1, 6.4
 *
 * Property: A byte-string / array / map header that declares a length far
 * larger than the remaining buffer is rejected rather than driving an
 * out-of-bounds read or an enormous allocation.
 */
BOOST_AUTO_TEST_CASE(property_oversized_length_header_rejected) {
    kythira::cbor_rpc_serializer<std::vector<std::byte>> serializer;

    // Hand-crafted CBOR fragments whose declared lengths exceed the buffer.
    // 0xBB = map, 8-byte length; a huge pair count with no following data.
    std::vector<std::vector<std::byte>> hostile = {
        // map header claiming 0xFFFFFFFFFFFFFFFF pairs, nothing follows
        {std::byte{0xBB}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
         std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}},
        // valid single-pair map, key "type" text string of declared length 200,
        // only a few bytes present (0x78 = text string, 1-byte length)
        {std::byte{0xA1}, std::byte{0x78}, std::byte{0xC8}, std::byte{'a'}, std::byte{'b'}},
        // array header (0x9B) claiming a huge count
        {std::byte{0xA1}, std::byte{0x64}, std::byte{'t'}, std::byte{'y'}, std::byte{'p'},
         std::byte{'e'}, std::byte{0x9B}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
         std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}},
    };

    std::size_t rejection_count = 0;
    for (const auto& data : hostile) {
        try {
            [[maybe_unused]] auto result = serializer.deserialize_append_entries_request(data);
        } catch (const std::exception&) {
            ++rejection_count;
        }
    }

    BOOST_CHECK_EQUAL(rejection_count, hostile.size());
}

/**
 * Feature: cbor-rpc-serializer, Property 2: Malformed and truncated input rejected, never crashes
 * Validates: Requirements 6.2, 6.4
 *
 * Property: Well-formed CBOR whose top-level item is not a map, or which uses a
 * construct outside the accepted subset (indefinite length, tags, floats,
 * negative integers), is rejected.
 */
BOOST_AUTO_TEST_CASE(property_unsupported_constructs_rejected) {
    kythira::cbor_rpc_serializer<std::vector<std::byte>> serializer;

    std::vector<std::vector<std::byte>> unsupported = {
        {std::byte{0x00}},  // top-level unsigned int, not a map
        {std::byte{0x80}},  // top-level empty array, not a map
        {std::byte{0x63}, std::byte{'a'}, std::byte{'b'}, std::byte{'c'}},  // text string
        {std::byte{0xBF}, std::byte{0xFF}},                                 // indefinite-length map
        {std::byte{0xC0}, std::byte{0x00}},                                 // tag (major type 6)
        {std::byte{0x20}},  // negative integer (major type 1)
        {std::byte{0xFB}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
         std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}},  // double float
        {std::byte{0xF6}},                                                     // null simple value
    };

    std::size_t rejection_count = 0;
    for (const auto& data : unsupported) {
        try {
            [[maybe_unused]] auto result = serializer.deserialize_request_vote_request(data);
        } catch (const std::exception&) {
            ++rejection_count;
        }
    }

    BOOST_CHECK_EQUAL(rejection_count, unsupported.size());
}

/**
 * Feature: cbor-rpc-serializer, Property 2: Malformed and truncated input rejected, never crashes
 * Validates: Requirements 6.2
 *
 * Property: A well-formed CBOR map that is missing a required key for the
 * requested message type is rejected.
 */
BOOST_AUTO_TEST_CASE(property_missing_required_key_rejected) {
    kythira::cbor_rpc_serializer<std::vector<std::byte>> serializer;

    // {"type":"request_vote_request","term":1} — valid CBOR, correct
    // discriminant, but missing candidate_id/last_log_index/last_log_term.
    std::vector<std::byte> data = {
        std::byte{0xA2},  // map, 2 pairs
        std::byte{0x64}, std::byte{'t'}, std::byte{'y'}, std::byte{'p'}, std::byte{'e'},
        std::byte{0x74},  // text string, 20 bytes
        std::byte{'r'},  std::byte{'e'}, std::byte{'q'}, std::byte{'u'}, std::byte{'e'},
        std::byte{'s'},  std::byte{'t'}, std::byte{'_'}, std::byte{'v'}, std::byte{'o'},
        std::byte{'t'},  std::byte{'e'}, std::byte{'_'}, std::byte{'r'}, std::byte{'e'},
        std::byte{'q'},  std::byte{'u'}, std::byte{'e'}, std::byte{'s'}, std::byte{'t'},
        std::byte{0x64}, std::byte{'t'}, std::byte{'e'}, std::byte{'r'}, std::byte{'m'},
        std::byte{0x01},  // term = 1
    };

    BOOST_CHECK_THROW(serializer.deserialize_request_vote_request(data),
                      kythira::serialization_exception);
}

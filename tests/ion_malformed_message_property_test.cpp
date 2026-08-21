// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE IonMalformedMessagePropertyTest
#include <boost/test/unit_test.hpp>

#include <raft/ion_serializer.hpp>

#include <random>
#include <string>
#include <string_view>
#include <vector>
#include <cstddef>

namespace {
constexpr std::size_t property_test_iterations = 100;
constexpr std::size_t max_random_bytes = 1000;

auto generate_random_bytes(std::mt19937& rng, std::size_t size) -> std::vector<std::byte> {
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::vector<std::byte> data;
    data.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        data.push_back(static_cast<std::byte>(byte_dist(rng)));
    }
    return data;
}

// Text Ion is a JSON superset, so malformed-but-well-formed-Ion vectors are
// easiest to author as text and hand to the encoding-agnostic reader as bytes.
auto text_ion(std::string_view s) -> std::vector<std::byte> {
    std::vector<std::byte> out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    return out;
}
}  // namespace

/**
 * Feature: ion-rpc-serializer, Property 4: Malformed input is rejected, never crashes
 * Validates: Requirements 5.1, 5.5, 5.6, 8.3
 *
 * Property: For any random byte sequence, every deserialize_* method rejects it
 * (with serialization_exception or another std::exception) rather than reading
 * out of bounds, crashing, hanging, or returning a partially-populated value.
 * The rejection rate for random bytes must be >= 95%.
 */
BOOST_AUTO_TEST_CASE(property_random_bytes_rejected) {
    std::mt19937 rng(std::random_device{}());
    kythira::ion_rpc_serializer<std::vector<std::byte>> serializer;

    std::uniform_int_distribution<std::size_t> size_dist(0, max_random_bytes);

    std::size_t handled = 0;
    std::size_t rejected = 0;
    std::size_t attempts = 0;

    auto attempt = [&](auto&& fn, const std::vector<std::byte>& data) {
        ++attempts;
        try {
            [[maybe_unused]] auto result = fn(data);
            // Vanishingly unlikely: random bytes decoded as a well-formed message
            // of exactly this type. Not a crash, so it counts as handled.
            ++handled;
        } catch (const kythira::serialization_exception&) {
            ++handled;
            ++rejected;
        } catch (const std::exception&) {
            ++handled;
            ++rejected;
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

    // No call may crash; all must be handled.
    BOOST_CHECK_EQUAL(handled, attempts);
    // >= 95% must be actively rejected (Requirement 8.3).
    BOOST_TEST(rejected * 100 >= attempts * 95);
}

/**
 * Feature: ion-rpc-serializer, Property 4: Malformed input is rejected, never crashes
 * Validates: Requirements 5.1, 5.6, 8.3
 *
 * Property: For any valid message truncated at an arbitrary byte offset, the
 * matching deserialize_* method rejects the truncated buffer rather than reading
 * past its end, for both binary and text encodings.
 */
BOOST_AUTO_TEST_CASE(property_truncated_message_rejected) {
    std::mt19937 rng(std::random_device{}());

    std::size_t rejection_count = 0;
    std::size_t truncation_count = 0;

    for (auto encoding : {kythira::ion_encoding::binary, kythira::ion_encoding::text}) {
        kythira::ion_rpc_serializer<std::vector<std::byte>> serializer(encoding);
        for (std::size_t i = 0; i < property_test_iterations; ++i) {
            kythira::append_entries_request<> req;
            req._term = 5;
            req._leader_id = 2;
            req._prev_log_index = 10;
            req._prev_log_term = 4;
            req._leader_commit = 8;
            kythira::log_entry<> entry;
            entry._term = 5;
            entry._index = 11;
            entry._command =
                std::vector<std::byte>{std::byte{0xAB}, std::byte{0xCD}, std::byte{0xEF}};
            req._entries.push_back(entry);

            auto full = serializer.serialize(req);
            if (full.size() < 2) {
                continue;
            }
            std::uniform_int_distribution<std::size_t> cut_dist(1, full.size() - 1);
            std::size_t cut = cut_dist(rng);
            std::vector<std::byte> truncated(full.begin(),
                                             full.begin() + static_cast<std::ptrdiff_t>(cut));
            ++truncation_count;

            try {
                [[maybe_unused]] auto result =
                    serializer.deserialize_append_entries_request(truncated);
                // A prefix should never decode to a complete, valid message.
            } catch (const std::exception&) {
                ++rejection_count;
            }
        }
    }

    BOOST_TEST_MESSAGE("Truncated message rejection: " << rejection_count << "/" << truncation_count
                                                       << " rejected");
    BOOST_CHECK_EQUAL(rejection_count, truncation_count);
}

/**
 * Feature: ion-rpc-serializer, Property 4: Malformed input is rejected, never crashes
 * Validates: Requirements 5.2
 *
 * Property: Well-formed Ion whose top-level annotation does not match the
 * expected message type is rejected (the annotation-mismatch analogue of
 * json_rpc_serializer's "type" field check).
 */
BOOST_AUTO_TEST_CASE(property_wrong_annotation_rejected) {
    kythira::ion_rpc_serializer<std::vector<std::byte>> serializer;

    // A valid request_vote_response encoding fed to every other deserialize_*
    // must be rejected for carrying the wrong annotation.
    auto encoded = serializer.serialize(kythira::request_vote_response<>{5, true});

    std::size_t rejected = 0;
    std::size_t attempts = 0;
    auto expect_throw = [&](auto&& fn) {
        ++attempts;
        try {
            [[maybe_unused]] auto result = fn(encoded);
        } catch (const kythira::serialization_exception&) {
            ++rejected;
        } catch (const std::exception&) {
            ++rejected;
        }
    };

    expect_throw([&](const auto& d) { return serializer.deserialize_request_vote_request(d); });
    expect_throw([&](const auto& d) { return serializer.deserialize_append_entries_request(d); });
    expect_throw(
        [&](const auto& d) { return serializer.deserialize_install_snapshot_response(d); });
    expect_throw([&](const auto& d) { return serializer.deserialize_cluster_join_response(d); });
    expect_throw(
        [&](const auto& d) { return serializer.deserialize_fetch_log_entries_request(d); });

    BOOST_CHECK_EQUAL(rejected, attempts);
}

/**
 * Feature: ion-rpc-serializer, Property 4: Malformed input is rejected, never crashes
 * Validates: Requirements 5.2, 5.3, 5.4
 *
 * Property: Well-formed Ion that is missing a required field, carries a field of
 * the wrong Ion type, or whose top-level value is not a struct, is rejected.
 * Authored as text Ion (a JSON superset) and read via the encoding-agnostic path.
 */
BOOST_AUTO_TEST_CASE(property_structural_defects_rejected) {
    kythira::ion_rpc_serializer<std::vector<std::byte>> serializer;

    // Non-struct top-level values (Requirement 5.4).
    const std::vector<std::vector<std::byte>> non_struct = {
        text_ion("42"),                        // bare int
        text_ion("[1, 2, 3]"),                 // list
        text_ion("null"),                      // null
        text_ion("request_vote_request::42"),  // annotation on a non-struct
        text_ion("\"a string\""),              // string
    };
    for (const auto& data : non_struct) {
        BOOST_CHECK_THROW(serializer.deserialize_request_vote_request(data),
                          kythira::serialization_exception);
    }

    // Missing required field (Requirement 5.3): correct annotation, only term.
    BOOST_CHECK_THROW(
        serializer.deserialize_request_vote_request(text_ion("request_vote_request::{term:1}")),
        kythira::serialization_exception);

    // Wrong Ion type for a field (Requirement 5.3): term is a string, not int.
    BOOST_CHECK_THROW(serializer.deserialize_request_vote_request(
                          text_ion("request_vote_request::{term:\"x\",candidate_id:2,"
                                   "last_log_index:3,last_log_term:4}")),
                      kythira::serialization_exception);

    // Wrong Ion type for a bool field: vote_granted is an int, not bool.
    BOOST_CHECK_THROW(serializer.deserialize_request_vote_response(
                          text_ion("request_vote_response::{term:1,vote_granted:0}")),
                      kythira::serialization_exception);

    // Missing annotation entirely (Requirement 5.2): a bare struct.
    BOOST_CHECK_THROW(serializer.deserialize_request_vote_request(
                          text_ion("{term:1,candidate_id:2,last_log_index:3,last_log_term:4}")),
                      kythira::serialization_exception);
}

/**
 * Feature: ion-rpc-serializer, Property 4: Malformed input is rejected, never crashes
 * Validates: Requirements 5.3
 *
 * Property: A numeric value out of range for its target C++ type is rejected
 * rather than silently truncated. term deserializes into std::uint64_t, so a
 * negative Ion int must be rejected.
 */
BOOST_AUTO_TEST_CASE(property_out_of_range_numeric_rejected) {
    kythira::ion_rpc_serializer<std::vector<std::byte>> serializer;

    // Negative term into an unsigned target.
    BOOST_CHECK_THROW(serializer.deserialize_request_vote_request(
                          text_ion("request_vote_request::{term:-1,candidate_id:2,"
                                   "last_log_index:3,last_log_term:4}")),
                      kythira::serialization_exception);
}

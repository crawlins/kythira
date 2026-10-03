// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE RpcMalformedMessagePropertyTest
#include <boost/test/unit_test.hpp>

#include <raft/json_serializer.hpp>

#include <random>
#include <string>
#include <vector>
#include <cstddef>

namespace {
constexpr std::size_t property_test_iterations = 100;
constexpr std::size_t max_random_bytes = 1000;
}

// Helper to generate random byte sequence
auto generate_random_bytes(std::mt19937& rng, std::size_t size) -> std::vector<std::byte> {
    std::uniform_int_distribution<int> byte_dist(0, 255);

    std::vector<std::byte> data;
    data.reserve(size);

    for (std::size_t i = 0; i < size; ++i) {
        data.push_back(static_cast<std::byte>(byte_dist(rng)));
    }

    return data;
}

// Helper to convert string to bytes
auto string_to_bytes(const std::string& str) -> std::vector<std::byte> {
    std::vector<std::byte> result;
    result.reserve(str.size());
    for (char c : str) {
        result.push_back(static_cast<std::byte>(c));
    }
    return result;
}

/**
 * Feature: raft-consensus, Property 7: Malformed Message Rejection
 * Validates: Requirements 2.6
 *
 * Property: For any byte sequence that does not represent a valid RequestVote
 * request, the deserializer rejects it with an appropriate error.
 */
BOOST_AUTO_TEST_CASE(property_malformed_request_vote_request_rejection) {
    std::mt19937 rng(std::random_device{}());
    kythira::json_rpc_serializer<std::vector<std::byte>> serializer;

    std::size_t rejection_count = 0;
    std::uniform_int_distribution<std::size_t> size_dist(1, max_random_bytes);

    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        // Generate random byte sequence
        std::size_t size = size_dist(rng);
        auto random_data = generate_random_bytes(rng, size);

        try {
            // Attempt to deserialize - should throw
            auto result = serializer.deserialize_request_vote_request(random_data);
            // If we get here, deserialization succeeded (unexpected for random data)
        } catch (const kythira::serialization_exception&) {
            // Expected - malformed data rejected. Any other exception type
            // escapes and fails the test (spec json-serializer-input-validation,
            // Requirement 3.4).
            ++rejection_count;
        }
    }

    BOOST_TEST_MESSAGE("Malformed RequestVote request rejection: "
                       << rejection_count << "/" << property_test_iterations << " rejected");

    // We expect most random byte sequences to be rejected
    // Allow a small margin for the unlikely case of valid JSON
    BOOST_CHECK_GE(rejection_count, property_test_iterations * 95 / 100);
}

/**
 * Feature: raft-consensus, Property 7: Malformed Message Rejection
 * Validates: Requirements 2.6
 *
 * Property: For any JSON with incorrect message type, the deserializer
 * rejects it with an appropriate error.
 */
BOOST_AUTO_TEST_CASE(property_wrong_message_type_rejection) {
    kythira::json_rpc_serializer<std::vector<std::byte>> serializer;

    std::size_t rejection_count = 0;

    // Test cases with wrong message types
    std::vector<std::string> wrong_type_messages = {
        R"({"type":"wrong_type","term":1,"candidate_id":1,"last_log_index":1,"last_log_term":1})",
        R"({"type":"append_entries_request","term":1,"candidate_id":1,"last_log_index":1,"last_log_term":1})",
        R"({"type":"request_vote_response","term":1,"candidate_id":1,"last_log_index":1,"last_log_term":1})",
        R"({"type":"","term":1,"candidate_id":1,"last_log_index":1,"last_log_term":1})",
    };

    for (const auto& msg : wrong_type_messages) {
        auto data = string_to_bytes(msg);

        try {
            auto result = serializer.deserialize_request_vote_request(data);
            // Should not reach here
        } catch (const kythira::serialization_exception&) {
            ++rejection_count;
        }
    }

    BOOST_TEST_MESSAGE("Wrong message type rejection: "
                       << rejection_count << "/" << wrong_type_messages.size() << " rejected");
    BOOST_CHECK_EQUAL(rejection_count, wrong_type_messages.size());
}

/**
 * Feature: raft-consensus, Property 7: Malformed Message Rejection
 * Validates: Requirements 2.6
 *
 * Property: For any JSON with missing required fields, the deserializer
 * rejects it with an appropriate error.
 */
BOOST_AUTO_TEST_CASE(property_missing_fields_rejection) {
    kythira::json_rpc_serializer<std::vector<std::byte>> serializer;

    std::size_t rejection_count = 0;

    // Test cases with missing required fields
    std::vector<std::string> missing_field_messages = {
        R"({"type":"request_vote_request"})",
        R"({"type":"request_vote_request","term":1})",
        R"({"type":"request_vote_request","term":1,"candidate_id":1})",
        R"({"type":"request_vote_request","term":1,"candidate_id":1,"last_log_index":1})",
        R"({"type":"request_vote_request","candidate_id":1,"last_log_index":1,"last_log_term":1})",
    };

    for (const auto& msg : missing_field_messages) {
        auto data = string_to_bytes(msg);

        try {
            auto result = serializer.deserialize_request_vote_request(data);
            // Should not reach here
        } catch (const kythira::serialization_exception&) {
            ++rejection_count;
        }
    }

    BOOST_TEST_MESSAGE("Missing fields rejection: "
                       << rejection_count << "/" << missing_field_messages.size() << " rejected");
    BOOST_CHECK_EQUAL(rejection_count, missing_field_messages.size());
}

/**
 * Feature: raft-consensus, Property 7: Malformed Message Rejection
 * Validates: Requirements 2.6
 *
 * Property: For any invalid JSON syntax, the deserializer rejects it
 * with an appropriate error.
 */
BOOST_AUTO_TEST_CASE(property_invalid_json_syntax_rejection) {
    kythira::json_rpc_serializer<std::vector<std::byte>> serializer;

    std::size_t rejection_count = 0;

    // Test cases with invalid JSON syntax
    std::vector<std::string> invalid_json_messages = {
        R"({invalid json})",
        R"({"type":"request_vote_request",})",
        R"({"type":"request_vote_request")",
        R"(not json at all)",
        R"({"type":"request_vote_request","term":"not a number","candidate_id":1,"last_log_index":1,"last_log_term":1})",
        R"()",
        R"(null)",
        R"([])",
    };

    for (const auto& msg : invalid_json_messages) {
        auto data = string_to_bytes(msg);

        try {
            auto result = serializer.deserialize_request_vote_request(data);
            // Should not reach here
        } catch (const kythira::serialization_exception&) {
            ++rejection_count;
        }
    }

    BOOST_TEST_MESSAGE("Invalid JSON syntax rejection: "
                       << rejection_count << "/" << invalid_json_messages.size() << " rejected");
    BOOST_CHECK_EQUAL(rejection_count, invalid_json_messages.size());
}

/**
 * Feature: raft-consensus, Property 7: Malformed Message Rejection
 * Validates: Requirements 2.6
 *
 * Property: For any malformed AppendEntries request, the deserializer
 * rejects it with an appropriate error.
 */
BOOST_AUTO_TEST_CASE(property_malformed_append_entries_request_rejection) {
    std::mt19937 rng(std::random_device{}());
    kythira::json_rpc_serializer<std::vector<std::byte>> serializer;

    std::size_t rejection_count = 0;
    std::uniform_int_distribution<std::size_t> size_dist(1, max_random_bytes);

    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        // Generate random byte sequence
        std::size_t size = size_dist(rng);
        auto random_data = generate_random_bytes(rng, size);

        try {
            auto result = serializer.deserialize_append_entries_request(random_data);
        } catch (const kythira::serialization_exception&) {
            ++rejection_count;
        }
    }

    BOOST_TEST_MESSAGE("Malformed AppendEntries request rejection: "
                       << rejection_count << "/" << property_test_iterations << " rejected");
    BOOST_CHECK_GE(rejection_count, property_test_iterations * 95 / 100);
}

/**
 * Feature: raft-consensus, Property 7: Malformed Message Rejection
 * Validates: Requirements 2.6
 *
 * Property: For any malformed InstallSnapshot request, the deserializer
 * rejects it with an appropriate error.
 */
BOOST_AUTO_TEST_CASE(property_malformed_install_snapshot_request_rejection) {
    std::mt19937 rng(std::random_device{}());
    kythira::json_rpc_serializer<std::vector<std::byte>> serializer;

    std::size_t rejection_count = 0;
    std::uniform_int_distribution<std::size_t> size_dist(1, max_random_bytes);

    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        // Generate random byte sequence
        std::size_t size = size_dist(rng);
        auto random_data = generate_random_bytes(rng, size);

        try {
            auto result = serializer.deserialize_install_snapshot_request(random_data);
        } catch (const kythira::serialization_exception&) {
            ++rejection_count;
        }
    }

    BOOST_TEST_MESSAGE("Malformed InstallSnapshot request rejection: "
                       << rejection_count << "/" << property_test_iterations << " rejected");
    BOOST_CHECK_GE(rejection_count, property_test_iterations * 95 / 100);
}

/**
 * Feature: raft-consensus, Property 7: Malformed Message Rejection
 * Validates: Requirements 2.6
 *
 * Property: For any AppendEntries request with invalid entry data,
 * the deserializer rejects it with an appropriate error.
 */
BOOST_AUTO_TEST_CASE(property_invalid_entry_data_rejection) {
    kythira::json_rpc_serializer<std::vector<std::byte>> serializer;

    std::size_t rejection_count = 0;

    // Test cases with invalid entry data
    std::vector<std::string> invalid_entry_messages = {
        R"({"type":"append_entries_request","term":1,"leader_id":1,"prev_log_index":1,"prev_log_term":1,"leader_commit":1,"entries":[{"term":1,"index":1}]})",
        R"({"type":"append_entries_request","term":1,"leader_id":1,"prev_log_index":1,"prev_log_term":1,"leader_commit":1,"entries":"not an array"})",
        R"({"type":"append_entries_request","term":1,"leader_id":1,"prev_log_index":1,"prev_log_term":1,"leader_commit":1,"entries":[{"term":"not a number","index":1,"command":"AQID"}]})",
    };

    for (const auto& msg : invalid_entry_messages) {
        auto data = string_to_bytes(msg);

        try {
            auto result = serializer.deserialize_append_entries_request(data);
            // Should not reach here
        } catch (const kythira::serialization_exception&) {
            ++rejection_count;
        }
    }

    BOOST_TEST_MESSAGE("Invalid entry data rejection: "
                       << rejection_count << "/" << invalid_entry_messages.size() << " rejected");
    BOOST_CHECK_EQUAL(rejection_count, invalid_entry_messages.size());
}

namespace {

// Base AppendEntries / InstallSnapshot texts with one field left as a
// placeholder, so each malformed case differs from a valid message in exactly
// one place.
auto append_entries_with(const std::string& top, const std::string& entry) -> std::string {
    return R"({"type":"append_entries_request","group_id":0,)" + top +
           R"("leader_id":1,"prev_log_index":1,"prev_log_term":1,"leader_commit":1,"entries":[{)" +
           entry + "}]}";
}

auto install_snapshot_with(const std::string& fields) -> std::string {
    return R"({"type":"install_snapshot_request","group_id":0,"term":1,"leader_id":1,)"
           R"("last_included_index":1,"last_included_term":1,"done":true,)" +
           fields + "}";
}

template<typename Decode>
auto expect_serialization_exception(const std::vector<std::string>& messages, Decode decode)
    -> void {
    for (const auto& msg : messages) {
        BOOST_TEST_CONTEXT(msg) {
            BOOST_CHECK_THROW(decode(string_to_bytes(msg)), kythira::serialization_exception);
        }
    }
}

}  // namespace

/**
 * Spec json-serializer-input-validation, Requirements 1.1-1.6 and 6.2: numbers
 * that do not fit their field are rejected instead of wrapped or truncated.
 */
BOOST_AUTO_TEST_CASE(property_out_of_range_numeric_rejected) {
    kythira::json_rpc_serializer<std::vector<std::byte>> serializer;
    const std::string ok_entry = R"("term":1,"index":1,"command":"AQID","entry_type":0)";

    expect_serialization_exception(
        {
            // Negative into unsigned: used to decode as 2^64-1.
            append_entries_with(R"("term":-1,)", ok_entry),
            append_entries_with(R"("term":1,)", R"("term":-1,"index":1,"command":"AQID")"),
            // entry_type above std::uint8_t: used to truncate 300 to 44.
            append_entries_with(R"("term":1,)",
                                R"("term":1,"index":1,"command":"AQID","entry_type":300)"),
            append_entries_with(R"("term":1,)",
                                R"("term":1,"index":1,"command":"AQID","entry_type":-1)"),
            // Doubles, even integral ones.
            append_entries_with(R"("term":1.0,)", ok_entry),
            append_entries_with(R"("term":1e3,)", ok_entry),
            // Above 2^64-1: Boost.JSON parses this as a double.
            append_entries_with(R"("term":18446744073709551616,)", ok_entry),
            // Wrong kinds.
            append_entries_with(R"("term":"1",)", ok_entry),
            append_entries_with(R"("term":true,)", ok_entry),
            append_entries_with(R"("term":null,)", ok_entry),
            append_entries_with(R"("term":1,)", R"("term":1,"index":1,"command":7)"),
        },
        [&](const auto& d) { return serializer.deserialize_append_entries_request(d); });

    expect_serialization_exception(
        {
            install_snapshot_with(R"("offset":-1,"data":"")"),
            install_snapshot_with(R"("offset":1.5,"data":"")"),
        },
        [&](const auto& d) { return serializer.deserialize_install_snapshot_request(d); });

    expect_serialization_exception(
        {
            // Negative integral group_id.
            R"({"type":"request_vote_request","group_id":-1,"term":1,"candidate_id":1,"last_log_index":1,"last_log_term":1})",
            R"({"type":"request_vote_request","group_id":1.0,"term":1,"candidate_id":1,"last_log_index":1,"last_log_term":1})",
            // A string where an integer node id is expected.
            R"({"type":"request_vote_request","term":1,"candidate_id":"n1","last_log_index":1,"last_log_term":1})",
            R"({"type":"request_vote_request","term":1,"candidate_id":-1,"last_log_index":1,"last_log_term":1})",
        },
        [&](const auto& d) { return serializer.deserialize_request_vote_request(d); });

    expect_serialization_exception(
        {R"({"type":"request_vote_response","term":1,"vote_granted":1})"},
        [&](const auto& d) { return serializer.deserialize_request_vote_response(d); });

    expect_serialization_exception(
        {R"({"type":"append_entries_response","term":1,"success":true,"conflict_index":-1})",
         R"({"type":"append_entries_response","term":1,"success":true,"conflict_term":"x"})"},
        [&](const auto& d) { return serializer.deserialize_append_entries_response(d); });

    expect_serialization_exception(
        {
            R"({"type":"fetch_log_entries_response","responder_id":-1,"available":true,"prev_log_term":0,"entries":[]})",
        },
        [&](const auto& d) { return serializer.deserialize_fetch_log_entries_response(d); });

    expect_serialization_exception(
        {
            R"({"type":"cluster_join_response","accepted":false,"redirect_node_id":-5,"redirect_address":"a"})",
            R"({"type":"cluster_join_response","accepted":false,"redirect_node_id":5})",
        },
        [&](const auto& d) { return serializer.deserialize_cluster_join_response(d); });
}

/**
 * Spec json-serializer-input-validation, Requirements 2.1-2.5 and 6.3: a
 * damaged `command` or `data` field is rejected, never truncated.
 */
BOOST_AUTO_TEST_CASE(property_bad_base64_rejected) {
    kythira::json_rpc_serializer<std::vector<std::byte>> serializer;

    const std::vector<std::string> bad = {
        "AQID!!!!",  // invalid character (used to decode to three bytes)
        "AQ==AQID",  // '=' mid-string (used to decode to one byte)
        "AQI",       // length not a multiple of 4
        "AQIDA",     // length not a multiple of 4
        "A===",      // too much padding
        "====",      // padding only
        "AB=C",      // '=' followed by data
        "AR==",      // non-zero pad bits (canonical form is "AQ==")
        "AQJ=",      // non-zero pad bits (canonical form is "AQI=")
        "AQ\nID",    // whitespace is not part of the alphabet
        "AQ-_",      // URL-safe alphabet is not accepted
    };

    std::vector<std::string> ae_messages;
    std::vector<std::string> is_messages;
    for (const auto& b : bad) {
        const auto escaped = b == "AQ\nID" ? std::string("AQ\\nID") : b;
        ae_messages.push_back(append_entries_with(
            R"("term":1,)", R"("term":1,"index":1,"command":")" + escaped + R"(")"));
        is_messages.push_back(install_snapshot_with(R"("offset":0,"data":")" + escaped + R"(")"));
    }

    expect_serialization_exception(ae_messages, [&](const auto& d) {
        return serializer.deserialize_append_entries_request(d);
    });
    expect_serialization_exception(is_messages, [&](const auto& d) {
        return serializer.deserialize_install_snapshot_request(d);
    });

    // Positive cases: the empty string is no bytes, and canonical padding works.
    const auto empty = serializer.deserialize_install_snapshot_request(
        string_to_bytes(install_snapshot_with(R"("offset":0,"data":"")")));
    BOOST_TEST(empty.data().empty());

    const auto two = serializer.deserialize_append_entries_request(string_to_bytes(
        append_entries_with(R"("term":1,)", R"("term":1,"index":1,"command":"AQI=")")));
    BOOST_REQUIRE_EQUAL(two.entries().size(), 1U);
    BOOST_TEST((two.entries()[0].command() == std::vector<std::byte>{std::byte{1}, std::byte{2}}));
}

/**
 * Spec json-serializer-input-validation, Requirements 3.1-3.3 and 3.5: every
 * structural defect is a serialization_exception, and optional fields keep
 * their defaults.
 */
BOOST_AUTO_TEST_CASE(property_structural_defects_rejected_and_optionals_default) {
    kythira::json_rpc_serializer<std::vector<std::byte>> serializer;

    expect_serialization_exception(
        {
            R"("just a string")",
            R"(42)",
            R"({"term":1})",  // no "type"
            R"({"type":7})",  // "type" of the wrong kind
            append_entries_with(R"("term":1,)", R"("term":1,"index":1)"),  // no command
            R"({"type":"append_entries_request","term":1,"leader_id":1,"prev_log_index":1,"prev_log_term":1,"leader_commit":1,"entries":[7]})",
            R"({"type":"append_entries_request","term":1,"leader_id":1,"prev_log_index":1,"prev_log_term":1,"leader_commit":1})",
        },
        [&](const auto& d) { return serializer.deserialize_append_entries_request(d); });

    // Absent optional fields decode to their defaults; unknown keys are ignored.
    const auto ae = serializer.deserialize_append_entries_request(string_to_bytes(
        R"({"type":"append_entries_request","term":3,"leader_id":1,"prev_log_index":1,"prev_log_term":1,"leader_commit":1,"future_field":[1,2],"entries":[{"term":1,"index":2,"command":""}]})"));
    BOOST_TEST(ae.group_id() == 0U);
    BOOST_TEST(ae.term() == 3U);
    BOOST_REQUIRE_EQUAL(ae.entries().size(), 1U);
    BOOST_TEST((ae.entries()[0].type() == kythira::entry_type::normal));

    const auto aer = serializer.deserialize_append_entries_response(
        string_to_bytes(R"({"type":"append_entries_response","term":1,"success":false})"));
    BOOST_TEST(!aer.conflict_index().has_value());
    BOOST_TEST(!aer.conflict_term().has_value());

    const auto cj = serializer.deserialize_cluster_join_response(
        string_to_bytes(R"({"type":"cluster_join_response","accepted":true})"));
    BOOST_TEST(!cj.redirect.has_value());
}

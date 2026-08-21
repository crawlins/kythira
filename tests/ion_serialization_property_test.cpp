// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE IonSerializationPropertyTest
#include <boost/test/unit_test.hpp>

#include <raft/ion_serializer.hpp>
#include <raft/coap_utils.hpp>

#include <array>
#include <optional>
#include <random>
#include <string>
#include <vector>
#include <cstddef>

namespace {
constexpr std::size_t property_test_iterations = 100;
constexpr std::uint64_t max_term = 1000000;
constexpr std::uint64_t max_index = 1000000;
constexpr std::uint64_t max_node_id = 10000;
constexpr std::size_t max_entries = 10;
constexpr std::size_t max_command_size = 100;
constexpr std::size_t max_snapshot_data_size = 1000;

// Both Ion encodings are exercised by every round-trip property, per Property 1
// (round-trip fidelity per message type, per encoding) and Requirement 8.2.
constexpr std::array<kythira::ion_encoding, 2> encodings = {kythira::ion_encoding::binary,
                                                            kythira::ion_encoding::text};
}  // namespace

// The input generators below intentionally mirror the ones in
// rpc_serialization_property_test.cpp / cbor_serialization_property_test.cpp,
// parameterized here over the Ion serializer, per the design's "reuse the
// input-generation helpers" guidance.
auto generate_random_term(std::mt19937& rng) -> std::uint64_t {
    std::uniform_int_distribution<std::uint64_t> dist(1, max_term);
    return dist(rng);
}

auto generate_random_log_index(std::mt19937& rng) -> std::uint64_t {
    std::uniform_int_distribution<std::uint64_t> dist(1, max_index);
    return dist(rng);
}

auto generate_random_node_id(std::mt19937& rng) -> std::uint64_t {
    std::uniform_int_distribution<std::uint64_t> dist(1, max_node_id);
    return dist(rng);
}

auto generate_random_string_node_id(std::mt19937& rng) -> std::string {
    return "node_" + std::to_string(generate_random_node_id(rng));
}

auto generate_random_command(std::mt19937& rng) -> std::vector<std::byte> {
    std::uniform_int_distribution<std::size_t> size_dist(1, max_command_size);
    std::uniform_int_distribution<int> byte_dist(0, 255);

    std::size_t size = size_dist(rng);
    std::vector<std::byte> command;
    command.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        command.push_back(static_cast<std::byte>(byte_dist(rng)));
    }
    return command;
}

auto generate_random_log_entries(std::mt19937& rng) -> std::vector<kythira::log_entry<>> {
    std::uniform_int_distribution<std::size_t> count_dist(0, max_entries);
    std::uniform_int_distribution<int> type_dist(0, 2);
    std::size_t count = count_dist(rng);

    std::vector<kythira::log_entry<>> entries;
    entries.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        kythira::log_entry<> entry;
        entry._term = generate_random_term(rng);
        entry._index = generate_random_log_index(rng);
        entry._command = generate_random_command(rng);
        entry._type = static_cast<kythira::entry_type>(type_dist(rng));
        entries.push_back(entry);
    }
    return entries;
}

auto generate_random_snapshot_data(std::mt19937& rng) -> std::vector<std::byte> {
    std::uniform_int_distribution<std::size_t> size_dist(1, max_snapshot_data_size);
    std::uniform_int_distribution<int> byte_dist(0, 255);

    std::size_t size = size_dist(rng);
    std::vector<std::byte> data;
    data.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        data.push_back(static_cast<std::byte>(byte_dist(rng)));
    }
    return data;
}

auto log_entries_equal(const kythira::log_entry<>& a, const kythira::log_entry<>& b) -> bool {
    return a.term() == b.term() && a.index() == b.index() && a.command() == b.command() &&
           a.type() == b.type();
}

/**
 * Feature: ion-rpc-serializer, Property 1: Round-trip fidelity (per message type, per encoding)
 * Validates: Requirements 2.7, 4.2, 4.3, 8.2
 *
 * Property: For any valid RequestVote / RequestPreVote request, serializing then
 * deserializing produces an equivalent message with all fields preserved, for
 * both binary and text Ion.
 */
BOOST_AUTO_TEST_CASE(property_request_vote_request_round_trip, *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    std::size_t failures = 0;
    for (auto encoding : encodings) {
        kythira::ion_rpc_serializer<std::vector<std::byte>> serializer(encoding);
        for (std::size_t i = 0; i < property_test_iterations; ++i) {
            kythira::request_vote_request<> original;
            original._term = generate_random_term(rng);
            original._candidate_id = generate_random_node_id(rng);
            original._last_log_index = generate_random_log_index(rng);
            original._last_log_term = generate_random_term(rng);

            kythira::request_pre_vote_request<> pv_original;
            pv_original._term = generate_random_term(rng);
            pv_original._candidate_id = generate_random_node_id(rng);
            pv_original._last_log_index = generate_random_log_index(rng);
            pv_original._last_log_term = generate_random_term(rng);

            try {
                auto d =
                    serializer.deserialize_request_vote_request(serializer.serialize(original));
                auto pv = serializer.deserialize_request_pre_vote_request(
                    serializer.serialize(pv_original));

                if (d.term() != original.term() || d.candidate_id() != original.candidate_id() ||
                    d.last_log_index() != original.last_log_index() ||
                    d.last_log_term() != original.last_log_term() ||
                    pv.term() != pv_original.term() ||
                    pv.candidate_id() != pv_original.candidate_id() ||
                    pv.last_log_index() != pv_original.last_log_index() ||
                    pv.last_log_term() != pv_original.last_log_term()) {
                    ++failures;
                }
            } catch (const std::exception& e) {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Exception: " << e.what());
            }
        }
    }
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: ion-rpc-serializer, Property 1: Round-trip fidelity (per message type, per encoding)
 * Validates: Requirements 2.7, 4.2, 4.3, 8.2
 */
BOOST_AUTO_TEST_CASE(property_request_vote_response_round_trip, *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> bool_dist(0, 1);
    std::size_t failures = 0;
    for (auto encoding : encodings) {
        kythira::ion_rpc_serializer<std::vector<std::byte>> serializer(encoding);
        for (std::size_t i = 0; i < property_test_iterations; ++i) {
            kythira::request_vote_response<> original;
            original._term = generate_random_term(rng);
            original._vote_granted = bool_dist(rng) == 1;

            kythira::request_pre_vote_response<> pv_original;
            pv_original._term = generate_random_term(rng);
            pv_original._vote_granted = bool_dist(rng) == 1;

            try {
                auto d =
                    serializer.deserialize_request_vote_response(serializer.serialize(original));
                auto pv = serializer.deserialize_request_pre_vote_response(
                    serializer.serialize(pv_original));

                if (d.term() != original.term() || d.vote_granted() != original.vote_granted() ||
                    pv.term() != pv_original.term() ||
                    pv.vote_granted() != pv_original.vote_granted()) {
                    ++failures;
                }
            } catch (const std::exception& e) {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Exception: " << e.what());
            }
        }
    }
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: ion-rpc-serializer, Property 1: Round-trip fidelity (per message type, per encoding)
 * Validates: Requirements 2.4, 2.7, 4.2, 4.3, 8.2
 *
 * Property: For any valid AppendEntries request (including the empty-entries()
 * edge case and every entry_type), serializing then deserializing preserves
 * every field and every entry, for both encodings.
 */
BOOST_AUTO_TEST_CASE(property_append_entries_request_round_trip, *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    std::size_t failures = 0;
    for (auto encoding : encodings) {
        kythira::ion_rpc_serializer<std::vector<std::byte>> serializer(encoding);
        for (std::size_t i = 0; i < property_test_iterations; ++i) {
            kythira::append_entries_request<> original;
            original._term = generate_random_term(rng);
            original._leader_id = generate_random_node_id(rng);
            original._prev_log_index = generate_random_log_index(rng);
            original._prev_log_term = generate_random_term(rng);
            // First iteration explicitly exercises the empty-entries() edge case
            // (must round-trip to an empty, not omitted or null, Ion list).
            original._entries =
                (i == 0) ? std::vector<kythira::log_entry<>>{} : generate_random_log_entries(rng);
            original._leader_commit = generate_random_log_index(rng);

            try {
                auto deserialized =
                    serializer.deserialize_append_entries_request(serializer.serialize(original));

                bool entries_match = deserialized.entries().size() == original.entries().size();
                for (std::size_t j = 0; entries_match && j < original.entries().size(); ++j) {
                    if (!log_entries_equal(deserialized.entries()[j], original.entries()[j])) {
                        entries_match = false;
                    }
                }

                if (deserialized.term() != original.term() ||
                    deserialized.leader_id() != original.leader_id() ||
                    deserialized.prev_log_index() != original.prev_log_index() ||
                    deserialized.prev_log_term() != original.prev_log_term() ||
                    deserialized.leader_commit() != original.leader_commit() || !entries_match) {
                    ++failures;
                }
            } catch (const std::exception& e) {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Exception: " << e.what());
            }
        }
    }
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: ion-rpc-serializer, Property 1: Round-trip fidelity (per message type, per encoding)
 * Validates: Requirements 2.5, 2.7, 4.2, 4.3, 8.2
 *
 * Property: For any valid AppendEntries response, serializing then deserializing
 * preserves every field, including the absent-conflict_index / absent-conflict_term
 * edge cases (omitted Ion struct fields deserialize back to std::nullopt).
 */
BOOST_AUTO_TEST_CASE(property_append_entries_response_round_trip, *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> bool_dist(0, 1);
    std::size_t failures = 0;
    for (auto encoding : encodings) {
        kythira::ion_rpc_serializer<std::vector<std::byte>> serializer(encoding);
        for (std::size_t i = 0; i < property_test_iterations; ++i) {
            kythira::append_entries_response<> original;
            original._term = generate_random_term(rng);
            original._success = bool_dist(rng) == 1;
            if (bool_dist(rng) == 1) {
                original._conflict_index = generate_random_log_index(rng);
            }
            if (bool_dist(rng) == 1) {
                original._conflict_term = generate_random_term(rng);
            }

            try {
                auto deserialized =
                    serializer.deserialize_append_entries_response(serializer.serialize(original));

                if (deserialized.term() != original.term() ||
                    deserialized.success() != original.success() ||
                    deserialized.conflict_index() != original.conflict_index() ||
                    deserialized.conflict_term() != original.conflict_term()) {
                    ++failures;
                }
            } catch (const std::exception& e) {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Exception: " << e.what());
            }
        }
    }
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: ion-rpc-serializer, Property 1: Round-trip fidelity (per message type, per encoding)
 * Validates: Requirements 2.5, 2.7, 3.2, 4.2, 4.3, 8.2
 *
 * Property: For any valid InstallSnapshot request, serializing then
 * deserializing preserves every field, including the raw byte data() carried as
 * a native Ion blob.
 */
BOOST_AUTO_TEST_CASE(property_install_snapshot_request_round_trip, *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<std::size_t> offset_dist(0, 1000000);
    std::uniform_int_distribution<int> bool_dist(0, 1);
    std::size_t failures = 0;
    for (auto encoding : encodings) {
        kythira::ion_rpc_serializer<std::vector<std::byte>> serializer(encoding);
        for (std::size_t i = 0; i < property_test_iterations; ++i) {
            kythira::install_snapshot_request<> original;
            original._term = generate_random_term(rng);
            original._leader_id = generate_random_node_id(rng);
            original._last_included_index = generate_random_log_index(rng);
            original._last_included_term = generate_random_term(rng);
            original._offset = offset_dist(rng);
            original._data = generate_random_snapshot_data(rng);
            original._done = bool_dist(rng) == 1;

            try {
                auto deserialized =
                    serializer.deserialize_install_snapshot_request(serializer.serialize(original));

                if (deserialized.term() != original.term() ||
                    deserialized.leader_id() != original.leader_id() ||
                    deserialized.last_included_index() != original.last_included_index() ||
                    deserialized.last_included_term() != original.last_included_term() ||
                    deserialized.offset() != original.offset() ||
                    deserialized.data() != original.data() ||
                    deserialized.done() != original.done()) {
                    ++failures;
                }
            } catch (const std::exception& e) {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Exception: " << e.what());
            }
        }
    }
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: ion-rpc-serializer, Property 1: Round-trip fidelity (per message type, per encoding)
 * Validates: Requirements 2.7, 4.2, 4.3, 8.2
 */
BOOST_AUTO_TEST_CASE(property_install_snapshot_response_round_trip,
                     *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    std::size_t failures = 0;
    for (auto encoding : encodings) {
        kythira::ion_rpc_serializer<std::vector<std::byte>> serializer(encoding);
        for (std::size_t i = 0; i < property_test_iterations; ++i) {
            kythira::install_snapshot_response<> original;
            original._term = generate_random_term(rng);
            try {
                auto deserialized = serializer.deserialize_install_snapshot_response(
                    serializer.serialize(original));
                if (deserialized.term() != original.term()) {
                    ++failures;
                }
            } catch (const std::exception& e) {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Exception: " << e.what());
            }
        }
    }
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: ion-rpc-serializer, Property 1: Round-trip fidelity (per message type, per encoding)
 * Validates: Requirements 2.6, 2.7, 4.2, 4.3, 8.2
 *
 * Property: For any valid ClusterJoin/ClusterLeave request/response (including
 * the absent-redirect edge case and the nested peer_info redirect struct),
 * serializing then deserializing preserves every field, for both encodings.
 */
BOOST_AUTO_TEST_CASE(property_cluster_membership_round_trip, *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> bool_dist(0, 1);
    std::size_t failures = 0;
    for (auto encoding : encodings) {
        kythira::ion_rpc_serializer<std::vector<std::byte>> serializer(encoding);
        for (std::size_t i = 0; i < property_test_iterations; ++i) {
            kythira::cluster_join_request<> join_req;
            join_req.node_id = generate_random_node_id(rng);
            join_req.contact_address = "10.0.0." + std::to_string(generate_random_node_id(rng));

            kythira::cluster_leave_request<> leave_req;
            leave_req.node_id = generate_random_node_id(rng);

            kythira::cluster_join_response<> join_resp;
            join_resp.accepted = bool_dist(rng) == 1;
            if (bool_dist(rng) == 1) {
                join_resp.redirect = kythira::peer_info<std::uint64_t, std::string>{
                    generate_random_node_id(rng), "192.168.0.1"};
            }

            kythira::cluster_leave_response<> leave_resp;
            leave_resp.accepted = bool_dist(rng) == 1;
            if (bool_dist(rng) == 1) {
                leave_resp.redirect = kythira::peer_info<std::uint64_t, std::string>{
                    generate_random_node_id(rng), "192.168.0.2"};
            }

            try {
                auto jq =
                    serializer.deserialize_cluster_join_request(serializer.serialize(join_req));
                auto lq =
                    serializer.deserialize_cluster_leave_request(serializer.serialize(leave_req));
                auto jr =
                    serializer.deserialize_cluster_join_response(serializer.serialize(join_resp));
                auto lr =
                    serializer.deserialize_cluster_leave_response(serializer.serialize(leave_resp));

                bool join_redirect_ok = jr.redirect.has_value() == join_resp.redirect.has_value() &&
                                        (!jr.redirect.has_value() ||
                                         (jr.redirect->node_id == join_resp.redirect->node_id &&
                                          jr.redirect->address == join_resp.redirect->address));
                bool leave_redirect_ok =
                    lr.redirect.has_value() == leave_resp.redirect.has_value() &&
                    (!lr.redirect.has_value() ||
                     (lr.redirect->node_id == leave_resp.redirect->node_id &&
                      lr.redirect->address == leave_resp.redirect->address));

                if (jq.node_id != join_req.node_id ||
                    jq.contact_address != join_req.contact_address ||
                    lq.node_id != leave_req.node_id || jr.accepted != join_resp.accepted ||
                    lr.accepted != leave_resp.accepted || !join_redirect_ok || !leave_redirect_ok) {
                    ++failures;
                }
            } catch (const std::exception& e) {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Exception: " << e.what());
            }
        }
    }
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: ion-rpc-serializer, Property 1: Round-trip fidelity (per message type, per encoding)
 * Validates: Requirements 2.4, 2.7, 4.2, 4.3, 8.2
 */
BOOST_AUTO_TEST_CASE(property_fetch_log_entries_round_trip, *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> bool_dist(0, 1);
    std::size_t failures = 0;
    for (auto encoding : encodings) {
        kythira::ion_rpc_serializer<std::vector<std::byte>> serializer(encoding);
        for (std::size_t i = 0; i < property_test_iterations; ++i) {
            kythira::fetch_log_entries_request<> req;
            req._requester_id = generate_random_node_id(rng);
            req._from_index = generate_random_log_index(rng);
            req._to_index = generate_random_log_index(rng);

            kythira::fetch_log_entries_response<> resp;
            resp._responder_id = generate_random_node_id(rng);
            resp._available = bool_dist(rng) == 1;
            resp._prev_log_term = generate_random_term(rng);
            resp._entries =
                (i == 0) ? std::vector<kythira::log_entry<>>{} : generate_random_log_entries(rng);

            try {
                auto dq =
                    serializer.deserialize_fetch_log_entries_request(serializer.serialize(req));
                auto dr =
                    serializer.deserialize_fetch_log_entries_response(serializer.serialize(resp));

                bool entries_match = dr.entries().size() == resp.entries().size();
                for (std::size_t j = 0; entries_match && j < resp.entries().size(); ++j) {
                    if (!log_entries_equal(dr.entries()[j], resp.entries()[j])) {
                        entries_match = false;
                    }
                }

                if (dq.requester_id() != req.requester_id() ||
                    dq.from_index() != req.from_index() || dq.to_index() != req.to_index() ||
                    dr.responder_id() != resp.responder_id() ||
                    dr.available() != resp.available() ||
                    dr.prev_log_term() != resp.prev_log_term() || !entries_match) {
                    ++failures;
                }
            } catch (const std::exception& e) {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Exception: " << e.what());
            }
        }
    }
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: ion-rpc-serializer, Property 1: Round-trip fidelity (per message type, per encoding)
 * Validates: Requirements 2.7, 8.2
 *
 * Property: For any valid message carrying std::string NodeId/Address values,
 * serializing then deserializing preserves every field (Ion string encoding
 * path rather than int), for both encodings.
 */
BOOST_AUTO_TEST_CASE(property_string_node_id_round_trip, *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    std::size_t failures = 0;
    for (auto encoding : encodings) {
        kythira::ion_rpc_serializer<std::vector<std::byte>> serializer(encoding);
        for (std::size_t i = 0; i < property_test_iterations; ++i) {
            kythira::request_vote_request<std::string> rv;
            rv._term = generate_random_term(rng);
            rv._candidate_id = generate_random_string_node_id(rng);
            rv._last_log_index = generate_random_log_index(rng);
            rv._last_log_term = generate_random_term(rng);

            kythira::cluster_join_request<std::string> join;
            join.node_id = generate_random_string_node_id(rng);
            join.contact_address = "host-" + generate_random_string_node_id(rng);

            try {
                auto rv_d = serializer.deserialize_request_vote_request<std::string>(
                    serializer.serialize(rv));
                auto join_d = serializer.deserialize_cluster_join_request<std::string>(
                    serializer.serialize(join));

                if (rv_d.term() != rv.term() || rv_d.candidate_id() != rv.candidate_id() ||
                    rv_d.last_log_index() != rv.last_log_index() ||
                    rv_d.last_log_term() != rv.last_log_term() || join_d.node_id != join.node_id ||
                    join_d.contact_address != join.contact_address) {
                    ++failures;
                }
            } catch (const std::exception& e) {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Exception: " << e.what());
            }
        }
    }
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: ion-rpc-serializer, Property 2: Binary-vs-text is transparent to the reader
 * Validates: Requirements 4.4, 8.5
 *
 * Property: For any valid message value, serializing with binary and with text
 * Ion and deserializing each through the same deserialize_* call (which does not
 * know which encoding wrote the bytes) both succeed and produce equal results —
 * i.e. a single encoding-agnostic read path accepts either, per the binary
 * version marker.
 */
BOOST_AUTO_TEST_CASE(property_binary_text_transparent_to_reader, *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    kythira::ion_rpc_serializer<std::vector<std::byte>> binary(kythira::ion_encoding::binary);
    kythira::ion_rpc_serializer<std::vector<std::byte>> text(kythira::ion_encoding::text);

    std::size_t failures = 0;
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        kythira::append_entries_request<> original;
        original._term = generate_random_term(rng);
        original._leader_id = generate_random_node_id(rng);
        original._prev_log_index = generate_random_log_index(rng);
        original._prev_log_term = generate_random_term(rng);
        original._entries = generate_random_log_entries(rng);
        original._leader_commit = generate_random_log_index(rng);

        try {
            // Deserialize both encodings through the SAME reader (binary's);
            // its single read path must accept the text bytes too.
            auto from_binary =
                binary.deserialize_append_entries_request(binary.serialize(original));
            auto from_text = binary.deserialize_append_entries_request(text.serialize(original));

            bool entries_match = from_binary.entries().size() == from_text.entries().size();
            for (std::size_t j = 0; entries_match && j < from_binary.entries().size(); ++j) {
                if (!log_entries_equal(from_binary.entries()[j], from_text.entries()[j])) {
                    entries_match = false;
                }
            }

            if (from_binary.term() != from_text.term() ||
                from_binary.leader_id() != from_text.leader_id() ||
                from_binary.prev_log_index() != from_text.prev_log_index() ||
                from_binary.prev_log_term() != from_text.prev_log_term() ||
                from_binary.leader_commit() != from_text.leader_commit() || !entries_match) {
                ++failures;
            }
        } catch (const std::exception& e) {
            ++failures;
            BOOST_TEST_MESSAGE("Iteration " << i << ": Exception: " << e.what());
        }
    }
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: ion-rpc-serializer, Property 3: Native blob round-trips arbitrary bytes
 * Validates: Requirements 3.2, 8.6
 *
 * Property: For any command/data byte sequence, including sequences with no
 * valid JSON-string / base64-safe interpretation (arbitrary high-bit bytes),
 * the Ion serializer round-trips it exactly via a native blob, with no base64
 * step — demonstrating the native-blob advantage over base64-in-JSON.
 */
BOOST_AUTO_TEST_CASE(property_native_blob_arbitrary_bytes_round_trip,
                     *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<std::size_t> size_dist(0, max_snapshot_data_size);
    std::uniform_int_distribution<int> byte_dist(0, 255);

    std::size_t failures = 0;
    for (auto encoding : encodings) {
        kythira::ion_rpc_serializer<std::vector<std::byte>> serializer(encoding);
        for (std::size_t i = 0; i < property_test_iterations; ++i) {
            // Deliberately favour high-bit / control bytes that have no
            // text-safe interpretation.
            std::size_t size = size_dist(rng);
            std::vector<std::byte> payload;
            payload.reserve(size);
            for (std::size_t j = 0; j < size; ++j) {
                payload.push_back(static_cast<std::byte>(byte_dist(rng)));
            }
            // Also always include the full 0x00..0xFF span at least once.
            if (i == 0) {
                payload.clear();
                for (int b = 0; b < 256; ++b) {
                    payload.push_back(static_cast<std::byte>(b));
                }
            }

            kythira::install_snapshot_request<> original;
            original._term = 7;
            original._leader_id = 3;
            original._last_included_index = 9;
            original._last_included_term = 6;
            original._offset = 0;
            original._data = payload;
            original._done = true;

            kythira::append_entries_request<> ae;
            ae._term = 7;
            ae._leader_id = 3;
            ae._prev_log_index = 8;
            ae._prev_log_term = 6;
            ae._leader_commit = 8;
            kythira::log_entry<> entry;
            entry._term = 7;
            entry._index = 9;
            entry._command = payload;
            entry._type = kythira::entry_type::normal;
            ae._entries.push_back(entry);

            try {
                auto snap =
                    serializer.deserialize_install_snapshot_request(serializer.serialize(original));
                auto req = serializer.deserialize_append_entries_request(serializer.serialize(ae));
                if (snap.data() != payload || req.entries().size() != 1 ||
                    req.entries()[0].command() != payload) {
                    ++failures;
                }
            } catch (const std::exception& e) {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Exception: " << e.what());
            }
        }
    }
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: ion-rpc-serializer, Property 1: Round-trip fidelity (per message type, per encoding)
 * Validates: Requirements 1.4, 4.5, 6.2
 *
 * Property: For any instantiation of ion_rpc_serializer<Data>, name() contains
 * the "ion" substring and passing it to get_content_format_for_serializer
 * resolves to application_ion.
 */
BOOST_AUTO_TEST_CASE(property_name_resolves_to_ion_content_format, *boost::unit_test::timeout(60)) {
    for (auto encoding : encodings) {
        kythira::ion_rpc_serializer<std::vector<std::byte>> serializer(encoding);
        for (std::size_t i = 0; i < property_test_iterations; ++i) {
            BOOST_REQUIRE(serializer.name().find("ion") != std::string::npos);
            BOOST_REQUIRE(
                (kythira::coap_utils::get_content_format_for_serializer(serializer.name()) ==
                 kythira::coap_utils::coap_content_format::application_ion));
        }
    }
}

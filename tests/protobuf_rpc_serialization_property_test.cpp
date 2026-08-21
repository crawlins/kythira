// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE ProtobufRpcSerializationPropertyTest
#include <boost/test/unit_test.hpp>

#include <raft/protobuf_serializer.hpp>

#include <random>
#include <string>
#include <vector>
#include <cstddef>

// Property-based round-trip tests for protobuf_rpc_serializer, mirroring
// tests/rpc_serialization_property_test.cpp's per-message-type structure.
//
// Feature: protobuf-rpc-serializer, Property 1: Round-trip fidelity for every
// message type. Validates Requirements 2.1-2.4, 6.1-6.6.
// Feature: protobuf-rpc-serializer, Property 2: Generic NodeId round-trips for
// both supported instantiations. Validates Requirements 3.1-3.4.

namespace {
constexpr std::size_t property_test_iterations = 100;
constexpr std::uint64_t max_term = 1000000;
constexpr std::uint64_t max_index = 1000000;
constexpr std::uint64_t max_node_id = 10000;
constexpr std::size_t max_entries = 10;
constexpr std::size_t max_command_size = 100;
constexpr std::size_t max_snapshot_data_size = 1000;
}

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

// Random command bytes including the full 0x00-0xFF range, exercising the
// native protobuf `bytes` field (no base64) -- Requirement 6.6.
auto generate_random_command(std::mt19937& rng) -> std::vector<std::byte> {
    std::uniform_int_distribution<std::size_t> size_dist(0, max_command_size);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::size_t size = size_dist(rng);
    std::vector<std::byte> command;
    command.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        command.push_back(static_cast<std::byte>(byte_dist(rng)));
    }
    return command;
}

// Random entry type across all kythira::entry_type values -- Requirement 6.5.
auto generate_random_entry_type(std::mt19937& rng) -> kythira::entry_type {
    std::uniform_int_distribution<int> dist(0, 2);
    return static_cast<kythira::entry_type>(dist(rng));
}

auto generate_random_log_entries(std::mt19937& rng) -> std::vector<kythira::log_entry<>> {
    std::uniform_int_distribution<std::size_t> count_dist(0, max_entries);
    std::size_t count = count_dist(rng);
    std::vector<kythira::log_entry<>> entries;
    entries.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        kythira::log_entry<> entry;
        entry._term = generate_random_term(rng);
        entry._index = generate_random_log_index(rng);
        entry._command = generate_random_command(rng);
        entry._type = generate_random_entry_type(rng);
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
 * Feature: protobuf-rpc-serializer, Property 1: Round-trip fidelity for every
 * message type.
 *
 * Property: For any valid RequestVote request, serializing then deserializing
 * produces an equivalent message with all fields preserved.
 */
BOOST_AUTO_TEST_CASE(property_request_vote_request_round_trip, *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    kythira::protobuf_rpc_serializer<std::vector<std::byte>> serializer;
    std::size_t failures = 0;

    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        kythira::request_vote_request<> original;
        original._term = generate_random_term(rng);
        original._candidate_id = generate_random_node_id(rng);
        original._last_log_index = generate_random_log_index(rng);
        original._last_log_term = generate_random_term(rng);
        try {
            auto deserialized =
                serializer.deserialize_request_vote_request(serializer.serialize(original));
            if (deserialized.term() != original.term() ||
                deserialized.candidate_id() != original.candidate_id() ||
                deserialized.last_log_index() != original.last_log_index() ||
                deserialized.last_log_term() != original.last_log_term()) {
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
 * Feature: protobuf-rpc-serializer, Property 1: Round-trip fidelity for every
 * message type.
 */
BOOST_AUTO_TEST_CASE(property_request_vote_response_round_trip, *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    kythira::protobuf_rpc_serializer<std::vector<std::byte>> serializer;
    std::size_t failures = 0;
    std::uniform_int_distribution<int> bool_dist(0, 1);

    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        kythira::request_vote_response<> original;
        original._term = generate_random_term(rng);
        original._vote_granted = bool_dist(rng) == 1;
        try {
            auto deserialized =
                serializer.deserialize_request_vote_response(serializer.serialize(original));
            if (deserialized.term() != original.term() ||
                deserialized.vote_granted() != original.vote_granted()) {
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
 * Feature: protobuf-rpc-serializer, Property 1: Round-trip fidelity for every
 * message type.
 */
BOOST_AUTO_TEST_CASE(property_request_pre_vote_round_trip, *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    kythira::protobuf_rpc_serializer<std::vector<std::byte>> serializer;
    std::size_t failures = 0;
    std::uniform_int_distribution<int> bool_dist(0, 1);

    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        kythira::request_pre_vote_request<> req;
        req._term = generate_random_term(rng);
        req._candidate_id = generate_random_node_id(rng);
        req._last_log_index = generate_random_log_index(rng);
        req._last_log_term = generate_random_term(rng);

        kythira::request_pre_vote_response<> resp;
        resp._term = generate_random_term(rng);
        resp._vote_granted = bool_dist(rng) == 1;
        try {
            auto dreq = serializer.deserialize_request_pre_vote_request(serializer.serialize(req));
            auto dresp =
                serializer.deserialize_request_pre_vote_response(serializer.serialize(resp));
            if (dreq.term() != req.term() || dreq.candidate_id() != req.candidate_id() ||
                dreq.last_log_index() != req.last_log_index() ||
                dreq.last_log_term() != req.last_log_term() || dresp.term() != resp.term() ||
                dresp.vote_granted() != resp.vote_granted()) {
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
 * Feature: protobuf-rpc-serializer, Property 1: Round-trip fidelity for every
 * message type.
 *
 * Covers the repeated LogEntry field including the empty-list edge case
 * (Requirement 6.2) and every entry_type value (Requirement 6.5).
 */
BOOST_AUTO_TEST_CASE(property_append_entries_request_round_trip, *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    kythira::protobuf_rpc_serializer<std::vector<std::byte>> serializer;
    std::size_t failures = 0;

    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        kythira::append_entries_request<> original;
        original._term = generate_random_term(rng);
        original._leader_id = generate_random_node_id(rng);
        original._prev_log_index = generate_random_log_index(rng);
        original._prev_log_term = generate_random_term(rng);
        // Force an empty-entries case on the first iteration explicitly.
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
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: protobuf-rpc-serializer, Property 1: Round-trip fidelity for every
 * message type.
 *
 * Covers proto3 optional conflict_index/conflict_term, both present and absent
 * (Requirement 6.3).
 */
BOOST_AUTO_TEST_CASE(property_append_entries_response_round_trip, *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    kythira::protobuf_rpc_serializer<std::vector<std::byte>> serializer;
    std::size_t failures = 0;
    std::uniform_int_distribution<int> bool_dist(0, 1);

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
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: protobuf-rpc-serializer, Property 1: Round-trip fidelity for every
 * message type.
 *
 * Covers arbitrary binary snapshot data (0x00 / non-UTF-8) round-tripping via a
 * native protobuf bytes field (Requirement 6.6).
 */
BOOST_AUTO_TEST_CASE(property_install_snapshot_round_trip, *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    kythira::protobuf_rpc_serializer<std::vector<std::byte>> serializer;
    std::size_t failures = 0;
    std::uniform_int_distribution<int> bool_dist(0, 1);
    std::uniform_int_distribution<std::size_t> offset_dist(0, 1000000);

    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        kythira::install_snapshot_request<> req;
        req._term = generate_random_term(rng);
        req._leader_id = generate_random_node_id(rng);
        req._last_included_index = generate_random_log_index(rng);
        req._last_included_term = generate_random_term(rng);
        req._offset = offset_dist(rng);
        req._data = generate_random_snapshot_data(rng);
        req._done = bool_dist(rng) == 1;

        kythira::install_snapshot_response<> resp;
        resp._term = generate_random_term(rng);
        try {
            auto dreq = serializer.deserialize_install_snapshot_request(serializer.serialize(req));
            auto dresp =
                serializer.deserialize_install_snapshot_response(serializer.serialize(resp));
            if (dreq.term() != req.term() || dreq.leader_id() != req.leader_id() ||
                dreq.last_included_index() != req.last_included_index() ||
                dreq.last_included_term() != req.last_included_term() ||
                dreq.offset() != req.offset() || dreq.data() != req.data() ||
                dreq.done() != req.done() || dresp.term() != resp.term()) {
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
 * Feature: protobuf-rpc-serializer, Property 1: Round-trip fidelity for every
 * message type.
 *
 * Covers cluster join/leave with an optional PeerInfo redirect, both present
 * and absent (Requirement 6.4).
 */
BOOST_AUTO_TEST_CASE(property_cluster_bootstrap_round_trip, *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    kythira::protobuf_rpc_serializer<std::vector<std::byte>> serializer;
    std::size_t failures = 0;
    std::uniform_int_distribution<int> bool_dist(0, 1);

    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        try {
            // Join request/response.
            kythira::cluster_join_request<> jreq;
            jreq.node_id = generate_random_node_id(rng);
            jreq.contact_address = "host_" + std::to_string(generate_random_node_id(rng));
            auto djreq = serializer.deserialize_cluster_join_request(serializer.serialize(jreq));
            if (djreq.node_id != jreq.node_id || djreq.contact_address != jreq.contact_address) {
                ++failures;
            }

            kythira::cluster_join_response<> jresp;
            jresp.accepted = bool_dist(rng) == 1;
            if (bool_dist(rng) == 1) {
                jresp.redirect = kythira::peer_info<std::uint64_t, std::string>{
                    generate_random_node_id(rng),
                    "leader_" + std::to_string(generate_random_node_id(rng))};
            }
            auto djresp = serializer.deserialize_cluster_join_response(serializer.serialize(jresp));
            bool jredir_ok = djresp.redirect.has_value() == jresp.redirect.has_value();
            if (jredir_ok && jresp.redirect.has_value()) {
                jredir_ok = djresp.redirect->node_id == jresp.redirect->node_id &&
                            djresp.redirect->address == jresp.redirect->address;
            }
            if (djresp.accepted != jresp.accepted || !jredir_ok) {
                ++failures;
            }

            // Leave request/response.
            kythira::cluster_leave_request<> lreq;
            lreq.node_id = generate_random_node_id(rng);
            auto dlreq = serializer.deserialize_cluster_leave_request(serializer.serialize(lreq));
            if (dlreq.node_id != lreq.node_id) {
                ++failures;
            }

            kythira::cluster_leave_response<> lresp;
            lresp.accepted = bool_dist(rng) == 1;
            if (bool_dist(rng) == 1) {
                lresp.redirect = kythira::peer_info<std::uint64_t, std::string>{
                    generate_random_node_id(rng),
                    "leader_" + std::to_string(generate_random_node_id(rng))};
            }
            auto dlresp =
                serializer.deserialize_cluster_leave_response(serializer.serialize(lresp));
            bool lredir_ok = dlresp.redirect.has_value() == lresp.redirect.has_value();
            if (lredir_ok && lresp.redirect.has_value()) {
                lredir_ok = dlresp.redirect->node_id == lresp.redirect->node_id &&
                            dlresp.redirect->address == lresp.redirect->address;
            }
            if (dlresp.accepted != lresp.accepted || !lredir_ok) {
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
 * Feature: protobuf-rpc-serializer, Property 1: Round-trip fidelity for every
 * message type.
 *
 * Covers the peer2peer fetch_log_entries RPC, whose responder_id stays a bare
 * uint64 (not a NodeIdValue).
 */
BOOST_AUTO_TEST_CASE(property_fetch_log_entries_round_trip, *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    kythira::protobuf_rpc_serializer<std::vector<std::byte>> serializer;
    std::size_t failures = 0;
    std::uniform_int_distribution<int> bool_dist(0, 1);

    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        kythira::fetch_log_entries_request<> req;
        req._requester_id = generate_random_node_id(rng);
        req._from_index = generate_random_log_index(rng);
        req._to_index = generate_random_log_index(rng);

        kythira::fetch_log_entries_response<> resp;
        resp._responder_id = generate_random_node_id(rng);
        resp._available = bool_dist(rng) == 1;
        resp._prev_log_term = generate_random_term(rng);
        resp._entries = generate_random_log_entries(rng);
        try {
            auto dreq = serializer.deserialize_fetch_log_entries_request(serializer.serialize(req));
            auto dresp =
                serializer.deserialize_fetch_log_entries_response(serializer.serialize(resp));
            bool entries_match = dresp.entries().size() == resp.entries().size();
            for (std::size_t j = 0; entries_match && j < resp.entries().size(); ++j) {
                if (!log_entries_equal(dresp.entries()[j], resp.entries()[j])) {
                    entries_match = false;
                }
            }
            if (dreq.requester_id() != req.requester_id() ||
                dreq.from_index() != req.from_index() || dreq.to_index() != req.to_index() ||
                dresp.responder_id() != resp.responder_id() ||
                dresp.available() != resp.available() ||
                dresp.prev_log_term() != resp.prev_log_term() || !entries_match) {
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
 * Feature: protobuf-rpc-serializer, Property 2: Generic NodeId round-trips for
 * both supported instantiations.
 *
 * Property: For any RPC struct with a NodeId-typed field, instantiating with
 * either std::uint64_t or std::string and round-tripping reproduces the value
 * exactly (Requirements 3.1-3.4).
 */
BOOST_AUTO_TEST_CASE(property_string_node_id_round_trip, *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    kythira::protobuf_rpc_serializer<std::vector<std::byte>> serializer;
    std::size_t failures = 0;

    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        try {
            // RequestVote with string NodeId.
            kythira::request_vote_request<std::string> rv;
            rv._term = generate_random_term(rng);
            rv._candidate_id = generate_random_string_node_id(rng);
            rv._last_log_index = generate_random_log_index(rng);
            rv._last_log_term = generate_random_term(rng);
            auto drv =
                serializer.deserialize_request_vote_request<std::string>(serializer.serialize(rv));
            if (drv.candidate_id() != rv.candidate_id() || drv.term() != rv.term() ||
                drv.last_log_index() != rv.last_log_index() ||
                drv.last_log_term() != rv.last_log_term()) {
                ++failures;
            }

            // AppendEntries with string NodeId.
            kythira::append_entries_request<std::string> ae;
            ae._term = generate_random_term(rng);
            ae._leader_id = generate_random_string_node_id(rng);
            ae._prev_log_index = generate_random_log_index(rng);
            ae._prev_log_term = generate_random_term(rng);
            ae._leader_commit = generate_random_log_index(rng);
            auto dae = serializer.deserialize_append_entries_request<std::string>(
                serializer.serialize(ae));
            if (dae.leader_id() != ae.leader_id()) {
                ++failures;
            }

            // ClusterJoin with string NodeId + redirect.
            kythira::cluster_join_response<std::string> cj;
            cj.accepted = true;
            cj.redirect = kythira::peer_info<std::string, std::string>{
                generate_random_string_node_id(rng), "addr"};
            auto dcj =
                serializer.deserialize_cluster_join_response<std::string>(serializer.serialize(cj));
            if (!dcj.redirect.has_value() || dcj.redirect->node_id != cj.redirect->node_id) {
                ++failures;
            }
        } catch (const std::exception& e) {
            ++failures;
            BOOST_TEST_MESSAGE("Iteration " << i << ": Exception: " << e.what());
        }
    }
    BOOST_CHECK_EQUAL(failures, 0);
}

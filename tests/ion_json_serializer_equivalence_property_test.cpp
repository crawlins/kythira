// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE IonJsonSerializerEquivalencePropertyTest
#include <boost/test/unit_test.hpp>

#include <raft/ion_serializer.hpp>
#include <raft/json_serializer.hpp>

#include <random>
#include <string>
#include <vector>
#include <cstddef>

namespace {
constexpr std::size_t property_test_iterations = 100;
constexpr std::uint64_t max_term = 1000000;
constexpr std::uint64_t max_index = 1000000;
constexpr std::uint64_t max_node_id = 10000;
constexpr std::size_t max_entries = 8;
constexpr std::size_t max_command_size = 64;

auto rand_term(std::mt19937& rng) -> std::uint64_t {
    std::uniform_int_distribution<std::uint64_t> dist(1, max_term);
    return dist(rng);
}
auto rand_index(std::mt19937& rng) -> std::uint64_t {
    std::uniform_int_distribution<std::uint64_t> dist(1, max_index);
    return dist(rng);
}
auto rand_node(std::mt19937& rng) -> std::uint64_t {
    std::uniform_int_distribution<std::uint64_t> dist(1, max_node_id);
    return dist(rng);
}
auto rand_command(std::mt19937& rng) -> std::vector<std::byte> {
    std::uniform_int_distribution<std::size_t> size_dist(1, max_command_size);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::size_t size = size_dist(rng);
    std::vector<std::byte> out;
    out.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(static_cast<std::byte>(byte_dist(rng)));
    }
    return out;
}
auto rand_entries(std::mt19937& rng) -> std::vector<kythira::log_entry<>> {
    std::uniform_int_distribution<std::size_t> count_dist(0, max_entries);
    std::size_t count = count_dist(rng);
    std::vector<kythira::log_entry<>> entries;
    for (std::size_t i = 0; i < count; ++i) {
        kythira::log_entry<> e;
        e._term = rand_term(rng);
        e._index = rand_index(rng);
        e._command = rand_command(rng);
        e._type = kythira::entry_type::normal;
        entries.push_back(e);
    }
    return entries;
}
auto entries_equal(const std::vector<kythira::log_entry<>>& a,
                   const std::vector<kythira::log_entry<>>& b) -> bool {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].term() != b[i].term() || a[i].index() != b[i].index() ||
            a[i].command() != b[i].command() || a[i].type() != b[i].type()) {
            return false;
        }
    }
    return true;
}
}  // namespace

/**
 * Feature: ion-rpc-serializer, Property 5: Ion and JSON serializers agree semantically
 * Validates: Requirement 8.4
 *
 * Property: For any valid message value, deserializing it after a round trip
 * through json_rpc_serializer and through ion_rpc_serializer produces
 * field-for-field-equal results (and both equal the original), even though the
 * two serializers' wire bytes are entirely different.
 */
BOOST_AUTO_TEST_CASE(property_ion_json_agree_request_vote, *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    kythira::json_rpc_serializer<std::vector<std::byte>> json;
    kythira::ion_rpc_serializer<std::vector<std::byte>> ion;

    std::size_t failures = 0;
    std::uniform_int_distribution<int> bool_dist(0, 1);
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        kythira::request_vote_request<> req;
        req._term = rand_term(rng);
        req._candidate_id = rand_node(rng);
        req._last_log_index = rand_index(rng);
        req._last_log_term = rand_term(rng);

        kythira::request_vote_response<> resp;
        resp._term = rand_term(rng);
        resp._vote_granted = bool_dist(rng) == 1;

        try {
            auto jq = json.deserialize_request_vote_request(json.serialize(req));
            auto iq = ion.deserialize_request_vote_request(ion.serialize(req));
            auto jr = json.deserialize_request_vote_response(json.serialize(resp));
            auto ir = ion.deserialize_request_vote_response(ion.serialize(resp));

            if (jq.term() != iq.term() || jq.candidate_id() != iq.candidate_id() ||
                jq.last_log_index() != iq.last_log_index() ||
                jq.last_log_term() != iq.last_log_term() || iq.term() != req.term() ||
                jr.term() != ir.term() || jr.vote_granted() != ir.vote_granted() ||
                ir.term() != resp.term() || ir.vote_granted() != resp.vote_granted()) {
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
 * Feature: ion-rpc-serializer, Property 5: Ion and JSON serializers agree semantically
 * Validates: Requirement 8.4
 */
BOOST_AUTO_TEST_CASE(property_ion_json_agree_append_entries, *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    kythira::json_rpc_serializer<std::vector<std::byte>> json;
    kythira::ion_rpc_serializer<std::vector<std::byte>> ion;

    std::size_t failures = 0;
    std::uniform_int_distribution<int> bool_dist(0, 1);
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        kythira::append_entries_request<> req;
        req._term = rand_term(rng);
        req._leader_id = rand_node(rng);
        req._prev_log_index = rand_index(rng);
        req._prev_log_term = rand_term(rng);
        req._entries = (i == 0) ? std::vector<kythira::log_entry<>>{} : rand_entries(rng);
        req._leader_commit = rand_index(rng);

        kythira::append_entries_response<> resp;
        resp._term = rand_term(rng);
        resp._success = bool_dist(rng) == 1;
        if (bool_dist(rng) == 1) {
            resp._conflict_index = rand_index(rng);
        }
        if (bool_dist(rng) == 1) {
            resp._conflict_term = rand_term(rng);
        }

        try {
            auto jq = json.deserialize_append_entries_request(json.serialize(req));
            auto iq = ion.deserialize_append_entries_request(ion.serialize(req));
            auto jr = json.deserialize_append_entries_response(json.serialize(resp));
            auto ir = ion.deserialize_append_entries_response(ion.serialize(resp));

            if (jq.term() != iq.term() || jq.leader_id() != iq.leader_id() ||
                jq.prev_log_index() != iq.prev_log_index() ||
                jq.prev_log_term() != iq.prev_log_term() ||
                jq.leader_commit() != iq.leader_commit() ||
                !entries_equal(jq.entries(), iq.entries()) ||
                !entries_equal(iq.entries(), req.entries()) || jr.term() != ir.term() ||
                jr.success() != ir.success() || jr.conflict_index() != ir.conflict_index() ||
                jr.conflict_term() != ir.conflict_term() ||
                ir.conflict_index() != resp.conflict_index() ||
                ir.conflict_term() != resp.conflict_term()) {
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
 * Feature: ion-rpc-serializer, Property 5: Ion and JSON serializers agree semantically
 * Validates: Requirement 8.4
 */
BOOST_AUTO_TEST_CASE(property_ion_json_agree_install_snapshot, *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    kythira::json_rpc_serializer<std::vector<std::byte>> json;
    kythira::ion_rpc_serializer<std::vector<std::byte>> ion;

    std::size_t failures = 0;
    std::uniform_int_distribution<std::size_t> offset_dist(0, 1000000);
    std::uniform_int_distribution<int> bool_dist(0, 1);
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        kythira::install_snapshot_request<> req;
        req._term = rand_term(rng);
        req._leader_id = rand_node(rng);
        req._last_included_index = rand_index(rng);
        req._last_included_term = rand_term(rng);
        req._offset = offset_dist(rng);
        req._data = rand_command(rng);
        req._done = bool_dist(rng) == 1;

        try {
            auto jq = json.deserialize_install_snapshot_request(json.serialize(req));
            auto iq = ion.deserialize_install_snapshot_request(ion.serialize(req));

            if (jq.term() != iq.term() || jq.leader_id() != iq.leader_id() ||
                jq.last_included_index() != iq.last_included_index() ||
                jq.last_included_term() != iq.last_included_term() || jq.offset() != iq.offset() ||
                jq.data() != iq.data() || jq.done() != iq.done() || iq.data() != req.data()) {
                ++failures;
            }
        } catch (const std::exception& e) {
            ++failures;
            BOOST_TEST_MESSAGE("Iteration " << i << ": Exception: " << e.what());
        }
    }
    BOOST_CHECK_EQUAL(failures, 0);
}

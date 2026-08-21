// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE ProtobufRpcMalformedMessagePropertyTest
#include <boost/test/unit_test.hpp>

#include <raft/protobuf_serializer.hpp>

#include <functional>
#include <random>
#include <string>
#include <vector>
#include <cstddef>

// Malformed / wrong-type rejection tests for protobuf_rpc_serializer, mirroring
// tests/rpc_malformed_message_property_test.cpp.
//
// Feature: protobuf-rpc-serializer, Property 3: Wrong-tag payloads are always
// rejected. Validates Requirements 5.1, 5.2, 5.6.
// Feature: protobuf-rpc-serializer, Property 4: Malformed/random payloads are
// rejected at a high rate. Validates Requirements 5.3, 5.4, 5.5.

namespace {
constexpr std::size_t property_test_iterations = 100;
constexpr std::size_t max_random_bytes = 1000;
using Data = std::vector<std::byte>;
using serializer_t = kythira::protobuf_rpc_serializer<Data>;
}

auto generate_random_bytes(std::mt19937& rng, std::size_t size) -> Data {
    std::uniform_int_distribution<int> byte_dist(0, 255);
    Data data;
    data.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        data.push_back(static_cast<std::byte>(byte_dist(rng)));
    }
    return data;
}

// Returns true if the deserializer rejected `data` with an exception.
auto rejected(const std::function<void(const Data&)>& call, const Data& data) -> bool {
    try {
        call(data);
        return false;
    } catch (const kythira::serialization_exception&) {
        return true;
    } catch (const std::exception&) {
        return true;
    }
}

/**
 * Feature: protobuf-rpc-serializer, Property 4: Malformed/random payloads are
 * rejected at a high rate.
 *
 * Random byte sequences fed to every deserialize_* method are rejected at least
 * 95% of the time. The leading tag byte makes this near-total: a random first
 * byte matches any specific expected tag with probability 1/256.
 */
BOOST_AUTO_TEST_CASE(property_malformed_random_bytes_rejection) {
    std::mt19937 rng(std::random_device{}());
    serializer_t s;
    std::uniform_int_distribution<std::size_t> size_dist(1, max_random_bytes);

    // One entry per public deserialize_* method (all fourteen).
    std::vector<std::function<void(const Data&)>> methods = {
        [&](const Data& d) { (void)s.deserialize_request_vote_request(d); },
        [&](const Data& d) { (void)s.deserialize_request_vote_response(d); },
        [&](const Data& d) { (void)s.deserialize_request_pre_vote_request(d); },
        [&](const Data& d) { (void)s.deserialize_request_pre_vote_response(d); },
        [&](const Data& d) { (void)s.deserialize_append_entries_request(d); },
        [&](const Data& d) { (void)s.deserialize_append_entries_response(d); },
        [&](const Data& d) { (void)s.deserialize_install_snapshot_request(d); },
        [&](const Data& d) { (void)s.deserialize_install_snapshot_response(d); },
        [&](const Data& d) { (void)s.deserialize_cluster_join_request(d); },
        [&](const Data& d) { (void)s.deserialize_cluster_join_response(d); },
        [&](const Data& d) { (void)s.deserialize_cluster_leave_request(d); },
        [&](const Data& d) { (void)s.deserialize_cluster_leave_response(d); },
        [&](const Data& d) { (void)s.deserialize_fetch_log_entries_request(d); },
        [&](const Data& d) { (void)s.deserialize_fetch_log_entries_response(d); },
    };

    for (const auto& method : methods) {
        std::size_t rejection_count = 0;
        for (std::size_t i = 0; i < property_test_iterations; ++i) {
            auto random_data = generate_random_bytes(rng, size_dist(rng));
            if (rejected(method, random_data)) {
                ++rejection_count;
            }
        }
        BOOST_TEST_MESSAGE("Random-bytes rejection: " << rejection_count << "/"
                                                      << property_test_iterations);
        BOOST_CHECK_GE(rejection_count, property_test_iterations * 95 / 100);
    }
}

/**
 * Feature: protobuf-rpc-serializer, Property 3: Wrong-tag payloads are always
 * rejected.
 *
 * A validly-tagged-and-encoded message of type X fed to the deserialize_*
 * method for any type Y != X is rejected 100% of the time -- the property the
 * leading tag byte exists to guarantee, strictly stronger than raw
 * ParseFromArray. This closes the gap where lenient protobuf parsing might
 * otherwise accept bytes from the wrong message shape.
 */
BOOST_AUTO_TEST_CASE(property_wrong_tag_rejection) {
    serializer_t s;

    // A representative valid payload of each message type.
    std::vector<Data> payloads;
    payloads.push_back(s.serialize(kythira::request_vote_request<>{1, 2, 3, 4}));
    payloads.push_back(s.serialize(kythira::request_vote_response<>{5, true}));
    payloads.push_back(s.serialize(kythira::request_pre_vote_request<>{1, 2, 3, 4}));
    payloads.push_back(s.serialize(kythira::request_pre_vote_response<>{5, false}));
    {
        kythira::append_entries_request<> m;
        m._term = 1;
        m._leader_id = 2;
        m._prev_log_index = 3;
        m._prev_log_term = 4;
        m._leader_commit = 5;
        payloads.push_back(s.serialize(m));
    }
    {
        kythira::append_entries_response<> m;
        m._term = 1;
        m._success = true;
        payloads.push_back(s.serialize(m));
    }
    {
        kythira::install_snapshot_request<> m;
        m._term = 1;
        m._leader_id = 2;
        m._last_included_index = 3;
        m._last_included_term = 4;
        m._offset = 0;
        m._done = true;
        payloads.push_back(s.serialize(m));
    }
    payloads.push_back(s.serialize(kythira::install_snapshot_response<>{7}));
    {
        kythira::cluster_join_request<> m;
        m.node_id = 1;
        m.contact_address = "h";
        payloads.push_back(s.serialize(m));
    }
    {
        kythira::cluster_join_response<> m;
        m.accepted = true;
        payloads.push_back(s.serialize(m));
    }
    {
        kythira::cluster_leave_request<> m;
        m.node_id = 1;
        payloads.push_back(s.serialize(m));
    }
    {
        kythira::cluster_leave_response<> m;
        m.accepted = false;
        payloads.push_back(s.serialize(m));
    }
    {
        kythira::fetch_log_entries_request<> m;
        m._requester_id = 1;
        m._from_index = 2;
        m._to_index = 3;
        payloads.push_back(s.serialize(m));
    }
    {
        kythira::fetch_log_entries_response<> m;
        m._responder_id = 1;
        m._available = true;
        m._prev_log_term = 2;
        payloads.push_back(s.serialize(m));
    }

    // Same fourteen deserialize_* methods, index-aligned with `payloads`.
    std::vector<std::function<void(const Data&)>> methods = {
        [&](const Data& d) { (void)s.deserialize_request_vote_request(d); },
        [&](const Data& d) { (void)s.deserialize_request_vote_response(d); },
        [&](const Data& d) { (void)s.deserialize_request_pre_vote_request(d); },
        [&](const Data& d) { (void)s.deserialize_request_pre_vote_response(d); },
        [&](const Data& d) { (void)s.deserialize_append_entries_request(d); },
        [&](const Data& d) { (void)s.deserialize_append_entries_response(d); },
        [&](const Data& d) { (void)s.deserialize_install_snapshot_request(d); },
        [&](const Data& d) { (void)s.deserialize_install_snapshot_response(d); },
        [&](const Data& d) { (void)s.deserialize_cluster_join_request(d); },
        [&](const Data& d) { (void)s.deserialize_cluster_join_response(d); },
        [&](const Data& d) { (void)s.deserialize_cluster_leave_request(d); },
        [&](const Data& d) { (void)s.deserialize_cluster_leave_response(d); },
        [&](const Data& d) { (void)s.deserialize_fetch_log_entries_request(d); },
        [&](const Data& d) { (void)s.deserialize_fetch_log_entries_response(d); },
    };

    BOOST_REQUIRE_EQUAL(payloads.size(), methods.size());

    for (std::size_t x = 0; x < payloads.size(); ++x) {
        for (std::size_t y = 0; y < methods.size(); ++y) {
            if (x == y) {
                // Correct method must accept its own payload.
                BOOST_CHECK(!rejected(methods[y], payloads[x]));
            } else {
                // Every mismatched method must reject via the tag check.
                BOOST_CHECK(rejected(methods[y], payloads[x]));
            }
        }
    }
}

/**
 * Feature: protobuf-rpc-serializer, Property 4: Malformed/random payloads are
 * rejected at a high rate.
 *
 * Truncated payloads -- empty (0 bytes) and a lone wrong tag byte -- are
 * rejected without out-of-bounds access (Requirement 5.4). Run under ASan/UBSan
 * in CI. An empty payload is shorter than the one-byte tag and is always
 * rejected; a single wrong tag byte is rejected by the tag check.
 */
BOOST_AUTO_TEST_CASE(property_truncated_payload_rejection) {
    serializer_t s;

    // Empty payload: shorter than the tag -> always rejected.
    BOOST_CHECK(
        rejected([&](const Data& d) { (void)s.deserialize_request_vote_request(d); }, Data{}));

    // Single wrong tag byte (0xFF is not a valid message tag) -> rejected by the
    // tag check for every deserializer, with no OOB read of the empty body.
    Data wrong_tag{std::byte{0xFF}};
    BOOST_CHECK(
        rejected([&](const Data& d) { (void)s.deserialize_request_vote_request(d); }, wrong_tag));
    BOOST_CHECK(
        rejected([&](const Data& d) { (void)s.deserialize_append_entries_request(d); }, wrong_tag));
    BOOST_CHECK(rejected([&](const Data& d) { (void)s.deserialize_fetch_log_entries_response(d); },
                         wrong_tag));

    // A correct tag byte with an empty body but a NodeId-bearing message is
    // rejected at the NodeIdValue presence check, not by an OOB read.
    Data tag_only_rvr{std::byte{0x00}};  // message_tag::request_vote_request
    BOOST_CHECK(rejected([&](const Data& d) { (void)s.deserialize_request_vote_request(d); },
                         tag_only_rvr));
}

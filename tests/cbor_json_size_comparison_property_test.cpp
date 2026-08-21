// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE CborJsonSizeComparisonPropertyTest
#include <boost/test/unit_test.hpp>

#include <raft/cbor_serializer.hpp>
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
constexpr std::size_t max_entries = 10;
// Byte-carrying fields are the source of CBOR's win over JSON's base64, so keep
// them comfortably non-empty.
constexpr std::size_t min_command_size = 1;
constexpr std::size_t max_command_size = 200;
constexpr std::size_t min_snapshot_data_size = 1;
constexpr std::size_t max_snapshot_data_size = 2000;
}

auto rand_in(std::mt19937& rng, std::uint64_t lo, std::uint64_t hi) -> std::uint64_t {
    std::uniform_int_distribution<std::uint64_t> dist(lo, hi);
    return dist(rng);
}

auto random_bytes(std::mt19937& rng, std::size_t lo, std::size_t hi) -> std::vector<std::byte> {
    std::uniform_int_distribution<std::size_t> size_dist(lo, hi);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::size_t size = size_dist(rng);
    std::vector<std::byte> data;
    data.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        data.push_back(static_cast<std::byte>(byte_dist(rng)));
    }
    return data;
}

/**
 * Feature: cbor-rpc-serializer, Property 4: CBOR output is never larger than JSON output for
 * byte-carrying messages
 * Validates: Requirement 10.1
 *
 * Property: For any AppendEntries request with at least one non-empty command
 * byte field, cbor_rpc_serializer's serialized size is no larger than
 * json_rpc_serializer's serialized size for the same logical value — CBOR byte
 * strings carry the raw bytes JSON must base64-inflate by ~33%.
 *
 * Note: This is a property test (a "no larger than" comparison), not a
 * specification of exact byte counts. CBOR integer-encoding width and JSON's
 * decimal-digit count both vary with field magnitude, so only the size
 * relationship is asserted, not any fixed size (Requirement 10.2).
 */
BOOST_AUTO_TEST_CASE(property_append_entries_cbor_not_larger_than_json,
                     *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    kythira::cbor_rpc_serializer<std::vector<std::byte>> cbor;
    kythira::json_rpc_serializer<std::vector<std::byte>> json;

    std::size_t failures = 0;
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        kythira::append_entries_request<> req;
        req._term = rand_in(rng, 1, max_term);
        req._leader_id = rand_in(rng, 1, max_node_id);
        req._prev_log_index = rand_in(rng, 1, max_index);
        req._prev_log_term = rand_in(rng, 1, max_term);
        req._leader_commit = rand_in(rng, 1, max_index);

        std::size_t entry_count = static_cast<std::size_t>(rand_in(rng, 1, max_entries));
        for (std::size_t j = 0; j < entry_count; ++j) {
            kythira::log_entry<> entry;
            entry._term = rand_in(rng, 1, max_term);
            entry._index = rand_in(rng, 1, max_index);
            entry._command = random_bytes(rng, min_command_size, max_command_size);
            req._entries.push_back(entry);
        }

        auto cbor_size = cbor.serialize(req).size();
        auto json_size = json.serialize(req).size();
        if (cbor_size > json_size) {
            ++failures;
            BOOST_TEST_MESSAGE("Iteration " << i << ": CBOR " << cbor_size << " > JSON "
                                            << json_size);
        }
    }
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: cbor-rpc-serializer, Property 4: CBOR output is never larger than JSON output for
 * byte-carrying messages
 * Validates: Requirement 10.1
 *
 * Property: For any InstallSnapshot request with a non-empty data chunk,
 * cbor_rpc_serializer's serialized size is no larger than
 * json_rpc_serializer's, since the data chunk rides as a raw CBOR byte string
 * rather than base64 text.
 */
BOOST_AUTO_TEST_CASE(property_install_snapshot_cbor_not_larger_than_json,
                     *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    kythira::cbor_rpc_serializer<std::vector<std::byte>> cbor;
    kythira::json_rpc_serializer<std::vector<std::byte>> json;

    std::size_t failures = 0;
    std::uniform_int_distribution<int> bool_dist(0, 1);
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        kythira::install_snapshot_request<> req;
        req._term = rand_in(rng, 1, max_term);
        req._leader_id = rand_in(rng, 1, max_node_id);
        req._last_included_index = rand_in(rng, 1, max_index);
        req._last_included_term = rand_in(rng, 1, max_term);
        req._offset = static_cast<std::size_t>(rand_in(rng, 0, 1000000));
        req._data = random_bytes(rng, min_snapshot_data_size, max_snapshot_data_size);
        req._done = bool_dist(rng) == 1;

        auto cbor_size = cbor.serialize(req).size();
        auto json_size = json.serialize(req).size();
        if (cbor_size > json_size) {
            ++failures;
            BOOST_TEST_MESSAGE("Iteration " << i << ": CBOR " << cbor_size << " > JSON "
                                            << json_size);
        }
    }
    BOOST_CHECK_EQUAL(failures, 0);
}

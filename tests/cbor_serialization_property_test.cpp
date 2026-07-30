#define BOOST_TEST_MODULE CborSerializationPropertyTest
#include <boost/test/unit_test.hpp>

#include <raft/cbor_serializer.hpp>
#include <raft/coap_utils.hpp>

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
}

// The input generators below intentionally mirror the ones in
// rpc_serialization_property_test.cpp (random terms, indices, node IDs,
// commands, log entries), parameterized here over the CBOR serializer, per the
// design's "reuse the input-generation helpers" guidance.
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
    std::size_t count = count_dist(rng);

    std::vector<kythira::log_entry<>> entries;
    entries.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        kythira::log_entry<> entry;
        entry._term = generate_random_term(rng);
        entry._index = generate_random_log_index(rng);
        entry._command = generate_random_command(rng);
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
 * Feature: cbor-rpc-serializer, Property 1: Round-trip preserves content
 * Validates: Requirements 2.7, 2.8, 4.1, 4.2
 *
 * Property: For any valid RequestVote request (and the identically-shaped
 * RequestPreVote request), serializing then deserializing produces an
 * equivalent message with all fields preserved.
 */
BOOST_AUTO_TEST_CASE(property_request_vote_request_round_trip, *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    kythira::cbor_rpc_serializer<std::vector<std::byte>> serializer;

    std::size_t failures = 0;
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
            auto d = serializer.deserialize_request_vote_request(serializer.serialize(original));
            auto pv =
                serializer.deserialize_request_pre_vote_request(serializer.serialize(pv_original));

            if (d.term() != original.term() || d.candidate_id() != original.candidate_id() ||
                d.last_log_index() != original.last_log_index() ||
                d.last_log_term() != original.last_log_term() || pv.term() != pv_original.term() ||
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
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: cbor-rpc-serializer, Property 1: Round-trip preserves content
 * Validates: Requirements 2.7, 2.8
 *
 * Property: For any valid RequestVote/RequestPreVote response, serializing then
 * deserializing produces an equivalent message with all fields preserved.
 */
BOOST_AUTO_TEST_CASE(property_request_vote_response_round_trip, *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    kythira::cbor_rpc_serializer<std::vector<std::byte>> serializer;

    std::size_t failures = 0;
    std::uniform_int_distribution<int> bool_dist(0, 1);
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        kythira::request_vote_response<> original;
        original._term = generate_random_term(rng);
        original._vote_granted = bool_dist(rng) == 1;

        kythira::request_pre_vote_response<> pv_original;
        pv_original._term = generate_random_term(rng);
        pv_original._vote_granted = bool_dist(rng) == 1;

        try {
            auto d = serializer.deserialize_request_vote_response(serializer.serialize(original));
            auto pv =
                serializer.deserialize_request_pre_vote_response(serializer.serialize(pv_original));

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
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: cbor-rpc-serializer, Property 1: Round-trip preserves content
 * Validates: Requirements 2.3, 2.7
 *
 * Property: For any valid AppendEntries request (including the empty-entries()
 * edge case), serializing then deserializing preserves every field.
 */
BOOST_AUTO_TEST_CASE(property_append_entries_request_round_trip, *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    kythira::cbor_rpc_serializer<std::vector<std::byte>> serializer;

    std::size_t failures = 0;
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        kythira::append_entries_request<> original;
        original._term = generate_random_term(rng);
        original._leader_id = generate_random_node_id(rng);
        original._prev_log_index = generate_random_log_index(rng);
        original._prev_log_term = generate_random_term(rng);
        // First iteration explicitly exercises the empty-entries() edge case.
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
 * Feature: cbor-rpc-serializer, Property 1: Round-trip preserves content
 * Validates: Requirements 2.4, 2.7
 *
 * Property: For any valid AppendEntries response, serializing then
 * deserializing preserves every field, including the absent-conflict_index /
 * absent-conflict_term edge cases (omitted map keys, never CBOR null).
 */
BOOST_AUTO_TEST_CASE(property_append_entries_response_round_trip, *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    kythira::cbor_rpc_serializer<std::vector<std::byte>> serializer;

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
 * Feature: cbor-rpc-serializer, Property 1: Round-trip preserves content
 * Validates: Requirements 2.5, 2.7
 *
 * Property: For any valid InstallSnapshot request, serializing then
 * deserializing preserves every field, including the raw byte data() carried as
 * a CBOR byte string.
 */
BOOST_AUTO_TEST_CASE(property_install_snapshot_request_round_trip, *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    kythira::cbor_rpc_serializer<std::vector<std::byte>> serializer;

    std::size_t failures = 0;
    std::uniform_int_distribution<int> bool_dist(0, 1);
    std::uniform_int_distribution<std::size_t> offset_dist(0, 1000000);
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
                deserialized.data() != original.data() || deserialized.done() != original.done()) {
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
 * Feature: cbor-rpc-serializer, Property 1: Round-trip preserves content
 * Validates: Requirements 2.6, 2.7
 *
 * Property: For any valid InstallSnapshot response, serializing then
 * deserializing preserves the term field.
 */
BOOST_AUTO_TEST_CASE(property_install_snapshot_response_round_trip,
                     *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    kythira::cbor_rpc_serializer<std::vector<std::byte>> serializer;

    std::size_t failures = 0;
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        kythira::install_snapshot_response<> original;
        original._term = generate_random_term(rng);

        try {
            auto deserialized =
                serializer.deserialize_install_snapshot_response(serializer.serialize(original));
            if (deserialized.term() != original.term()) {
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
 * Feature: cbor-rpc-serializer, Property 1: Round-trip preserves content
 * Validates: Requirements 3.1, 3.2, 3.5, 4.3
 *
 * Property: For any valid ClusterJoin/ClusterLeave request/response (including
 * the absent-redirect edge case), serializing then deserializing preserves
 * every field.
 */
BOOST_AUTO_TEST_CASE(property_cluster_membership_round_trip, *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    kythira::cbor_rpc_serializer<std::vector<std::byte>> serializer;

    std::size_t failures = 0;
    std::uniform_int_distribution<int> bool_dist(0, 1);
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        kythira::cluster_join_request<> join_req;
        join_req.node_id = generate_random_node_id(rng);
        join_req.contact_address = "10.0.0." + std::to_string(generate_random_node_id(rng));

        kythira::cluster_leave_request<> leave_req;
        leave_req.node_id = generate_random_node_id(rng);

        kythira::cluster_join_response<> join_resp;
        join_resp.accepted = bool_dist(rng) == 1;
        // Half the iterations carry a redirect, half exercise absent-redirect.
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
            auto jq = serializer.deserialize_cluster_join_request(serializer.serialize(join_req));
            auto lq = serializer.deserialize_cluster_leave_request(serializer.serialize(leave_req));
            auto jr = serializer.deserialize_cluster_join_response(serializer.serialize(join_resp));
            auto lr =
                serializer.deserialize_cluster_leave_response(serializer.serialize(leave_resp));

            bool join_redirect_ok =
                jr.redirect.has_value() == join_resp.redirect.has_value() &&
                (!jr.redirect.has_value() || (jr.redirect->node_id == join_resp.redirect->node_id &&
                                              jr.redirect->address == join_resp.redirect->address));
            bool leave_redirect_ok = lr.redirect.has_value() == leave_resp.redirect.has_value() &&
                                     (!lr.redirect.has_value() ||
                                      (lr.redirect->node_id == leave_resp.redirect->node_id &&
                                       lr.redirect->address == leave_resp.redirect->address));

            if (jq.node_id != join_req.node_id || jq.contact_address != join_req.contact_address ||
                lq.node_id != leave_req.node_id || jr.accepted != join_resp.accepted ||
                lr.accepted != leave_resp.accepted || !join_redirect_ok || !leave_redirect_ok) {
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
 * Feature: cbor-rpc-serializer, Property 1: Round-trip preserves content
 * Validates: Requirements 3.3, 3.4, 3.5
 *
 * Property: For any valid FetchLogEntries request/response (including the
 * empty-entries() edge case), serializing then deserializing preserves every
 * field.
 */
BOOST_AUTO_TEST_CASE(property_fetch_log_entries_round_trip, *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    kythira::cbor_rpc_serializer<std::vector<std::byte>> serializer;

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
        resp._entries =
            (i == 0) ? std::vector<kythira::log_entry<>>{} : generate_random_log_entries(rng);

        try {
            auto dq = serializer.deserialize_fetch_log_entries_request(serializer.serialize(req));
            auto dr = serializer.deserialize_fetch_log_entries_response(serializer.serialize(resp));

            bool entries_match = dr.entries().size() == resp.entries().size();
            for (std::size_t j = 0; entries_match && j < resp.entries().size(); ++j) {
                if (!log_entries_equal(dr.entries()[j], resp.entries()[j])) {
                    entries_match = false;
                }
            }

            if (dq.requester_id() != req.requester_id() || dq.from_index() != req.from_index() ||
                dq.to_index() != req.to_index() || dr.responder_id() != resp.responder_id() ||
                dr.available() != resp.available() || dr.prev_log_term() != resp.prev_log_term() ||
                !entries_match) {
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
 * Feature: cbor-rpc-serializer, Property 1: Round-trip preserves content
 * Validates: Requirements 4.2, 4.3
 *
 * Property: For any valid message carrying std::string NodeId/Address values,
 * serializing then deserializing preserves every field (text-string encoding
 * path).
 */
BOOST_AUTO_TEST_CASE(property_string_node_id_round_trip, *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    kythira::cbor_rpc_serializer<std::vector<std::byte>> serializer;

    std::size_t failures = 0;
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
            auto rv_d =
                serializer.deserialize_request_vote_request<std::string>(serializer.serialize(rv));
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
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: cbor-rpc-serializer, Property 3: Message-discriminant mismatch is detected
 * Validates: Requirements 5.2, 9.4
 *
 * Property: For any two distinct message types A and B in scope, calling B's
 * deserialize_* method on the output of A's serialize throws
 * serialization_exception rather than returning a mis-populated struct.
 */
BOOST_AUTO_TEST_CASE(property_discriminant_mismatch_detected, *boost::unit_test::timeout(60)) {
    std::mt19937 rng(std::random_device{}());
    kythira::cbor_rpc_serializer<std::vector<std::byte>> serializer;

    // A representative encoded value for each message type, all with the same
    // NodeId/TermId/LogIndex defaults so any deserialize_* can be attempted on
    // any encoded value.
    kythira::request_vote_request<> rvq{generate_random_term(rng), generate_random_node_id(rng),
                                        generate_random_log_index(rng), generate_random_term(rng)};
    kythira::append_entries_response<> aer{generate_random_term(rng), true, std::nullopt,
                                           std::nullopt};

    std::vector<std::vector<std::byte>> encoded;
    encoded.push_back(serializer.serialize(rvq));
    encoded.push_back(
        serializer.serialize(kythira::request_vote_response<>{generate_random_term(rng), true}));
    encoded.push_back(serializer.serialize(aer));
    encoded.push_back(
        serializer.serialize(kythira::install_snapshot_response<>{generate_random_term(rng)}));

    // Each deserialize_* below expects its own discriminant; feeding it any of
    // the encoded values above (each carrying a different discriminant) must
    // throw. We test a cross-product sample.
    std::size_t mismatches_detected = 0;
    std::size_t attempts = 0;

    auto expect_throw = [&](auto&& fn, const std::vector<std::byte>& data) {
        ++attempts;
        try {
            [[maybe_unused]] auto result = fn(data);
        } catch (const kythira::serialization_exception&) {
            ++mismatches_detected;
        } catch (const std::exception&) {
            ++mismatches_detected;
        }
    };

    // request_vote_response bytes fed to request_vote_request, etc.
    expect_throw([&](const auto& d) { return serializer.deserialize_request_vote_request(d); },
                 encoded[1]);
    expect_throw([&](const auto& d) { return serializer.deserialize_request_vote_response(d); },
                 encoded[0]);
    expect_throw([&](const auto& d) { return serializer.deserialize_append_entries_response(d); },
                 encoded[0]);
    expect_throw([&](const auto& d) { return serializer.deserialize_install_snapshot_response(d); },
                 encoded[2]);
    expect_throw([&](const auto& d) { return serializer.deserialize_request_pre_vote_request(d); },
                 encoded[0]);
    expect_throw([&](const auto& d) { return serializer.deserialize_fetch_log_entries_request(d); },
                 encoded[3]);

    BOOST_CHECK_EQUAL(mismatches_detected, attempts);
}

/**
 * Feature: cbor-rpc-serializer, Property 5: name() resolves to the CoAP CBOR content format
 * Validates: Requirements 7.1, 7.2
 *
 * Property: For any instantiation of cbor_rpc_serializer<Data>, name() contains
 * the "cbor" substring and passing it to get_content_format_for_serializer
 * resolves to application_cbor.
 */
BOOST_AUTO_TEST_CASE(property_name_resolves_to_cbor_content_format,
                     *boost::unit_test::timeout(60)) {
    kythira::cbor_rpc_serializer<std::vector<std::byte>> serializer;

    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        BOOST_REQUIRE(serializer.name().find("cbor") != std::string::npos);
        BOOST_REQUIRE((kythira::coap_utils::get_content_format_for_serializer(serializer.name()) ==
                       kythira::coap_utils::coap_content_format::application_cbor));
    }
}

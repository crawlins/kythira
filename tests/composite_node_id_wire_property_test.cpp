// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Requirement 8.5 and 8.6 of the cloud-composite-node-ids spec: every message
// that carries a node id round-trips through every RPC serializer with each
// composite id type as `NodeId`, travels as the id's canonical text, and fails
// the decode when that text does not parse. Also covers the numeric
// fetch_log_entries_response responder (Requirement 8.2, 8.3) and the
// protobuf fallback to the legacy `responder_id` field.
//
// JSON and CBOR are always built. Protobuf and ION are compiled in when their
// serializer targets exist (KYTHIRA_WIRE_TEST_HAS_PROTOBUF / _ION).

#define BOOST_TEST_MODULE composite_node_id_wire_property_test
#include <boost/test/unit_test.hpp>

#include <raft/cbor_serializer.hpp>
#include <raft/composite_node_id.hpp>
#include <raft/json_serializer.hpp>
#include <raft/types.hpp>

#ifdef KYTHIRA_WIRE_TEST_HAS_PROTOBUF
#include <raft/protobuf_serializer.hpp>
#endif
#ifdef KYTHIRA_WIRE_TEST_HAS_ION
#include <raft/ion_serializer.hpp>
#endif

#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace kythira;

namespace {

using bytes = std::vector<std::byte>;

constexpr int k_iterations = 50;

auto hex_string(std::mt19937_64& rng, std::size_t length) -> std::string {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < length; ++i) {
        out.push_back(digits[rng() % 16]);
    }
    return out;
}

/// A random valid id of each composite type, varied in the parts each
/// provider's rules allow to vary.
template<typename Id> auto make_id(std::mt19937_64& rng) -> Id {
    const auto n = std::to_string(rng() % 100000);
    if constexpr (std::same_as<Id, aws_ec2_node_id>) {
        // Both the 17-digit and the legacy 8-digit forms.
        return Id{"us-east-1", "i-" + hex_string(rng, rng() % 2 == 0 ? 17 : 8)};
    } else if constexpr (std::same_as<Id, alibaba_ecs_node_id>) {
        return Id{"cn-hangzhou", "i-bp1" + hex_string(rng, 17)};
    } else if constexpr (std::same_as<Id, azure_vm_node_id>) {
        return Id{"rg(prod).1_x", "kythira-vm-" + n};
    } else if constexpr (std::same_as<Id, gcp_instance_node_id>) {
        // A domain-scoped project, so the ':' percent-encoding is on the wire.
        return Id{"example.com:proj/us-central1-a", "kythira-node-" + n};
    } else if constexpr (std::same_as<Id, oci_instance_node_id>) {
        return Id{"phx", "ocid1.instance.oc1.phx." + hex_string(rng, 24)};
    } else {
        static_assert(std::same_as<Id, docker_container_node_id>);
        return Id{"kythira_test", "Node-" + n + ".a"};
    }
}

auto make_entries(std::mt19937_64& rng) -> std::vector<log_entry<>> {
    std::vector<log_entry<>> entries;
    const auto count = rng() % 3;
    for (std::uint64_t i = 0; i < count; ++i) {
        entries.push_back(log_entry<>{._term = rng() % 100,
                                      ._index = i + 1,
                                      ._command = {std::byte{0x01}, std::byte{0x02}},
                                      ._type = entry_type::normal});
    }
    return entries;
}

/// Round-trips each message type that carries a node id through `s` with
/// `Id` as its `NodeId`, checking every id field and one non-id field.
template<typename Id, typename Serializer> void round_trip_all(const Serializer& s) {
    std::mt19937_64 rng(0x5eed + sizeof(Id));
    for (int i = 0; i < k_iterations; ++i) {
        const Id a = make_id<Id>(rng);
        const Id b = make_id<Id>(rng);
        const std::uint64_t term = rng() % 1000;

        {
            const request_vote_request<Id> in{
                ._term = term, ._candidate_id = a, ._last_log_index = 7, ._last_log_term = 3};
            const auto out = s.template deserialize_request_vote_request<Id>(s.serialize(in));
            BOOST_CHECK(out.candidate_id() == a);
            BOOST_CHECK_EQUAL(out.term(), term);
        }
        {
            const request_pre_vote_request<Id> in{
                ._term = term, ._candidate_id = a, ._last_log_index = 7, ._last_log_term = 3};
            const auto out = s.template deserialize_request_pre_vote_request<Id>(s.serialize(in));
            BOOST_CHECK(out.candidate_id() == a);
            BOOST_CHECK_EQUAL(out.term(), term);
        }
        {
            const timeout_now_request<Id> in{._term = term, ._leader_id = a, ._last_log_index = 9};
            const auto out = s.template deserialize_timeout_now_request<Id>(s.serialize(in));
            BOOST_CHECK(out.leader_id() == a);
            BOOST_CHECK_EQUAL(out.last_log_index(), 9U);
        }
        {
            const auto entries = make_entries(rng);
            const append_entries_request<Id> in{._term = term,
                                                ._leader_id = a,
                                                ._prev_log_index = 4,
                                                ._prev_log_term = 2,
                                                ._entries = entries,
                                                ._leader_commit = 3};
            const auto out = s.template deserialize_append_entries_request<Id>(s.serialize(in));
            BOOST_CHECK(out.leader_id() == a);
            BOOST_CHECK_EQUAL(out.entries().size(), entries.size());
        }
        {
            const install_snapshot_request<Id> in{._term = term,
                                                  ._leader_id = a,
                                                  ._last_included_index = 10,
                                                  ._last_included_term = 2,
                                                  ._offset = 0,
                                                  ._data = {std::byte{0x2a}},
                                                  ._done = true};
            const auto out = s.template deserialize_install_snapshot_request<Id>(s.serialize(in));
            BOOST_CHECK(out.leader_id() == a);
            BOOST_CHECK(out.done());
        }
        {
            const fetch_log_entries_request<Id> in{
                ._requester_id = a, ._from_index = 2, ._to_index = 5};
            const auto out = s.template deserialize_fetch_log_entries_request<Id>(s.serialize(in));
            BOOST_CHECK(out.requester_id() == a);
            BOOST_CHECK_EQUAL(out.to_index(), 5U);
        }
        {
            const fetch_log_entries_response<Id> in{._responder_id = b,
                                                    ._available = true,
                                                    ._prev_log_term = 1,
                                                    ._entries = make_entries(rng)};
            const auto out = s.template deserialize_fetch_log_entries_response<Id>(s.serialize(in));
            BOOST_CHECK(out.responder_id() == b);
            BOOST_CHECK(out.available());
        }
        {
            const cluster_join_request<Id> in{.node_id = a, .contact_address = "10.0.0.1:7000"};
            const auto out = s.template deserialize_cluster_join_request<Id>(s.serialize(in));
            BOOST_CHECK(out.joining_node_id() == a);
            BOOST_CHECK_EQUAL(out.joining_address(), "10.0.0.1:7000");
        }
        {
            cluster_join_response<Id> in;
            in.redirect = peer_info<Id, std::string>{.node_id = b, .address = "10.0.0.2:7000"};
            const auto out = s.template deserialize_cluster_join_response<Id>(s.serialize(in));
            BOOST_REQUIRE(out.redirect_peer().has_value());
            BOOST_CHECK(out.redirect_peer()->node_id == b);
        }
        {
            const cluster_leave_request<Id> in{.node_id = a};
            const auto out = s.template deserialize_cluster_leave_request<Id>(s.serialize(in));
            BOOST_CHECK(out.leaving_node_id() == a);
        }
        {
            cluster_leave_response<Id> in;
            in.redirect = peer_info<Id, std::string>{.node_id = b, .address = "10.0.0.3:7000"};
            const auto out = s.template deserialize_cluster_leave_response<Id>(s.serialize(in));
            BOOST_REQUIRE(out.redirect_peer().has_value());
            BOOST_CHECK(out.redirect_peer()->node_id == b);
        }
    }
}

template<typename Serializer> void round_trip_every_composite_type(const Serializer& s) {
    round_trip_all<aws_ec2_node_id>(s);
    round_trip_all<alibaba_ecs_node_id>(s);
    round_trip_all<azure_vm_node_id>(s);
    round_trip_all<gcp_instance_node_id>(s);
    round_trip_all<oci_instance_node_id>(s);
    round_trip_all<docker_container_node_id>(s);
}

/// Text that is not a canonical id of the decoding type fails the decode:
/// a `std::string`-id payload carrying text from another provider is read
/// back as an `aws_ec2_node_id`.
template<typename Serializer> void refuses_foreign_text(const Serializer& s) {
    const request_vote_request<std::string> in{._term = 1,
                                               ._candidate_id = "docker:kythira_test:n1",
                                               ._last_log_index = 0,
                                               ._last_log_term = 0};
    BOOST_CHECK_THROW(
        static_cast<void>(
            s.template deserialize_request_vote_request<aws_ec2_node_id>(s.serialize(in))),
        std::exception);
}

/// A composite id goes on the wire as exactly its canonical text, so a
/// `std::string`-id peer reads the same bytes back as that text.
template<typename Serializer> void travels_as_canonical_text(const Serializer& s) {
    const aws_ec2_node_id id{"eu-west-2", "i-1234abcd"};
    const fetch_log_entries_response<aws_ec2_node_id> in{
        ._responder_id = id, ._available = false, ._prev_log_term = 0, ._entries = {}};
    const auto out =
        s.template deserialize_fetch_log_entries_response<std::string>(s.serialize(in));
    BOOST_CHECK_EQUAL(out.responder_id(), "aws-ec2:eu-west-2:i-1234abcd");
}

/// The numeric responder keeps its full range (Requirement 8.4's 2^63 case).
template<typename Serializer> void numeric_responder_round_trips(const Serializer& s) {
    for (const std::uint64_t id : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{1} << 63U,
                                   std::numeric_limits<std::uint64_t>::max()}) {
        const fetch_log_entries_response<> in{
            ._responder_id = id, ._available = true, ._prev_log_term = 2, ._entries = {}};
        const auto out = s.deserialize_fetch_log_entries_response(s.serialize(in));
        BOOST_CHECK_EQUAL(out.responder_id(), id);
    }
}

}  // namespace

BOOST_AUTO_TEST_SUITE(json)

BOOST_AUTO_TEST_CASE(composite_ids_round_trip) {
    round_trip_every_composite_type(json_rpc_serializer<bytes>{});
}

BOOST_AUTO_TEST_CASE(foreign_text_is_refused) {
    refuses_foreign_text(json_rpc_serializer<bytes>{});
}

BOOST_AUTO_TEST_CASE(composite_id_is_canonical_text) {
    travels_as_canonical_text(json_rpc_serializer<bytes>{});
}

BOOST_AUTO_TEST_CASE(numeric_responder_keeps_full_range) {
    numeric_responder_round_trips(json_rpc_serializer<bytes>{});
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(cbor)

BOOST_AUTO_TEST_CASE(composite_ids_round_trip) {
    round_trip_every_composite_type(cbor_rpc_serializer<bytes>{});
}

BOOST_AUTO_TEST_CASE(foreign_text_is_refused) {
    refuses_foreign_text(cbor_rpc_serializer<bytes>{});
}

BOOST_AUTO_TEST_CASE(composite_id_is_canonical_text) {
    travels_as_canonical_text(cbor_rpc_serializer<bytes>{});
}

BOOST_AUTO_TEST_CASE(numeric_responder_keeps_full_range) {
    numeric_responder_round_trips(cbor_rpc_serializer<bytes>{});
}

BOOST_AUTO_TEST_SUITE_END()

#ifdef KYTHIRA_WIRE_TEST_HAS_PROTOBUF
BOOST_AUTO_TEST_SUITE(protobuf)

BOOST_AUTO_TEST_CASE(composite_ids_round_trip) {
    round_trip_every_composite_type(protobuf_rpc_serializer<bytes>{});
}

BOOST_AUTO_TEST_CASE(foreign_text_is_refused) {
    refuses_foreign_text(protobuf_rpc_serializer<bytes>{});
}

BOOST_AUTO_TEST_CASE(composite_id_is_canonical_text) {
    travels_as_canonical_text(protobuf_rpc_serializer<bytes>{});
}

BOOST_AUTO_TEST_CASE(numeric_responder_keeps_full_range) {
    numeric_responder_round_trips(protobuf_rpc_serializer<bytes>{});
}

// A numeric id fills both fields, so a peer that predates `responder` still
// reads `responder_id`; a textual id fills only `responder`.
BOOST_AUTO_TEST_CASE(numeric_responder_fills_the_legacy_field) {
    const protobuf_rpc_serializer<bytes> s;
    const fetch_log_entries_response<> numeric{
        ._responder_id = 42, ._available = true, ._prev_log_term = 1, ._entries = {}};
    const auto wire = s.serialize(numeric);
    raft_pb::FetchLogEntriesResponse msg;
    // Skip the serializer's one-byte message tag.
    BOOST_REQUIRE(msg.ParseFromArray(wire.data() + 1, static_cast<int>(wire.size() - 1)));
    BOOST_CHECK_EQUAL(msg.responder_id(), 42U);
    BOOST_REQUIRE(msg.has_responder());
    BOOST_CHECK_EQUAL(msg.responder().numeric(), 42U);

    const fetch_log_entries_response<aws_ec2_node_id> textual{
        ._responder_id = aws_ec2_node_id{"us-east-1", "i-1234abcd"},
        ._available = true,
        ._prev_log_term = 1,
        ._entries = {}};
    const auto text_wire = s.serialize(textual);
    BOOST_REQUIRE(msg.ParseFromArray(text_wire.data() + 1, static_cast<int>(text_wire.size() - 1)));
    BOOST_CHECK_EQUAL(msg.responder_id(), 0U);
    BOOST_CHECK_EQUAL(msg.responder().text(), "aws-ec2:us-east-1:i-1234abcd");
}

// A payload from a peer that predates `responder` carries only the legacy
// field: a numeric decoder falls back to it, a textual one refuses it.
BOOST_AUTO_TEST_CASE(legacy_payload_decodes_through_the_fallback) {
    const protobuf_rpc_serializer<bytes> s;
    const fetch_log_entries_response<> numeric{
        ._responder_id = 42, ._available = true, ._prev_log_term = 1, ._entries = {}};
    auto wire = s.serialize(numeric);
    raft_pb::FetchLogEntriesResponse msg;
    BOOST_REQUIRE(msg.ParseFromArray(wire.data() + 1, static_cast<int>(wire.size() - 1)));
    msg.clear_responder();
    const auto body = msg.SerializeAsString();
    bytes legacy(wire.begin(), wire.begin() + 1);
    for (const char c : body) {
        legacy.push_back(static_cast<std::byte>(c));
    }

    BOOST_CHECK_EQUAL(s.deserialize_fetch_log_entries_response(legacy).responder_id(), 42U);
    BOOST_CHECK_THROW(
        static_cast<void>(s.deserialize_fetch_log_entries_response<aws_ec2_node_id>(legacy)),
        std::exception);
}

BOOST_AUTO_TEST_SUITE_END()
#endif

#ifdef KYTHIRA_WIRE_TEST_HAS_ION
BOOST_AUTO_TEST_SUITE(ion)

BOOST_AUTO_TEST_CASE(composite_ids_round_trip) {
    round_trip_every_composite_type(ion_rpc_serializer<bytes>{});
}

BOOST_AUTO_TEST_CASE(foreign_text_is_refused) {
    refuses_foreign_text(ion_rpc_serializer<bytes>{});
}

BOOST_AUTO_TEST_CASE(composite_id_is_canonical_text) {
    travels_as_canonical_text(ion_rpc_serializer<bytes>{});
}

BOOST_AUTO_TEST_SUITE_END()
#endif

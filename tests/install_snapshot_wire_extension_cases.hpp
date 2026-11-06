// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Shared cases for the file-install-snapshot wire fields
// (.kiro/specs/file-install-snapshot task 1): InstallSnapshot request
// `format` and `manifest`, and response `bytes_received` and
// `reject_reason`. Every rpc_serializer runs the same cases, so the four
// encodings cannot drift apart on presence rules or bounds.
//
// Use from a Boost.Test translation unit after including the serializer:
//
//   KYTHIRA_INSTALL_SNAPSHOT_WIRE_CASES(kythira::cbor_serializer, cbor, true)
//
// The third argument says whether the encoding carries field names as
// text (JSON, CBOR and Ion do; protobuf uses field numbers), which lets
// the "default message omits the new fields" case look for the key.

#include <raft/exceptions.hpp>
#include <raft/types.hpp>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace kythira::test::install_snapshot_wire {

inline auto bytes_of(std::string_view s) -> std::vector<std::byte> {
    std::vector<std::byte> out;
    out.reserve(s.size());
    for (const char c : s) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

/// True when `needle` occurs anywhere in the encoded bytes.
inline auto contains_text(const std::vector<std::byte>& haystack, std::string_view needle) -> bool {
    const auto n = bytes_of(needle);
    return std::ranges::search(haystack, n).begin() != haystack.end();
}

inline auto base_request() -> install_snapshot_request<> {
    return install_snapshot_request<>{._term = 7,
                                      ._leader_id = 3,
                                      ._last_included_index = 1000,
                                      ._last_included_term = 6,
                                      ._offset = 4096,
                                      ._data = bytes_of("chunk-bytes"),
                                      ._done = false,
                                      ._group_id = 0};
}

template<typename Serializer> auto request_round_trip_with_manifest() -> void {
    Serializer s;
    auto req = base_request();
    req._format = install_snapshot_format::file_set;
    req._manifest = bytes_of("KYMF-manifest-payload");
    const auto back = s.deserialize_install_snapshot_request(s.serialize(req));
    BOOST_TEST(back.format() == install_snapshot_format::file_set);
    BOOST_TEST((back.manifest() == req.manifest()));
    BOOST_TEST(back.offset() == req.offset());
    BOOST_TEST((back.data() == req.data()));
    BOOST_TEST(back.done() == req.done());
}

template<typename Serializer> auto request_default_omits_new_fields(bool names_on_wire) -> void {
    Serializer s;
    const auto req = base_request();
    const auto wire = s.serialize(req);
    const auto back = s.deserialize_install_snapshot_request(wire);
    BOOST_TEST(back.format() == install_snapshot_format::byte_buffer);
    BOOST_TEST(back.manifest().empty());
    if (names_on_wire) {
        BOOST_TEST(!contains_text(wire, "format"));
        BOOST_TEST(!contains_text(wire, "manifest"));
    }
    // A format-1 request must encode longer, which proves the fields are
    // really written and the default really leaves them out.
    auto with = req;
    with._format = install_snapshot_format::file_set;
    BOOST_TEST(s.serialize(with).size() > wire.size());
}

template<typename Serializer> auto request_manifest_over_cap_is_refused() -> void {
    Serializer s;
    auto req = base_request();
    req._format = install_snapshot_format::file_set;
    req._manifest.assign(k_max_install_snapshot_manifest_bytes + 1, std::byte{0x5a});
    const auto wire = s.serialize(req);
    BOOST_CHECK_THROW((void)s.deserialize_install_snapshot_request(wire),
                      kythira::serialization_exception);

    // Exactly at the cap is accepted.
    req._manifest.resize(k_max_install_snapshot_manifest_bytes);
    const auto back = s.deserialize_install_snapshot_request(s.serialize(req));
    BOOST_TEST(back.manifest().size() == k_max_install_snapshot_manifest_bytes);
}

/// The smallest default request-size limit among the transports that carry
/// InstallSnapshot without their own block layer: 10 MiB for HTTP (httplib,
/// Beast and Proxygen `max_request_body_size`), against 16 MiB for gRPC
/// (`max_receive_message_size`) and 64 MiB for tcp_rpc (`k_max_frame_bytes`).
/// CoAP is configured separately (`coap_server_config::max_request_size`).
inline constexpr std::size_t k_smallest_default_transport_limit = 10U * 1024U * 1024U;

/// file-install-snapshot Requirement 2.3: the largest first chunk a node
/// sends with default settings, a full `snapshot_chunk_size` plus a manifest
/// at the cap, must fit every transport's default limit in every encoding
/// (JSON's base64 is the largest).
template<typename Serializer> auto worst_case_first_chunk_fits_transport_limits() -> void {
    Serializer s;
    auto req = base_request();
    req._offset = 0;
    req._format = install_snapshot_format::file_set;
    req._data.assign(raft_configuration{}.snapshot_chunk_size(), std::byte{0xA5});
    req._manifest.assign(k_max_install_snapshot_manifest_bytes, std::byte{0x5A});
    const auto wire = s.serialize(req);
    BOOST_TEST(wire.size() <= k_smallest_default_transport_limit,
               "worst-case first chunk encodes to " << wire.size() << " bytes");
}

template<typename Serializer> auto response_round_trip() -> void {
    Serializer s;
    install_snapshot_response<> resp{._term = 9, ._group_id = 0};
    resp._bytes_received = 123'456'789'012ULL;
    resp._reject_reason = install_snapshot_reject_reason::offset_mismatch;
    const auto back = s.deserialize_install_snapshot_response(s.serialize(resp));
    BOOST_TEST(back.term() == 9U);
    BOOST_REQUIRE(back.bytes_received().has_value());
    BOOST_TEST(*back.bytes_received() == 123'456'789'012ULL);
    BOOST_REQUIRE(back.reject_reason().has_value());
    BOOST_TEST((*back.reject_reason() == install_snapshot_reject_reason::offset_mismatch));

    // bytes_received of zero is a real value, distinct from absent.
    resp._bytes_received = 0;
    resp._reject_reason.reset();
    const auto zero = s.deserialize_install_snapshot_response(s.serialize(resp));
    BOOST_REQUIRE(zero.bytes_received().has_value());
    BOOST_TEST(*zero.bytes_received() == 0U);
    BOOST_TEST(!zero.reject_reason().has_value());
}

template<typename Serializer> auto response_default_omits_new_fields(bool names_on_wire) -> void {
    Serializer s;
    const install_snapshot_response<> resp{._term = 9, ._group_id = 0};
    const auto wire = s.serialize(resp);
    const auto back = s.deserialize_install_snapshot_response(wire);
    BOOST_TEST(!back.bytes_received().has_value());
    BOOST_TEST(!back.reject_reason().has_value());
    if (names_on_wire) {
        BOOST_TEST(!contains_text(wire, "bytes_received"));
        BOOST_TEST(!contains_text(wire, "reject_reason"));
    }
}

template<typename Serializer> auto response_unknown_reason_code_is_preserved() -> void {
    // A newer follower may send a reason this build does not name. The
    // code must survive decoding so the leader can log it, rather than
    // failing the whole response.
    Serializer s;
    install_snapshot_response<> resp{._term = 2, ._group_id = 0};
    resp._reject_reason = static_cast<install_snapshot_reject_reason>(200);
    const auto back = s.deserialize_install_snapshot_response(s.serialize(resp));
    BOOST_REQUIRE(back.reject_reason().has_value());
    BOOST_TEST(static_cast<int>(*back.reject_reason()) == 200);
}

}  // namespace kythira::test::install_snapshot_wire

#define KYTHIRA_INSTALL_SNAPSHOT_WIRE_CASES(SERIALIZER, PREFIX, NAMES_ON_WIRE)                  \
    BOOST_AUTO_TEST_CASE(PREFIX##_install_snapshot_request_manifest_round_trip) {               \
        ::kythira::test::install_snapshot_wire::request_round_trip_with_manifest<SERIALIZER>(); \
    }                                                                                           \
    BOOST_AUTO_TEST_CASE(PREFIX##_install_snapshot_request_default_omits_new_fields) {          \
        ::kythira::test::install_snapshot_wire::request_default_omits_new_fields<SERIALIZER>(   \
            NAMES_ON_WIRE);                                                                     \
    }                                                                                           \
    BOOST_AUTO_TEST_CASE(PREFIX##_install_snapshot_request_manifest_cap) {                      \
        ::kythira::test::install_snapshot_wire::request_manifest_over_cap_is_refused<           \
            SERIALIZER>();                                                                      \
    }                                                                                           \
    BOOST_AUTO_TEST_CASE(PREFIX##_install_snapshot_worst_case_first_chunk_fits) {               \
        ::kythira::test::install_snapshot_wire::worst_case_first_chunk_fits_transport_limits<   \
            SERIALIZER>();                                                                      \
    }                                                                                           \
    BOOST_AUTO_TEST_CASE(PREFIX##_install_snapshot_response_round_trip) {                       \
        ::kythira::test::install_snapshot_wire::response_round_trip<SERIALIZER>();              \
    }                                                                                           \
    BOOST_AUTO_TEST_CASE(PREFIX##_install_snapshot_response_default_omits_new_fields) {         \
        ::kythira::test::install_snapshot_wire::response_default_omits_new_fields<SERIALIZER>(  \
            NAMES_ON_WIRE);                                                                     \
    }                                                                                           \
    BOOST_AUTO_TEST_CASE(PREFIX##_install_snapshot_response_unknown_reason_code) {              \
        ::kythira::test::install_snapshot_wire::response_unknown_reason_code_is_preserved<      \
            SERIALIZER>();                                                                      \
    }

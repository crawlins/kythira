// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE IonMalformedMessagePropertyTest
#include <boost/test/unit_test.hpp>

#include <raft/ion_serializer.hpp>

#include "allocation_probe.hpp"

#include <algorithm>
#include <cstdint>
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

using bytes = std::vector<std::byte>;

// Binary Ion VarUInt: 7 bits per byte, most significant group first, the last
// byte flagged by its high bit.
auto var_uint(std::uint64_t value) -> bytes {
    bytes groups;
    do {
        groups.push_back(static_cast<std::byte>(value & 0x7FU));
        value >>= 7U;
    } while (value != 0);
    std::ranges::reverse(groups);
    groups.back() |= std::byte{0x80};
    return groups;
}

auto read_var_uint(const bytes& doc, std::size_t& pos) -> std::uint64_t {
    std::uint64_t value = 0;
    for (;;) {
        const auto b = std::to_integer<std::uint8_t>(doc.at(pos++));
        value = (value << 7U) | (b & 0x7FU);
        if ((b & 0x80U) != 0) {
            return value;
        }
    }
}

// A binary value's header (type descriptor plus any VarUInt length) at `pos`.
struct value_header {
    std::size_t size;      // header bytes
    std::uint64_t length;  // declared content bytes
};

auto read_header(const bytes& doc, std::size_t pos) -> value_header {
    const auto td = std::to_integer<std::uint8_t>(doc.at(pos));
    const unsigned type = td >> 4U;
    const unsigned low = td & 0x0FU;
    std::size_t cursor = pos + 1;
    std::uint64_t length = low;
    if (type == 1 || low == 15) {
        length = 0;
    } else if (low == 14 || (type == 13 && low == 1)) {
        length = read_var_uint(doc, cursor);
    }
    return {cursor - pos, length};
}

auto find(const bytes& doc, const bytes& needle) -> std::size_t {
    const auto it = std::ranges::search(doc, needle).begin();
    BOOST_REQUIRE(it != doc.end());
    return static_cast<std::size_t>(it - doc.begin());
}

/**
 * Rewrite the binary value whose header starts at `value_pos` so it declares
 * `declared` content bytes while the body stays as it was. With `reframe`,
 * the enclosing message struct and its annotation wrapper are re-declared to
 * cover the inflated value too, so only the end of input contradicts them;
 * without it, the value overruns its struct.
 */
auto inflate(const bytes& doc, std::size_t value_pos, std::uint64_t declared, bool reframe)
    -> bytes {
    const auto type = std::to_integer<std::uint8_t>(doc.at(value_pos)) & 0xF0U;
    const auto old_value = read_header(doc, value_pos);
    bytes value_header{static_cast<std::byte>(type | 0x0EU)};
    std::ranges::copy(var_uint(declared), std::back_inserter(value_header));

    bytes out(doc.begin(), doc.begin() + static_cast<std::ptrdiff_t>(value_pos));
    std::ranges::copy(value_header, std::back_inserter(out));
    out.insert(out.end(), doc.begin() + static_cast<std::ptrdiff_t>(value_pos + old_value.size),
               doc.end());
    if (!reframe) {
        return out;
    }

    // The message is the last top-level value: skip the version marker and
    // the local symbol table to find its annotation wrapper.
    std::size_t wrapper = 4;
    for (;;) {
        const auto h = read_header(doc, wrapper);
        if (wrapper + h.size + h.length == doc.size()) {
            break;
        }
        wrapper += h.size + h.length;
    }
    const auto old_wrapper = read_header(doc, wrapper);
    std::size_t cursor = wrapper + old_wrapper.size;
    const auto annotation_bytes = read_var_uint(doc, cursor);
    const std::size_t structure = cursor + annotation_bytes;
    const auto old_struct = read_header(doc, structure);
    BOOST_REQUIRE(value_pos > structure);

    const std::uint64_t growth =
        (value_header.size() - old_value.size) + (declared - old_value.length);
    bytes struct_header{std::byte{0xDE}};
    std::ranges::copy(var_uint(old_struct.length + growth), std::back_inserter(struct_header));
    const std::uint64_t wrapper_length =
        old_wrapper.length + growth + struct_header.size() - old_struct.size;
    bytes wrapper_header{std::byte{0xEE}};
    std::ranges::copy(var_uint(wrapper_length), std::back_inserter(wrapper_header));

    bytes framed(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(wrapper));
    std::ranges::copy(wrapper_header, std::back_inserter(framed));
    framed.insert(framed.end(),
                  out.begin() + static_cast<std::ptrdiff_t>(wrapper + old_wrapper.size),
                  out.begin() + static_cast<std::ptrdiff_t>(structure));
    std::ranges::copy(struct_header, std::back_inserter(framed));
    framed.insert(framed.end(),
                  out.begin() + static_cast<std::ptrdiff_t>(structure + old_struct.size),
                  out.end());
    return framed;
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

/**
 * Feature: json-serializer-input-validation, parity audit Z3
 * Validates: declared lengths are checked against the input before allocating
 *
 * Property: A binary Ion value whose header declares more content than the
 * body holds is rejected before anything is sized from that length. ion-c
 * bounds a value by its container only when it steps in, and a top-level
 * container by nothing, so before the preflight a 158-byte body declaring a
 * 1 GiB blob made the decoder allocate the full gigabyte. Ints and strings
 * are sized inside ion-c with malloc, which the probe cannot see, so the
 * rejection message shows the preflight caught them first.
 */
BOOST_AUTO_TEST_CASE(property_oversized_declared_length_rejected) {
    kythira::ion_rpc_serializer<bytes> serializer(kythira::ion_encoding::binary);
    constexpr std::uint64_t one_gib = std::uint64_t{1} << 30U;

    kythira::install_snapshot_request<> snapshot{};
    snapshot._term = 0x1234567890;
    snapshot._leader_id = 1;
    snapshot._data = bytes(8, std::byte{0x5A});
    const auto snapshot_doc = serializer.serialize(snapshot);

    bytes blob_needle{std::byte{0xA8}};
    blob_needle.insert(blob_needle.end(), 8, std::byte{0x5A});
    const auto blob_at = find(snapshot_doc, blob_needle);
    const auto term_at = find(snapshot_doc, {std::byte{0x25}, std::byte{0x12}, std::byte{0x34},
                                             std::byte{0x56}, std::byte{0x78}, std::byte{0x90}});

    kythira::cluster_join_request<> join{};
    join.node_id = 7;
    join.contact_address = "zzzzzzzz";
    const auto join_doc = serializer.serialize(join);
    bytes string_needle{std::byte{0x88}};
    string_needle.insert(string_needle.end(), 8, std::byte{'z'});
    const auto string_at = find(join_doc, string_needle);

    auto check = [](const char* label, const bytes& input, auto&& decode) {
        BOOST_TEST_CONTEXT(label) {
            std::size_t largest = 0;
            std::string what;
            {
                kythira::test::allocation_probe probe;
                try {
                    decode(input);
                } catch (const kythira::serialization_exception& e) {
                    what = e.what();
                }
                largest = kythira::test::allocation_probe::largest();
            }
            BOOST_CHECK_MESSAGE(
                what.find("declared length exceeds remaining input") != std::string::npos,
                "rejection: \"" << what << "\"");
            BOOST_CHECK_MESSAGE(
                largest <= 16 * input.size() + 4096,
                "largest allocation " << largest << " for " << input.size() << " input bytes");
        }
    };
    auto snapshot_decode = [&](const bytes& d) {
        (void)serializer.deserialize_install_snapshot_request(d);
    };
    auto join_decode = [&](const bytes& d) {
        (void)serializer.deserialize_cluster_join_request(d);
    };

    for (const bool reframe : {false, true}) {
        check(reframe ? "blob, reframed" : "blob", inflate(snapshot_doc, blob_at, one_gib, reframe),
              snapshot_decode);
        check(reframe ? "int, reframed" : "int", inflate(snapshot_doc, term_at, one_gib, reframe),
              snapshot_decode);
        check(reframe ? "string, reframed" : "string",
              inflate(join_doc, string_at, one_gib, reframe), join_decode);
    }
    // Declaring a single byte more than the input holds is enough to be
    // refused. (Unframed, one extra byte only borrows the next field's.)
    check("blob +1, reframed", inflate(snapshot_doc, blob_at, 9, true), snapshot_decode);

    // The preflight walks a well-formed binary message without objection.
    BOOST_CHECK_NO_THROW((void)serializer.deserialize_install_snapshot_request(snapshot_doc));
    BOOST_CHECK_NO_THROW((void)serializer.deserialize_cluster_join_request(join_doc));
}

/**
 * Feature: json-serializer-input-validation, parity audit Z3
 *
 * Property: Decoding a valid binary message never allocates more than a small
 * multiple of its size, whatever the payload length.
 */
BOOST_AUTO_TEST_CASE(property_decode_allocation_proportional_to_input) {
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<std::size_t> size_dist(0, 64 * 1024);
    kythira::ion_rpc_serializer<bytes> serializer(kythira::ion_encoding::binary);

    for (std::size_t i = 0; i < property_test_iterations / 10; ++i) {
        kythira::install_snapshot_request<> req{};
        req._term = 3;
        req._leader_id = 1;
        req._data = generate_random_bytes(rng, size_dist(rng));
        const auto doc = serializer.serialize(req);

        kythira::test::allocation_probe probe;
        const auto decoded = serializer.deserialize_install_snapshot_request(doc);
        BOOST_CHECK(decoded._data == req._data);
        BOOST_CHECK_LE(kythira::test::allocation_probe::largest(), 16 * doc.size() + 4096);
    }
}

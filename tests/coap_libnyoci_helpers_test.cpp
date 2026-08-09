#include "test_timeout_scale.hpp"
#define BOOST_TEST_MODULE coap_libnyoci_helpers_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

// Set test timeout to prevent hanging tests
#define BOOST_TEST_TIMEOUT (30 * KYTHIRA_TEST_TIMEOUT_SCALE)

// The pure helpers in `libnyoci_detail` -- URI assembly, Uri-Path splitting,
// CoAP unsigned option encoding and the inner-request builder. None of them
// touch libnyoci, all of them compile whether or not the backend was found, and
// none had a direct test: they were reached only through a round trip in a
// build that had libnyoci installed.
//
// build_request_uri is the one that most deserves this. Its whole job is to
// force the scheme to match the `secure` flag rather than trust the configured
// endpoint, because nyoci_outbound_set_uri() picks the session type out of the
// scheme -- so a helper that let "coap://" through on a DTLS client would send
// the request in the clear, and nothing else in the stack would notice.
//
// Deliberately does not include raft/coap_transport.hpp: libcoap's option
// macros rewrite libnyoci's option enum, so they cannot share a translation
// unit.
#include <raft/coap_transport_libnyoci_impl.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace detail = kythira::libnyoci_detail;
namespace osc = kythira::oscore;

namespace {

auto as_text(const std::vector<std::byte>& value) -> std::string {
    std::string out;
    out.reserve(value.size());
    for (const auto b : value) {
        out.push_back(static_cast<char>(b));
    }
    return out;
}

auto path_segments(const std::vector<osc::coap_option>& options) -> std::vector<std::string> {
    std::vector<std::string> segments;
    for (const auto& option : options) {
        if (option.number == 11) {
            segments.push_back(as_text(option.value));
        }
    }
    return segments;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(coap_libnyoci_helpers_tests)

// ── build_request_uri ──────────────────────────────────────────────────────

// The security-relevant case: the configured endpoint says "coap://", the
// client is secure, and the result must still be "coaps://". Trusting the
// endpoint string here would downgrade the request silently.
BOOST_AUTO_TEST_CASE(secure_forces_the_coaps_scheme_regardless_of_the_endpoint,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    BOOST_TEST(detail::build_request_uri("coap://127.0.0.1:5683", "/raft/vote", true) ==
               "coaps://127.0.0.1:5683/raft/vote");
    BOOST_TEST(detail::build_request_uri("coaps://127.0.0.1:5684", "/raft/vote", true) ==
               "coaps://127.0.0.1:5684/raft/vote");
    BOOST_TEST(detail::build_request_uri("127.0.0.1:5683", "/raft/vote", true) ==
               "coaps://127.0.0.1:5683/raft/vote");
}

// ...and the mirror image, so the flag is genuinely what decides, in both
// directions: a "coaps://" endpoint on a plaintext client is downgraded rather
// than quietly upgraded to a session the peer is not expecting.
BOOST_AUTO_TEST_CASE(insecure_forces_the_coap_scheme_regardless_of_the_endpoint,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    BOOST_TEST(detail::build_request_uri("coaps://127.0.0.1:5684", "/raft/vote", false) ==
               "coap://127.0.0.1:5684/raft/vote");
    BOOST_TEST(detail::build_request_uri("coap://127.0.0.1:5683", "/raft/vote", false) ==
               "coap://127.0.0.1:5683/raft/vote");
    BOOST_TEST(detail::build_request_uri("127.0.0.1:5683", "/raft/vote", false) ==
               "coap://127.0.0.1:5683/raft/vote");
}

// A trailing slash on the endpoint plus a leading slash on the path would
// otherwise produce "host//raft/vote", which is a different resource.
BOOST_AUTO_TEST_CASE(trailing_slashes_on_the_endpoint_are_stripped,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    BOOST_TEST(detail::build_request_uri("coap://127.0.0.1:5683/", "/raft/vote", false) ==
               "coap://127.0.0.1:5683/raft/vote");
    BOOST_TEST(detail::build_request_uri("coap://127.0.0.1:5683///", "/raft/vote", false) ==
               "coap://127.0.0.1:5683/raft/vote");
}

// IPv6 literals keep their brackets and their internal colons.
BOOST_AUTO_TEST_CASE(ipv6_literal_endpoints_survive_intact,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    BOOST_TEST(detail::build_request_uri("coap://[::1]:5683", "/raft/vote", false) ==
               "coap://[::1]:5683/raft/vote");
}

// ── uri_path_options / path_from_options ───────────────────────────────────

BOOST_AUTO_TEST_CASE(a_path_splits_into_one_option_per_segment,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    const auto segments = path_segments(detail::uri_path_options("/raft/append_entries"));
    BOOST_TEST_REQUIRE(segments.size() == 2u);
    BOOST_TEST(segments[0] == "raft");
    BOOST_TEST(segments[1] == "append_entries");
}

// Empty segments carry no meaning in a CoAP path, so repeated and trailing
// slashes must not produce empty Uri-Path options.
BOOST_AUTO_TEST_CASE(redundant_slashes_produce_no_empty_segments,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    for (const auto* path : {"//raft//vote//", "raft/vote", "/raft/vote/"}) {
        const auto segments = path_segments(detail::uri_path_options(path));
        BOOST_TEST_REQUIRE(segments.size() == 2u, "for path " << path);
        BOOST_TEST(segments[0] == "raft");
        BOOST_TEST(segments[1] == "vote");
    }
}

BOOST_AUTO_TEST_CASE(an_empty_path_produces_no_options,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    BOOST_TEST(detail::uri_path_options("").empty());
    BOOST_TEST(detail::uri_path_options("/").empty());
}

// The server dispatches on what path_from_options() returns, so it has to
// rebuild exactly the path the client asked for.
BOOST_AUTO_TEST_CASE(path_survives_the_split_and_rejoin,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    for (const auto* path : {"/raft/vote", "/raft/append_entries", "/a", "/a/b/c"}) {
        osc::coap_message message;
        message.options = detail::uri_path_options(path);
        BOOST_TEST(detail::path_from_options(message) == path);
    }
}

// path_from_options must ignore everything that is not a Uri-Path, or a
// Content-Format would end up spliced into the resource name.
BOOST_AUTO_TEST_CASE(only_uri_path_options_contribute_to_the_path,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    osc::coap_message message;
    message.options.push_back({3, {std::byte{'h'}}});  // Uri-Host
    message.options.push_back(
        {11, {std::byte{'r'}, std::byte{'a'}, std::byte{'f'}, std::byte{'t'}}});
    message.options.push_back({12, {std::byte{60}}});  // Content-Format
    message.options.push_back({17, {std::byte{60}}});  // Accept

    BOOST_TEST(detail::path_from_options(message) == "/raft");
}

// ── encode_uint_option / decode_uint_option ────────────────────────────────

// CoAP unsigned option values are minimum-length: zero is the *empty* value,
// not a zero byte (RFC 7252 Section 3.2).
BOOST_AUTO_TEST_CASE(zero_encodes_as_an_empty_option_value,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    BOOST_TEST(detail::encode_uint_option(0).empty());
    BOOST_TEST(detail::decode_uint_option({}) == 0u);
}

BOOST_AUTO_TEST_CASE(uint_options_use_minimum_length_encoding,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    BOOST_TEST(detail::encode_uint_option(1).size() == 1u);
    BOOST_TEST(detail::encode_uint_option(255).size() == 1u);
    BOOST_TEST(detail::encode_uint_option(256).size() == 2u);
    BOOST_TEST(detail::encode_uint_option(65535).size() == 2u);
}

BOOST_AUTO_TEST_CASE(uint_options_round_trip_across_the_boundaries,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    for (const std::uint16_t value :
         {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{60}, std::uint16_t{255},
          std::uint16_t{256}, std::uint16_t{11542}, std::uint16_t{65535}}) {
        BOOST_TEST(detail::decode_uint_option(detail::encode_uint_option(value)) == value,
                   "round trip failed for " << value);
    }
}

// ── build_inner_request ────────────────────────────────────────────────────

// Everything this builds ends up inside the OSCORE ciphertext, so the shape has
// to be right before protection rather than fixed up afterwards.
BOOST_AUTO_TEST_CASE(inner_request_is_a_post_carrying_path_format_and_accepts,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    const std::vector<std::uint16_t> accepts{60, 50};
    const std::vector<std::byte> payload{std::byte{0xDE}, std::byte{0xAD}};

    const auto message = detail::build_inner_request("/raft/vote", 60, accepts, payload);

    BOOST_TEST(message.code == 0x02);  // POST
    BOOST_TEST(message.has_payload);
    BOOST_TEST((message.payload == payload));

    BOOST_TEST((path_segments(message.options) == std::vector<std::string>{"raft", "vote"}));

    int content_formats = 0;
    int accept_count = 0;
    for (const auto& option : message.options) {
        if (option.number == 12) {
            ++content_formats;
            BOOST_TEST(detail::decode_uint_option(option.value) == 60u);
        }
        if (option.number == 17) {
            ++accept_count;
        }
    }
    BOOST_TEST(content_formats == 1);
    BOOST_TEST(accept_count == 2);
}

// Options go on the wire in ascending order, and the delta encoding depends on
// it -- an unsorted list would serialize to a different set of numbers.
BOOST_AUTO_TEST_CASE(inner_request_options_are_sorted_by_number,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    const std::vector<std::uint16_t> accepts{60, 50, 11542};
    const std::vector<std::byte> payload{std::byte{0x01}};

    const auto message = detail::build_inner_request("/raft/append_entries", 60, accepts, payload);

    for (std::size_t i = 1; i < message.options.size(); ++i) {
        BOOST_TEST(message.options[i - 1].number <= message.options[i].number,
                   "options out of order at index " << i);
    }
}

// The relative order of the Accept options is the preference order, and
// stable_sort is what preserves it -- reordering them would change which
// serializer the peer picks.
BOOST_AUTO_TEST_CASE(accept_preference_order_is_preserved,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    const std::vector<std::uint16_t> accepts{11542, 60, 50};
    const auto message = detail::build_inner_request("/raft/vote", 60, accepts, {});

    std::vector<std::uint32_t> seen;
    for (const auto& option : message.options) {
        if (option.number == 17) {
            seen.push_back(detail::decode_uint_option(option.value));
        }
    }
    BOOST_TEST((seen == std::vector<std::uint32_t>{11542, 60, 50}));
}

// An empty body must not be advertised as a payload: the 0xFF marker would
// otherwise appear with nothing behind it.
BOOST_AUTO_TEST_CASE(an_empty_payload_is_not_marked_as_present,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    const auto message = detail::build_inner_request("/raft/vote", 60, {}, {});
    BOOST_TEST(!message.has_payload);
    BOOST_TEST(message.payload.empty());
}

BOOST_AUTO_TEST_SUITE_END()

#define BOOST_TEST_MODULE CoapContentNegotiationUnitTest
#include <boost/test/unit_test.hpp>

#include <raft/coap_utils.hpp>
#include <raft/serializer_registry.hpp>
#include <raft/json_serializer.hpp>
#include <raft/cbor_serializer.hpp>

// coap_utils.hpp deliberately compiles with and without libcoap, so it does not
// pull in the C API. The varint case below calls it directly.
#ifdef LIBCOAP_AVAILABLE
#include <coap3/coap.h>
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>

using namespace kythira;

namespace {

using json_serializer = json_rpc_serializer<std::vector<std::byte>>;
using cbor_serializer = cbor_rpc_serializer<std::vector<std::byte>>;

using json_registry = single_serializer_registry<json_serializer>;
using json_cbor_registry = multi_serializer_registry<json_serializer, cbor_serializer>;

}  // namespace

/**
 * Covers the two pieces of CoAP negotiation that are protocol-independent and
 * therefore testable without standing up a libcoap client and server: the
 * media-type <-> Content-Format mapping the wire format is built on, and the
 * variable-length integer encoding those option values must use.
 *
 * The end-to-end path is exercised by `coap_cbor_end_to_end_test`, which does a
 * real client-to-server round trip. That was confirmed by mutation, not
 * assumed: putting a bogus Content-Format on the client's request makes it fail
 * while `coap_integration_test`, `coap_post_method_property_test` and
 * `coap_content_format_property_test` all still pass. Those three do not
 * negotiate, which is precisely why the cases below exist.
 */
BOOST_AUTO_TEST_SUITE(coap_content_negotiation)

/**
 * The registry-scoped inverse lookup must round-trip every media type the
 * registry offers.
 *
 * This is the operation both the client's response path and the server's
 * request path depend on: a number arrives on the wire and has to become the
 * one media type this registry can decode.
 */
BOOST_AUTO_TEST_CASE(content_format_round_trips_through_the_registry,
                     *boost::unit_test::timeout(30)) {
    const json_cbor_registry registry;

    for (const auto& media_type : registry.preferred_media_types()) {
        const auto format = coap_utils::media_type_to_coap_content_format(media_type);
        BOOST_REQUIRE_MESSAGE(format.has_value(),
                              "no CoAP Content-Format for registered media type " + media_type);

        const auto resolved = coap_utils::registry_media_type_for_content_format(registry, *format);
        BOOST_REQUIRE(resolved.has_value());
        BOOST_CHECK_EQUAL(*resolved, media_type);
    }
}

/**
 * A Content-Format this registry has no serializer for must resolve to
 * `nullopt` rather than to a plausible-looking default.
 *
 * `nullopt` is what drives CoAP 4.15 on the server and
 * `Unsupported_Media_Type_Error` on the client. Returning a default here
 * instead would turn "I cannot read this" into "I read it as JSON", which
 * hands the caller a decoded-from-garbage message that looks like a successful
 * RPC — the failure the `std::optional` return type exists to prevent.
 */
BOOST_AUTO_TEST_CASE(unregistered_content_format_resolves_to_nullopt,
                     *boost::unit_test::timeout(30)) {
    const json_registry registry;  // JSON only

    // CBOR (60) is a real Content-Format, just not one this registry can decode.
    const auto cbor = coap_utils::registry_media_type_for_content_format(
        registry, coap_utils::coap_content_format::application_cbor);
    BOOST_CHECK(!cbor.has_value());

    // A number no registry maps at all.
    const auto nonsense = coap_utils::registry_media_type_for_content_format(
        registry, static_cast<coap_utils::coap_content_format>(9999));
    BOOST_CHECK(!nonsense.has_value());

    // ...while the one type it does hold still resolves.
    const auto json = coap_utils::registry_media_type_for_content_format(
        registry, coap_utils::coap_content_format::application_json);
    BOOST_REQUIRE(json.has_value());
    BOOST_CHECK_EQUAL(*json, registry.default_media_type());
}

#ifdef LIBCOAP_AVAILABLE
/**
 * Option values must be CoAP variable-length integers, not the raw bytes of a
 * `uint16_t`.
 *
 * This pins a bug the negotiation work had to fix rather than a new invariant.
 * Every Content-Format option in this transport was written as
 * `coap_add_option(..., sizeof(uint16_t), (const uint8_t*)&value)`, which puts
 * the host-order bytes of the integer on the wire: on a little-endian box
 * Content-Format 60 (CBOR) went out as 0x3C 0x00 and decodes as 15360. Nothing
 * noticed because nothing ever *read* the option — both ends assumed their own
 * single serializer. Reading it is exactly what negotiation does.
 *
 * Asserted as encode-then-decode, which is the property that actually matters
 * and the one the old code failed.
 */
BOOST_AUTO_TEST_CASE(content_format_option_encodes_as_a_coap_varint,
                     *boost::unit_test::timeout(30)) {
    for (const auto format : {coap_utils::coap_content_format::text_plain,
                              coap_utils::coap_content_format::application_json,
                              coap_utils::coap_content_format::application_cbor,
                              coap_utils::coap_content_format::application_ion}) {
        const auto value = static_cast<std::uint16_t>(format);

        std::array<std::uint8_t, 2> buf{};
        const auto len =
            coap_encode_var_safe(buf.data(), buf.size(), static_cast<unsigned int>(value));

        // No lower bound on `len`: CoAP strips leading zero bytes, so
        // Content-Format 0 (text/plain) is a *zero-length* option value, and an
        // empty option means 0. An earlier draft of this case asserted
        // `len > 0` and failed here — the assertion was wrong, not the
        // encoding. Two bytes is the ceiling instead, which is what makes the
        // fixed-size buffer in the transport safe.
        BOOST_CHECK_LE(len, buf.size());

        // The property that matters, and the one the old raw-uint16_t write
        // failed: what we put on the wire reads back as the number we meant.
        BOOST_CHECK_EQUAL(coap_decode_var_bytes(buf.data(), len), value);
    }
}
#endif

BOOST_AUTO_TEST_SUITE_END()

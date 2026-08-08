#pragma once

#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <chrono>
#include <stdexcept>
#include <utility>
#include <raft/coap_exceptions.hpp>

namespace kythira {

/// @brief Maximum CoAP token length, in bytes.
///
/// RFC 7252 §5.3.1 gives the Token Length field 4 bits and reserves values
/// 9-15, so a token is 0-8 bytes. libcoap enforces this in coap_send(), which
/// *drops* an over-long PDU rather than failing the call that built it -- so
/// exceeding this is silent at the point of use and has to be prevented at the
/// point of generation.
///
/// Declared here, rather than used from `coap3/coap_pdu.h`, because the token
/// helpers below and coap_client::generate_message_token() are all compiled in
/// the stub build too, where libcoap's headers are not included at all.
/// coap_transport.hpp static_asserts this against COAP_TOKEN_DEFAULT_MAX
/// wherever libcoap is actually present, so the two cannot drift.
inline constexpr std::size_t coap_max_token_length = 8;

namespace coap_utils {

// Endpoint parsing utilities
struct parsed_endpoint {
    std::string scheme;  // "coap" or "coaps"
    std::string host;    // hostname or IP address
    std::uint16_t port;  // port number
    std::string path;    // resource path (optional)

    parsed_endpoint() = default;
    parsed_endpoint(std::string s, std::string h, std::uint16_t p, std::string path = "")
        : scheme(std::move(s)), host(std::move(h)), port(p), path(std::move(path)) {}
};

inline auto parse_coap_endpoint(const std::string& endpoint) -> parsed_endpoint {
    parsed_endpoint result;

    if (endpoint.empty()) {
        throw coap_network_error("Empty endpoint");
    }

    // Parse scheme
    std::string rest;
    if (endpoint.find("coaps://") == 0) {
        result.scheme = "coaps";
        rest = endpoint.substr(8);  // Remove "coaps://"
    } else if (endpoint.find("coap://") == 0) {
        result.scheme = "coap";
        rest = endpoint.substr(7);  // Remove "coap://"
    } else {
        throw coap_network_error("Invalid scheme - must be coap:// or coaps://");
    }

    // Parse path if present
    auto slash_pos = rest.find('/');
    std::string host_port;
    if (slash_pos != std::string::npos) {
        host_port = rest.substr(0, slash_pos);
        result.path = rest.substr(slash_pos);  // Include the leading slash
    } else {
        host_port = rest;
    }

    // Parse host and port
    auto colon_pos = host_port.find(':');
    if (colon_pos != std::string::npos) {
        result.host = host_port.substr(0, colon_pos);
        auto port_str = host_port.substr(colon_pos + 1);
        try {
            int port_int = std::stoi(port_str);
            if (port_int <= 0 || port_int > 65535) {
                throw coap_network_error("Invalid port number - must be 1-65535");
            }
            result.port = static_cast<std::uint16_t>(port_int);
        } catch (const std::exception&) {
            throw coap_network_error("Invalid port number");
        }
    } else {
        result.host = host_port;
        result.port = (result.scheme == "coaps") ? 5684 : 5683;  // Default ports
    }

    return result;
}

inline auto format_coap_endpoint(const parsed_endpoint& endpoint) -> std::string {
    if (endpoint.scheme != "coap" && endpoint.scheme != "coaps") {
        throw coap_network_error("Invalid scheme - must be coap or coaps");
    }
    if (endpoint.host.empty()) {
        throw coap_network_error("Empty host");
    }
    if (endpoint.port == 0) {
        throw coap_network_error("Invalid port - must be non-zero");
    }

    return endpoint.scheme + "://" + endpoint.host + ":" + std::to_string(endpoint.port) +
           endpoint.path;
}

inline auto is_valid_coap_endpoint(const std::string& endpoint) -> bool {
    if (endpoint.empty()) {
        return false;
    }

    if (endpoint.find("coap://") != 0 && endpoint.find("coaps://") != 0) {
        return false;
    }

    // Try to parse and validate
    try {
        auto parsed = parse_coap_endpoint(endpoint);
        return parsed.port > 0 && parsed.port <= 65535 && !parsed.host.empty();
    } catch (...) {
        return false;
    }
}

// Token generation utilities
inline auto generate_coap_token(std::size_t length = 4) -> std::vector<std::byte> {
    if (length == 0) {
        throw coap_transport_error("Token length must be at least 1");
    }
    if (length > coap_max_token_length) {
        throw coap_transport_error("Token length must be at most " +
                                   std::to_string(coap_max_token_length) + " bytes");
    }

    std::vector<std::byte> token(length);

    // Use current time and random number for token generation
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<std::uint8_t> dis(0, 255);

    for (std::size_t i = 0; i < length; ++i) {
        token[i] = static_cast<std::byte>(dis(gen));
    }

    return token;
}

inline auto is_valid_coap_token(const std::vector<std::byte>& token) -> bool {
    return token.size() <= coap_max_token_length;
}

/// @brief Render a request counter as a fixed-width, printable CoAP token.
///
/// The result is *always* exactly `coap_max_token_length` bytes, for every
/// input. That invariant is the whole point: the previous scheme built tokens
/// as `"token_" + std::to_string(n)`, which is 7 bytes at n=0 and 9 bytes from
/// n=100 onward. Nothing rejected the over-long token -- coap_add_token()
/// accepted it and coap_send() silently dropped the PDU -- so a client stopped
/// transmitting after its 100th request with only a libcoap warning to show
/// for it. A width that cannot vary with the counter removes that failure mode
/// by construction rather than by check.
///
/// Lowercase hex keeps tokens printable, which matters because the transport
/// logs the token on ~45 paths and uses it as an `_pending_requests` map key.
/// Most-significant nibble first, so lexicographic order matches numeric order
/// when reading logs.
inline auto format_sequential_token(std::uint32_t counter) -> std::string {
    // Eight hex digits carry exactly the 32 bits of `counter`. If the cap ever
    // changes, this mapping has to be revisited rather than silently truncate.
    static_assert(coap_max_token_length == 8,
                  "format_sequential_token encodes a uint32_t as exactly 8 hex digits");
    static constexpr char hex_digits[] = "0123456789abcdef";
    std::string token(coap_max_token_length, '0');
    for (std::size_t i = 0; i < coap_max_token_length; ++i) {
        const auto shift = 4U * static_cast<unsigned>(coap_max_token_length - 1U - i);
        token[i] = hex_digits[(counter >> shift) & 0xFU];
    }
    return token;
}

// CoAP option handling utilities
enum class coap_content_format : std::uint16_t {  // NOLINT(performance-enum-size)
    text_plain = 0,
    application_link_format = 40,
    application_xml = 41,
    application_octet_stream = 42,
    application_exi = 47,
    application_json = 50,
    application_cbor = 60,
    // No IANA-registered CoAP Content-Format exists for Amazon Ion, so this
    // uses a value from RFC 7252 §12.3's experimental/private-use range
    // (65000-65535). A single value covers both binary and text Ion: Ion's
    // binary version marker makes the two encodings self-describing to a
    // receiver, so no separate number per encoding is needed
    // (ion-rpc-serializer spec, Requirement 6.3). If IANA ever registers an
    // official Ion Content-Format, migrate to it (design.md Future Enhancements).
    application_ion = 65000
};

/// @brief CoAP Content-Format for a media type, or `nullopt` if none is defined.
///
/// Supersedes `get_content_format_for_serializer`, which keys off `name()` — a
/// free-form human/metrics label — via substring search, and answers
/// `application_cbor` for anything it does not recognise. Both halves of that
/// are wrong for negotiation. Substring search means a serializer named
/// `"json-lines"` silently claims Content-Format 50, and the CBOR default means
/// an unknown serializer does not fail: it puts a confidently wrong number on
/// the wire and the peer decodes garbage. Returning `std::optional` forces the
/// caller to have an answer for "no mapping exists" rather than being handed a
/// plausible one.
///
/// Keying off `media_type()` also makes this the same lookup HTTP does — one
/// negotiated token, two protocol encodings of it — instead of two independent
/// pieces of guesswork that can disagree about the same serializer.
///
/// Note this is *not* the inverse of `content_format_to_string`, which renders
/// `application_ion` as `"application/ion"`; no serializer reports that string.
/// Nor is it injective: Ion's binary and text encodings share Content-Format
/// 65000 (see the enumerator's own comment), which is why
/// `validate_registry_content_formats` below rejects a registry holding both.
[[nodiscard]] inline auto media_type_to_coap_content_format(const std::string& media_type)
    -> std::optional<coap_content_format> {
    // An explicit table, not substring matching: every entry here is a media
    // type some serializer actually reports, and an exact-match table is the
    // only form in which "this media type has no CoAP number" is expressible.
    if (media_type == "application/json") {
        return coap_content_format::application_json;
    }
    if (media_type == "application/cbor") {
        return coap_content_format::application_cbor;
    }
    if (media_type == "application/x-protobuf") {
        // No IANA CoAP Content-Format is registered for Protocol Buffers, so it
        // travels as generic binary (42) rather than being mislabelled. A peer
        // therefore cannot tell protobuf from any other octet-stream by the
        // Content-Format alone — acceptable because a CoAP registry that offers
        // protobuf alongside another octet-stream format is rejected at
        // construction (see `validate_registry_content_formats`) rather than
        // left to guess at runtime.
        return coap_content_format::application_octet_stream;
    }
    // Both Ion encodings share the one private-use number; see the enumerator.
    if (media_type == "application/x-amzn-ion" || media_type == "text/x-amzn-ion") {
        return coap_content_format::application_ion;
    }
    if (media_type == "application/xml") {
        return coap_content_format::application_xml;
    }
    if (media_type == "text/plain") {
        return coap_content_format::text_plain;
    }
    if (media_type == "application/link-format") {
        return coap_content_format::application_link_format;
    }
    if (media_type == "application/octet-stream") {
        return coap_content_format::application_octet_stream;
    }
    if (media_type == "application/exi") {
        return coap_content_format::application_exi;
    }
    return std::nullopt;
}

/// @brief Reject a serializer registry that CoAP cannot faithfully represent.
///
/// Two ways a registry can be unusable over CoAP, both of which are
/// configuration mistakes that must surface at construction rather than as
/// mis-decoded payloads on the wire:
///
///  1. A registered media type has no CoAP Content-Format at all. There is no
///     option value to put on the request, and the alternative — omitting the
///     option — means "default", which is a different encoding.
///  2. Two registered media types map to the *same* Content-Format. CoAP
///     negotiates by number, so the receiver cannot tell which of the two the
///     sender meant. Ion binary + Ion text in one registry is the case that is
///     reachable with the serializers shipped today; protobuf beside a future
///     `application/octet-stream` serializer would be the second, which is why
///     this is a general check rather than an Ion special case.
///
/// HTTP has neither problem, since it negotiates on the media-type string
/// itself — which is why this check is CoAP's and not the registry's own.
///
/// @throws coap_unsupported_content_format_error naming the offending media
///         type — and, for a collision, both of the media types involved.
template<typename Registry>
inline void validate_registry_content_formats(const Registry& registry) {
    const auto media_types = registry.preferred_media_types();
    std::vector<std::pair<coap_content_format, std::string>> seen;
    seen.reserve(media_types.size());

    for (const auto& media_type : media_types) {
        const auto format = media_type_to_coap_content_format(media_type);
        if (!format) {
            throw coap_unsupported_content_format_error(media_type);
        }
        for (const auto& [prior_format, prior_media_type] : seen) {
            if (prior_format == *format) {
                // Names both sides: knowing only the loser makes a collision
                // between two legitimate-looking entries needlessly hard to act
                // on, since the fix is to drop one of the *pair*.
                throw coap_unsupported_content_format_error(
                    media_type + " collides with " + prior_media_type + " on CoAP Content-Format " +
                    std::to_string(static_cast<std::uint16_t>(*format)));
            }
        }
        seen.emplace_back(*format, media_type);
    }
}

/// @brief The registry's media type for a Content-Format seen on the wire, or
///     `nullopt` if this registry has no serializer for it.
///
/// The inverse of `media_type_to_coap_content_format`, resolved *through the
/// registry* rather than by a second hardcoded table. That is deliberate: the
/// forward table is not injective — Ion binary and Ion text share 65000,
/// protobuf and octet-stream share 42 — so a standalone reverse table would
/// have to invent an answer for those numbers. Scanning the registry cannot,
/// because `validate_registry_content_formats` has already rejected any
/// registry holding two media types that collide on one number. Within a
/// registry that passed construction, the mapping is a bijection, and this
/// returns the one media type that registry can actually decode.
///
/// `nullopt` is the negotiation-failure case, not an error to swallow: on the
/// server it is CoAP 4.15, on the client it is `Unsupported_Media_Type_Error`.
template<typename Registry>
[[nodiscard]] inline auto registry_media_type_for_content_format(const Registry& registry,
                                                                 coap_content_format format)
    -> std::optional<std::string> {
    for (const auto& media_type : registry.preferred_media_types()) {
        if (media_type_to_coap_content_format(media_type) == format) {
            return media_type;
        }
    }
    return std::nullopt;
}

/// @deprecated Superseded by `media_type_to_coap_content_format`. Retained only
///     for the three call sites that still pass a serializer `name()`
///     (`coap_transport_impl.hpp`, `beast_http_transport_impl.hpp`); do not
///     extend it or add entries — add them to the media-type table instead.
///     Its substring matching and silent `application_cbor` fallback are the
///     behaviours the replacement exists to remove.
inline auto get_content_format_for_serializer(const std::string& serializer_name)
    -> coap_content_format {
    // Check for Ion variants ("ion-binary"/"ion-text") first. Checked before
    // JSON: "json" and "ion" never collide as substrings, so order is safe.
    if (serializer_name.find("ion") != std::string::npos ||
        serializer_name.find("ION") != std::string::npos) {
        return coap_content_format::application_ion;
    }
    // Check for JSON variants
    if (serializer_name.find("json") != std::string::npos ||
        serializer_name.find("JSON") != std::string::npos) {
        return coap_content_format::application_json;
    }
    // Check for CBOR variants
    if (serializer_name.find("cbor") != std::string::npos ||
        serializer_name.find("CBOR") != std::string::npos) {
        return coap_content_format::application_cbor;
    }
    // Check for Protocol Buffers variants (protobuf_rpc_serializer::name() ==
    // "protobuf"). No IANA CoAP content-format is registered for protobuf, so
    // label it as the generic binary octet-stream (registered format 42) rather
    // than mislabeling it as CBOR -- correct and interoperable on both the HTTP
    // (application/octet-stream Content-Type) and CoAP wire.
    if (serializer_name.find("protobuf") != std::string::npos ||
        serializer_name.find("PROTOBUF") != std::string::npos) {
        return coap_content_format::application_octet_stream;
    }
    // Check for XML
    if (serializer_name == "xml" || serializer_name == "XML") {
        return coap_content_format::application_xml;
    }
    // Check for text/plain
    if (serializer_name == "text" || serializer_name == "TEXT") {
        return coap_content_format::text_plain;
    }

    // Default to CBOR for unknown serializers
    return coap_content_format::application_cbor;
}

inline auto content_format_to_string(coap_content_format format) -> std::string {
    switch (format) {
        case coap_content_format::text_plain:
            return "text/plain";
        case coap_content_format::application_link_format:
            return "application/link-format";
        case coap_content_format::application_xml:
            return "application/xml";
        case coap_content_format::application_octet_stream:
            return "application/octet-stream";
        case coap_content_format::application_exi:
            return "application/exi";
        case coap_content_format::application_json:
            return "application/json";
        case coap_content_format::application_cbor:
            return "application/cbor";
        case coap_content_format::application_ion:
            return "application/ion";
        default:
            return "application/octet-stream";
    }
}

inline auto parse_content_format(std::uint16_t format_value) -> coap_content_format {
    // Validate that the format value is one of the known formats
    switch (format_value) {
        case 0:      // text_plain
        case 40:     // application_link_format
        case 41:     // application_xml
        case 42:     // application_octet_stream
        case 47:     // application_exi
        case 50:     // application_json
        case 60:     // application_cbor
        case 65000:  // application_ion (private-use range, RFC 7252 §12.3)
            return static_cast<coap_content_format>(format_value);
        default:
            throw coap_protocol_error("Unknown content format value: " +
                                      std::to_string(format_value));
    }
}

// Block option utilities
inline auto calculate_block_size_szx(std::size_t block_size) -> std::uint8_t {
    // Validate block size
    if (block_size < 16) {
        throw coap_transport_error("Block size must be at least 16 bytes");
    }
    if (block_size > 1024) {
        throw coap_transport_error("Block size must be at most 1024 bytes");
    }
    if ((block_size & (block_size - 1)) != 0) {
        throw coap_transport_error("Block size must be a power of 2");
    }

    // Calculate SZX from block size (SZX = log2(block_size) - 4)
    std::uint8_t szx = 0;
    std::size_t size = block_size;
    while (size > 16) {
        size >>= 1;
        szx++;
    }
    return szx;
}

inline auto szx_to_block_size(std::uint8_t szx) -> std::size_t {
    // Validate SZX value
    if (szx > 6) {
        throw coap_transport_error("SZX value must be 0-6 (block sizes 16-1024)");
    }

    // Convert SZX to block size (block_size = 2^(SZX+4))
    return 1U << (szx + 4);
}

inline auto is_valid_block_size(std::size_t block_size) -> bool {
    // Valid block sizes are powers of 2 from 16 to 1024
    return block_size >= 16 && block_size <= 1024 && (block_size & (block_size - 1)) == 0;
}

}  // namespace coap_utils
}  // namespace kythira
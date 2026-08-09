#include "test_timeout_scale.hpp"
#define BOOST_TEST_MODULE oscore_rfc8613_vectors_test
#include <boost/test/unit_test.hpp>

// Set test timeout to prevent hanging tests
#define BOOST_TEST_TIMEOUT (60 * KYTHIRA_TEST_TIMEOUT_SCALE)

#include <raft/oscore.hpp>

#include <string>
#include <vector>

// Every expected value in this file is copied verbatim from RFC 8613
// Appendix C. They are the reason the OSCORE implementation is trustworthy at
// all: the constructions in Sections 3.2.1, 5.2 and 5.4 are easy to implement
// plausibly and wrongly, and only the published vectors distinguish the two.
//
// If a change here makes a vector fail, the change is wrong -- not the vector.

namespace {

[[nodiscard]] auto from_hex(std::string_view hex) -> std::vector<std::byte> {
    BOOST_REQUIRE_MESSAGE(hex.size() % 2 == 0, "hex literal must have an even length");
    std::vector<std::byte> out;
    out.reserve(hex.size() / 2);
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1]);
        BOOST_REQUIRE_MESSAGE(hi >= 0 && lo >= 0, "invalid hex literal");
        out.push_back(static_cast<std::byte>((hi << 4) | lo));
    }
    return out;
}

[[nodiscard]] auto to_hex(std::span<const std::byte> bytes) -> std::string {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto b : bytes) {
        const auto v = static_cast<unsigned>(b);
        out.push_back(digits[(v >> 4U) & 0x0FU]);
        out.push_back(digits[v & 0x0FU]);
    }
    return out;
}

/// Derives the three context values the way RFC 8613 Section 3.2.1 does, so
/// each vector below reads as inputs-in, published-outputs-out.
struct derived_context {
    std::vector<std::byte> sender_key;
    std::vector<std::byte> recipient_key;
    std::vector<std::byte> common_iv;
    std::vector<std::byte> sender_info;
    std::vector<std::byte> recipient_info;
    std::vector<std::byte> iv_info;
};

[[nodiscard]] auto derive(std::string_view master_secret_hex, std::string_view master_salt_hex,
                          std::string_view sender_id_hex, std::string_view recipient_id_hex,
                          std::string_view id_context_hex = "") -> derived_context {
    namespace osc = kythira::oscore;
    const auto secret = from_hex(master_secret_hex);
    const auto salt = from_hex(master_salt_hex);
    const auto sender_id = from_hex(sender_id_hex);
    const auto recipient_id = from_hex(recipient_id_hex);
    const auto id_context = from_hex(id_context_hex);

    derived_context out;
    out.sender_info = osc::build_info(sender_id, id_context, osc::aead_alg_aes_ccm_16_64_128, "Key",
                                      osc::aead_key_length);
    out.recipient_info = osc::build_info(recipient_id, id_context, osc::aead_alg_aes_ccm_16_64_128,
                                         "Key", osc::aead_key_length);
    out.iv_info = osc::build_info({}, id_context, osc::aead_alg_aes_ccm_16_64_128, "IV",
                                  osc::aead_nonce_length);
    out.sender_key = osc::hkdf_sha256(salt, secret, out.sender_info, osc::aead_key_length);
    out.recipient_key = osc::hkdf_sha256(salt, secret, out.recipient_info, osc::aead_key_length);
    out.common_iv = osc::hkdf_sha256(salt, secret, out.iv_info, osc::aead_nonce_length);
    return out;
}

constexpr std::string_view master_secret = "0102030405060708090a0b0c0d0e0f10";

}  // namespace

BOOST_AUTO_TEST_SUITE(oscore_rfc8613_vectors)

// ── C.1: key derivation with a Master Salt ─────────────────────────────────

BOOST_AUTO_TEST_CASE(test_c1_1_client_key_derivation) {
    const auto ctx = derive(master_secret, "9e7ca92223786340", "", "01");

    BOOST_TEST(to_hex(ctx.sender_info) == "8540f60a634b657910");
    BOOST_TEST(to_hex(ctx.recipient_info) == "854101f60a634b657910");
    BOOST_TEST(to_hex(ctx.iv_info) == "8540f60a6249560d");

    BOOST_TEST(to_hex(ctx.sender_key) == "f0910ed7295e6ad4b54fc793154302ff");
    BOOST_TEST(to_hex(ctx.recipient_key) == "ffb14e093c94c9cac9471648b4f98710");
    BOOST_TEST(to_hex(ctx.common_iv) == "4622d4dd6d944168eefb54987c");
}

BOOST_AUTO_TEST_CASE(test_c1_1_client_nonces) {
    namespace osc = kythira::oscore;
    const auto common_iv = from_hex("4622d4dd6d944168eefb54987c");
    const auto zero_piv = from_hex("00");

    // Sender ID is empty for the client, 0x01 for the server it talks to.
    BOOST_TEST(to_hex(osc::compute_nonce(common_iv, from_hex(""), zero_piv)) ==
               "4622d4dd6d944168eefb54987c");
    BOOST_TEST(to_hex(osc::compute_nonce(common_iv, from_hex("01"), zero_piv)) ==
               "4722d4dd6d944169eefb54987c");
}

BOOST_AUTO_TEST_CASE(test_c1_2_server_key_derivation) {
    // The server's context is the client's with the two IDs swapped, so its
    // Sender Key must equal the client's Recipient Key and vice versa.
    const auto ctx = derive(master_secret, "9e7ca92223786340", "01", "");

    BOOST_TEST(to_hex(ctx.sender_info) == "854101f60a634b657910");
    BOOST_TEST(to_hex(ctx.recipient_info) == "8540f60a634b657910");

    BOOST_TEST(to_hex(ctx.sender_key) == "ffb14e093c94c9cac9471648b4f98710");
    BOOST_TEST(to_hex(ctx.recipient_key) == "f0910ed7295e6ad4b54fc793154302ff");
    BOOST_TEST(to_hex(ctx.common_iv) == "4622d4dd6d944168eefb54987c");
}

// ── C.2: key derivation without a Master Salt ──────────────────────────────
// The interesting case for the HKDF-Extract salt default: an absent salt is
// the empty byte string, which RFC 5869 turns into a block of zeros.

BOOST_AUTO_TEST_CASE(test_c2_1_client_key_derivation_without_salt) {
    const auto ctx = derive(master_secret, "", "00", "01");

    BOOST_TEST(to_hex(ctx.sender_info) == "854100f60a634b657910");
    BOOST_TEST(to_hex(ctx.recipient_info) == "854101f60a634b657910");
    BOOST_TEST(to_hex(ctx.iv_info) == "8540f60a6249560d");

    BOOST_TEST(to_hex(ctx.sender_key) == "321b26943253c7ffb6003b0b64d74041");
    BOOST_TEST(to_hex(ctx.recipient_key) == "e57b5635815177cd679ab4bcec9d7dda");
    BOOST_TEST(to_hex(ctx.common_iv) == "be35ae297d2dace910c52e99f9");
}

BOOST_AUTO_TEST_CASE(test_c2_2_server_key_derivation_without_salt) {
    const auto ctx = derive(master_secret, "", "01", "00");

    BOOST_TEST(to_hex(ctx.sender_key) == "e57b5635815177cd679ab4bcec9d7dda");
    BOOST_TEST(to_hex(ctx.recipient_key) == "321b26943253c7ffb6003b0b64d74041");
    BOOST_TEST(to_hex(ctx.common_iv) == "be35ae297d2dace910c52e99f9");
}

// ── C.3: key derivation with an ID Context ─────────────────────────────────
// Exercises the id_context slot of `info`, which is CBOR null in every other
// vector -- the one place a wrong encoding would go unnoticed above.

BOOST_AUTO_TEST_CASE(test_c3_1_client_key_derivation_with_id_context) {
    const auto ctx = derive(master_secret, "9e7ca92223786340", "", "01", "37cbf3210017a2d3");

    BOOST_TEST(to_hex(ctx.sender_info) == "85404837cbf3210017a2d30a634b657910");
    BOOST_TEST(to_hex(ctx.recipient_info) == "8541014837cbf3210017a2d30a634b657910");
    BOOST_TEST(to_hex(ctx.iv_info) == "85404837cbf3210017a2d30a6249560d");

    BOOST_TEST(to_hex(ctx.sender_key) == "af2a1300a5e95788b356336eeecd2b92");
    BOOST_TEST(to_hex(ctx.recipient_key) == "e39a0c7c77b43f03b4b39ab9a268699f");
    BOOST_TEST(to_hex(ctx.common_iv) == "2ca58fb85ff1b81c0b7181b85e");
}

// ── Section 5.4: the AAD construction ──────────────────────────────────────

BOOST_AUTO_TEST_CASE(test_section_5_4_aad_example) {
    namespace osc = kythira::oscore;
    // The worked example in Section 5.4: kid = 0x00, Partial IV = 0x25, no
    // Class I options.
    const auto aad =
        osc::build_aad(osc::aead_alg_aes_ccm_16_64_128, from_hex("00"), from_hex("25"), {});
    BOOST_TEST(to_hex(aad) == "8368456e63727970743040498501810a4100412540");
}

BOOST_AUTO_TEST_CASE(test_c4_aad_matches_the_published_vector) {
    namespace osc = kythira::oscore;
    // C.4: kid is the empty byte string, Partial IV is 0x14.
    const auto aad =
        osc::build_aad(osc::aead_alg_aes_ccm_16_64_128, from_hex(""), from_hex("14"), {});
    BOOST_TEST(to_hex(aad) == "8368456e63727970743040488501810a40411440");
}

// ── C.4: nonce, AEAD and the compressed option, end to end ─────────────────

BOOST_AUTO_TEST_CASE(test_c4_nonce) {
    namespace osc = kythira::oscore;
    const auto nonce =
        osc::compute_nonce(from_hex("4622d4dd6d944168eefb54987c"), from_hex(""), from_hex("14"));
    BOOST_TEST(to_hex(nonce) == "4622d4dd6d944168eefb549868");
}

BOOST_AUTO_TEST_CASE(test_c4_ciphertext) {
    namespace osc = kythira::oscore;
    // Encrypting C.4's plaintext under its key, nonce and AAD must reproduce
    // the published ciphertext exactly -- this is the whole AEAD path.
    const auto ciphertext = osc::aead_encrypt(
        from_hex("f0910ed7295e6ad4b54fc793154302ff"), from_hex("4622d4dd6d944168eefb549868"),
        from_hex("8368456e63727970743040488501810a40411440"), from_hex("01b3747631"));
    BOOST_TEST(to_hex(ciphertext) == "612f1092f1776f1c1668b3825e");

    // ...and decrypting it must round-trip.
    const auto plaintext = osc::aead_decrypt(
        from_hex("f0910ed7295e6ad4b54fc793154302ff"), from_hex("4622d4dd6d944168eefb549868"),
        from_hex("8368456e63727970743040488501810a40411440"), ciphertext);
    BOOST_TEST(to_hex(plaintext) == "01b3747631");
}

BOOST_AUTO_TEST_CASE(test_c4_option_encoding) {
    namespace osc = kythira::oscore;
    osc::option_fields fields;
    fields.partial_iv = from_hex("14");
    fields.has_kid = true;  // kid is present but empty for this client
    BOOST_TEST(to_hex(osc::encode_option(fields)) == "0914");

    const auto decoded = osc::decode_option(from_hex("0914"));
    BOOST_TEST(to_hex(decoded.partial_iv) == "14");
    BOOST_TEST(decoded.has_kid);
    BOOST_TEST(decoded.kid.empty());
    BOOST_TEST(!decoded.has_kid_context);
}

// ── C.7: a response, whose AAD keeps the *request's* kid and Partial IV ────

BOOST_AUTO_TEST_CASE(test_c7_response_ciphertext) {
    namespace osc = kythira::oscore;
    // The server answers C.4's request. Its nonce is the request's (no Partial
    // IV in the response), but it encrypts under its own Sender Key, and the
    // AAD still names the *request's* kid and Partial IV -- the detail that
    // binds a response to its request.
    const auto ciphertext = osc::aead_encrypt(from_hex("ffb14e093c94c9cac9471648b4f98710"),
                                              from_hex("4622d4dd6d944168eefb549868"),
                                              from_hex("8368456e63727970743040488501810a40411440"),
                                              from_hex("45ff48656c6c6f20576f726c6421"));
    BOOST_TEST(to_hex(ciphertext) == "dbaad1e9a7e7b2a813d3c31524378303cdafae119106");
}

// ── Negative cases: verification must fail loudly ──────────────────────────

BOOST_AUTO_TEST_CASE(test_tampered_ciphertext_is_rejected) {
    namespace osc = kythira::oscore;
    auto ciphertext = from_hex("612f1092f1776f1c1668b3825e");
    ciphertext[0] ^= std::byte{0x01};
    BOOST_CHECK_THROW(
        osc::aead_decrypt(from_hex("f0910ed7295e6ad4b54fc793154302ff"),
                          from_hex("4622d4dd6d944168eefb549868"),
                          from_hex("8368456e63727970743040488501810a40411440"), ciphertext),
        osc::verification_error);
}

BOOST_AUTO_TEST_CASE(test_wrong_key_is_rejected) {
    namespace osc = kythira::oscore;
    BOOST_CHECK_THROW(osc::aead_decrypt(from_hex("ffb14e093c94c9cac9471648b4f98710"),
                                        from_hex("4622d4dd6d944168eefb549868"),
                                        from_hex("8368456e63727970743040488501810a40411440"),
                                        from_hex("612f1092f1776f1c1668b3825e")),
                      osc::verification_error);
}

BOOST_AUTO_TEST_CASE(test_aad_mismatch_is_rejected) {
    namespace osc = kythira::oscore;
    // Same key, same nonce, same ciphertext -- but an AAD naming a different
    // request Partial IV. This is what stops a response being replayed against
    // a different request.
    BOOST_CHECK_THROW(osc::aead_decrypt(from_hex("f0910ed7295e6ad4b54fc793154302ff"),
                                        from_hex("4622d4dd6d944168eefb549868"),
                                        from_hex("8368456e63727970743040488501810a40411540"),
                                        from_hex("612f1092f1776f1c1668b3825e")),
                      osc::verification_error);
}

BOOST_AUTO_TEST_CASE(test_reserved_option_bits_are_rejected) {
    namespace osc = kythira::oscore;
    BOOST_CHECK_THROW(osc::decode_option(from_hex("2014")), osc::verification_error);
}

BOOST_AUTO_TEST_CASE(test_truncated_option_is_rejected) {
    namespace osc = kythira::oscore;
    // Flags claim a 4-byte Partial IV; only two follow.
    BOOST_CHECK_THROW(osc::decode_option(from_hex("0c1234")), osc::verification_error);
}

// ── Whole-message protection: the C.4 and C.7 vectors, byte for byte ──────
// These are the ones that matter most. Everything above verifies a single
// construction in isolation; these run a real CoAP message through
// protect_request()/protect_response() and compare against the exact protected
// bytes the RFC publishes -- outer code, option re-ordering, delta re-coding of
// the inner options, payload marker and all.

namespace {

[[nodiscard]] auto c1_client_credentials() -> kythira::oscore_credentials {
    kythira::oscore_credentials creds;
    creds.master_secret = from_hex(std::string{master_secret});
    creds.master_salt = from_hex("9e7ca92223786340");
    creds.sender_id = from_hex("");
    creds.recipient_id = from_hex("01");
    return creds;
}

[[nodiscard]] auto c1_server_credentials() -> kythira::oscore_credentials {
    kythira::oscore_credentials creds;
    creds.master_secret = from_hex(std::string{master_secret});
    creds.master_salt = from_hex("9e7ca92223786340");
    creds.sender_id = from_hex("01");
    creds.recipient_id = from_hex("");
    return creds;
}

}  // namespace

BOOST_AUTO_TEST_CASE(test_c4_protected_request_matches_byte_for_byte) {
    namespace osc = kythira::oscore;
    osc::security_context context{c1_client_credentials()};
    // C.4 uses Sender Sequence Number 20 (Partial IV 0x14).
    context.set_sender_sequence_for_testing(20);

    const auto unprotected =
        osc::parse_message(from_hex("44015d1f00003974396c6f63616c686f737483747631"));
    // Sanity-check the parse before trusting what comes out of it.
    BOOST_TEST(unprotected.code == 0x01U);  // GET
    BOOST_TEST(unprotected.token.size() == 4U);
    BOOST_REQUIRE(unprotected.options.size() == 2U);
    BOOST_TEST(unprotected.options[0].number == 3U);   // Uri-Host, Class U
    BOOST_TEST(unprotected.options[1].number == 11U);  // Uri-Path, Class E

    osc::request_binding binding;
    const auto protected_message = context.protect_request(unprotected, binding);
    BOOST_TEST(to_hex(osc::serialize_message(protected_message)) ==
               "44025d1f00003974396c6f63616c686f7374620914ff612f1092f1776f1c1668b3825e");
    BOOST_TEST(to_hex(binding.partial_iv) == "14");
    BOOST_TEST(to_hex(binding.nonce) == "4622d4dd6d944168eefb549868");
}

BOOST_AUTO_TEST_CASE(test_c4_request_verifies_on_the_server) {
    namespace osc = kythira::oscore;
    osc::security_context server{c1_server_credentials()};

    const auto protected_message = osc::parse_message(
        from_hex("44025d1f00003974396c6f63616c686f7374620914ff612f1092f1776f1c1668b3825e"));
    osc::request_binding binding;
    const auto recovered = server.unprotect_request(protected_message, binding);

    // Recovering the *original* message is the real test: inner Uri-Path back
    // out of the ciphertext, outer Uri-Host retained, GET code restored.
    BOOST_TEST(to_hex(osc::serialize_message(recovered)) ==
               "44015d1f00003974396c6f63616c686f737483747631");
    BOOST_TEST(to_hex(binding.partial_iv) == "14");
}

BOOST_AUTO_TEST_CASE(test_c7_protected_response_matches_byte_for_byte) {
    namespace osc = kythira::oscore;
    osc::security_context server{c1_server_credentials()};

    // The binding the server would have obtained from verifying C.4's request.
    osc::request_binding binding;
    binding.kid = from_hex("");
    binding.partial_iv = from_hex("14");
    binding.nonce = from_hex("4622d4dd6d944168eefb549868");

    // Unprotected 2.05 Content response carrying "Hello World!".
    osc::coap_message response;
    response.version = 1;
    response.type = 2;  // ACK
    response.code = 0x45;
    response.message_id = 0x5d1f;
    response.token = from_hex("00003974");
    response.payload = from_hex("48656c6c6f20576f726c6421");
    response.has_payload = true;

    const auto protected_response = server.protect_response(response, binding);
    BOOST_TEST(to_hex(osc::serialize_message(protected_response)) ==
               "64445d1f0000397490ffdbaad1e9a7e7b2a813d3c31524378303cdafae119106");
}

BOOST_AUTO_TEST_CASE(test_c7_response_verifies_on_the_client) {
    namespace osc = kythira::oscore;
    osc::security_context client{c1_client_credentials()};

    osc::request_binding binding;
    binding.kid = from_hex("");
    binding.partial_iv = from_hex("14");
    binding.nonce = from_hex("4622d4dd6d944168eefb549868");

    const auto protected_response = osc::parse_message(
        from_hex("64445d1f0000397490ffdbaad1e9a7e7b2a813d3c31524378303cdafae119106"));
    const auto recovered = client.unprotect_response(protected_response, binding);

    BOOST_TEST(recovered.code == 0x45U);  // 2.05 Content, back from the ciphertext
    BOOST_TEST(to_hex(recovered.payload) == "48656c6c6f20576f726c6421");
}

// ── Round trip and refusals over the full procedures ───────────────────────

BOOST_AUTO_TEST_CASE(test_round_trip_with_payload_and_many_options) {
    namespace osc = kythira::oscore;
    osc::security_context client{c1_client_credentials()};
    osc::security_context server{c1_server_credentials()};

    osc::coap_message request;
    request.code = 0x02;  // POST
    request.message_id = 0x1234;
    request.token = from_hex("aabbccdd");
    request.options = {
        {3, from_hex("6c6f63616c686f7374")},  // Uri-Host  (outer)
        {11, from_hex("72616674")},           // Uri-Path "raft"   (inner)
        {11, from_hex("6165")},               // Uri-Path "ae"     (inner, repeated number)
        {12, from_hex("3c")},                 // Content-Format 60 (inner)
        {17, from_hex("3c")},                 // Accept 60         (inner)
    };
    request.payload = from_hex("a16474657374f5");
    request.has_payload = true;

    osc::request_binding sent;
    const auto protected_request = client.protect_request(request, sent);

    osc::request_binding received;
    const auto recovered = server.unprotect_request(
        osc::parse_message(osc::serialize_message(protected_request)), received);
    BOOST_TEST(to_hex(osc::serialize_message(recovered)) ==
               to_hex(osc::serialize_message(request)));

    osc::coap_message response;
    response.type = 2;
    response.code = 0x45;
    response.message_id = request.message_id;
    response.token = request.token;
    response.payload = from_hex("a2626f6bf5");
    response.has_payload = true;

    const auto protected_response = server.protect_response(response, received);
    const auto recovered_response = client.unprotect_response(
        osc::parse_message(osc::serialize_message(protected_response)), sent);
    BOOST_TEST(to_hex(recovered_response.payload) == "a2626f6bf5");
    BOOST_TEST(recovered_response.code == 0x45U);
}

BOOST_AUTO_TEST_CASE(test_outer_message_leaks_no_inner_option) {
    namespace osc = kythira::oscore;
    osc::security_context client{c1_client_credentials()};

    osc::coap_message request;
    request.code = 0x01;
    request.token = from_hex("01");
    request.options = {
        {3, from_hex("6c6f63616c686f7374")},  // Uri-Host stays visible
        {11, from_hex("736563726574")},       // Uri-Path "secret" must not
    };

    osc::request_binding binding;
    const auto protected_request = client.protect_request(request, binding);
    const auto wire = osc::serialize_message(protected_request);

    // The Uri-Path value must not appear anywhere in the protected bytes.
    const auto hex = to_hex(wire);
    BOOST_TEST(hex.find("736563726574") == std::string::npos,
               "inner option value leaked into the protected message: " << hex);
    for (const auto& option : protected_request.options) {
        BOOST_TEST((option.number == 3U || option.number == 9U),
                   "unexpected outer option " << option.number);
    }
}

BOOST_AUTO_TEST_CASE(test_replayed_request_is_rejected) {
    namespace osc = kythira::oscore;
    osc::security_context client{c1_client_credentials()};
    osc::security_context server{c1_server_credentials()};

    osc::coap_message request;
    request.code = 0x01;
    request.token = from_hex("07");
    request.options = {{11, from_hex("7476")}};

    osc::request_binding sent;
    const auto protected_request = client.protect_request(request, sent);
    const auto wire = osc::serialize_message(protected_request);

    osc::request_binding received;
    BOOST_REQUIRE_NO_THROW((void)server.unprotect_request(osc::parse_message(wire), received));
    // The same bytes a second time: same Partial IV, so the replay window must
    // reject it even though the AEAD tag is perfectly valid.
    BOOST_CHECK_THROW((void)server.unprotect_request(osc::parse_message(wire), received),
                      osc::verification_error);
}

BOOST_AUTO_TEST_CASE(test_observe_and_block_options_are_carried_as_inner_options) {
    namespace osc = kythira::oscore;
    osc::security_context client{c1_client_credentials()};
    osc::security_context server{c1_server_credentials()};

    // Observe (6), Block2 (23) and Block1 (27) are the "E and U" rows of
    // RFC 8613 Figure 5. This implementation emits only the inner form, so each
    // must survive a round trip through the ciphertext and never appear outside.
    for (const std::uint16_t number : {std::uint16_t{6}, std::uint16_t{23}, std::uint16_t{27}}) {
        osc::coap_message request;
        request.code = 0x02;
        request.token = from_hex("01");
        request.options = {{number, from_hex("05")}};

        osc::request_binding sent;
        const auto protected_request = client.protect_request(request, sent);
        for (const auto& option : protected_request.options) {
            BOOST_TEST(option.number == 9U,
                       "option " << number << " leaked outside the ciphertext");
        }

        osc::request_binding received;
        const auto recovered = server.unprotect_request(
            osc::parse_message(osc::serialize_message(protected_request)), received);
        BOOST_REQUIRE(recovered.options.size() == 1U);
        BOOST_TEST(recovered.options[0].number == number);
    }
}

BOOST_AUTO_TEST_CASE(test_a_message_already_carrying_oscore_is_refused) {
    namespace osc = kythira::oscore;
    osc::security_context client{c1_client_credentials()};
    // OSCORE-in-OSCORE is out of scope and must be refused rather than nested.
    osc::coap_message request;
    request.code = 0x02;
    request.options = {{osc::coap_option_oscore, from_hex("09")}};
    osc::request_binding binding;
    BOOST_CHECK_THROW((void)client.protect_request(request, binding),
                      osc::unsupported_feature_error);
}

BOOST_AUTO_TEST_CASE(test_identical_sender_and_recipient_ids_are_refused) {
    // RFC 8613 Section 3.3: identical IDs derive identical keys, which breaks
    // the nonce-uniqueness argument the AEAD depends on.
    kythira::oscore_credentials creds;
    creds.master_secret = from_hex(std::string{master_secret});
    creds.sender_id = from_hex("01");
    creds.recipient_id = from_hex("01");
    BOOST_CHECK_THROW(kythira::oscore::security_context{creds},
                      kythira::coap_security_config_error);
}

BOOST_AUTO_TEST_CASE(test_partial_iv_encoding_is_minimum_length) {
    namespace osc = kythira::oscore;
    // Sequence 0 still occupies one byte; the rest are minimum-length
    // big-endian, which is what keeps the option small.
    BOOST_TEST(to_hex(osc::detail::encode_partial_iv(0)) == "00");
    BOOST_TEST(to_hex(osc::detail::encode_partial_iv(20)) == "14");
    BOOST_TEST(to_hex(osc::detail::encode_partial_iv(255)) == "ff");
    BOOST_TEST(to_hex(osc::detail::encode_partial_iv(256)) == "0100");
    BOOST_TEST(to_hex(osc::detail::encode_partial_iv(0x0102030405ULL)) == "0102030405");
}

// ── C.8: a response carrying its own Partial IV (the Observe case) ────────
// The nonce here is built from the *server's* Sender ID and the response's own
// sequence number, while the AAD still names the request's kid and Partial IV.
// Getting either of those the wrong way round still produces a valid-looking
// message that the peer cannot decrypt, which is why the published vector
// matters.

BOOST_AUTO_TEST_CASE(test_c8_response_with_its_own_partial_iv) {
    namespace osc = kythira::oscore;
    osc::security_context server{c1_server_credentials()};
    server.set_sender_sequence_for_testing(0);  // C.8 uses Sender Sequence Number 0

    osc::request_binding binding;
    binding.kid = from_hex("");
    binding.partial_iv = from_hex("14");
    binding.nonce = from_hex("4622d4dd6d944168eefb549868");

    osc::coap_message response;
    response.type = 2;  // ACK
    response.code = 0x45;
    response.message_id = 0x5d1f;
    response.token = from_hex("00003974");
    response.payload = from_hex("48656c6c6f20576f726c6421");
    response.has_payload = true;

    const auto protected_response = server.protect_response(response, binding, true);
    BOOST_TEST(to_hex(osc::serialize_message(protected_response)) ==
               "64445d1f00003974920100ff4d4c13669384b67354b2b6175ff4b8658c666a6cf88e");
}

BOOST_AUTO_TEST_CASE(test_c8_response_verifies_on_the_client) {
    namespace osc = kythira::oscore;
    osc::security_context client{c1_client_credentials()};

    osc::request_binding binding;
    binding.kid = from_hex("");
    binding.partial_iv = from_hex("14");
    binding.nonce = from_hex("4622d4dd6d944168eefb549868");

    const auto protected_response = osc::parse_message(
        from_hex("64445d1f00003974920100ff4d4c13669384b67354b2b6175ff4b8658c666a6cf88e"));
    const auto recovered = client.unprotect_response(protected_response, binding);
    BOOST_TEST(recovered.code == 0x45U);
    BOOST_TEST(to_hex(recovered.payload) == "48656c6c6f20576f726c6421");
}

// ── Observe: notification ordering (Section 8.4.2) ─────────────────────────

BOOST_AUTO_TEST_CASE(test_notifications_must_advance) {
    namespace osc = kythira::oscore;
    osc::security_context client{c1_client_credentials()};
    osc::security_context server{c1_server_credentials()};

    osc::request_binding binding;
    binding.kid = from_hex("");
    binding.partial_iv = from_hex("14");
    binding.nonce = from_hex("4622d4dd6d944168eefb549868");

    const auto notify = [&](std::uint64_t sequence, std::uint32_t observe_value) {
        server.set_sender_sequence_for_testing(sequence);
        osc::coap_message response;
        response.code = 0x45;
        response.options = {{6, {static_cast<std::byte>(observe_value)}}};  // Observe, Class E
        response.payload = {static_cast<std::byte>(observe_value)};
        response.has_payload = true;
        return osc::serialize_message(server.protect_response(response, binding, true));
    };

    const auto first = notify(1, 1);
    const auto second = notify(2, 2);

    std::uint64_t last = 0;
    BOOST_REQUIRE_NO_THROW(
        (void)client.unprotect_notification(osc::parse_message(first), binding, last));
    BOOST_TEST(last == 1U);

    const auto accepted = client.unprotect_notification(osc::parse_message(second), binding, last);
    BOOST_TEST(last == 2U);
    // The Observe option itself must come back out of the ciphertext.
    BOOST_REQUIRE(accepted.options.size() == 1U);
    BOOST_TEST(accepted.options[0].number == 6U);

    // Replaying the first notification authenticates perfectly and must still
    // be refused: accepting it would roll the observer's view backwards.
    BOOST_CHECK_THROW((void)client.unprotect_notification(osc::parse_message(first), binding, last),
                      osc::verification_error);
    BOOST_TEST(last == 2U, "a rejected notification must not move the window");
}

BOOST_AUTO_TEST_CASE(test_notification_without_a_partial_iv_is_refused) {
    namespace osc = kythira::oscore;
    osc::security_context client{c1_client_credentials()};
    osc::security_context server{c1_server_credentials()};

    osc::request_binding binding;
    binding.kid = from_hex("");
    binding.partial_iv = from_hex("14");
    binding.nonce = from_hex("4622d4dd6d944168eefb549868");

    osc::coap_message response;
    response.code = 0x45;
    response.payload = from_hex("00");
    response.has_payload = true;
    // Protected as a plain response: no Partial IV of its own, so it cannot be
    // ordered against other notifications.
    const auto wire = osc::serialize_message(server.protect_response(response, binding, false));

    std::uint64_t last = 0;
    BOOST_CHECK_THROW((void)client.unprotect_notification(osc::parse_message(wire), binding, last),
                      osc::verification_error);
}

// ── Inner block options travel inside the ciphertext ───────────────────────

BOOST_AUTO_TEST_CASE(test_inner_block_options_are_protected_not_exposed) {
    namespace osc = kythira::oscore;
    osc::security_context client{c1_client_credentials()};
    osc::security_context server{c1_server_credentials()};

    osc::coap_message request;
    request.code = 0x02;
    request.token = from_hex("aa");
    request.options = {
        {3, from_hex("6c6f63616c686f7374")},  // Uri-Host, outer
        {11, from_hex("72616674")},           // Uri-Path, inner
        {27, from_hex("10")},                 // Block1 num=1 more=0 szx=0, inner
    };
    request.payload = from_hex("0badc0de");
    request.has_payload = true;

    osc::request_binding sent;
    const auto protected_request = client.protect_request(request, sent);

    // Only Uri-Host and OSCORE may appear outside.
    for (const auto& option : protected_request.options) {
        BOOST_TEST((option.number == 3U || option.number == 9U),
                   "block option leaked to the outer message: " << option.number);
    }

    osc::request_binding received;
    const auto recovered = server.unprotect_request(
        osc::parse_message(osc::serialize_message(protected_request)), received);
    BOOST_TEST(to_hex(osc::serialize_message(recovered)) ==
               to_hex(osc::serialize_message(request)));
}

BOOST_AUTO_TEST_SUITE_END()

#include "test_timeout_scale.hpp"
#define BOOST_TEST_MODULE oscore_transport_neutral_test
#include <boost/test/unit_test.hpp>

// Set test timeout to prevent hanging tests
#define BOOST_TEST_TIMEOUT (30 * KYTHIRA_TEST_TIMEOUT_SCALE)

// raft/oscore.hpp was written for the libnyoci backend and made
// transport-neutral on principle -- it owns bytes, not sockets. The cantcoap
// backend then picked it up unchanged, which is the payoff.
//
// oscore_rfc8613_vectors_test pins the cryptography against the RFC's published
// vectors. What that leaves untested without a CoAP backend installed is
// everything around the cryptography: the message codec, the Class E/U option
// split, inner block options, and the request/response binding that RFC 8613
// builds between the two halves of an exchange. Those run today only in a build
// that has libnyoci or cantcoap, which is neither of the two configurations CI
// exercises by default.
//
// So this file drives the whole surface through a pair of mirrored security
// contexts and no transport at all -- which is the strongest statement of the
// neutrality claim available, and it runs in every build.
#include <raft/coap_exceptions.hpp>
#include <raft/coap_security.hpp>
#include <raft/oscore.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

auto bytes(std::initializer_list<unsigned char> values) -> std::vector<std::byte> {
    std::vector<std::byte> out;
    out.reserve(values.size());
    for (const auto value : values) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

auto text_bytes(std::string_view text) -> std::vector<std::byte> {
    std::vector<std::byte> out;
    out.reserve(text.size());
    for (const auto c : text) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    return out;
}

// Two endpoints of one Security Context: each one's Sender ID is the other's
// Recipient ID (RFC 8613 Section 3.1).
auto client_credentials() -> kythira::oscore_credentials {
    kythira::oscore_credentials credentials;
    credentials.sender_id = bytes({0x00});
    credentials.recipient_id = bytes({0x01});
    credentials.master_secret = bytes({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
                                       0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10});
    credentials.master_salt = bytes({0x9E, 0x7C, 0xA9, 0x22, 0x23, 0x78, 0x63, 0x40});
    return credentials;
}

auto server_credentials() -> kythira::oscore_credentials {
    auto credentials = client_credentials();
    credentials.sender_id = bytes({0x01});
    credentials.recipient_id = bytes({0x00});
    return credentials;
}

// A GET for /raft/vote with a payload, carrying one Class U option (Uri-Host)
// and one Class E option (Uri-Path) so the split has something to do.
auto sample_request() -> kythira::oscore::coap_message {
    kythira::oscore::coap_message message;
    message.type = 0;     // CON
    message.code = 0x01;  // GET
    message.message_id = 0x1234;
    message.token = bytes({0xAB, 0xCD});
    message.options.push_back({3, text_bytes("node-2")});  // Uri-Host, Class U
    message.options.push_back({11, text_bytes("raft")});   // Uri-Path, Class E
    message.options.push_back({11, text_bytes("vote")});   // Uri-Path, Class E
    message.payload = text_bytes("term=7");
    message.has_payload = true;
    return message;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(oscore_transport_neutral_tests)

// ── Message codec ──────────────────────────────────────────────────────────

// The codec is the boundary every backend crosses to reach this header, so a
// message must survive the trip unchanged in every field.
BOOST_AUTO_TEST_CASE(message_survives_a_serialize_parse_round_trip,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    const auto original = sample_request();
    const auto parsed =
        kythira::oscore::parse_message(kythira::oscore::serialize_message(original));

    BOOST_TEST(parsed.version == original.version);
    BOOST_TEST(parsed.type == original.type);
    BOOST_TEST(parsed.code == original.code);
    BOOST_TEST(parsed.message_id == original.message_id);
    BOOST_TEST((parsed.token == original.token));
    BOOST_TEST(parsed.has_payload == original.has_payload);
    BOOST_TEST((parsed.payload == original.payload));
    BOOST_TEST_REQUIRE(parsed.options.size() == original.options.size());
    for (std::size_t i = 0; i < parsed.options.size(); ++i) {
        BOOST_TEST(parsed.options[i].number == original.options[i].number);
        BOOST_TEST((parsed.options[i].value == original.options[i].value));
    }
}

// Repeated Uri-Path options are the normal case, and their order *is* the path
// -- "raft/vote" and "vote/raft" must not be interchangeable.
BOOST_AUTO_TEST_CASE(repeated_options_keep_their_relative_order,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    kythira::oscore::coap_message message;
    message.code = 0x01;
    message.options.push_back({11, text_bytes("a")});
    message.options.push_back({11, text_bytes("b")});
    message.options.push_back({11, text_bytes("c")});

    const auto parsed = kythira::oscore::parse_message(kythira::oscore::serialize_message(message));

    BOOST_TEST_REQUIRE(parsed.options.size() == 3u);
    BOOST_TEST((parsed.options[0].value == text_bytes("a")));
    BOOST_TEST((parsed.options[1].value == text_bytes("b")));
    BOOST_TEST((parsed.options[2].value == text_bytes("c")));
}

// A message with no payload must not come back claiming an empty one: the 0xFF
// marker is what distinguishes "no payload" from "zero-length payload".
BOOST_AUTO_TEST_CASE(absent_payload_stays_absent,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    kythira::oscore::coap_message message;
    message.code = 0x01;
    message.token = bytes({0x01});

    const auto parsed = kythira::oscore::parse_message(kythira::oscore::serialize_message(message));

    BOOST_TEST(!parsed.has_payload);
    BOOST_TEST(parsed.payload.empty());
}

// Option numbers above 12 use the extended delta encodings (13-bit and 14-bit
// forms); a jump to 65535-scale numbers exercises both.
BOOST_AUTO_TEST_CASE(extended_option_deltas_round_trip,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    kythira::oscore::coap_message message;
    message.code = 0x01;
    message.options.push_back({1, text_bytes("x")});
    message.options.push_back({20, text_bytes("y")});     // 13-bit delta form
    message.options.push_back({500, text_bytes("z")});    // 14-bit delta form
    message.options.push_back({60000, text_bytes("w")});  // 14-bit delta again

    const auto parsed = kythira::oscore::parse_message(kythira::oscore::serialize_message(message));

    BOOST_TEST_REQUIRE(parsed.options.size() == 4u);
    BOOST_TEST(parsed.options[0].number == 1);
    BOOST_TEST(parsed.options[1].number == 20);
    BOOST_TEST(parsed.options[2].number == 500);
    BOOST_TEST(parsed.options[3].number == 60000);
}

// ── Option classes ─────────────────────────────────────────────────────────

// RFC 8613 Figure 5. The default matters more than the list: anything not
// named is Class E, so an option this code has never heard of gets protected
// rather than leaked into the clear.
BOOST_AUTO_TEST_CASE(option_classes_follow_figure_5,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    BOOST_TEST(kythira::oscore::is_outer_option(3));   // Uri-Host
    BOOST_TEST(kythira::oscore::is_outer_option(7));   // Uri-Port
    BOOST_TEST(kythira::oscore::is_outer_option(35));  // Proxy-Uri

    BOOST_TEST(!kythira::oscore::is_outer_option(11));  // Uri-Path, Class E
    BOOST_TEST(!kythira::oscore::is_outer_option(12));  // Content-Format, Class E
    BOOST_TEST(!kythira::oscore::is_outer_option(17));  // Accept, Class E

    // An option number no RFC has assigned: Class E by default.
    BOOST_TEST(!kythira::oscore::is_outer_option(64000));
}

// ── Inner block options (RFC 7959 over RFC 8613 Section 4.1.3.4.1) ─────────

BOOST_AUTO_TEST_CASE(block_option_round_trips_across_the_size_range,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    for (std::uint8_t szx = 0; szx <= 6; ++szx) {
        for (const std::uint32_t number : {0U, 1U, 15U, 4095U, 1048575U}) {
            for (const bool more : {false, true}) {
                const kythira::oscore::block_option block{number, more, szx};
                const auto decoded = kythira::oscore::decode_block_option(
                    kythira::oscore::encode_block_option(block));
                BOOST_TEST(decoded.number == number);
                BOOST_TEST(decoded.more == more);
                BOOST_TEST(decoded.szx == szx);
                BOOST_TEST(decoded.size() == (std::size_t{1} << (szx + 4)));
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(block_szx_above_six_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    const kythira::oscore::block_option block{0, false, 7};
    BOOST_CHECK_THROW(static_cast<void>(kythira::oscore::encode_block_option(block)),
                      kythira::oscore::unsupported_feature_error);
}

// szx 7 is reserved on the wire; a peer sending it is malformed, not merely
// unsupported, so it is rejected on decode too.
BOOST_AUTO_TEST_CASE(reserved_block_szx_seven_is_rejected_on_decode,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    BOOST_CHECK_THROW(static_cast<void>(kythira::oscore::decode_block_option(bytes({0x07}))),
                      kythira::oscore::verification_error);
    BOOST_CHECK_THROW(
        static_cast<void>(kythira::oscore::decode_block_option(bytes({0x01, 0x02, 0x03, 0x04}))),
        kythira::oscore::verification_error);
}

BOOST_AUTO_TEST_CASE(find_block_option_locates_block1_and_block2,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    kythira::oscore::coap_message message;
    message.code = 0x02;
    message.options.push_back({23, kythira::oscore::encode_block_option({2, true, 4})});   // Block2
    message.options.push_back({27, kythira::oscore::encode_block_option({5, false, 6})});  // Block1

    const auto block2 = kythira::oscore::find_block_option(message, 23);
    BOOST_TEST_REQUIRE(block2.has_value());
    BOOST_TEST(block2->number == 2u);
    BOOST_TEST(block2->more);

    const auto block1 = kythira::oscore::find_block_option(message, 27);
    BOOST_TEST_REQUIRE(block1.has_value());
    BOOST_TEST(block1->number == 5u);
    BOOST_TEST(!block1->more);

    BOOST_TEST(!kythira::oscore::find_block_option(message, 6).has_value());
}

// ── Request / response protection ──────────────────────────────────────────

// The whole point, end to end: what the client protects is what the server
// reads back, with the Class E options recovered and the outer message
// carrying none of them.
BOOST_AUTO_TEST_CASE(protected_request_round_trips_between_mirrored_contexts,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    const kythira::oscore::security_context client(client_credentials());
    kythira::oscore::security_context server(server_credentials());

    const auto original = sample_request();
    kythira::oscore::request_binding client_binding;
    const auto protected_message = client.protect_request(original, client_binding);

    // Section 8.1: the outer code is POST, and the Class E options are gone.
    BOOST_TEST(protected_message.code == 0x02);
    for (const auto& option : protected_message.options) {
        const bool stays_outside =
            kythira::oscore::is_outer_option(option.number) || option.number == 9;
        BOOST_CHECK_MESSAGE(stays_outside,
                            "Class E option " << option.number << " leaked into the outer message");
    }
    // The plaintext payload must not be recoverable from the outer message.
    BOOST_TEST((protected_message.payload != original.payload));

    kythira::oscore::request_binding server_binding;
    const auto recovered = server.unprotect_request(protected_message, server_binding);

    BOOST_TEST(recovered.code == original.code);
    BOOST_TEST((recovered.payload == original.payload));
    BOOST_TEST(recovered.has_payload);

    // Both Uri-Path segments came back, in order.
    std::vector<std::vector<std::byte>> path;
    for (const auto& option : recovered.options) {
        if (option.number == 11) {
            path.push_back(option.value);
        }
    }
    BOOST_TEST_REQUIRE(path.size() == 2u);
    BOOST_TEST((path[0] == text_bytes("raft")));
    BOOST_TEST((path[1] == text_bytes("vote")));
}

// Section 5.2: a response without its own Partial IV reuses the request's
// nonce, which is what the binding carries between the two halves.
BOOST_AUTO_TEST_CASE(response_without_its_own_partial_iv_reuses_the_request_nonce,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    const kythira::oscore::security_context client(client_credentials());
    kythira::oscore::security_context server(server_credentials());

    kythira::oscore::request_binding client_binding;
    const auto protected_request = client.protect_request(sample_request(), client_binding);
    kythira::oscore::request_binding server_binding;
    static_cast<void>(server.unprotect_request(protected_request, server_binding));

    kythira::oscore::coap_message response;
    response.type = 2;     // ACK
    response.code = 0x45;  // 2.05 Content
    response.message_id = protected_request.message_id;
    response.token = protected_request.token;
    response.payload = text_bytes("granted");
    response.has_payload = true;

    const auto protected_response = server.protect_response(response, server_binding, false);
    const auto recovered = client.unprotect_response(protected_response, client_binding);

    BOOST_TEST(recovered.code == 0x45);
    BOOST_TEST((recovered.payload == text_bytes("granted")));
}

// ...and a response that does carry one gets its own nonce, derived from the
// server's Sender ID.
BOOST_AUTO_TEST_CASE(response_with_its_own_partial_iv_round_trips,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    const kythira::oscore::security_context client(client_credentials());
    kythira::oscore::security_context server(server_credentials());

    kythira::oscore::request_binding client_binding;
    const auto protected_request = client.protect_request(sample_request(), client_binding);
    kythira::oscore::request_binding server_binding;
    static_cast<void>(server.unprotect_request(protected_request, server_binding));

    kythira::oscore::coap_message response;
    response.type = 2;
    response.code = 0x45;
    response.token = protected_request.token;
    response.payload = text_bytes("granted");
    response.has_payload = true;

    const auto protected_response = server.protect_response(response, server_binding, true);
    const auto recovered = client.unprotect_response(protected_response, client_binding);

    BOOST_TEST((recovered.payload == text_bytes("granted")));
}

// A context whose Master Secret differs must not be able to read the traffic --
// the negative control without which the positive cases prove nothing.
BOOST_AUTO_TEST_CASE(a_wrong_master_secret_cannot_unprotect,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    const kythira::oscore::security_context client(client_credentials());

    auto wrong = server_credentials();
    wrong.master_secret = bytes({0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8, 0xF7, 0xF6, 0xF5,
                                 0xF4, 0xF3, 0xF2, 0xF1, 0xF0});
    kythira::oscore::security_context impostor(wrong);

    kythira::oscore::request_binding client_binding;
    const auto protected_request = client.protect_request(sample_request(), client_binding);

    kythira::oscore::request_binding server_binding;
    BOOST_CHECK_THROW(
        static_cast<void>(impostor.unprotect_request(protected_request, server_binding)),
        kythira::oscore::verification_error);
}

// Section 7.4: a request replayed to the server must be refused, or an
// attacker can re-cast a vote by resending the datagram.
BOOST_AUTO_TEST_CASE(a_replayed_request_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    const kythira::oscore::security_context client(client_credentials());
    kythira::oscore::security_context server(server_credentials());

    kythira::oscore::request_binding client_binding;
    const auto protected_request = client.protect_request(sample_request(), client_binding);

    kythira::oscore::request_binding first;
    static_cast<void>(server.unprotect_request(protected_request, first));

    kythira::oscore::request_binding second;
    BOOST_CHECK_THROW(static_cast<void>(server.unprotect_request(protected_request, second)),
                      kythira::oscore::verification_error);
}

// Each request must consume a fresh sequence number: reuse would repeat a
// nonce, which is fatal for AES-CCM.
BOOST_AUTO_TEST_CASE(sequential_requests_use_distinct_partial_ivs,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    const kythira::oscore::security_context client(client_credentials());

    kythira::oscore::request_binding first;
    static_cast<void>(client.protect_request(sample_request(), first));
    kythira::oscore::request_binding second;
    static_cast<void>(client.protect_request(sample_request(), second));

    BOOST_TEST((first.partial_iv != second.partial_iv));
    BOOST_TEST((first.nonce != second.nonce));
}

BOOST_AUTO_TEST_CASE(protect_request_refuses_a_response,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    const kythira::oscore::security_context client(client_credentials());

    kythira::oscore::coap_message response;
    response.code = 0x45;  // 2.05 Content, not a request
    kythira::oscore::request_binding binding;

    BOOST_CHECK_THROW(static_cast<void>(client.protect_request(response, binding)),
                      kythira::oscore::unsupported_feature_error);
}

// ── Observe notifications (Section 8.4.2) ──────────────────────────────────

// A valid AEAD tag is not enough for a notification stream: an old
// notification authenticates perfectly, so replaying one would roll the
// observer's view backwards. The Partial IV is what orders them.
BOOST_AUTO_TEST_CASE(notifications_advance_and_a_replay_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    kythira::oscore::security_context client(client_credentials());
    kythira::oscore::security_context server(server_credentials());

    kythira::oscore::request_binding client_binding;
    const auto protected_request = client.protect_request(sample_request(), client_binding);
    kythira::oscore::request_binding server_binding;
    static_cast<void>(server.unprotect_request(protected_request, server_binding));

    const auto make_notification = [&](std::string_view body) {
        kythira::oscore::coap_message notification;
        notification.type = 1;  // NON
        notification.code = 0x45;
        notification.token = protected_request.token;
        notification.payload = text_bytes(body);
        notification.has_payload = true;
        return server.protect_response(notification, server_binding, true);
    };

    const auto first = make_notification("one");
    const auto second = make_notification("two");

    // Left empty, not zeroed: the first notification of a fresh observation
    // legitimately carries Partial IV 0.
    std::optional<std::uint64_t> last_partial_iv;
    const auto got_first = client.unprotect_notification(first, client_binding, last_partial_iv);
    BOOST_TEST((got_first.payload == text_bytes("one")));
    BOOST_TEST_REQUIRE(last_partial_iv.has_value());
    const auto after_first = *last_partial_iv;

    const auto got_second = client.unprotect_notification(second, client_binding, last_partial_iv);
    BOOST_TEST((got_second.payload == text_bytes("two")));
    BOOST_TEST(*last_partial_iv > after_first);

    // Replaying the first notification now goes backwards, and must be refused
    // even though its tag still verifies.
    BOOST_CHECK_THROW(
        static_cast<void>(client.unprotect_notification(first, client_binding, last_partial_iv)),
        kythira::oscore::verification_error);
    // The window did not move.
    BOOST_TEST(*last_partial_iv > after_first);
}

// Without a Partial IV there is nothing to order the stream by, so such a
// notification is refused rather than accepted unordered.
BOOST_AUTO_TEST_CASE(a_notification_without_a_partial_iv_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    kythira::oscore::security_context client(client_credentials());
    kythira::oscore::security_context server(server_credentials());

    kythira::oscore::request_binding client_binding;
    const auto protected_request = client.protect_request(sample_request(), client_binding);
    kythira::oscore::request_binding server_binding;
    static_cast<void>(server.unprotect_request(protected_request, server_binding));

    kythira::oscore::coap_message notification;
    notification.type = 1;
    notification.code = 0x45;
    notification.token = protected_request.token;
    notification.payload = text_bytes("unordered");
    notification.has_payload = true;
    const auto without_partial_iv = server.protect_response(notification, server_binding, false);

    std::optional<std::uint64_t> last_partial_iv;
    BOOST_CHECK_THROW(static_cast<void>(client.unprotect_notification(
                          without_partial_iv, client_binding, last_partial_iv)),
                      kythira::oscore::verification_error);
}

// ── Context construction ───────────────────────────────────────────────────

// Section 3.3: identical Sender and Recipient IDs derive identical keys, and
// the nonce-uniqueness argument collapses. Refused at construction.
BOOST_AUTO_TEST_CASE(identical_sender_and_recipient_ids_are_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    auto credentials = client_credentials();
    credentials.recipient_id = credentials.sender_id;

    BOOST_CHECK_THROW(kythira::oscore::security_context{credentials},
                      kythira::coap_security_config_error);
}

BOOST_AUTO_TEST_CASE(a_missing_master_secret_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    auto credentials = client_credentials();
    credentials.master_secret.clear();

    BOOST_CHECK_THROW(kythira::oscore::security_context{credentials},
                      kythira::coap_security_config_error);
}

BOOST_AUTO_TEST_CASE(an_unimplemented_aead_algorithm_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    auto credentials = client_credentials();
    credentials.aead_algorithm = "AES-GCM-256";

    BOOST_CHECK_THROW(kythira::oscore::security_context{credentials},
                      kythira::coap_security_config_error);
}

// Mirrored contexts must derive mirrored keys: each side's sender key is the
// other's recipient key, and both derive the same Common IV.
BOOST_AUTO_TEST_CASE(mirrored_contexts_derive_mirrored_keys,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    const kythira::oscore::security_context client(client_credentials());
    const kythira::oscore::security_context server(server_credentials());

    BOOST_TEST((client.sender_key() == server.recipient_key()));
    BOOST_TEST((client.recipient_key() == server.sender_key()));
    BOOST_TEST((client.common_iv() == server.common_iv()));
    BOOST_TEST((client.sender_key() != client.recipient_key()));
}

BOOST_AUTO_TEST_SUITE_END()

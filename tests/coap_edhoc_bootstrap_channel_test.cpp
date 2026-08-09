#include "test_timeout_scale.hpp"
#define BOOST_TEST_MODULE coap_edhoc_bootstrap_channel_test
#include <boost/test/unit_test.hpp>

// Set test timeout to prevent hanging tests
#define BOOST_TEST_TIMEOUT (30 * KYTHIRA_TEST_TIMEOUT_SCALE)

// coap_edhoc_bootstrap.hpp carries EDHOC messages between two peers, and it was
// written to be free of any CoAP library so that either backend could drive it.
// This test holds that claim to account: it exercises both transports with no
// CoAP backend, no socket and no lakers, because a carrier that genuinely is
// transport-neutral must be testable without a transport.
//
// The handshake itself is covered by coap_edhoc_oscore_bootstrap_test, which
// needs lakers and therefore does not run in every build. What is covered here
// is the part that has nothing to do with cryptography: the send-then-receive
// discipline on the Initiator side, and the rendezvous between two threads on
// the Responder side, including how it comes apart when a peer disappears.
#include <raft/coap_edhoc_bootstrap.hpp>
#include <raft/coap_exceptions.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <thread>
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

}  // namespace

BOOST_AUTO_TEST_SUITE(coap_edhoc_bootstrap_channel_tests)

// --- Initiator -------------------------------------------------------------

// The Initiator drives: every send() is a POST whose response is the peer's
// next EDHOC message, collected by the receive() that follows.
BOOST_AUTO_TEST_CASE(initiator_returns_the_post_response_from_receive,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    std::vector<std::vector<std::byte>> posted;
    kythira::edhoc_initiator_transport transport(
        [&posted](const std::vector<std::byte>& message) -> std::vector<std::byte> {
            posted.push_back(message);
            return bytes({0x02, 0x02});
        });

    transport.send(bytes({0x01, 0x01}));
    const auto reply = transport.receive();

    BOOST_TEST_REQUIRE(posted.size() == 1u);
    BOOST_TEST((posted.front() == bytes({0x01, 0x01})));
    BOOST_TEST((reply == bytes({0x02, 0x02})));
}

// Two full steps in a row, to prove the stash is consumed rather than
// accumulated -- message_2 must not still be sitting there when message_3's
// reply arrives.
BOOST_AUTO_TEST_CASE(initiator_consumes_each_stashed_reply,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    int posts = 0;
    kythira::edhoc_initiator_transport transport(
        [&posts](const std::vector<std::byte>&) -> std::vector<std::byte> {
            ++posts;
            return bytes({static_cast<unsigned char>(posts)});
        });

    transport.send(bytes({0x01}));
    BOOST_TEST((transport.receive() == bytes({0x01})));
    transport.send(bytes({0x03}));
    BOOST_TEST((transport.receive() == bytes({0x02})));
    BOOST_TEST(posts == 2);
}

BOOST_AUTO_TEST_CASE(initiator_receive_before_send_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    kythira::edhoc_initiator_transport transport(
        [](const std::vector<std::byte>&) { return bytes({0x00}); });

    BOOST_CHECK_THROW(static_cast<void>(transport.receive()),
                      kythira::coap_credential_bootstrap_error);
}

// A second receive() with nothing sent in between is the same fault as the
// first: the stash is empty either way.
BOOST_AUTO_TEST_CASE(initiator_receive_twice_without_sending_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    kythira::edhoc_initiator_transport transport(
        [](const std::vector<std::byte>&) { return bytes({0x07}); });

    transport.send(bytes({0x01}));
    BOOST_TEST((transport.receive() == bytes({0x07})));
    BOOST_CHECK_THROW(static_cast<void>(transport.receive()),
                      kythira::coap_credential_bootstrap_error);
}

// An empty response is a peer that answered without an EDHOC message in it.
// Returning it would hand lakers a zero-length message; failing here names the
// problem instead.
BOOST_AUTO_TEST_CASE(initiator_rejects_an_empty_peer_response,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    kythira::edhoc_initiator_transport transport(
        [](const std::vector<std::byte>&) { return std::vector<std::byte>{}; });

    transport.send(bytes({0x01}));
    BOOST_CHECK_THROW(static_cast<void>(transport.receive()),
                      kythira::coap_credential_bootstrap_error);
}

// --- Responder -------------------------------------------------------------

// The shape of a real handshake: two request/response pairs, the handler thread
// blocking in exchange() while the responder thread works, and finish()
// releasing the handler that delivered message_3.
BOOST_AUTO_TEST_CASE(responder_rendezvous_carries_a_three_message_exchange,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(20))) {
    kythira::edhoc_responder_channel channel;

    // Stands in for run_edhoc_handshake(): receive message_1, answer with
    // message_2, receive message_3, then finish.
    std::thread responder([&channel] {
        const auto message_1 = channel.receive();
        BOOST_TEST((message_1 == bytes({0x01})));
        channel.send(bytes({0x02}));
        const auto message_3 = channel.receive();
        BOOST_TEST((message_3 == bytes({0x03})));
        channel.finish();
    });

    const auto reply_to_1 = channel.exchange(bytes({0x01}));
    BOOST_TEST_REQUIRE(reply_to_1.has_value());
    BOOST_TEST((*reply_to_1 == bytes({0x02})));

    // message_3 has no EDHOC reply; finish() releases this handler with an
    // empty payload, which is what becomes the 2.04.
    const auto reply_to_3 = channel.exchange(bytes({0x03}));
    BOOST_TEST_REQUIRE(reply_to_3.has_value());
    BOOST_TEST(reply_to_3->empty());

    responder.join();
}

// A handshake that throws must not leave the CoAP handler blocked until its
// 20-second timeout -- fail() releases it promptly with nullopt.
BOOST_AUTO_TEST_CASE(fail_releases_a_handler_already_blocked_in_exchange,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(20))) {
    kythira::edhoc_responder_channel channel;

    std::thread responder([&channel] {
        static_cast<void>(channel.receive());
        channel.fail();
    });

    const auto start = std::chrono::steady_clock::now();
    const auto reply = channel.exchange(bytes({0x01}));
    const auto elapsed = std::chrono::steady_clock::now() - start;

    BOOST_TEST(!reply.has_value());
    // Well inside edhoc_step_timeout: the point is that it did not wait it out.
    BOOST_TEST(elapsed < std::chrono::seconds(5));

    responder.join();
}

// Once failed, the channel stays failed -- a later request is refused at the
// door rather than parked on a condition variable nobody will notify.
BOOST_AUTO_TEST_CASE(exchange_after_failure_returns_immediately,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    kythira::edhoc_responder_channel channel;
    channel.fail();

    const auto start = std::chrono::steady_clock::now();
    const auto reply = channel.exchange(bytes({0x01}));
    const auto elapsed = std::chrono::steady_clock::now() - start;

    BOOST_TEST(!reply.has_value());
    BOOST_TEST(elapsed < std::chrono::seconds(5));
}

// Shutdown: abandon() wakes the responder thread out of receive() rather than
// leaving it parked for the step timeout.
BOOST_AUTO_TEST_CASE(abandon_wakes_the_responder_out_of_receive,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(20))) {
    kythira::edhoc_responder_channel channel;

    bool threw = false;
    std::thread responder([&channel, &threw] {
        try {
            static_cast<void>(channel.receive());
        } catch (const kythira::coap_credential_bootstrap_error&) {
            threw = true;
        }
    });

    // Give the responder a moment to reach receive(), then pull the rug.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    channel.abandon();
    responder.join();

    BOOST_TEST(threw);
}

// Shutdown beats a message already in the queue: receive() tests `_abandoned`
// before it looks at `_inbound`, so a handshake caught mid-step is dropped
// rather than half-completed. Pinned deliberately -- it is the behaviour a
// shutdown path wants, and the opposite would let a stopping responder pick up
// one more handshake step on its way out.
//
// It also pins the division of labour between the two release paths: abandon()
// frees the responder thread, and only fail() frees a handler blocked in
// exchange(). A shutdown must therefore call both, which is why abandon() alone
// leaves the handler below still waiting.
BOOST_AUTO_TEST_CASE(abandon_discards_a_queued_message_and_fail_frees_the_handler,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(20))) {
    kythira::edhoc_responder_channel channel;

    // Queue message_1 from a handler thread, which then blocks awaiting a reply.
    std::optional<std::vector<std::byte>> handler_reply;
    std::thread handler(
        [&channel, &handler_reply] { handler_reply = channel.exchange(bytes({0x09})); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    channel.abandon();
    BOOST_CHECK_THROW(static_cast<void>(channel.receive()),
                      kythira::coap_credential_bootstrap_error);

    // The handler is still parked -- abandon() does not touch exchange()'s
    // predicate. fail() is what releases it.
    channel.fail();
    handler.join();
    BOOST_TEST(!handler_reply.has_value());
}

BOOST_AUTO_TEST_SUITE_END()

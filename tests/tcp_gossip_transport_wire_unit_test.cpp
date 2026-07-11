// Unit test for tcp_gossip_transport's wire encode/decode round-trip.
// See .kiro/specs/peer2peer-gossip-transport/, Requirements 5.1/5.2, 10.1.
//
// Fanout selection (min(fanout, eligible.size()), excluding self) is
// exercised end-to-end by tcp_gossip_transport_integration_test.cpp rather
// than here: run_one_round()/exchange_with() are private and always perform
// real socket I/O, so there is no pure-logic seam to test fanout selection
// in isolation without also standing up real listeners (Requirement 10.2).
#define BOOST_TEST_MODULE tcp_gossip_transport_wire_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/tcp_gossip_transport.hpp>

BOOST_AUTO_TEST_CASE(encode_decode_round_trip_multiple_digests) {
    using message_t = kythira::gossip_exchange_message<std::uint64_t, std::string, std::uint64_t>;

    message_t msg;
    msg.sender_node_id = 2;
    msg.digests = {
        {1, "10.0.0.1:9000", 3, 250, 1799999500},
        {2, "10.0.0.2:9000", 3, 251, 1799999620},
        {3, "10.0.0.3:9000", 4, 0, 1799999700},
    };

    auto encoded = kythira::encode_gossip_message(msg);
    auto decoded =
        kythira::decode_gossip_message<std::uint64_t, std::string, std::uint64_t>(encoded);

    BOOST_CHECK_EQUAL(decoded.sender_node_id, msg.sender_node_id);
    BOOST_REQUIRE_EQUAL(decoded.digests.size(), msg.digests.size());
    for (std::size_t i = 0; i < msg.digests.size(); ++i) {
        BOOST_CHECK_EQUAL(decoded.digests[i].node_id, msg.digests[i].node_id);
        BOOST_CHECK_EQUAL(decoded.digests[i].address, msg.digests[i].address);
        BOOST_CHECK_EQUAL(decoded.digests[i].term, msg.digests[i].term);
        BOOST_CHECK_EQUAL(decoded.digests[i].last_log_index, msg.digests[i].last_log_index);
        BOOST_CHECK_EQUAL(decoded.digests[i].fresh_until, msg.digests[i].fresh_until);
    }
}

BOOST_AUTO_TEST_CASE(encode_decode_empty_digests) {
    using message_t = kythira::gossip_exchange_message<std::uint64_t, std::string, std::uint64_t>;

    message_t msg;
    msg.sender_node_id = 42;

    auto encoded = kythira::encode_gossip_message(msg);
    auto decoded =
        kythira::decode_gossip_message<std::uint64_t, std::string, std::uint64_t>(encoded);

    BOOST_CHECK_EQUAL(decoded.sender_node_id, 42u);
    BOOST_CHECK(decoded.digests.empty());
}

BOOST_AUTO_TEST_CASE(decode_malformed_payload_throws) {
    BOOST_CHECK_THROW((kythira::decode_gossip_message<std::uint64_t, std::string, std::uint64_t>(
                          "not json at all")),
                      std::exception);
    BOOST_CHECK_THROW(
        (kythira::decode_gossip_message<std::uint64_t, std::string, std::uint64_t>("{}")),
        std::exception);
}

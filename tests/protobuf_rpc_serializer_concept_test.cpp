#include <raft/protobuf_serializer.hpp>

#define BOOST_TEST_MODULE protobuf_rpc_serializer_concept_test
#include <boost/test/unit_test.hpp>

// Concept-conformance and smoke tests for protobuf_rpc_serializer, mirroring
// tests/rpc_serializer_concept_test.cpp. Validates Requirement 10.1 / 4.5.

BOOST_AUTO_TEST_CASE(test_protobuf_rpc_serializer_concept) {
    // protobuf_rpc_serializer must satisfy the rpc_serializer concept, mirroring
    // json_serializer.hpp's compile-time static_assert.
    static_assert(kythira::rpc_serializer<kythira::protobuf_rpc_serializer<std::vector<std::byte>>,
                                          std::vector<std::byte>>,
                  "protobuf_rpc_serializer should satisfy rpc_serializer concept");
}

BOOST_AUTO_TEST_CASE(test_protobuf_serializer_instantiation) {
    // Verify we can instantiate the serializer and round-trip a request.
    kythira::protobuf_rpc_serializer<std::vector<std::byte>> serializer;

    kythira::request_vote_request<> req{1, 2, 3, 4};

    auto data = serializer.serialize(req);
    BOOST_TEST(!data.empty());

    auto deserialized = serializer.deserialize_request_vote_request(data);

    BOOST_TEST(deserialized.term() == req.term());
    BOOST_TEST(deserialized.candidate_id() == req.candidate_id());
    BOOST_TEST(deserialized.last_log_index() == req.last_log_index());
    BOOST_TEST(deserialized.last_log_term() == req.last_log_term());
}

BOOST_AUTO_TEST_CASE(test_protobuf_serializer_name) {
    // name() returns "protobuf", distinguishing it from json_rpc_serializer's
    // "json" (Requirement 4.4). Transports derive their Content-Type/Content-
    // Format from this string.
    kythira::protobuf_rpc_serializer<std::vector<std::byte>> serializer;
    BOOST_TEST(serializer.name() == "protobuf");
}

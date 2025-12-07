#include <raft/types.hpp>
#include <raft/json_serializer.hpp>

#define BOOST_TEST_MODULE rpc_serializer_concept_test
#include <boost/test/included/unit_test.hpp>

namespace {
    constexpr const char* test_name = "rpc_serializer_concept_test";
}

BOOST_AUTO_TEST_CASE(test_serialized_data_concept) {
    // Test that std::vector<std::byte> satisfies serialized_data concept
    static_assert(raft::serialized_data<std::vector<std::byte>>, 
                  "std::vector<std::byte> should satisfy serialized_data concept");
    
    // Test that std::vector<char> does NOT satisfy serialized_data concept
    static_assert(!raft::serialized_data<std::vector<char>>, 
                  "std::vector<char> should NOT satisfy serialized_data concept");
    
    // Test that std::string does NOT satisfy serialized_data concept
    static_assert(!raft::serialized_data<std::string>, 
                  "std::string should NOT satisfy serialized_data concept");
}

BOOST_AUTO_TEST_CASE(test_rpc_serializer_concept) {
    // Test that json_rpc_serializer satisfies rpc_serializer concept
    static_assert(raft::rpc_serializer<raft::json_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>, 
                  "json_rpc_serializer should satisfy rpc_serializer concept");
}

BOOST_AUTO_TEST_CASE(test_json_serializer_instantiation) {
    // Verify we can instantiate the serializer
    raft::json_rpc_serializer<std::vector<std::byte>> serializer;
    
    // Create a simple request
    raft::request_vote_request<> req{1, 2, 3, 4};
    
    // Serialize it
    auto data = serializer.serialize(req);
    
    // Verify we got some data
    BOOST_TEST(!data.empty());
    
    // Deserialize it
    auto deserialized = serializer.deserialize_request_vote_request(data);
    
    // Verify the data matches
    BOOST_TEST(deserialized.term() == req.term());
    BOOST_TEST(deserialized.candidate_id() == req.candidate_id());
    BOOST_TEST(deserialized.last_log_index() == req.last_log_index());
    BOOST_TEST(deserialized.last_log_term() == req.last_log_term());
}

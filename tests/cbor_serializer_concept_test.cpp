#include <raft/cbor_serializer.hpp>

#define BOOST_TEST_MODULE cbor_serializer_concept_test
#include <boost/test/unit_test.hpp>

#include <raft/coap_utils.hpp>

namespace {
constexpr const char* test_name = "cbor_serializer_concept_test";
}

BOOST_AUTO_TEST_CASE(test_serialized_data_concept) {
    // Test that std::vector<std::byte> satisfies serialized_data concept
    static_assert(kythira::serialized_data<std::vector<std::byte>>,
                  "std::vector<std::byte> should satisfy serialized_data concept");

    // Test that std::vector<char> does NOT satisfy serialized_data concept
    static_assert(!kythira::serialized_data<std::vector<char>>,
                  "std::vector<char> should NOT satisfy serialized_data concept");

    // Test that std::string does NOT satisfy serialized_data concept
    static_assert(!kythira::serialized_data<std::string>,
                  "std::string should NOT satisfy serialized_data concept");
}

BOOST_AUTO_TEST_CASE(test_rpc_serializer_concept) {
    // Test that cbor_rpc_serializer satisfies rpc_serializer concept
    static_assert(kythira::rpc_serializer<kythira::cbor_rpc_serializer<std::vector<std::byte>>,
                                          std::vector<std::byte>>,
                  "cbor_rpc_serializer should satisfy rpc_serializer concept");

    // The convenience alias resolves to the same instantiation.
    static_assert(std::same_as<kythira::cbor_serializer,
                               kythira::cbor_rpc_serializer<std::vector<std::byte>>>,
                  "cbor_serializer alias should name cbor_rpc_serializer<std::vector<std::byte>>");
}

BOOST_AUTO_TEST_CASE(test_cbor_serializer_instantiation) {
    // Verify we can instantiate the serializer
    kythira::cbor_rpc_serializer<std::vector<std::byte>> serializer;

    // Create a simple request
    kythira::request_vote_request<> req{1, 2, 3, 4};

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

BOOST_AUTO_TEST_CASE(test_name_resolves_to_cbor_content_format) {
    // name() must contain the "cbor" substring the CoAP content-format detection
    // keys off of, and resolving that name must land on application/cbor with no
    // change to get_content_format_for_serializer.
    kythira::cbor_rpc_serializer<std::vector<std::byte>> serializer;

    BOOST_TEST(serializer.name().find("cbor") != std::string::npos);
    BOOST_TEST((kythira::coap_utils::get_content_format_for_serializer(serializer.name()) ==
                kythira::coap_utils::coap_content_format::application_cbor));
}

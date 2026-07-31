#include <raft/ion_serializer.hpp>

#define BOOST_TEST_MODULE ion_serializer_concept_test
#include <boost/test/unit_test.hpp>

#include <raft/coap_utils.hpp>

namespace {
constexpr const char* test_name = "ion_serializer_concept_test";
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
    // Requirement 8.1: ion_rpc_serializer<std::vector<std::byte>> satisfies the
    // rpc_serializer concept (mirrors rpc_serializer_concept_test.cpp for JSON).
    static_assert(kythira::rpc_serializer<kythira::ion_rpc_serializer<std::vector<std::byte>>,
                                          std::vector<std::byte>>,
                  "ion_rpc_serializer should satisfy rpc_serializer concept");

    // The convenience alias resolves to the same instantiation.
    static_assert(
        std::same_as<kythira::ion_serializer, kythira::ion_rpc_serializer<std::vector<std::byte>>>,
        "ion_serializer alias should name ion_rpc_serializer<std::vector<std::byte>>");
}

BOOST_AUTO_TEST_CASE(test_ion_serializer_smoke_binary) {
    // Default construction selects binary encoding.
    kythira::ion_rpc_serializer<std::vector<std::byte>> serializer;

    kythira::request_vote_request<> req{1, 2, 3, 4};
    auto data = serializer.serialize(req);
    BOOST_TEST(!data.empty());

    // Requirement 4.2: binary output begins with Ion's binary version marker.
    BOOST_REQUIRE(data.size() >= 4);
    BOOST_TEST(static_cast<unsigned>(data[0]) == 0xE0u);
    BOOST_TEST(static_cast<unsigned>(data[1]) == 0x01u);
    BOOST_TEST(static_cast<unsigned>(data[2]) == 0x00u);
    BOOST_TEST(static_cast<unsigned>(data[3]) == 0xEAu);

    auto deserialized = serializer.deserialize_request_vote_request(data);
    BOOST_TEST(deserialized.term() == req.term());
    BOOST_TEST(deserialized.candidate_id() == req.candidate_id());
    BOOST_TEST(deserialized.last_log_index() == req.last_log_index());
    BOOST_TEST(deserialized.last_log_term() == req.last_log_term());
}

BOOST_AUTO_TEST_CASE(test_ion_serializer_smoke_text) {
    kythira::ion_rpc_serializer<std::vector<std::byte>> serializer(kythira::ion_encoding::text);

    kythira::request_vote_request<> req{1, 2, 3, 4};
    auto data = serializer.serialize(req);
    BOOST_TEST(!data.empty());

    // Requirement 4.3: text output does NOT begin with the binary marker.
    BOOST_TEST(static_cast<unsigned>(data[0]) != 0xE0u);

    auto deserialized = serializer.deserialize_request_vote_request(data);
    BOOST_TEST(deserialized.term() == req.term());
    BOOST_TEST(deserialized.candidate_id() == req.candidate_id());
    BOOST_TEST(deserialized.last_log_index() == req.last_log_index());
    BOOST_TEST(deserialized.last_log_term() == req.last_log_term());
}

BOOST_AUTO_TEST_CASE(test_name_distinguishes_encoding_and_resolves_to_ion_format) {
    // Requirement 1.4 / 4.5: name() distinguishes binary from text, and both
    // resolve to application/ion via the shared content-format detection.
    kythira::ion_rpc_serializer<std::vector<std::byte>> binary(kythira::ion_encoding::binary);
    kythira::ion_rpc_serializer<std::vector<std::byte>> text(kythira::ion_encoding::text);

    BOOST_TEST(binary.name() == "ion-binary");
    BOOST_TEST(text.name() == "ion-text");
    BOOST_TEST(binary.name() != text.name());

    BOOST_TEST(binary.name().find("ion") != std::string::npos);
    BOOST_TEST(text.name().find("ion") != std::string::npos);

    BOOST_TEST((kythira::coap_utils::get_content_format_for_serializer(binary.name()) ==
                kythira::coap_utils::coap_content_format::application_ion));
    BOOST_TEST((kythira::coap_utils::get_content_format_for_serializer(text.name()) ==
                kythira::coap_utils::coap_content_format::application_ion));
    BOOST_TEST(kythira::coap_utils::content_format_to_string(
                   kythira::coap_utils::coap_content_format::application_ion) == "application/ion");
}

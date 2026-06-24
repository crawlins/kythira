#define BOOST_TEST_MODULE membership_change_leave_rpc_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/json_serializer.hpp>
#include <raft/types.hpp>

#include <folly/init/Init.h>

#include <string>

struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("membership_change_leave_rpc_unit_test"), nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

BOOST_AUTO_TEST_SUITE(leave_rpc_serialization)

BOOST_AUTO_TEST_CASE(cluster_leave_request_round_trip) {
    kythira::json_rpc_serializer<std::vector<std::byte>> ser;

    kythira::cluster_leave_request<std::uint64_t, std::string> req;
    req.node_id = 42;

    auto data = ser.serialize(req);
    auto decoded = ser.deserialize_cluster_leave_request(data);

    BOOST_CHECK_EQUAL(decoded.leaving_node_id(), 42u);
}

BOOST_AUTO_TEST_CASE(cluster_leave_response_accepted_round_trip) {
    kythira::json_rpc_serializer<std::vector<std::byte>> ser;

    kythira::cluster_leave_response<std::uint64_t, std::string> resp;
    resp.accepted = true;

    auto data = ser.serialize(resp);
    auto decoded = ser.deserialize_cluster_leave_response(data);

    BOOST_CHECK(decoded.is_accepted());
    BOOST_CHECK(!decoded.redirect_peer().has_value());
}

BOOST_AUTO_TEST_CASE(cluster_leave_response_redirect_round_trip) {
    kythira::json_rpc_serializer<std::vector<std::byte>> ser;

    kythira::cluster_leave_response<std::uint64_t, std::string> resp;
    resp.accepted = false;
    kythira::peer_info<std::uint64_t, std::string> pi;
    pi.node_id = 7;
    pi.address = "7";
    resp.redirect = pi;

    auto data = ser.serialize(resp);
    auto decoded = ser.deserialize_cluster_leave_response(data);

    BOOST_CHECK(!decoded.is_accepted());
    BOOST_REQUIRE(decoded.redirect_peer().has_value());
    BOOST_CHECK_EQUAL(decoded.redirect_peer()->node_id, 7u);
    BOOST_CHECK_EQUAL(decoded.redirect_peer()->address, "7");
}

BOOST_AUTO_TEST_CASE(deserialize_wrong_type_throws) {
    kythira::json_rpc_serializer<std::vector<std::byte>> ser;

    kythira::cluster_leave_request<std::uint64_t, std::string> req;
    req.node_id = 1;
    auto data = ser.serialize(req);

    // Trying to deserialize a request as a response must throw
    BOOST_CHECK_THROW(ser.deserialize_cluster_leave_response(data),
                      kythira::serialization_exception);
}

BOOST_AUTO_TEST_SUITE_END()

#define BOOST_TEST_MODULE acme_test_server_unit_test

#include <boost/test/unit_test.hpp>

#include "acme_test_server.hpp"

#include <raft/acme_jws.hpp>

#include <httplib.h>
#include <boost/json.hpp>

using namespace raft::testing;

BOOST_AUTO_TEST_CASE(server_starts_and_serves_directory, *boost::unit_test::timeout(15)) {
    acme_test_server server;
    BOOST_TEST(!server.directory_url().empty());
    BOOST_TEST(!server.root_certificate_pem().empty());

    auto colon = server.base_url().rfind(':');
    int port = std::stoi(server.base_url().substr(colon + 1));
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(2, 0);

    auto res = client.Get("/directory");
    BOOST_REQUIRE(res);
    BOOST_TEST(res->status == 200);
    auto dir = boost::json::parse(res->body).as_object();
    BOOST_TEST(dir.contains("newNonce"));
    BOOST_TEST(dir.contains("newAccount"));
    BOOST_TEST(dir.contains("newOrder"));
}

BOOST_AUTO_TEST_CASE(new_nonce_returns_replay_nonce_header, *boost::unit_test::timeout(15)) {
    acme_test_server server;
    auto colon = server.base_url().rfind(':');
    int port = std::stoi(server.base_url().substr(colon + 1));
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(2, 0);

    auto res = client.Get("/new-nonce");
    BOOST_REQUIRE(res);
    BOOST_TEST(res->status == 204);
    BOOST_TEST(!res->get_header_value("Replay-Nonce").empty());
}

BOOST_AUTO_TEST_CASE(new_account_with_valid_jws_succeeds, *boost::unit_test::timeout(15)) {
    acme_test_server server;
    auto colon = server.base_url().rfind(':');
    int port = std::stoi(server.base_url().substr(colon + 1));
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(2, 0);

    auto nonce_res = client.Get("/new-nonce");
    BOOST_REQUIRE(nonce_res);
    std::string nonce = nonce_res->get_header_value("Replay-Nonce");

    auto key = acme_jws::generate_p256_key();
    boost::json::object header;
    header["jwk"] = acme_jws::jwk_from_public_key(key.get());
    header["nonce"] = nonce;
    header["url"] = server.base_url() + "/new-account";

    std::string payload =
        boost::json::serialize(boost::json::object{{"termsOfServiceAgreed", true}});
    auto compact = acme_jws::sign(payload, header, key.get());

    auto dot1 = compact.find('.');
    auto dot2 = compact.find('.', dot1 + 1);
    boost::json::object flattened;
    flattened["protected"] = compact.substr(0, dot1);
    flattened["payload"] = compact.substr(dot1 + 1, dot2 - dot1 - 1);
    flattened["signature"] = compact.substr(dot2 + 1);

    auto res = client.Post("/new-account", boost::json::serialize(flattened), "application/json");
    BOOST_REQUIRE(res);
    BOOST_TEST_MESSAGE("body: " << res->body);
    BOOST_TEST(res->status == 201);
    BOOST_TEST(!res->get_header_value("Location").empty());
    BOOST_TEST(!res->get_header_value("Replay-Nonce").empty());
}

BOOST_AUTO_TEST_CASE(new_account_with_bad_signature_rejected, *boost::unit_test::timeout(15)) {
    acme_test_server server;
    auto colon = server.base_url().rfind(':');
    int port = std::stoi(server.base_url().substr(colon + 1));
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(2, 0);

    auto nonce_res = client.Get("/new-nonce");
    std::string nonce = nonce_res->get_header_value("Replay-Nonce");

    auto signer_key = acme_jws::generate_p256_key();
    auto claimed_key = acme_jws::generate_p256_key();  // JWK in header != actual signer

    boost::json::object header;
    header["jwk"] = acme_jws::jwk_from_public_key(claimed_key.get());
    header["nonce"] = nonce;
    header["url"] = server.base_url() + "/new-account";
    auto compact =
        acme_jws::sign(boost::json::serialize(boost::json::object{{"termsOfServiceAgreed", true}}),
                       header, signer_key.get());

    auto dot1 = compact.find('.');
    auto dot2 = compact.find('.', dot1 + 1);
    boost::json::object flattened;
    flattened["protected"] = compact.substr(0, dot1);
    flattened["payload"] = compact.substr(dot1 + 1, dot2 - dot1 - 1);
    flattened["signature"] = compact.substr(dot2 + 1);

    auto res = client.Post("/new-account", boost::json::serialize(flattened), "application/json");
    BOOST_REQUIRE(res);
    BOOST_TEST(res->status == 400);
    auto problem = boost::json::parse(res->body).as_object();
    BOOST_TEST(problem.at("type").as_string() == "urn:ietf:params:acme:error:badSignature");
}

BOOST_AUTO_TEST_CASE(reused_nonce_rejected, *boost::unit_test::timeout(15)) {
    acme_test_server server;
    auto colon = server.base_url().rfind(':');
    int port = std::stoi(server.base_url().substr(colon + 1));
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(2, 0);

    auto nonce_res = client.Get("/new-nonce");
    std::string nonce = nonce_res->get_header_value("Replay-Nonce");
    auto key = acme_jws::generate_p256_key();

    auto make_flattened = [&]() {
        boost::json::object header;
        header["jwk"] = acme_jws::jwk_from_public_key(key.get());
        header["nonce"] = nonce;
        header["url"] = server.base_url() + "/new-account";
        auto compact = acme_jws::sign(
            boost::json::serialize(boost::json::object{{"termsOfServiceAgreed", true}}), header,
            key.get());
        auto dot1 = compact.find('.');
        auto dot2 = compact.find('.', dot1 + 1);
        boost::json::object flattened;
        flattened["protected"] = compact.substr(0, dot1);
        flattened["payload"] = compact.substr(dot1 + 1, dot2 - dot1 - 1);
        flattened["signature"] = compact.substr(dot2 + 1);
        return boost::json::serialize(flattened);
    };

    auto body = make_flattened();
    auto res1 = client.Post("/new-account", body, "application/json");
    BOOST_REQUIRE(res1);
    BOOST_TEST(res1->status == 201);

    // Reuse the SAME nonce on a second, distinct request.
    auto res2 = client.Post("/new-account", body, "application/json");
    BOOST_REQUIRE(res2);
    BOOST_TEST(res2->status == 400);
    auto problem = boost::json::parse(res2->body).as_object();
    BOOST_TEST(problem.at("type").as_string() == "urn:ietf:params:acme:error:badNonce");
}

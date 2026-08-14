#define BOOST_TEST_MODULE alibaba_oss_client_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/alibaba_oss_client.hpp>

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <map>
#include <string>
#include <thread>

// Unit coverage for the OSS object client
// (.kiro/specs/alibaba-cloud-services/ Requirement 16.2): V4 signing shape,
// path-style addressing under endpoint_override, the four operations, and —
// the load-bearing one for a persistence backend — that a structurally
// unexpected listing is an error rather than a silently short answer.

using namespace kythira;
using namespace std::chrono_literals;

namespace {

struct MockServer {
    httplib::Server server;
    std::thread thread;
    int port{0};
    std::atomic<int> request_count{0};

    void start() {
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
        for (int i = 0; i < 200 && !server.is_running(); ++i) {
            std::this_thread::sleep_for(5ms);
        }
    }

    ~MockServer() {
        server.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }

    MockServer() = default;
    MockServer(const MockServer&) = delete;
    auto operator=(const MockServer&) -> MockServer& = delete;
    MockServer(MockServer&&) = delete;
    auto operator=(MockServer&&) -> MockServer& = delete;

    [[nodiscard]] auto origin() const -> std::string {
        return "http://127.0.0.1:" + std::to_string(port);
    }
};

auto config_for(const MockServer& mock) -> alibaba_client_config {
    alibaba_client_config cfg;
    cfg.region = "cn-hangzhou";
    cfg.access_key_id = "test-ak";
    cfg.access_key_secret = "test-secret";
    cfg.endpoint_override = mock.origin();
    cfg.api_timeout = 5s;
    return cfg;
}

auto listing_page(const std::string& keys_xml, bool truncated, const std::string& next_token = {})
    -> std::string {
    std::string body = "<?xml version=\"1.0\"?><ListBucketResult>";
    body += keys_xml;
    body += std::string("<IsTruncated>") + (truncated ? "true" : "false") + "</IsTruncated>";
    if (!next_token.empty()) {
        body += "<NextContinuationToken>" + next_token + "</NextContinuationToken>";
    }
    body += "</ListBucketResult>";
    return body;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(alibaba_oss_addressing)

// Virtual-host style against real OSS; path-style under the override so a
// local mock needs no wildcard DNS (Requirement 2.3).
BOOST_AUTO_TEST_CASE(virtual_host_style_by_default_path_style_under_override) {
    alibaba_client_config cfg;
    cfg.region = "cn-hangzhou";
    cfg.access_key_id = "id";
    cfg.access_key_secret = "secret";

    const alibaba_oss_client real{cfg};
    const auto [origin, path] = real.origin_and_path("mybucket", "raft/term");
    BOOST_TEST(origin == "https://mybucket.oss-cn-hangzhou.aliyuncs.com");
    BOOST_TEST(path == "/raft/term");

    cfg.endpoint_override = "http://127.0.0.1:9999";
    const alibaba_oss_client mocked{cfg};
    const auto [mock_origin, mock_path] = mocked.origin_and_path("mybucket", "raft/term");
    BOOST_TEST(mock_origin == "http://127.0.0.1:9999");
    BOOST_TEST(mock_path == "/mybucket/raft/term");
}

// The signature covers the path-style resource in both addressings, which is
// what makes one signing path serve both.
BOOST_AUTO_TEST_CASE(v4_authorization_has_the_documented_shape) {
    alibaba_client_config cfg;
    cfg.region = "cn-hangzhou";
    cfg.access_key_id = "LTAIexample";
    cfg.access_key_secret = "secret";
    const alibaba_oss_client client{cfg};

    const auto headers = client.prepared_headers(
        "bucket", "key", "PUT", {}, std::chrono::system_clock::from_time_t(1744353684));

    BOOST_TEST(headers.at("x-oss-content-sha256") == "UNSIGNED-PAYLOAD");
    BOOST_TEST(headers.at("x-oss-date") == "20250411T064124Z");
    const std::string auth = headers.at("Authorization");
    BOOST_TEST(auth.find("OSS4-HMAC-SHA256 Credential=LTAIexample/20250411/cn-hangzhou/oss/"
                         "aliyun_v4_request") == 0U);
    BOOST_TEST(auth.find(",Signature=") != std::string::npos);
}

// The bug the first live call found (spike-notes.md Finding 2): OSS V4 signs
// Content-Type when the request carries one, and httplib sets that header
// from its body-overload argument — so a PUT whose signature omits it is
// rejected with SignatureDoesNotMatch. Pinned here because no mock can catch
// it: this repo's mock servers do not verify signatures.
BOOST_AUTO_TEST_CASE(content_type_is_signed_when_the_request_carries_one) {
    alibaba_client_config cfg;
    cfg.region = "cn-hangzhou";
    cfg.access_key_id = "id";
    cfg.access_key_secret = "secret";
    const alibaba_oss_client client{cfg};
    const auto when = std::chrono::system_clock::from_time_t(1744353684);

    const auto with_body =
        client.prepared_headers("b", "k", "PUT", {}, when, "application/octet-stream");
    BOOST_TEST(with_body.at("content-type") == "application/octet-stream");

    // A bodyless request carries no Content-Type, so signing one would
    // canonicalise a header the wire never sees — the same failure mirrored.
    const auto bodyless = client.prepared_headers("b", "k", "GET", {}, when);
    BOOST_TEST(bodyless.count("content-type") == 0U);

    // Different signed sets must produce different signatures.
    BOOST_TEST(with_body.at("Authorization") != bodyless.at("Authorization"));
}

BOOST_AUTO_TEST_CASE(security_token_is_signed_when_present) {
    alibaba_client_config cfg;
    cfg.region = "cn-hangzhou";
    cfg.access_key_id = "id";
    cfg.access_key_secret = "secret";
    cfg.security_token = "sts-token";
    const alibaba_oss_client client{cfg};

    const auto headers =
        client.prepared_headers("b", "k", "GET", {}, std::chrono::system_clock::now());
    BOOST_TEST(headers.at("x-oss-security-token") == "sts-token");
}

BOOST_AUTO_TEST_CASE(missing_credentials_throw) {
    alibaba_client_config cfg;
    cfg.region = "cn-hangzhou";
    const alibaba_oss_client client{cfg};
    BOOST_CHECK_THROW(
        client.prepared_headers("b", "k", "GET", {}, std::chrono::system_clock::now()),
        std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(alibaba_oss_operations)

BOOST_AUTO_TEST_CASE(put_get_delete_round_trip) {
    MockServer mock;
    std::map<std::string, std::string> store;
    mock.server.Put(
        "/mybucket/raft/term", [&store](const httplib::Request& req, httplib::Response& res) {
            BOOST_TEST(req.get_header_value("x-oss-content-sha256") == "UNSIGNED-PAYLOAD");
            BOOST_TEST(!req.get_header_value("Authorization").empty());
            store["raft/term"] = req.body;
            res.status = 200;
        });
    mock.server.Get("/mybucket/raft/term",
                    [&store](const httplib::Request&, httplib::Response& res) {
                        auto it = store.find("raft/term");
                        if (it == store.end()) {
                            res.status = 404;
                            return;
                        }
                        res.set_content(it->second, "application/octet-stream");
                    });
    mock.server.Delete("/mybucket/raft/term",
                       [&store](const httplib::Request&, httplib::Response& res) {
                           store.erase("raft/term");
                           res.status = 204;
                       });
    mock.start();

    const alibaba_oss_client client{config_for(mock)};
    client.put_object("mybucket", "raft/term", "42");
    BOOST_TEST(client.get_object("mybucket", "raft/term").value() == "42");
    client.delete_object("mybucket", "raft/term");
    BOOST_TEST(!client.get_object("mybucket", "raft/term").has_value());
}

// Absent is an answer, not an error — a persistence load path needs to tell
// "no such object" from "the store is broken".
BOOST_AUTO_TEST_CASE(missing_object_is_nullopt_not_throw) {
    MockServer mock;
    mock.server.Get("/b/absent", [](const httplib::Request&, httplib::Response& res) {
        res.status = 404;
        res.set_content("<Error><Code>NoSuchKey</Code></Error>", "application/xml");
    });
    mock.start();

    const alibaba_oss_client client{config_for(mock)};
    BOOST_TEST(!client.get_object("b", "absent").has_value());
}

BOOST_AUTO_TEST_CASE(binary_bytes_survive_a_round_trip) {
    MockServer mock;
    std::string stored;
    mock.server.Put("/b/blob", [&stored](const httplib::Request& req, httplib::Response& res) {
        stored = req.body;
        res.status = 200;
    });
    mock.server.Get("/b/blob", [&stored](const httplib::Request&, httplib::Response& res) {
        res.set_content(stored, "application/octet-stream");
    });
    mock.start();

    // Split literal, deliberately: a hex escape is greedy and 'b' is a hex
    // digit, so "\xffbinary" parses as the single escape \xffb — out of range
    // for char. GCC accepts it with a warning; clang++-18 (what CI builds
    // with) rejects it outright. Concatenation terminates the escape.
    const std::string payload(
        "\x00\x01\xff"
        "binary\x00",
        11);
    const alibaba_oss_client client{config_for(mock)};
    client.put_object("b", "blob", payload);
    BOOST_TEST(client.get_object("b", "blob").value() == payload);
}

BOOST_AUTO_TEST_CASE(oss_error_xml_surfaces_in_the_exception) {
    MockServer mock;
    mock.server.Put("/b/k", [](const httplib::Request&, httplib::Response& res) {
        res.status = 403;
        res.set_content("<Error><Code>AccessDenied</Code><Message>no permission</Message></Error>",
                        "application/xml");
    });
    mock.start();

    const alibaba_oss_client client{config_for(mock)};
    try {
        client.put_object("b", "k", "x");
        BOOST_FAIL("expected a runtime_error");
    } catch (const std::runtime_error& e) {
        const std::string what = e.what();
        BOOST_TEST(what.find("AccessDenied") != std::string::npos);
        BOOST_TEST(what.find("no permission") != std::string::npos);
    }
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(alibaba_oss_listing)

BOOST_AUTO_TEST_CASE(single_page_listing_decodes_keys) {
    MockServer mock;
    mock.server.Get("/b/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(listing_page("<Key>raft%2Fterm</Key><Key>raft%2Flog%2F001</Key>", false),
                        "application/xml");
    });
    mock.start();

    const alibaba_oss_client client{config_for(mock)};
    const auto keys = client.list_keys("b", "raft/");
    BOOST_REQUIRE(keys.size() == 2U);
    BOOST_TEST(keys[0] == "raft/term");
    BOOST_TEST(keys[1] == "raft/log/001");
}

// Pagination followed to exhaustion: a persistence engine that read only the
// first page would silently lose the tail of its log.
BOOST_AUTO_TEST_CASE(pagination_is_followed_to_exhaustion) {
    MockServer mock;
    mock.server.Get("/b/", [&mock](const httplib::Request& req, httplib::Response& res) {
        ++mock.request_count;
        if (req.get_param_value("continuation-token").empty()) {
            res.set_content(listing_page("<Key>a</Key>", true, "TOKEN-2"), "application/xml");
        } else {
            BOOST_TEST(req.get_param_value("continuation-token") == "TOKEN-2");
            res.set_content(listing_page("<Key>b</Key>", false), "application/xml");
        }
    });
    mock.start();

    const alibaba_oss_client client{config_for(mock)};
    const auto keys = client.list_keys("b", "");
    BOOST_REQUIRE(keys.size() == 2U);
    BOOST_TEST(keys[0] == "a");
    BOOST_TEST(keys[1] == "b");
    BOOST_TEST(mock.request_count.load() == 2);
}

// Requirement 2.4's hard rule: a listing this client cannot interpret is an
// error. Treating it as "no more keys" is silent data loss.
BOOST_AUTO_TEST_CASE(listing_without_is_truncated_throws) {
    MockServer mock;
    mock.server.Get("/b/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("<ListBucketResult><Key>a</Key></ListBucketResult>", "application/xml");
    });
    mock.start();

    const alibaba_oss_client client{config_for(mock)};
    BOOST_CHECK_THROW(client.list_keys("b", ""), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(truncated_listing_without_token_throws) {
    MockServer mock;
    mock.server.Get("/b/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(listing_page("<Key>a</Key>", true), "application/xml");
    });
    mock.start();

    const alibaba_oss_client client{config_for(mock)};
    BOOST_CHECK_THROW(client.list_keys("b", ""), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(unterminated_element_throws) {
    MockServer mock;
    mock.server.Get("/b/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("<ListBucketResult><Key>a", "application/xml");
    });
    mock.start();

    const alibaba_oss_client client{config_for(mock)};
    BOOST_CHECK_THROW(client.list_keys("b", ""), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()

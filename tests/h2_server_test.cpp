// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// HTTP/2 in the core HTTPS server (include/raft/beast_https_server.hpp,
// include/raft/h2_server_session.hpp; Kinesis SubscribeToShard,
// .kiro/specs/kinesis-compatible-streams/ task 13): h2c with prior
// knowledge and h2 over TLS by ALPN, multiplexed requests, streamed bodies
// next to ordinary requests on one connection, a client reset cancelling a
// stream, the body limit, HTTP/1.1 still served alongside, and shutdown
// with a stream open. Built only with nghttp2.

#define BOOST_TEST_MODULE h2_server_test
#include <boost/test/unit_test.hpp>

#include <raft/beast_https_server.hpp>
#include <raft/certificate_authority.hpp>
#include <raft/private_file.hpp>

#include "h2_test_client.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <openssl/ssl.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace ssl = boost::asio::ssl;
namespace https = kythira::https;
using namespace std::chrono_literals;
using kythira::testing::h2_client;

struct pki {
    std::filesystem::path dir;
    std::string ca_path;
    std::string server_cert;
    std::string server_key;

    pki() {
        dir = raft::make_private_temp_directory(std::filesystem::temp_directory_path(),
                                                "h2-server-test-");
        raft::testing::certificate_authority ca;
        auto write = [&](const std::string& name, const std::string& pem) {
            const auto p = (dir / name).string();
            raft::write_private_file(p, pem);
            return p;
        };
        ca_path = write("ca.pem", ca.root_certificate_pem());
        raft::testing::leaf_certificate_options server;
        server.subject.common_name = "localhost";
        server.dns_names = {"localhost"};
        server.ip_addresses = {"127.0.0.1"};
        auto s = ca.issue(server);
        server_cert = write("server.pem", s.chain_pem);
        server_key = write("server.key", s.private_key_pem);
    }
    ~pki() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    pki(const pki&) = delete;
    auto operator=(const pki&) -> pki& = delete;
};

// Answers with the method, target, a header and the body it saw.
auto echo(const https::request& req, const https::request_context& ctx) -> https::response {
    https::response res{http::status::ok, req.version()};
    res.set(http::field::content_type, "text/plain");
    res.set("x-seen-tls", ctx.tls ? "yes" : "no");
    res.body() = std::string(req.method_string()) + " " + std::string(req.target()) + " " +
                 std::string(req["x-test"]) + " " + req.body();
    return res;
}

// The stream handler: /stream answers with `count` pieces, `gap` apart, and
// records a cancel.
struct streamer {
    int count = 3;
    std::chrono::milliseconds gap{20};
    std::shared_ptr<std::atomic<int>> cancelled = std::make_shared<std::atomic<int>>(0);

    auto operator()(const https::request& req, const https::request_context& /*ctx*/) const
        -> std::optional<https::streaming_response> {
        if (req.target() != "/stream") {
            return std::nullopt;
        }
        https::streaming_response s;
        s.head = https::response{http::status::ok, req.version()};
        s.head.set(http::field::content_type, "application/x-pieces");
        auto sent = std::make_shared<int>(0);
        s.next = [sent, n = count, gap = gap](std::string& chunk) {
            std::this_thread::sleep_for(gap);
            if (*sent == n) {
                return false;
            }
            chunk = "piece" + std::to_string((*sent)++) + ";";
            return true;
        };
        s.cancel = [c = cancelled] { ++*c; };
        return s;
    }
};

auto plain_config() -> https::server_config {
    https::server_config cfg;
    cfg.bind_address = "127.0.0.1";
    cfg.insecure_plaintext = true;
    cfg.http2 = true;
    return cfg;
}

struct h2c_connection {
    asio::io_context ioc;
    asio::ip::tcp::socket socket{ioc};
    explicit h2c_connection(std::uint16_t port) {
        socket.connect({asio::ip::make_address("127.0.0.1"), port});
        kythira::testing::set_receive_timeout(socket.native_handle(), 10s);
    }
};

}  // namespace

BOOST_AUTO_TEST_CASE(h2c_prior_knowledge_serves_requests) {
    https::server server(plain_config(), echo);
    server.start();
    h2c_connection c(server.port());
    h2_client client(c.socket);

    const auto get = client.submit("GET", "/a?b=c", {{"x-test", "one"}});
    const auto post = client.submit("POST", "/p", {{"x-test", "two"}}, "hello body");
    const auto r2 = client.wait(post);
    const auto r1 = client.wait(get);
    BOOST_TEST(r1.status == 200);
    BOOST_TEST(r1.body == "GET /a?b=c one ");
    BOOST_TEST(r1.headers.at("content-type") == "text/plain");
    BOOST_TEST(r1.headers.at("x-seen-tls") == "no");
    BOOST_TEST(r2.status == 200);
    BOOST_TEST(r2.body == "POST /p two hello body");
    BOOST_TEST(r2.headers.at("content-length") == std::to_string(r2.body.size()));
    BOOST_TEST(server.stats().requests.load() == 2U);
}

BOOST_AUTO_TEST_CASE(http1_still_served_with_http2_on) {
    https::server server(plain_config(), echo);
    server.start();
    asio::io_context ioc;
    beast::tcp_stream s(ioc);
    s.connect({asio::ip::make_address("127.0.0.1"), server.port()});
    http::request<http::string_body> req{http::verb::post, "/one", 11};
    req.set(http::field::host, "localhost");
    req.body() = "x";
    req.prepare_payload();
    http::write(s, req);
    beast::flat_buffer buf;
    http::response<http::string_body> res;
    http::read(s, buf, res);
    BOOST_TEST(res.result_int() == 200U);
    BOOST_TEST(res.body() == "POST /one  x");
}

BOOST_AUTO_TEST_CASE(streamed_body_shares_the_connection) {
    streamer st;
    st.count = 10;
    st.gap = 30ms;
    https::server server(plain_config(), echo);
    server.set_stream_handler(st);
    server.start();
    h2c_connection c(server.port());
    h2_client client(c.socket);

    const auto stream = client.submit("POST", "/stream", {});
    // An ordinary request answers while the stream is still producing.
    const auto quick = client.submit("GET", "/quick", {});
    const auto q = client.wait(quick);
    BOOST_TEST(q.body == "GET /quick  ");
    const auto r = client.wait(stream);
    BOOST_TEST(r.status == 200);
    BOOST_TEST(r.error_code == 0U);
    BOOST_TEST(r.headers.at("content-type") == "application/x-pieces");
    BOOST_TEST(!r.headers.contains("content-length"));
    std::string want;
    for (int i = 0; i < 10; ++i) {
        want += "piece" + std::to_string(i) + ";";
    }
    BOOST_TEST(r.body == want);
    BOOST_TEST(st.cancelled->load() == 0);

    // The connection stays open for more.
    const auto after = client.wait(client.submit("GET", "/after", {}));
    BOOST_TEST(after.body == "GET /after  ");
}

BOOST_AUTO_TEST_CASE(client_reset_cancels_the_stream) {
    streamer st;
    st.count = 1000;
    st.gap = 10ms;
    https::server server(plain_config(), echo);
    server.set_stream_handler(st);
    server.start();
    h2c_connection c(server.port());
    h2_client client(c.socket);

    const auto id = client.submit("POST", "/stream", {});
    int pieces = 0;
    (void)client.wait(id, [&](std::int32_t, std::string_view) { return ++pieces < 3; });
    client.reset(id);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (st.cancelled->load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    BOOST_TEST(st.cancelled->load() == 1);
    const auto ok = client.wait(client.submit("GET", "/still", {}));
    BOOST_TEST(ok.body == "GET /still  ");
}

BOOST_AUTO_TEST_CASE(body_over_the_limit_is_413) {
    auto cfg = plain_config();
    cfg.max_body_bytes = 1000;
    https::server server(cfg, echo);
    server.start();
    h2c_connection c(server.port());
    h2_client client(c.socket);
    const auto big = client.wait(client.submit("POST", "/big", {}, std::string(5000, 'x')));
    BOOST_TEST(big.status == 413);
    const auto small = client.wait(client.submit("POST", "/small", {}, "ok"));
    BOOST_TEST(small.status == 200);
}

BOOST_AUTO_TEST_CASE(tls_alpn_picks_h2_and_falls_back_to_http1) {
    pki p;
    https::server_config cfg;
    cfg.bind_address = "127.0.0.1";
    cfg.tls_cert_path = p.server_cert;
    cfg.tls_key_path = p.server_key;
    cfg.http2 = true;
    streamer st;
    https::server server(cfg, echo);
    server.set_stream_handler(st);
    server.start();

    asio::io_context ioc;
    ssl::context ctx(ssl::context::tls_client);
    ctx.load_verify_file(p.ca_path);
    ctx.set_verify_mode(ssl::verify_peer);
    {
        ssl::stream<asio::ip::tcp::socket> s(ioc, ctx);
        static constexpr unsigned char protos[] = "\x02h2";
        SSL_set_alpn_protos(s.native_handle(), protos, sizeof protos - 1);
        s.next_layer().connect({asio::ip::make_address("127.0.0.1"), server.port()});
        kythira::testing::set_receive_timeout(s.next_layer().native_handle(), 10s);
        s.handshake(ssl::stream_base::client);
        const unsigned char* chosen = nullptr;
        unsigned int len = 0;
        SSL_get0_alpn_selected(s.native_handle(), &chosen, &len);
        BOOST_REQUIRE(std::string_view(reinterpret_cast<const char*>(chosen), len) == "h2");
        h2_client client(s);
        const auto r = client.wait(client.submit("GET", "/tls", {}));
        BOOST_TEST(r.body == "GET /tls  ");
        BOOST_TEST(r.headers.at("x-seen-tls") == "yes");
        const auto streamed = client.wait(client.submit("POST", "/stream", {}));
        BOOST_TEST(streamed.body == "piece0;piece1;piece2;");
    }
    {
        // No ALPN: HTTP/1.1 as before.
        beast::ssl_stream<beast::tcp_stream> s(ioc, ctx);
        beast::get_lowest_layer(s).connect({asio::ip::make_address("127.0.0.1"), server.port()});
        s.handshake(ssl::stream_base::client);
        http::request<http::string_body> req{http::verb::get, "/h1", 11};
        req.set(http::field::host, "localhost");
        http::write(s, req);
        beast::flat_buffer buf;
        http::response<http::string_body> res;
        http::read(s, buf, res);
        BOOST_TEST(res.body() == "GET /h1  ");
    }
}

BOOST_AUTO_TEST_CASE(stop_ends_an_open_stream) {
    streamer st;
    st.count = 100000;
    st.gap = 10ms;
    https::server server(plain_config(), echo);
    server.set_stream_handler(st);
    server.start();
    h2c_connection c(server.port());
    h2_client client(c.socket);
    const auto id = client.submit("POST", "/stream", {});
    (void)client.wait(id, [](std::int32_t, std::string_view) { return false; });
    const auto started = std::chrono::steady_clock::now();
    server.stop();
    BOOST_TEST((std::chrono::steady_clock::now() - started < 5s));
    BOOST_TEST(st.cancelled->load() == 1);
}

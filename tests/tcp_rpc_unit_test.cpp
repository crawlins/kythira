#define BOOST_TEST_MODULE tcp_rpc_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/tcp_rpc.hpp>
#include <raft/types.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <thread>

// ── helpers ───────────────────────────────────────────────────────────────────

static std::uint16_t find_free_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    BOOST_REQUIRE_GE(fd, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = 0;
    ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    socklen_t len = sizeof(a);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    std::uint16_t port = ntohs(a.sin_port);
    ::close(fd);
    return port;
}

// Connected UNIX socket pair — used for framing tests without binding a TCP port.
struct SockPair {
    int r, w;
    SockPair() {
        int fds[2];
        BOOST_REQUIRE_EQUAL(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
        r = fds[0];
        w = fds[1];
    }
    ~SockPair() {
        ::close(r);
        ::close(w);
    }
};

// ── Concept assertions ────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(test_concepts_satisfied) {
    static_assert(kythira::network_client<kythira::tcp_rpc_client>,
                  "tcp_rpc_client must satisfy network_client");
    static_assert(kythira::network_server<kythira::tcp_rpc_server>,
                  "tcp_rpc_server must satisfy network_server");
}

// ── Framing ───────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(test_frame_send_recv_round_trip, *boost::unit_test::timeout(10)) {
    SockPair sp;
    std::string msg = R"({"type":"request_vote_request","term":5})";
    BOOST_REQUIRE(kythira::tcp_detail::frame_send(sp.w, msg));
    ::shutdown(sp.w, SHUT_WR);
    auto received = kythira::tcp_detail::frame_recv(sp.r);
    BOOST_REQUIRE(received.has_value());
    BOOST_TEST(*received == msg);
}

BOOST_AUTO_TEST_CASE(test_frame_zero_length_rejected, *boost::unit_test::timeout(10)) {
    SockPair sp;
    // frame_recv rejects len=0 (protocol invariant: all real frames are non-empty)
    std::uint32_t zero = 0;
    ::write(sp.w, &zero, 4);
    ::shutdown(sp.w, SHUT_WR);
    auto received = kythira::tcp_detail::frame_recv(sp.r);
    BOOST_TEST(!received.has_value());
}

BOOST_AUTO_TEST_CASE(test_frame_large_payload, *boost::unit_test::timeout(10)) {
    SockPair sp;
    std::string big(64 * 1024, 'X');
    BOOST_REQUIRE(kythira::tcp_detail::frame_send(sp.w, big));
    ::shutdown(sp.w, SHUT_WR);
    auto received = kythira::tcp_detail::frame_recv(sp.r);
    BOOST_REQUIRE(received.has_value());
    BOOST_TEST(*received == big);
}

BOOST_AUTO_TEST_CASE(test_frame_recv_eof_returns_nullopt, *boost::unit_test::timeout(10)) {
    SockPair sp;
    ::shutdown(sp.w, SHUT_WR);
    auto received = kythira::tcp_detail::frame_recv(sp.r);
    BOOST_TEST(!received.has_value());
}

// ── Byte conversion ───────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(test_str_bytes_round_trip, *boost::unit_test::timeout(5)) {
    std::string s = "hello\x00\xFF\x01world";
    s.resize(13);
    auto bytes = kythira::tcp_detail::str_to_bytes(s);
    BOOST_TEST(bytes.size() == s.size());
    auto back = kythira::tcp_detail::bytes_to_str(bytes);
    BOOST_TEST(back == s);
}

// ── extract_type_field ────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(test_extract_type_field, *boost::unit_test::timeout(5)) {
    BOOST_TEST(kythira::tcp_detail::extract_type_field(
                   R"({"type":"request_vote_request","term":1})") == "request_vote_request");
    BOOST_TEST(kythira::tcp_detail::extract_type_field(
                   R"({"term":1,"type":"append_entries_request"})") == "append_entries_request");
    BOOST_TEST(kythira::tcp_detail::extract_type_field("{}").empty());
    BOOST_TEST(kythira::tcp_detail::extract_type_field("not json").empty());
}

// ── peer_registry ─────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(test_peer_registry, *boost::unit_test::timeout(5)) {
    kythira::tcp_detail::peer_registry<std::uint64_t> reg;

    BOOST_TEST(!reg.lookup(1).has_value());

    reg.add_peer(1, "127.0.0.1", 7001);
    reg.add_peer(2, "127.0.0.2", 7002);

    auto p1 = reg.lookup(1);
    BOOST_REQUIRE(p1.has_value());
    BOOST_TEST(p1->first == "127.0.0.1");
    BOOST_TEST(p1->second == 7001u);

    auto p2 = reg.lookup(2);
    BOOST_REQUIRE(p2.has_value());
    BOOST_TEST(p2->second == 7002u);

    BOOST_TEST(!reg.lookup(99).has_value());
}

BOOST_AUTO_TEST_CASE(test_peer_registry_overwrite, *boost::unit_test::timeout(5)) {
    kythira::tcp_detail::peer_registry<std::uint64_t> reg;
    reg.add_peer(1, "127.0.0.1", 7001);
    reg.add_peer(1, "10.0.0.1", 8001);
    auto p = reg.lookup(1);
    BOOST_REQUIRE(p.has_value());
    BOOST_TEST(p->first == "10.0.0.1");
    BOOST_TEST(p->second == 8001u);
}

// ── Server lifecycle ──────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(test_server_start_stop, *boost::unit_test::timeout(10)) {
    std::uint16_t port = find_free_port();
    kythira::tcp_rpc_server server(port);
    BOOST_TEST(!server.is_running());
    server.start();
    BOOST_TEST(server.is_running());
    server.stop();
    BOOST_TEST(!server.is_running());
}

BOOST_AUTO_TEST_CASE(test_server_double_stop_safe, *boost::unit_test::timeout(10)) {
    std::uint16_t port = find_free_port();
    kythira::tcp_rpc_server server(port);
    server.start();
    server.stop();
    server.stop();
    BOOST_TEST(!server.is_running());
}

BOOST_AUTO_TEST_CASE(test_server_move_before_start, *boost::unit_test::timeout(10)) {
    std::uint16_t port = find_free_port();
    kythira::tcp_rpc_server s1(port);
    kythira::tcp_rpc_server s2(std::move(s1));
    BOOST_TEST(!s2.is_running());
    s2.start();
    BOOST_TEST(s2.is_running());
    s2.stop();
}

// ── Client unknown peer ───────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(test_client_unknown_peer_returns_exceptional_future,
                     *boost::unit_test::timeout(10)) {
    kythira::tcp_rpc_client client;
    kythira::request_vote_request<> req{};
    req._term = 1;
    req._candidate_id = 1;
    auto fut = client.send_request_vote(99, req, std::chrono::milliseconds{100});
    BOOST_CHECK_THROW(std::move(fut).get(), std::exception);
}

// ── Client-server RPC round-trip ──────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(test_request_vote_round_trip, *boost::unit_test::timeout(15)) {
    std::uint16_t port = find_free_port();
    kythira::tcp_rpc_server server(port);

    kythira::request_vote_response<> canned_resp{};
    canned_resp._term = 2;
    canned_resp._vote_granted = true;

    server.register_request_vote_handler(
        [canned_resp](const kythira::request_vote_request<>&) { return canned_resp; });
    server.start();

    kythira::tcp_rpc_client client;
    client.add_peer(1, "127.0.0.1", port);

    kythira::request_vote_request<> req{};
    req._term = 2;
    req._candidate_id = 1;

    auto fut = client.send_request_vote(1, req, std::chrono::milliseconds{5000});
    auto resp = std::move(fut).get();

    BOOST_TEST(resp._term == 2u);
    BOOST_TEST(resp._vote_granted == true);

    server.stop();
}

BOOST_AUTO_TEST_CASE(test_append_entries_round_trip, *boost::unit_test::timeout(15)) {
    std::uint16_t port = find_free_port();
    kythira::tcp_rpc_server server(port);

    kythira::append_entries_response<> canned{};
    canned._term = 3;
    canned._success = false;

    server.register_append_entries_handler(
        [canned](const kythira::append_entries_request<>&) { return canned; });
    server.start();

    kythira::tcp_rpc_client client;
    client.add_peer(1, "127.0.0.1", port);

    kythira::append_entries_request<> req{};
    req._term = 3;
    req._leader_id = 1;

    auto fut = client.send_append_entries(1, req, std::chrono::milliseconds{5000});
    auto resp = std::move(fut).get();

    BOOST_TEST(resp._term == 3u);
    BOOST_TEST(resp._success == false);

    server.stop();
}

BOOST_AUTO_TEST_CASE(test_install_snapshot_round_trip, *boost::unit_test::timeout(15)) {
    std::uint16_t port = find_free_port();
    kythira::tcp_rpc_server server(port);

    kythira::install_snapshot_response<> canned{};
    canned._term = 5;

    server.register_install_snapshot_handler(
        [canned](const kythira::install_snapshot_request<>&) { return canned; });
    server.start();

    kythira::tcp_rpc_client client;
    client.add_peer(1, "127.0.0.1", port);

    kythira::install_snapshot_request<> req{};
    req._term = 5;
    req._leader_id = 1;
    req._last_included_index = 42;
    req._last_included_term = 4;
    req._offset = 0;
    req._data = {};
    req._done = true;

    auto fut = client.send_install_snapshot(1, req, std::chrono::milliseconds{5000});
    auto resp = std::move(fut).get();

    BOOST_TEST(resp._term == 5u);

    server.stop();
}

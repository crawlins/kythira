// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE tcp_rpc_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/tcp_rpc.hpp>
#include <raft/types.hpp>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

// Whether a TCP connect to address:port succeeds (IPv4 or IPv6 literal).
static bool can_connect(const std::string& address, std::uint16_t port) {
    sockaddr_storage ss{};
    socklen_t len = 0;
    auto& v4 = reinterpret_cast<sockaddr_in&>(ss);
    auto& v6 = reinterpret_cast<sockaddr_in6&>(ss);
    if (::inet_pton(AF_INET, address.c_str(), &v4.sin_addr) == 1) {
        v4.sin_family = AF_INET;
        v4.sin_port = htons(port);
        len = sizeof(sockaddr_in);
    } else {
        BOOST_REQUIRE_EQUAL(::inet_pton(AF_INET6, address.c_str(), &v6.sin6_addr), 1);
        v6.sin6_family = AF_INET6;
        v6.sin6_port = htons(port);
        len = sizeof(sockaddr_in6);
    }
    int fd = ::socket(ss.ss_family, SOCK_STREAM, 0);
    if (fd < 0) return false;
    bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&ss), len) == 0;
    ::close(fd);
    return ok;
}

// Whether this host can open an IPv6 socket on ::1 at all; CI containers and
// sandboxes often run with IPv6 disabled.
static bool ipv6_loopback_available() {
    int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in6 a{};
    a.sin6_family = AF_INET6;
    a.sin6_addr = in6addr_loopback;
    bool ok = ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
    ::close(fd);
    return ok;
}

// The first IPv4 address of an up, non-loopback interface, if the host has one.
static std::optional<std::string> non_loopback_ipv4() {
    ifaddrs* ifs = nullptr;
    if (::getifaddrs(&ifs) != 0) return std::nullopt;
    std::optional<std::string> found;
    for (ifaddrs* i = ifs; i != nullptr && !found; i = i->ifa_next) {
        if (i->ifa_addr == nullptr || i->ifa_addr->sa_family != AF_INET) continue;
        auto* in = reinterpret_cast<sockaddr_in*>(i->ifa_addr);
        if ((ntohl(in->sin_addr.s_addr) >> 24) == 127) continue;
        char buf[INET_ADDRSTRLEN];
        if (::inet_ntop(AF_INET, &in->sin_addr, buf, sizeof(buf)) != nullptr) found = buf;
    }
    ::freeifaddrs(ifs);
    return found;
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

BOOST_AUTO_TEST_CASE(test_server_rejects_unusable_bind_address, *boost::unit_test::timeout(30)) {
    std::uint16_t port = find_free_port();
    // RFC 6761 reserves .invalid: it never resolves.
    BOOST_CHECK_THROW(kythira::tcp_rpc_server(port, "kythira-test.invalid"), std::invalid_argument);
    BOOST_CHECK_THROW(kythira::tcp_rpc_server(port, ""), std::invalid_argument);
    BOOST_CHECK_THROW(kythira::tcp_rpc_server(port, "127.0.0.1:7000"), std::invalid_argument);
}

static kythira::tcp_detail::bind_endpoint v4_endpoint(const char* a) {
    kythira::tcp_detail::bind_endpoint ep;
    auto& sin = reinterpret_cast<sockaddr_in&>(ep.addr);
    sin.sin_family = AF_INET;
    BOOST_REQUIRE_EQUAL(::inet_pton(AF_INET, a, &sin.sin_addr), 1);
    ep.len = sizeof(sockaddr_in);
    return ep;
}

static kythira::tcp_detail::bind_endpoint v6_endpoint(const char* a, std::uint32_t scope) {
    kythira::tcp_detail::bind_endpoint ep;
    auto& sin6 = reinterpret_cast<sockaddr_in6&>(ep.addr);
    sin6.sin6_family = AF_INET6;
    sin6.sin6_scope_id = scope;
    BOOST_REQUIRE_EQUAL(::inet_pton(AF_INET6, a, &sin6.sin6_addr), 1);
    ep.len = sizeof(sockaddr_in6);
    return ep;
}

// A host name may only resolve to this host's own addresses (or loopback).
BOOST_AUTO_TEST_CASE(test_resolved_names_must_be_local, *boost::unit_test::timeout(5)) {
    using kythira::tcp_detail::require_local_endpoints;
    std::vector locals{v4_endpoint("192.0.2.10"), v6_endpoint("fe80::1", 3)};

    std::vector ok{v4_endpoint("192.0.2.10"), v4_endpoint("127.0.0.1"), v6_endpoint("::1", 0)};
    BOOST_CHECK_NO_THROW(require_local_endpoints(ok, locals, "node-a", "test"));

    std::vector mixed{v4_endpoint("192.0.2.10"), v4_endpoint("198.51.100.7")};
    BOOST_CHECK_THROW(require_local_endpoints(mixed, locals, "node-a", "test"),
                      std::invalid_argument);

    // getaddrinfo() leaves a link-local address's scope at zero; the match
    // supplies the interface's, which bind() needs.
    std::vector link_local{v6_endpoint("fe80::1", 0)};
    require_local_endpoints(link_local, locals, "node-a", "test");
    BOOST_TEST(reinterpret_cast<const sockaddr_in6&>(link_local[0].addr).sin6_scope_id == 3u);
}

// This host's own name must be accepted whenever /etc/hosts maps it to this
// host.
BOOST_AUTO_TEST_CASE(test_server_binds_own_hostname, *boost::unit_test::timeout(30)) {
    char name[256] = {};
    BOOST_REQUIRE_EQUAL(::gethostname(name, sizeof(name) - 1), 0);
    std::vector<kythira::tcp_detail::bind_endpoint> eps;
    try {
        eps = kythira::tcp_detail::resolve_bind_addresses(name, "test");
    } catch (const std::invalid_argument& e) {
        BOOST_TEST_MESSAGE("own host name not usable here (" << e.what() << "); skipped");
        return;
    }
    std::uint16_t port = find_free_port();
    kythira::tcp_rpc_server server(port, name);
    server.start();
    BOOST_TEST(server.is_running());
    server.stop();
}

// A scratch hosts file, removed when the test ends.
struct temp_hosts_file {
    std::string path;
    explicit temp_hosts_file(const std::string& contents) {
        char tmpl[] = "/tmp/kythira_hosts_XXXXXX";
        int fd = ::mkstemp(tmpl);
        BOOST_REQUIRE(fd >= 0);
        ::close(fd);
        path = tmpl;
        std::ofstream(path) << contents;
    }
    ~temp_hosts_file() { ::unlink(path.c_str()); }
};

static std::string endpoint_text(const kythira::tcp_detail::bind_endpoint& ep) {
    char buf[INET6_ADDRSTRLEN] = {};
    const void* raw =
        ep.addr.ss_family == AF_INET
            ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in&>(ep.addr).sin_addr)
            : static_cast<const void*>(&reinterpret_cast<const sockaddr_in6&>(ep.addr).sin6_addr);
    ::inet_ntop(ep.addr.ss_family, raw, buf, sizeof(buf));
    return buf;
}

// Names come from the hosts file only: comments, aliases and case are
// handled, a name the file doesn't list is refused without asking DNS, and
// a listed address that isn't this host's is refused.
BOOST_AUTO_TEST_CASE(test_bind_names_come_from_hosts_file_only, *boost::unit_test::timeout(5)) {
    using kythira::tcp_detail::resolve_bind_addresses;
    temp_hosts_file hosts(
        "# scratch hosts file\n"
        "127.0.0.1   localhost\n"
        "127.0.1.1   rpc-node rpc-alias   # this host\n"
        "127.0.1.1   rpc-node\n"
        "192.0.2.10  elsewhere\n"
        "not-an-ip   broken\n");

    auto eps = resolve_bind_addresses("RPC-Alias.", "test", hosts.path);
    BOOST_REQUIRE_EQUAL(eps.size(), 1U);
    BOOST_TEST(endpoint_text(eps[0]) == "127.0.1.1");

    BOOST_CHECK_THROW(resolve_bind_addresses("elsewhere", "test", hosts.path),
                      std::invalid_argument);
    BOOST_CHECK_THROW(resolve_bind_addresses("broken", "test", hosts.path), std::invalid_argument);
    BOOST_CHECK_THROW(resolve_bind_addresses("this", "test", hosts.path), std::invalid_argument);
    // A name DNS would resolve but the hosts file doesn't list.
    BOOST_CHECK_THROW(resolve_bind_addresses("example.com", "test", hosts.path),
                      std::invalid_argument);
}

// A non-loopback address of this host works when the hosts file lists it.
BOOST_AUTO_TEST_CASE(test_bind_name_on_local_interface, *boost::unit_test::timeout(5)) {
    auto ip = non_loopback_ipv4();
    if (!ip) {
        BOOST_TEST_MESSAGE("no non-loopback IPv4 address; skipped");
        return;
    }
    temp_hosts_file hosts(*ip + " rpc-node\n");
    auto eps = kythira::tcp_detail::resolve_bind_addresses("rpc-node", "test", hosts.path);
    BOOST_REQUIRE_EQUAL(eps.size(), 1U);
    BOOST_TEST(endpoint_text(eps[0]) == *ip);
}

// "localhost" still works when the hosts file doesn't list it (RFC 6761).
BOOST_AUTO_TEST_CASE(test_localhost_without_hosts_entry, *boost::unit_test::timeout(5)) {
    temp_hosts_file hosts("# empty\n");
    auto eps = kythira::tcp_detail::resolve_bind_addresses("localhost", "test", hosts.path);
    BOOST_REQUIRE_EQUAL(eps.size(), 2U);
    BOOST_TEST(endpoint_text(eps[0]) == "127.0.0.1");
    BOOST_TEST(endpoint_text(eps[1]) == "::1");
}

// An IPv6 hosts entry may carry a zone; the scope id is taken from it.
BOOST_AUTO_TEST_CASE(test_hosts_file_ipv6_zone, *boost::unit_test::timeout(5)) {
    auto ep = kythira::tcp_detail::parse_hosts_address("fe80::1%7");
    BOOST_REQUIRE(ep.has_value());
    BOOST_TEST(reinterpret_cast<const sockaddr_in6&>(ep->addr).sin6_scope_id == 7U);
    BOOST_TEST(!kythira::tcp_detail::parse_hosts_address("fe80::1%no-such-if").has_value());
}

// "*" is both wildcards, two sockets, so dual-stack never depends on the
// net.ipv6.bindv6only sysctl.
BOOST_AUTO_TEST_CASE(test_star_binds_both_wildcards, *boost::unit_test::timeout(5)) {
    auto eps = kythira::tcp_detail::resolve_bind_addresses("*", "test");
    BOOST_REQUIRE_EQUAL(eps.size(), 2U);
    BOOST_TEST(endpoint_text(eps[0]) == "0.0.0.0");
    BOOST_TEST(endpoint_text(eps[1]) == "::");
}

// With port 0, every listener shares the one ephemeral port the kernel
// picked for the first.
BOOST_AUTO_TEST_CASE(test_ephemeral_port_shared_across_listeners, *boost::unit_test::timeout(5)) {
    std::vector<kythira::tcp_detail::bind_endpoint> eps{v4_endpoint("127.0.0.1"),
                                                        v4_endpoint("127.0.0.2")};
    auto fds = kythira::tcp_detail::open_listeners(eps, 0, "test");
    BOOST_REQUIRE_EQUAL(fds.size(), 2U);
    std::uint16_t ports[2] = {};
    for (int i = 0; i < 2; ++i) {
        sockaddr_in a{};
        socklen_t len = sizeof(a);
        BOOST_REQUIRE_EQUAL(::getsockname(fds[i], reinterpret_cast<sockaddr*>(&a), &len), 0);
        ports[i] = ntohs(a.sin_port);
        ::close(fds[i]);
    }
    BOOST_TEST(ports[0] != 0);
    BOOST_TEST(ports[0] == ports[1]);
}

BOOST_AUTO_TEST_CASE(test_loopback_bind_address_classification, *boost::unit_test::timeout(5)) {
    using kythira::tcp_detail::is_loopback_bind_address;
    BOOST_TEST(is_loopback_bind_address("127.0.0.1"));
    BOOST_TEST(is_loopback_bind_address("127.5.6.7"));
    BOOST_TEST(is_loopback_bind_address("::1"));
    BOOST_TEST(is_loopback_bind_address("localhost"));
    BOOST_TEST(!is_loopback_bind_address("0.0.0.0"));
    BOOST_TEST(!is_loopback_bind_address("::"));
    BOOST_TEST(!is_loopback_bind_address("*"));
    BOOST_TEST(!is_loopback_bind_address("10.1.2.3"));
    BOOST_TEST(!is_loopback_bind_address("::ffff:127.0.0.1"));
    BOOST_TEST(!is_loopback_bind_address("kythira-test.invalid"));
}

BOOST_AUTO_TEST_CASE(test_localhost_resolves_to_loopback_only, *boost::unit_test::timeout(5)) {
    auto eps = kythira::tcp_detail::resolve_bind_addresses("localhost", "test");
    BOOST_REQUIRE(!eps.empty());
    for (const auto& ep : eps) {
        BOOST_TEST(kythira::tcp_detail::endpoint_is_loopback(ep));
    }
}

// A loopback bind must not be reachable through the host's other addresses;
// ca_cluster_node relies on this to allow plaintext RPC on loopback only.
BOOST_AUTO_TEST_CASE(test_server_loopback_bind_not_reachable_off_loopback,
                     *boost::unit_test::timeout(10)) {
    std::uint16_t port = find_free_port();
    kythira::tcp_rpc_server s1(port, "127.0.0.1");
    // Moving before start() must keep the bind address.
    kythira::tcp_rpc_server server(std::move(s1));
    server.start();
    BOOST_TEST(can_connect("127.0.0.1", port));
    if (auto other = non_loopback_ipv4()) {
        BOOST_TEST(!can_connect(*other, port), "loopback-bound server reachable via " << *other);
    } else {
        BOOST_TEST_MESSAGE("no non-loopback IPv4 interface; off-loopback check skipped");
    }
    server.stop();
}

BOOST_AUTO_TEST_CASE(test_server_ipv6_loopback_bind, *boost::unit_test::timeout(10)) {
    if (!ipv6_loopback_available()) {
        BOOST_TEST_MESSAGE("no IPv6 loopback on this host; skipped");
        return;
    }
    std::uint16_t port = find_free_port();
    kythira::tcp_rpc_server server(port, "::1");
    server.start();
    BOOST_TEST(can_connect("::1", port));
    BOOST_TEST(!can_connect("127.0.0.1", port), "::1-bound server reachable over IPv4");
    server.stop();
}

// "localhost" binds every loopback address it resolves to and nothing else.
BOOST_AUTO_TEST_CASE(test_server_localhost_bind, *boost::unit_test::timeout(10)) {
    std::uint16_t port = find_free_port();
    kythira::tcp_rpc_server server(port, "localhost");
    server.start();
    for (const auto& ep : kythira::tcp_detail::resolve_bind_addresses("localhost", "test")) {
        bool v6 = ep.addr.ss_family == AF_INET6;
        if (v6 && !ipv6_loopback_available()) continue;
        BOOST_TEST(can_connect(v6 ? "::1" : "127.0.0.1", port),
                   "localhost bind missed " << (v6 ? "::1" : "127.0.0.1"));
    }
    if (auto other = non_loopback_ipv4()) {
        BOOST_TEST(!can_connect(*other, port), "localhost-bound server reachable via " << *other);
    }
    server.stop();
    BOOST_TEST(!server.is_running());
}

// The client tries every address a peer name resolves to. "localhost" often
// resolves to ::1 first; a server listening on 127.0.0.1 alone must still be
// reachable through it.
BOOST_AUTO_TEST_CASE(test_connect_to_falls_back_across_resolved_addresses,
                     *boost::unit_test::timeout(10)) {
    std::uint16_t port = find_free_port();
    kythira::tcp_rpc_server server(port, "127.0.0.1");
    server.start();
    int fd = kythira::tcp_detail::connect_to("localhost", port, std::chrono::milliseconds(2000));
    BOOST_TEST(fd >= 0);
    if (fd >= 0) ::close(fd);
    server.stop();
}

BOOST_AUTO_TEST_CASE(test_server_default_bind_is_all_interfaces, *boost::unit_test::timeout(10)) {
    auto other = non_loopback_ipv4();
    if (!other) {
        BOOST_TEST_MESSAGE("no non-loopback IPv4 interface; skipped");
        return;
    }
    std::uint16_t port = find_free_port();
    kythira::tcp_rpc_server server(port);
    server.start();
    BOOST_TEST(can_connect("127.0.0.1", port));
    BOOST_TEST(can_connect(*other, port));
    server.stop();
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

// ── ClusterJoin / ClusterLeave and dynamic peer addresses (Req 19) ───────────

BOOST_AUTO_TEST_CASE(test_parse_host_port, *boost::unit_test::timeout(5)) {
    using kythira::tcp_detail::parse_host_port;
    auto hp = parse_host_port("node-4:7000");
    BOOST_REQUIRE(hp.has_value());
    BOOST_TEST(hp->first == "node-4");
    BOOST_TEST(hp->second == 7000);
    auto v6 = parse_host_port("[::1]:7001");
    BOOST_REQUIRE(v6.has_value());
    BOOST_TEST(v6->first == "::1");
    BOOST_TEST(v6->second == 7001);
    BOOST_TEST(!parse_host_port("node-4").has_value());
    BOOST_TEST(!parse_host_port("node-4:").has_value());
    BOOST_TEST(!parse_host_port(":7000").has_value());
    BOOST_TEST(!parse_host_port("node-4:70000").has_value());
    BOOST_TEST(!parse_host_port("node-4:7x").has_value());
}

BOOST_AUTO_TEST_CASE(test_cluster_join_round_trip_by_address, *boost::unit_test::timeout(15)) {
    std::uint16_t port = find_free_port();
    kythira::tcp_rpc_server server(port);
    std::optional<kythira::cluster_join_request<>> seen;
    server.register_cluster_join_handler([&seen](const kythira::cluster_join_request<>& req) {
        seen = req;
        return kythira::cluster_join_response<>{true, std::nullopt};
    });
    server.start();

    // No add_peer: a joining node knows its seed only by address.
    kythira::tcp_rpc_client client;
    kythira::cluster_join_request<> req{.node_id = 4, .contact_address = "node-4:7000"};
    auto resp = client
                    .send_cluster_join_request("127.0.0.1:" + std::to_string(port), req,
                                               std::chrono::milliseconds{5000})
                    .get();

    BOOST_TEST(resp.is_accepted());
    BOOST_REQUIRE(seen.has_value());
    BOOST_TEST(seen->node_id == 4u);
    BOOST_TEST(seen->contact_address == "node-4:7000");
    server.stop();
}

// node<Types> redirects a joiner to the leader as the leader's bare node ID,
// so a numeric address must resolve through the peer table.
BOOST_AUTO_TEST_CASE(test_cluster_join_to_node_id_address, *boost::unit_test::timeout(15)) {
    std::uint16_t port = find_free_port();
    kythira::tcp_rpc_server server(port);
    kythira::cluster_join_response<> redirect{
        false, kythira::peer_info<std::uint64_t, std::string>{1, "1"}};
    server.register_cluster_join_handler(
        [redirect](const kythira::cluster_join_request<>&) { return redirect; });
    server.start();

    kythira::tcp_rpc_client client;
    client.add_peer(2, "127.0.0.1", port);
    kythira::cluster_join_request<> req{.node_id = 4, .contact_address = "node-4:7000"};
    auto resp = client.send_cluster_join_request("2", req, std::chrono::milliseconds{5000}).get();

    BOOST_TEST(!resp.is_accepted());
    BOOST_REQUIRE(resp.redirect_peer().has_value());
    BOOST_TEST(resp.redirect_peer()->address == "1");
    server.stop();
}

BOOST_AUTO_TEST_CASE(test_cluster_join_rejects_malformed_address, *boost::unit_test::timeout(5)) {
    kythira::tcp_rpc_client client;
    kythira::cluster_join_request<> req{.node_id = 4, .contact_address = ""};
    BOOST_CHECK_THROW(
        client.send_cluster_join_request("no-port", req, std::chrono::milliseconds{100}).get(),
        kythira::network_exception);
}

BOOST_AUTO_TEST_CASE(test_cluster_leave_round_trip, *boost::unit_test::timeout(15)) {
    std::uint16_t port = find_free_port();
    kythira::tcp_rpc_server server(port);
    std::optional<std::uint64_t> leaving;
    server.register_cluster_leave_handler([&leaving](const kythira::cluster_leave_request<>& req) {
        leaving = req.node_id;
        return kythira::cluster_leave_response<>{true, std::nullopt};
    });
    server.start();

    kythira::tcp_rpc_client client;
    client.add_peer(1, "127.0.0.1", port);
    auto resp = client
                    .send_cluster_leave_request("1", kythira::cluster_leave_request<>{.node_id = 3},
                                                std::chrono::milliseconds{5000})
                    .get();
    BOOST_TEST(resp.is_accepted());
    BOOST_REQUIRE(leaving.has_value());
    BOOST_TEST(*leaving == 3u);
    server.stop();
}

// update_peer_address() is how a leader learns a joining node's address from
// its ClusterJoin; RPCs to that node then reach it.
BOOST_AUTO_TEST_CASE(test_update_peer_address_routes_rpcs, *boost::unit_test::timeout(15)) {
    std::uint16_t port = find_free_port();
    kythira::tcp_rpc_server server(port);
    kythira::request_vote_response<> canned{};
    canned._term = 9;
    server.register_request_vote_handler(
        [canned](const kythira::request_vote_request<>&) { return canned; });
    server.start();

    kythira::tcp_rpc_client client;
    client.update_peer_address(4, "not-an-address");  // ignored
    kythira::request_vote_request<> req{};
    BOOST_CHECK_THROW(client.send_request_vote(4, req, std::chrono::milliseconds{100}).get(),
                      kythira::network_exception);

    client.update_peer_address(4, "127.0.0.1:" + std::to_string(port));
    auto resp = client.send_request_vote(4, req, std::chrono::milliseconds{5000}).get();
    BOOST_TEST(resp._term == 9u);
    server.stop();
}

// A resolver names peers that joined after this node's peer table was built;
// a registered address still wins over it.
BOOST_AUTO_TEST_CASE(test_peer_resolver_fills_registry_misses, *boost::unit_test::timeout(15)) {
    std::uint16_t port = find_free_port();
    kythira::tcp_rpc_server server(port);
    kythira::request_vote_response<> canned{};
    canned._term = 11;
    server.register_request_vote_handler(
        [canned](const kythira::request_vote_request<>&) { return canned; });
    server.start();

    kythira::tcp_rpc_client client;
    std::vector<std::uint64_t> resolved;
    client.set_peer_resolver(
        [&resolved,
         port](const std::uint64_t& id) -> std::optional<std::pair<std::string, std::uint16_t>> {
            resolved.push_back(id);
            if (id == 5) {
                return std::pair{std::string{"127.0.0.1"}, port};
            }
            return std::nullopt;
        });

    kythira::request_vote_request<> req{};
    auto resp = client.send_request_vote(5, req, std::chrono::milliseconds{5000}).get();
    BOOST_TEST(resp._term == 11u);
    BOOST_CHECK_THROW(client.send_request_vote(6, req, std::chrono::milliseconds{100}).get(),
                      kythira::network_exception);

    client.add_peer(7, "127.0.0.1", port);
    client.send_request_vote(7, req, std::chrono::milliseconds{5000}).get();
    BOOST_TEST((resolved == std::vector<std::uint64_t>{5, 6}));
    server.stop();
}

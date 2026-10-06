// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE httplib_listeners_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/httplib_listeners.hpp>

#include <httplib.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <stdexcept>
#include <string>
#include <thread>

namespace {

using kythira::net_bind::httplib_listeners;

auto ipv6_loopback_available() -> bool {
    int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    sockaddr_in6 a{};
    a.sin6_family = AF_INET6;
    a.sin6_addr = in6addr_loopback;
    bool ok = ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
    ::close(fd);
    return ok;
}

auto add_ping(httplib::Server& s) -> void {
    s.Get("/ping", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("pong", "text/plain");
    });
}

auto ping(const std::string& host, std::uint16_t port) -> bool {
    httplib::Client client(host, port);
    client.set_connection_timeout(2);
    auto res = client.Get("/ping");
    return res && res->status == 200 && res->body == "pong";
}

}  // namespace

BOOST_AUTO_TEST_CASE(literal_bind_serves_one_address, *boost::unit_test::timeout(30)) {
    httplib_listeners<> listeners;
    auto port = listeners.bind("127.0.0.1", 0, add_ping, "test");
    BOOST_TEST(port != 0);
    BOOST_TEST(listeners.servers().size() == 1U);
    listeners.start();
    listeners.wait_until_ready();
    BOOST_TEST(ping("127.0.0.1", port));
    listeners.stop();
}

// "*" is 0.0.0.0 and :: on separate sockets sharing one port, so IPv4 and
// IPv6 clients both reach the same routes.
BOOST_AUTO_TEST_CASE(star_bind_serves_both_families, *boost::unit_test::timeout(30)) {
    httplib_listeners<> listeners;
    auto port = listeners.bind("*", 0, add_ping, "test");
    listeners.start();
    listeners.wait_until_ready();
    BOOST_TEST(ping("127.0.0.1", port));
    if (ipv6_loopback_available()) {
        BOOST_TEST(listeners.servers().size() == 2U);
        BOOST_TEST(ping("::1", port));
    } else {
        BOOST_TEST_MESSAGE("no IPv6 on this host; :: listener skipped");
        BOOST_TEST(listeners.servers().size() == 1U);
    }
    listeners.stop();
}

BOOST_AUTO_TEST_CASE(localhost_bind_serves_loopback, *boost::unit_test::timeout(30)) {
    httplib_listeners<> listeners;
    auto port = listeners.bind("localhost", 0, add_ping, "test");
    listeners.start();
    listeners.wait_until_ready();
    BOOST_TEST(ping("127.0.0.1", port));
    listeners.stop();
}

// wait() returns once request_stop() has stopped every accept loop, the
// shape the daemons use: one thread waits, a signal handler stops.
BOOST_AUTO_TEST_CASE(request_stop_releases_wait, *boost::unit_test::timeout(30)) {
    httplib_listeners<> listeners;
    auto port = listeners.bind("127.0.0.1", 0, add_ping, "test");
    listeners.start();
    listeners.wait_until_ready();
    std::thread waiter([&] { listeners.wait(); });
    listeners.request_stop();
    waiter.join();
    BOOST_TEST(!ping("127.0.0.1", port));
    listeners.stop();
}

BOOST_AUTO_TEST_CASE(unlisted_name_is_refused, *boost::unit_test::timeout(10)) {
    httplib_listeners<> listeners;
    BOOST_CHECK_THROW(listeners.bind("kythira-test.invalid", 0, add_ping, "test"),
                      std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(split_host_port_forms, *boost::unit_test::timeout(5)) {
    using kythira::net_bind::split_host_port;
    BOOST_TEST((split_host_port("0.0.0.0:80", 0) == std::pair<std::string, int>{"0.0.0.0", 80}));
    BOOST_TEST((split_host_port("[::1]:8080", 0) == std::pair<std::string, int>{"::1", 8080}));
    BOOST_TEST((split_host_port("*:80", 0) == std::pair<std::string, int>{"*", 80}));
    BOOST_TEST((split_host_port("localhost", 7) == std::pair<std::string, int>{"localhost", 7}));
    BOOST_CHECK_THROW(split_host_port("[::1:80", 0), std::invalid_argument);
}

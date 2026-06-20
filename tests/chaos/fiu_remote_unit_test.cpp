#include <fiu.h>
#include "fiu_remote.hpp"

#define BOOST_TEST_MODULE fiu_remote_unit_test
#include <boost/test/included/unit_test.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string>
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

// Send one newline-terminated command and read the integer reply.
static int send_cmd(std::uint16_t port, const std::string& cmd) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -999;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
        ::close(fd);
        return -999;
    }
    std::string line = cmd + "\n";
    ::send(fd, line.data(), line.size(), MSG_NOSIGNAL);
    ::shutdown(fd, SHUT_WR);
    char buf[32]{};
    int n = ::recv(fd, buf, sizeof(buf) - 1, 0);
    ::close(fd);
    if (n <= 0) return -999;
    return std::stoi(std::string(buf, static_cast<std::size_t>(n)));
}

// ── Fixture ───────────────────────────────────────────────────────────────────

struct FiuFixture {
    std::uint16_t port;
    chaos_node::fiu_tcp_rc server;

    FiuFixture() : port(find_free_port()), server(port) {
        fiu_init(0);
        server.start();
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
    }
    ~FiuFixture() { server.stop(); }
};

// ── Tests — libfiu 1.2 requires key=value format for fiu_rc_string ────────────

BOOST_FIXTURE_TEST_CASE(test_enable_and_disable, FiuFixture, *boost::unit_test::timeout(10)) {
    BOOST_TEST(fiu_fail("test/remote/enable") == 0);

    int rc = send_cmd(port, "enable name=test/remote/enable failnum=-1 failinfo=0");
    BOOST_TEST(rc == 0);
    BOOST_TEST(fiu_fail("test/remote/enable") != 0);

    rc = send_cmd(port, "disable name=test/remote/enable");
    BOOST_TEST(rc == 0);
    BOOST_TEST(fiu_fail("test/remote/enable") == 0);
}

BOOST_FIXTURE_TEST_CASE(test_enable_random, FiuFixture, *boost::unit_test::timeout(10)) {
    int rc = send_cmd(
        port, "enable_random name=test/remote/random failnum=-1 failinfo=0 probability=1.0");
    BOOST_TEST(rc == 0);
    BOOST_TEST(fiu_fail("test/remote/random") != 0);

    rc = send_cmd(port, "disable name=test/remote/random");
    BOOST_TEST(rc == 0);
    BOOST_TEST(fiu_fail("test/remote/random") == 0);
}

BOOST_FIXTURE_TEST_CASE(test_disable_all, FiuFixture, *boost::unit_test::timeout(10)) {
    send_cmd(port, "enable name=test/remote/da_a failnum=-1 failinfo=0");
    send_cmd(port, "enable name=test/remote/da_b failnum=-1 failinfo=0");
    BOOST_TEST(fiu_fail("test/remote/da_a") != 0);
    BOOST_TEST(fiu_fail("test/remote/da_b") != 0);

    int rc = send_cmd(port, "disable_all");
    BOOST_TEST(rc == 0);

    BOOST_TEST(fiu_fail("test/remote/da_a") == 0);
    BOOST_TEST(fiu_fail("test/remote/da_b") == 0);
}

BOOST_FIXTURE_TEST_CASE(test_enable_once, FiuFixture, *boost::unit_test::timeout(10)) {
    // "one" keyword → FIU_ONETIME: fire exactly once then auto-disable
    int rc = send_cmd(port, "enable name=test/remote/once failnum=1 one");
    BOOST_TEST(rc == 0);
    BOOST_TEST(fiu_fail("test/remote/once") != 0);
    BOOST_TEST(fiu_fail("test/remote/once") == 0);
}

BOOST_FIXTURE_TEST_CASE(test_invalid_command_returns_nonzero, FiuFixture,
                        *boost::unit_test::timeout(10)) {
    int rc = send_cmd(port, "bogus_command_xyz");
    BOOST_TEST(rc != 0);
}

BOOST_FIXTURE_TEST_CASE(test_server_handles_sequential_connections, FiuFixture,
                        *boost::unit_test::timeout(10)) {
    for (int i = 0; i < 5; ++i) {
        std::string name = "test/remote/seq" + std::to_string(i);
        int rc = send_cmd(port, "enable name=" + name + " failnum=-1 failinfo=0");
        BOOST_TEST(rc == 0);
    }
    int rc = send_cmd(port, "disable_all");
    BOOST_TEST(rc == 0);
}

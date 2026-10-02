// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Feature: grpc-plaintext-opt-in
//
// Table-driven checks of grpc_detail::target_is_local(), the rule that
// decides whether a plaintext gRPC client may dial a target without
// allow_plaintext (.kiro/specs/grpc-plaintext-opt-in/, Requirement 3.3, 3.4).
// No sockets are opened and nothing is resolved: names come from a scratch
// hosts file, and a name the file does not list must come out not local even
// when DNS would resolve it to loopback (Property 3).

#define BOOST_TEST_MODULE GrpcTargetClassificationUnitTest
#include <boost/test/unit_test.hpp>

#include <raft/grpc_target.hpp>

#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using kythira::grpc_detail::target_is_local;

// A scratch hosts file, removed when the test ends.
struct temp_hosts_file {
    std::string path;
    explicit temp_hosts_file(const std::string& contents) {
        char tmpl[] = "/tmp/kythira_grpc_hosts_XXXXXX";
        int fd = ::mkstemp(tmpl);
        BOOST_REQUIRE(fd >= 0);
        ::close(fd);
        path = tmpl;
        std::ofstream(path) << contents;
    }
    ~temp_hosts_file() { ::unlink(path.c_str()); }
};

auto check_table(const std::vector<std::pair<std::string, bool>>& table, const std::string& hosts)
    -> void {
    for (const auto& [target, expected] : table) {
        BOOST_TEST_INFO("target '" << target << "'");
        BOOST_TEST(target_is_local(target, hosts) == expected);
    }
}

}  // namespace

BOOST_AUTO_TEST_CASE(local_socket_schemes_are_local) {
    temp_hosts_file hosts("");
    check_table({{"unix:/run/kythira/raft.sock", true},
                 {"unix:///run/kythira/raft.sock", true},
                 {"unix-abstract:kythira-raft", true},
                 {"vsock:3:50051", true}},
                hosts.path);
}

BOOST_AUTO_TEST_CASE(ipv4_scheme_needs_every_address_on_loopback) {
    temp_hosts_file hosts("");
    check_table({{"ipv4:127.0.0.1:50051", true},
                 {"ipv4:127.255.255.254:50051", true},
                 {"ipv4:127.0.0.1", true},
                 {"ipv4:127.0.0.1:50051,127.0.0.2:50052", true},
                 {"ipv4:127.0.0.1:50051,10.0.0.1:50052", false},
                 {"ipv4:10.0.0.1:50051", false},
                 {"ipv4:0.0.0.0:50051", false},
                 {"ipv4:::1", false},  // an IPv6 address under ipv4:
                 {"ipv4:localhost:50051", false},
                 {"ipv4:", false},
                 {"ipv4:127.0.0.1:port", false}},
                hosts.path);
}

BOOST_AUTO_TEST_CASE(ipv6_scheme_needs_every_address_on_loopback) {
    temp_hosts_file hosts("");
    check_table({{"ipv6:[::1]:50051", true},
                 {"ipv6:::1", true},
                 {"ipv6:[::1]:50051,[::1]:50052", true},
                 {"ipv6:[::ffff:127.0.0.1]:50051", true},
                 {"ipv6:[::1]:50051,[2001:db8::1]:50052", false},
                 {"ipv6:[::]:50051", false},
                 {"ipv6:127.0.0.1:50051", false},  // an IPv4 address under ipv6:
                 {"ipv6:[::1:50051", false}},
                hosts.path);
}

BOOST_AUTO_TEST_CASE(dns_scheme_checks_the_host_part) {
    temp_hosts_file hosts(
        "127.0.0.1 raft-local\n"
        "10.1.2.3 raft-remote\n");
    check_table({{"dns:///localhost:50051", true},
                 {"dns:localhost:50051", true},
                 {"dns:///127.0.0.1:50051", true},
                 {"dns:///[::1]:50051", true},
                 {"dns:///raft-local:50051", true},
                 // The authority picks the resolver, not the host: still local.
                 {"dns://8.8.8.8/localhost:50051", true},
                 {"dns:///raft-remote:50051", false},
                 {"dns:///example.com:50051", false},
                 {"dns://8.8.8.8", false},
                 {"dns:///10.0.0.1:50051", false}},
                hosts.path);
}

BOOST_AUTO_TEST_CASE(bare_host_port_checks_the_host) {
    temp_hosts_file hosts(
        "127.0.0.1 raft-local\n"
        "::1 raft-local6\n"
        "10.1.2.3 raft-remote\n"
        "127.0.0.1 raft-mixed\n"
        "10.1.2.3 raft-mixed\n");
    check_table({{"127.0.0.1:50051", true},
                 {"127.255.255.254:50051", true},
                 {"[::1]:50051", true},
                 {"::1", true},
                 {"[::ffff:127.0.0.1]:50051", true},
                 {"::ffff:127.0.0.1", true},
                 {"localhost:50051", true},
                 {"localhost", true},
                 {"raft-local:50051", true},
                 {"RAFT-LOCAL.:50051", true},
                 {"raft-local6:50051", true},
                 {"raft-remote:50051", false},
                 // A name with any non-loopback entry is not loopback-only.
                 {"raft-mixed:50051", false},
                 {"10.0.0.1:5000", false},
                 {"0.0.0.0:50051", false},
                 {"[::]:50051", false},
                 {"[2001:db8::1]:50051", false},
                 {"::ffff:10.0.0.1", false},
                 {"localhost:port", false},
                 {"[::1]junk", false},
                 {"", false}},
                hosts.path);
}

// A name DNS would resolve but the hosts file does not list is not local:
// the decision never goes to a resolver.
BOOST_AUTO_TEST_CASE(unlisted_names_are_not_local) {
    temp_hosts_file hosts("127.0.0.1 raft-local\n");
    check_table({{"example.com:50051", false},
                 {"kythira-test.invalid:50051", false},
                 {"dns:///kythira-test.invalid:50051", false}},
                hosts.path);
}

// Schemes this code does not know lead somewhere it cannot check.
BOOST_AUTO_TEST_CASE(unknown_schemes_fail_closed) {
    temp_hosts_file hosts("127.0.0.1 xds\n");
    check_table({{"xds:///svc", false},
                 {"xds:///localhost:50051", false},
                 {"custom-resolver://localhost/svc", false},
                 {"https://localhost:50051", false}},
                hosts.path);
}

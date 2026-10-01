// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// redis_gateway_node's environment parsing, for the one rule that is a
// security boundary: Raft RPC has no TLS, so a non-loopback KYTHIRA_RAFT_BIND
// needs the explicit KYTHIRA_ALLOW_PLAINTEXT_RAFT opt-in.

#define BOOST_TEST_MODULE redis_gateway_node_config_unit_test
#include <boost/test/unit_test.hpp>

#include "config.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace {

using kythira::redis_node::from_env;
using kythira::redis_node::is_loopback_address;

// Sets the minimum environment from_env() requires and clears the variables
// under test, restoring a clean slate on destruction.
struct gateway_env {
    gateway_env() {
        ::setenv("KYTHIRA_NODE_ID", "1", 1);
        ::setenv("KYTHIRA_PEERS", "1=http://127.0.0.1:7000", 1);
        ::setenv("KYTHIRA_REDIS_ALLOW_ANONYMOUS", "true", 1);
        ::unsetenv("KYTHIRA_RAFT_BIND");
        ::unsetenv("KYTHIRA_ALLOW_PLAINTEXT_RAFT");
    }
    ~gateway_env() {
        for (const char* v : {"KYTHIRA_NODE_ID", "KYTHIRA_PEERS", "KYTHIRA_REDIS_ALLOW_ANONYMOUS",
                              "KYTHIRA_RAFT_BIND", "KYTHIRA_ALLOW_PLAINTEXT_RAFT"}) {
            ::unsetenv(v);
        }
    }
    gateway_env(const gateway_env&) = delete;
    auto operator=(const gateway_env&) -> gateway_env& = delete;
};

}  // namespace

BOOST_AUTO_TEST_CASE(loopback_addresses) {
    BOOST_TEST(is_loopback_address("127.0.0.1"));
    BOOST_TEST(is_loopback_address("127.1.2.3"));
    BOOST_TEST(is_loopback_address("::1"));
    BOOST_TEST(!is_loopback_address("0.0.0.0"));
    BOOST_TEST(!is_loopback_address("::"));
    BOOST_TEST(!is_loopback_address("10.0.0.1"));
    BOOST_TEST(!is_loopback_address("128.0.0.1"));
    BOOST_TEST(!is_loopback_address("localhost"));
    BOOST_TEST(!is_loopback_address(""));
}

// The default bind is 0.0.0.0, so with no opt-in the daemon must refuse.
BOOST_AUTO_TEST_CASE(default_bind_without_opt_in_is_refused) {
    gateway_env env;
    BOOST_CHECK_THROW((void)from_env(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(wildcard_bind_with_opt_in_is_allowed) {
    gateway_env env;
    ::setenv("KYTHIRA_ALLOW_PLAINTEXT_RAFT", "true", 1);
    auto opt = from_env();
    BOOST_TEST(opt._allow_plaintext_raft);
    BOOST_TEST(opt._bind_address == "0.0.0.0");
}

BOOST_AUTO_TEST_CASE(opt_in_false_is_refused) {
    gateway_env env;
    ::setenv("KYTHIRA_ALLOW_PLAINTEXT_RAFT", "false", 1);
    BOOST_CHECK_THROW((void)from_env(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(malformed_opt_in_is_refused) {
    gateway_env env;
    ::setenv("KYTHIRA_ALLOW_PLAINTEXT_RAFT", "maybe", 1);
    BOOST_CHECK_THROW((void)from_env(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(loopback_bind_needs_no_opt_in) {
    gateway_env env;
    ::setenv("KYTHIRA_RAFT_BIND", "127.0.0.1", 1);
    auto opt = from_env();
    BOOST_TEST(!opt._allow_plaintext_raft);
    BOOST_TEST(opt._bind_address == "127.0.0.1");
}

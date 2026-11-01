// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// redis_gateway_node's environment parsing, for the rules that are security
// boundaries: Raft RPC has no TLS, so a non-loopback KYTHIRA_RAFT_BIND needs
// the explicit KYTHIRA_ALLOW_PLAINTEXT_RAFT opt-in; and forwarding carries the
// internal secret, so it is TLS whenever a TLS listener is configured and
// plaintext only by KYTHIRA_REDIS_ALLOW_PLAINTEXT_FORWARDING. Also the
// disk-backed cache's KYTHIRA_CACHE_* variables
// (.kiro/specs/disk-backed-build-cache/ task 7).

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
        for (const char* v : k_cleared) {
            ::unsetenv(v);
        }
        // The tests that predate the disk-backed cache run the deployment
        // they were written for; the cache tests switch to file.
        ::setenv("KYTHIRA_CACHE_PERSISTENCE", "memory", 1);
    }
    ~gateway_env() {
        for (const char* v :
             {"KYTHIRA_NODE_ID", "KYTHIRA_PEERS", "KYTHIRA_REDIS_ALLOW_ANONYMOUS"}) {
            ::unsetenv(v);
        }
        for (const char* v : k_cleared) {
            ::unsetenv(v);
        }
    }
    static constexpr const char* k_cleared[] = {
        "KYTHIRA_RAFT_BIND",
        "KYTHIRA_ALLOW_PLAINTEXT_RAFT",
        "KYTHIRA_REDIS_TLS_LISTEN",
        "KYTHIRA_REDIS_TLS_CERT",
        "KYTHIRA_REDIS_TLS_KEY",
        "KYTHIRA_REDIS_TLS_CA",
        "KYTHIRA_REDIS_FORWARD_TLS",
        "KYTHIRA_REDIS_FORWARD_TLS_CA",
        "KYTHIRA_REDIS_FORWARD_TLS_CERT",
        "KYTHIRA_REDIS_FORWARD_TLS_KEY",
        "KYTHIRA_REDIS_ALLOW_PLAINTEXT_FORWARDING",
        "KYTHIRA_REDIS_PEER_GATEWAYS",
        "KYTHIRA_REDIS_MAX_VALUE_BYTES",
        "KYTHIRA_WIRE_SERIALIZER",
        "KYTHIRA_CACHE_PERSISTENCE",
        "KYTHIRA_CACHE_DATA_DIR",
        "KYTHIRA_CACHE_MAX_DISK_BYTES",
        "KYTHIRA_CACHE_CHUNK_BYTES",
        "KYTHIRA_CACHE_HTTP_LISTEN",
        "KYTHIRA_CACHE_HTTPS_LISTEN",
        "KYTHIRA_CACHE_HTTPS_CERT",
        "KYTHIRA_CACHE_HTTPS_KEY",
        "KYTHIRA_CACHE_ALLOW_PLAINTEXT_HTTP",
        "KYTHIRA_CACHE_PEER_HTTP",
        "KYTHIRA_CACHE_MIN_FREE_BYTES",
    };
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
    BOOST_TEST(is_loopback_address("localhost"));
    BOOST_TEST(!is_loopback_address("*"));
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

BOOST_AUTO_TEST_CASE(localhost_bind_needs_no_opt_in) {
    gateway_env env;
    ::setenv("KYTHIRA_RAFT_BIND", "localhost", 1);
    auto opt = from_env();
    BOOST_TEST(!opt._allow_plaintext_raft);
    BOOST_TEST(opt._bind_address == "localhost");
}

// A plaintext-only node neither forwards over TLS nor, without the opt-in,
// over plaintext: the gateway answers with the retry error instead.
BOOST_AUTO_TEST_CASE(plaintext_forwarding_is_off_by_default) {
    gateway_env env;
    ::setenv("KYTHIRA_RAFT_BIND", "127.0.0.1", 1);
    ::setenv("KYTHIRA_PEERS", "1=http://127.0.0.1:7000,2=http://10.0.0.2:7000", 1);
    auto opt = from_env();
    BOOST_TEST(!opt._gateway._forward_tls);
    BOOST_TEST(!opt._gateway._allow_plaintext_forwarding);
    BOOST_TEST(opt._peer_gateways.at(2) == "10.0.0.2:6379");
}

BOOST_AUTO_TEST_CASE(plaintext_forwarding_opt_in) {
    gateway_env env;
    ::setenv("KYTHIRA_RAFT_BIND", "127.0.0.1", 1);
    ::setenv("KYTHIRA_REDIS_ALLOW_PLAINTEXT_FORWARDING", "true", 1);
    auto opt = from_env();
    BOOST_TEST(opt._gateway._allow_plaintext_forwarding);
    BOOST_TEST(!opt._gateway._forward_tls);
}

// A TLS listener turns on TLS forwarding, and the derived peer endpoints
// point at the peers' TLS listeners rather than their plaintext ones.
BOOST_AUTO_TEST_CASE(tls_listener_forwards_over_tls_to_tls_ports) {
    gateway_env env;
    ::setenv("KYTHIRA_RAFT_BIND", "127.0.0.1", 1);
    ::setenv("KYTHIRA_PEERS", "1=http://127.0.0.1:7000,2=http://kv2:7000", 1);
    ::setenv("KYTHIRA_REDIS_TLS_LISTEN", "0.0.0.0:6380", 1);
    ::setenv("KYTHIRA_REDIS_TLS_CERT", "/etc/kythira/kv.crt", 1);
    ::setenv("KYTHIRA_REDIS_TLS_KEY", "/etc/kythira/kv.key", 1);
    ::setenv("KYTHIRA_REDIS_TLS_CA", "/etc/kythira/ca.crt", 1);
    auto opt = from_env();
    BOOST_TEST(opt._gateway._forward_tls);
    BOOST_TEST(opt._peer_gateways.at(2) == "kv2:6380");
}

BOOST_AUTO_TEST_CASE(tls_forwarding_can_be_turned_off) {
    gateway_env env;
    ::setenv("KYTHIRA_RAFT_BIND", "127.0.0.1", 1);
    ::setenv("KYTHIRA_PEERS", "1=http://127.0.0.1:7000,2=http://kv2:7000", 1);
    ::setenv("KYTHIRA_REDIS_TLS_LISTEN", "0.0.0.0:6380", 1);
    ::setenv("KYTHIRA_REDIS_TLS_CERT", "/etc/kythira/kv.crt", 1);
    ::setenv("KYTHIRA_REDIS_TLS_KEY", "/etc/kythira/kv.key", 1);
    ::setenv("KYTHIRA_REDIS_FORWARD_TLS", "false", 1);
    auto opt = from_env();
    BOOST_TEST(!opt._gateway._forward_tls);
    BOOST_TEST(opt._peer_gateways.at(2) == "kv2:6379");
}

// Without a CA the client could only trust every certificate, which hands
// the internal secret to whoever answers.
BOOST_AUTO_TEST_CASE(tls_forwarding_without_a_ca_is_refused) {
    gateway_env env;
    ::setenv("KYTHIRA_RAFT_BIND", "127.0.0.1", 1);
    ::setenv("KYTHIRA_REDIS_TLS_LISTEN", "0.0.0.0:6380", 1);
    ::setenv("KYTHIRA_REDIS_TLS_CERT", "/etc/kythira/kv.crt", 1);
    ::setenv("KYTHIRA_REDIS_TLS_KEY", "/etc/kythira/kv.key", 1);
    BOOST_CHECK_THROW((void)from_env(), std::invalid_argument);
    ::setenv("KYTHIRA_REDIS_FORWARD_TLS_CA", "/etc/kythira/peers.crt", 1);
    auto opt = from_env();
    BOOST_TEST(opt._gateway._forward_tls_ca_path == "/etc/kythira/peers.crt");
}

BOOST_AUTO_TEST_CASE(half_a_forwarding_client_certificate_is_refused) {
    gateway_env env;
    ::setenv("KYTHIRA_RAFT_BIND", "127.0.0.1", 1);
    ::setenv("KYTHIRA_REDIS_FORWARD_TLS_CERT", "/etc/kythira/client.crt", 1);
    BOOST_CHECK_THROW((void)from_env(), std::invalid_argument);
}

// ── the disk-backed cache ───────────────────────────────────────────────────

namespace {

/// The least a file-mode node needs on top of `gateway_env`.
void file_mode() {
    ::setenv("KYTHIRA_RAFT_BIND", "127.0.0.1", 1);
    ::setenv("KYTHIRA_CACHE_PERSISTENCE", "file", 1);
    ::setenv("KYTHIRA_CACHE_DATA_DIR", "/var/lib/kythira", 1);
    ::setenv("KYTHIRA_CACHE_MAX_DISK_BYTES", "107374182400", 1);
}

}  // namespace

// Requirement 5.4: file is the default, and it needs a data directory and a
// budget, so a deployment that sets neither fails at start-up rather than
// filling a disk.
BOOST_AUTO_TEST_CASE(file_persistence_is_the_default_and_needs_a_data_dir_and_budget) {
    gateway_env env;
    ::setenv("KYTHIRA_RAFT_BIND", "127.0.0.1", 1);
    ::unsetenv("KYTHIRA_CACHE_PERSISTENCE");
    BOOST_CHECK_THROW((void)from_env(), std::invalid_argument);
    ::setenv("KYTHIRA_CACHE_DATA_DIR", "/var/lib/kythira", 1);
    BOOST_CHECK_THROW((void)from_env(), std::invalid_argument);
    ::setenv("KYTHIRA_CACHE_MAX_DISK_BYTES", "0", 1);
    BOOST_CHECK_THROW((void)from_env(), std::invalid_argument);
    ::setenv("KYTHIRA_CACHE_MAX_DISK_BYTES", "107374182400", 1);
    auto opt = from_env();
    BOOST_TEST((opt._cache._persistence == kythira::redis_node::cache_persistence::file));
    BOOST_TEST(opt._cache._data_dir == "/var/lib/kythira");
    BOOST_TEST(opt._cache._max_disk_bytes == 107374182400ULL);
    BOOST_TEST(opt._cache._chunk_bytes == 4U * 1024U * 1024U);
    BOOST_TEST(opt._cache._min_free_inodes == 10000U);
}

BOOST_AUTO_TEST_CASE(memory_persistence_refuses_disk_variables) {
    gateway_env env;
    ::setenv("KYTHIRA_RAFT_BIND", "127.0.0.1", 1);
    auto opt = from_env();
    BOOST_TEST((opt._cache._persistence == kythira::redis_node::cache_persistence::memory));
    ::setenv("KYTHIRA_CACHE_DATA_DIR", "/var/lib/kythira", 1);
    BOOST_CHECK_THROW((void)from_env(), std::invalid_argument);
    ::unsetenv("KYTHIRA_CACHE_DATA_DIR");
    ::setenv("KYTHIRA_CACHE_HTTPS_LISTEN", "0.0.0.0:8443", 1);
    BOOST_CHECK_THROW((void)from_env(), std::invalid_argument);
    ::unsetenv("KYTHIRA_CACHE_HTTPS_LISTEN");
    ::setenv("KYTHIRA_CACHE_PERSISTENCE", "disk", 1);
    BOOST_CHECK_THROW((void)from_env(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(chunk_size_and_resp_values_are_bounded_by_one_chunk) {
    gateway_env env;
    file_mode();
    ::setenv("KYTHIRA_CACHE_CHUNK_BYTES", "16777217", 1);
    BOOST_CHECK_THROW((void)from_env(), std::invalid_argument);
    ::setenv("KYTHIRA_CACHE_CHUNK_BYTES", "0", 1);
    BOOST_CHECK_THROW((void)from_env(), std::invalid_argument);
    ::setenv("KYTHIRA_CACHE_CHUNK_BYTES", "16777216", 1);
    BOOST_TEST(from_env()._cache._chunk_bytes == 16777216U);
    // A SET is one log entry stored as one chunk.
    ::setenv("KYTHIRA_REDIS_MAX_VALUE_BYTES", "33554432", 1);
    BOOST_CHECK_THROW((void)from_env(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(file_persistence_needs_the_cbor_wire) {
    gateway_env env;
    file_mode();
    ::setenv("KYTHIRA_WIRE_SERIALIZER", "json", 1);
    BOOST_CHECK_THROW((void)from_env(), std::invalid_argument);
}

// Requirement 8.5: the plaintext front end carries credentials in the clear.
BOOST_AUTO_TEST_CASE(plaintext_http_needs_its_opt_in) {
    gateway_env env;
    file_mode();
    ::setenv("KYTHIRA_CACHE_HTTP_LISTEN", "127.0.0.1:8080", 1);
    BOOST_CHECK_THROW((void)from_env(), std::invalid_argument);
    ::setenv("KYTHIRA_CACHE_ALLOW_PLAINTEXT_HTTP", "true", 1);
    auto opt = from_env();
    BOOST_TEST(opt._cache._allow_plaintext_http);
    BOOST_TEST(opt._cache._peer_http.at(1) == "127.0.0.1:8080");
}

BOOST_AUTO_TEST_CASE(https_takes_the_resp_certificate_by_default) {
    gateway_env env;
    file_mode();
    ::setenv("KYTHIRA_PEERS", "1=http://127.0.0.1:7000,2=http://kv2:7000", 1);
    ::setenv("KYTHIRA_CACHE_HTTPS_LISTEN", "0.0.0.0:8443", 1);
    BOOST_CHECK_THROW((void)from_env(), std::invalid_argument);
    ::setenv("KYTHIRA_REDIS_TLS_LISTEN", "0.0.0.0:6380", 1);
    ::setenv("KYTHIRA_REDIS_TLS_CERT", "/etc/kythira/kv.crt", 1);
    ::setenv("KYTHIRA_REDIS_TLS_KEY", "/etc/kythira/kv.key", 1);
    ::setenv("KYTHIRA_REDIS_TLS_CA", "/etc/kythira/ca.crt", 1);
    auto opt = from_env();
    BOOST_TEST(opt._cache._tls_cert_path == "/etc/kythira/kv.crt");
    BOOST_TEST(opt._cache._tls_key_path == "/etc/kythira/kv.key");
    // Forwarding is over TLS, so peers' front ends are their HTTPS ones.
    BOOST_TEST(opt._cache._peer_http.at(2) == "kv2:8443");
}

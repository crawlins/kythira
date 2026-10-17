// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Cross-backend CoAP interop: a client on the includer's backend against a
// coap_interop_peer server on each backend that was built, libcoap, cantcoap
// and libnyoci alike (the client's own backend is the control). Two
// processes, because no two backends can share a translation unit; see
// coap_interop_peer.cpp. Plaintext only.
//
// The includer defines, in namespace coap_interop:
//
//   backend_client<Types>   the backend's client template
//   client_config()         a coap_client_config for that backend
//
// and CMake passes the peers it built as KYTHIRA_INTEROP_PEER_PATH_LIBCOAP,
// _CANTCOAP and _LIBNYOCI (absolute paths). A peer that was not built has no
// case, so the case list names exactly the pairs that ran.

#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/serializer_registry.hpp>
#include <raft/types.hpp>

#include "test_timeout_scale.hpp"

#include <boost/test/unit_test.hpp>

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace coap_interop {

using serializer = kythira::json_rpc_serializer<std::vector<std::byte>>;

struct interop_types {
    using serializer_type = serializer;
    using serializer_registry_type = kythira::single_serializer_registry<serializer>;
    using rpc_serializer_type = serializer;
    using metrics_type = kythira::noop_metrics;
    using logger_type = kythira::console_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;
    using executor_type = int;

    template<typename T> using future_template = kythira::future_default<T>;
    template<typename T> using promise_template = kythira::promise_default<T>;
};

constexpr std::uint64_t peer_id = 2;

/// A coap_interop_peer child process. Construction starts it and reads the
/// port it bound; destruction closes its stdin, which is its signal to stop,
/// and reaps it, killing it if it has not exited within a few seconds.
class peer_process {
public:
    explicit peer_process(const std::string& path) {
        int to_child[2];
        int from_child[2];
        if (::pipe(to_child) != 0 || ::pipe(from_child) != 0) {
            throw std::runtime_error("peer_process: pipe() failed");
        }
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, to_child[0], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&actions, from_child[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, to_child[1]);
        posix_spawn_file_actions_addclose(&actions, from_child[0]);
        std::vector<char*> argv{const_cast<char*>(path.c_str()), nullptr};
        const int rc = ::posix_spawn(&_pid, path.c_str(), &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        ::close(to_child[0]);
        ::close(from_child[1]);
        _stdin = to_child[1];
        if (rc != 0) {
            ::close(from_child[0]);
            ::close(_stdin);
            throw std::runtime_error("peer_process: cannot start " + path);
        }

        // One line, `PORT <n>`; anything else means the peer failed to start
        // (its reason is on stderr, which the test log inherits).
        FILE* out = ::fdopen(from_child[0], "r");
        unsigned port = 0;
        const int matched = out != nullptr ? std::fscanf(out, "PORT %u", &port) : 0;
        if (out != nullptr) {
            std::fclose(out);
        }
        if (matched != 1 || port == 0 || port > 65535) {
            stop();
            throw std::runtime_error("peer_process: " + path + " did not report a port");
        }
        _port = static_cast<std::uint16_t>(port);
    }

    ~peer_process() { stop(); }
    peer_process(const peer_process&) = delete;
    auto operator=(const peer_process&) -> peer_process& = delete;

    [[nodiscard]] auto endpoint() const -> std::string {
        return "coap://127.0.0.1:" + std::to_string(_port);
    }

    /// Stops the peer and returns its exit status (-1 if it had to be killed).
    auto stop() -> int {
        if (_stdin >= 0) {
            ::close(_stdin);
            _stdin = -1;
        }
        if (_pid <= 0) {
            return _status;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        int status = 0;
        while (::waitpid(_pid, &status, WNOHANG) == 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                ::kill(_pid, SIGKILL);
                ::waitpid(_pid, &status, 0);
                _pid = 0;
                return _status = -1;
            }
            ::usleep(10'000);
        }
        _pid = 0;
        return _status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

private:
    pid_t _pid{0};
    int _stdin{-1};
    std::uint16_t _port{0};
    int _status{-1};
};

/// Every RPC kind the peer serves, each checked against the value the peer
/// computes from the request (coap_interop_peer.cpp), so a match proves the
/// request body crossed backends intact and the response came back decoded.
inline auto run_rpc_kinds_against(const std::string& peer_path) -> void {
    peer_process peer{peer_path};
    backend_client<interop_types> client{
        {{peer_id, peer.endpoint()}}, client_config(), kythira::noop_metrics{}};
    const auto timeout = kythira::testing::scaled_deadline(10000);

    kythira::request_vote_request<> vote{};
    vote._term = 9;
    vote._candidate_id = 3;
    vote._last_log_index = 3;
    vote._last_log_term = 5;
    vote._group_id = 41;
    const auto voted = client.send_request_vote(peer_id, vote, timeout).get();
    BOOST_TEST(voted.term() == 14U);
    BOOST_TEST(voted.vote_granted());
    BOOST_TEST(voted.group_id() == 41U);

    kythira::append_entries_request<> append{};
    append._term = 4;
    append._leader_id = 1;
    append._prev_log_index = 10;
    append._prev_log_term = 1;
    append._leader_commit = 10;
    append._group_id = 42;
    append._entries.push_back(
        kythira::log_entry<>{4, 11, std::vector<std::byte>{std::byte{0x01}, std::byte{0x02}},
                             kythira::entry_type::normal});
    append._entries.push_back(kythira::log_entry<>{4, 12, std::vector<std::byte>{std::byte{0x03}},
                                                   kythira::entry_type::normal});
    const auto appended = client.send_append_entries(peer_id, append, timeout).get();
    BOOST_TEST(appended.term() == 4U);
    BOOST_TEST(appended.success());
    BOOST_REQUIRE(appended.conflict_index().has_value());
    BOOST_TEST(*appended.conflict_index() == 12U);
    BOOST_TEST(appended.group_id() == 42U);

    kythira::install_snapshot_request<> snapshot{};
    snapshot._term = 12;
    snapshot._leader_id = 1;
    snapshot._last_included_index = 100;
    snapshot._last_included_term = 11;
    snapshot._offset = 0;
    snapshot._data = std::vector<std::byte>(300, std::byte{0xAB});
    snapshot._done = true;
    snapshot._group_id = 43;
    const auto installed = client.send_install_snapshot(peer_id, snapshot, timeout).get();
    BOOST_TEST(installed.term() == 400U);
    BOOST_TEST(installed.group_id() == 43U);

    kythira::timeout_now_request<> transfer{};
    transfer._term = 7;
    transfer._leader_id = 1;
    transfer._last_log_index = 35;
    transfer._group_id = 44;
    const auto transferred = client.send_timeout_now(peer_id, transfer, timeout).get();
    BOOST_TEST(transferred.term() == 42U);
    BOOST_TEST(transferred.success());
    BOOST_TEST(transferred.group_id() == 44U);

    BOOST_TEST(peer.stop() == 0);
}

}  // namespace coap_interop

BOOST_AUTO_TEST_SUITE(coap_cross_backend_interop)

#ifdef KYTHIRA_INTEROP_PEER_PATH_LIBCOAP
BOOST_AUTO_TEST_CASE(every_rpc_kind_against_a_libcoap_server,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    coap_interop::run_rpc_kinds_against(KYTHIRA_INTEROP_PEER_PATH_LIBCOAP);
}
#endif

#ifdef KYTHIRA_INTEROP_PEER_PATH_CANTCOAP
BOOST_AUTO_TEST_CASE(every_rpc_kind_against_a_cantcoap_server,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    coap_interop::run_rpc_kinds_against(KYTHIRA_INTEROP_PEER_PATH_CANTCOAP);
}
#endif

#ifdef KYTHIRA_INTEROP_PEER_PATH_LIBNYOCI
BOOST_AUTO_TEST_CASE(every_rpc_kind_against_a_libnyoci_server,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    coap_interop::run_rpc_kinds_against(KYTHIRA_INTEROP_PEER_PATH_LIBNYOCI);
}
#endif

BOOST_AUTO_TEST_SUITE_END()

// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file coap_interop_peer.cpp
/// @brief A CoAP Raft server on one backend, for the cross-backend interop
///        suites (coap_interop_cases.hpp).
///
/// No two CoAP backends can share a translation unit -- libcoap spells the
/// option numbers as macros and cantcoap and libnyoci as enumerators, so
/// including libcoap first rewrites the others' enums -- so a client on one
/// backend can only talk to a server on another across a process boundary.
/// This is that other process. It is built once per backend, chosen by one of
/// KYTHIRA_INTEROP_PEER_{LIBCOAP,CANTCOAP,LIBNYOCI}, and:
///
///   1. binds a CoAP server to 127.0.0.1 on an ephemeral port, in the clear
///      or over DTLS as its command line says (coap_interop_security.hpp);
///   2. prints `PORT <n>` on stdout, and nothing else (the transports' own
///      logging goes to stdout too, so it is pointed at stderr first);
///   3. serves until its stdin reaches end of file, then stops and exits 0.
///
/// Every handler answers with a value computed from the request
/// (coap_interop_cases.hpp checks each one), so a response only matches if
/// the request body survived the trip and the server really decoded it.

#if defined(KYTHIRA_INTEROP_PEER_LIBCOAP)
#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#elif defined(KYTHIRA_INTEROP_PEER_CANTCOAP)
#include <raft/coap_transport_cantcoap_impl.hpp>
#elif defined(KYTHIRA_INTEROP_PEER_LIBNYOCI)
#include <raft/coap_transport_libnyoci_impl.hpp>
#else
#error "define one of KYTHIRA_INTEROP_PEER_LIBCOAP, _CANTCOAP or _LIBNYOCI"
#endif

#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/serializer_registry.hpp>
#include <raft/types.hpp>

#include "coap_interop_security.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

using serializer = kythira::json_rpc_serializer<std::vector<std::byte>>;

struct peer_types {
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

#if defined(KYTHIRA_INTEROP_PEER_LIBCOAP)
using server_type = kythira::coap_server<peer_types>;
#elif defined(KYTHIRA_INTEROP_PEER_CANTCOAP)
using server_type = kythira::coap_cantcoap_server<peer_types>;
#else
using server_type = kythira::coap_libnyoci_server<peer_types>;
#endif

}  // namespace

auto main(int argc, char** argv) -> int {
    // console_logger writes to stdout, which is the parent's pipe: keep that
    // pipe for the one PORT line and send everything else to stderr, which
    // the test log inherits.
    const int report_fd = ::dup(STDOUT_FILENO);
    if (report_fd < 0 || ::dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
        std::cerr << "coap_interop_peer: cannot redirect stdout\n";
        return 1;
    }
    try {
        kythira::coap_server_config config;
        config.security =
            coap_interop::peer_security(std::vector<std::string>(argv + 1, argv + argc));
        server_type server{"127.0.0.1", 0, std::move(config), kythira::noop_metrics{}};

        server.register_request_vote_handler([](const kythira::request_vote_request<>& request) {
            kythira::request_vote_response<> response{};
            response._term = request.term() + request.last_log_term();
            response._vote_granted = request.candidate_id() == request.last_log_index();
            response._group_id = request.group_id();
            return response;
        });
        server.register_append_entries_handler(
            [](const kythira::append_entries_request<>& request) {
                kythira::append_entries_response<> response{};
                response._term = request.term();
                response._success = request.leader_id() == request.prev_log_term();
                response._conflict_index = request.prev_log_index() + request.entries().size();
                response._group_id = request.group_id();
                return response;
            });
        server.register_install_snapshot_handler(
            [](const kythira::install_snapshot_request<>& request) {
                kythira::install_snapshot_response<> response{};
                response._term = request.last_included_index() + request.data().size();
                response._group_id = request.group_id();
                return response;
            });
        server.register_timeout_now_handler([](const kythira::timeout_now_request<>& request) {
            kythira::timeout_now_response<> response{};
            response._term = request.term() + request.last_log_index();
            response._success = request.leader_id() != 0;
            response._group_id = request.group_id();
            return response;
        });

        server.start();
        const auto line = "PORT " + std::to_string(server.bound_port()) + "\n";
        const bool reported =
            ::write(report_fd, line.data(), line.size()) == static_cast<ssize_t>(line.size());
        ::close(report_fd);
        if (!reported) {
            server.stop();
            std::cerr << "coap_interop_peer: cannot report the port\n";
            return 1;
        }

        // The parent closes our stdin to stop us; reading it to EOF is also
        // what makes a parent that dies mid-test take this process with it.
        for (std::string line; std::getline(std::cin, line);) {
        }
        server.stop();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "coap_interop_peer: " << e.what() << '\n';
        return 1;
    }
}

// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file coap_edhoc_bootstrap.hpp
/// @brief Carrying EDHOC (RFC 9528) over CoAP's `/.well-known/edhoc` resource,
///        so a pair of peers can derive an OSCORE Security Context instead of
///        being handed one.
///
/// WHY THIS IS SEPARATE FROM coap_edhoc.hpp
/// ----------------------------------------
/// `run_edhoc_handshake()` is already transport-agnostic: it drives an abstract
/// `edhoc_transport` with alternating send()/receive() calls and lakers does the
/// crypto. What was missing was anything that actually *carried* those messages
/// between two nodes. This file supplies that for CoAP, and it is deliberately
/// free of any CoAP library — it works in terms of a `post` callable and a
/// message queue, so either backend can drive it. (Today only the libnyoci one
/// does; the libcoap backend gets OSCORE from libcoap itself.)
///
/// THE SHAPE OF THE EXCHANGE
/// -------------------------
/// EDHOC over CoAP is three messages in two request/response pairs, POSTed to
/// `/.well-known/edhoc` (RFC 9528 Appendix A.2):
///
///     Initiator --- message_1 --->  Responder
///     Initiator <-- message_2 ----  Responder
///     Initiator --- message_3 --->  Responder
///     Initiator <-- (empty 2.04) -  Responder
///
/// The exchange runs *unprotected*, which is not a gap: EDHOC authenticates and
/// key-agrees on its own, and its whole purpose is to bootstrap the protection
/// that follows.
///
/// The two roles are asymmetric in an awkward way. The Initiator drives, so its
/// transport is trivial — send() posts and keeps the reply, receive() returns
/// it. The Responder is driven *by* inbound requests, but
/// `run_edhoc_handshake()` blocks in receive(); so the Responder runs on its own
/// thread and rendezvouses with the CoAP handler through this file's queues.

#include <raft/coap_edhoc.hpp>
#include <raft/coap_exceptions.hpp>
#include <raft/coap_security.hpp>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace kythira {

/// The CoAP resource EDHOC messages are POSTed to (RFC 9528 Appendix A.2).
inline constexpr const char* edhoc_well_known_path = "/.well-known/edhoc";

/// How long either side waits for its peer to take the next step before giving
/// up. Generous: the handshake involves elliptic-curve work on both ends and
/// possibly a cold start, but it must not be unbounded, or a peer that vanishes
/// mid-handshake parks a thread forever.
inline constexpr std::chrono::seconds edhoc_step_timeout{20};

/// Initiator side. `post` sends one EDHOC message to the peer's
/// `/.well-known/edhoc` and returns the response payload; everything else is
/// bookkeeping.
class edhoc_initiator_transport final : public edhoc_transport {
public:
    using post_function = std::function<std::vector<std::byte>(const std::vector<std::byte>&)>;

    explicit edhoc_initiator_transport(post_function post) : _post(std::move(post)) {}

    auto send(const std::vector<std::byte>& message) -> void override {
        // Each EDHOC message is a POST, and the peer's next message comes back
        // as its response -- so the reply is stashed for the receive() that
        // follows. message_3 has no reply, and the empty payload it does get is
        // simply never collected.
        _pending_reply = _post(message);
    }

    auto receive() -> std::vector<std::byte> override {
        if (!_pending_reply) {
            throw coap_credential_bootstrap_error(
                "edhoc",
                "initiator tried to receive before sending; the CoAP exchange is strictly "
                "send-then-receive");
        }
        auto reply = std::move(*_pending_reply);
        _pending_reply.reset();
        if (reply.empty()) {
            throw coap_credential_bootstrap_error(
                "edhoc", "the peer's /.well-known/edhoc returned an empty EDHOC message");
        }
        return reply;
    }

private:
    post_function _post;
    std::optional<std::vector<std::byte>> _pending_reply;
};

/// Responder side: the rendezvous between libnyoci's request-handling thread
/// and the thread running `run_edhoc_handshake()`.
///
/// The handler calls `exchange()` with the inbound EDHOC message and blocks
/// until the responder produces the reply. The responder's transport pops from
/// `_inbound` and pushes to `_outbound`. A completed handshake pushes one empty
/// reply, which is what releases the handler that delivered message_3.
class edhoc_responder_channel final : public edhoc_transport {
public:
    /// Called from the CoAP handler thread. Returns the responder's reply, or
    /// nullopt if the handshake failed or timed out.
    [[nodiscard]] auto exchange(std::vector<std::byte> inbound)
        -> std::optional<std::vector<std::byte>> {
        std::unique_lock lock(_mutex);
        if (_failed) {
            return std::nullopt;
        }
        _inbound.push_back(std::move(inbound));
        _inbound_ready.notify_one();
        if (!_outbound_ready.wait_for(lock, edhoc_step_timeout,
                                      [this] { return !_outbound.empty() || _failed; })) {
            return std::nullopt;
        }
        if (_failed) {
            return std::nullopt;
        }
        auto reply = std::move(_outbound.front());
        _outbound.pop_front();
        return reply;
    }

    // --- edhoc_transport, called from the responder thread ---

    auto send(const std::vector<std::byte>& message) -> void override {
        const std::lock_guard lock(_mutex);
        _outbound.push_back(message);
        _outbound_ready.notify_one();
    }

    auto receive() -> std::vector<std::byte> override {
        std::unique_lock lock(_mutex);
        if (!_inbound_ready.wait_for(lock, edhoc_step_timeout,
                                     [this] { return !_inbound.empty() || _abandoned; })) {
            throw coap_credential_bootstrap_error(
                "edhoc", "timed out waiting for the peer's next EDHOC message");
        }
        if (_abandoned) {
            throw coap_credential_bootstrap_error("edhoc", "responder shut down mid-handshake");
        }
        auto message = std::move(_inbound.front());
        _inbound.pop_front();
        return message;
    }

    /// Releases the handler still holding message_3, which has no EDHOC reply.
    auto finish() -> void {
        const std::lock_guard lock(_mutex);
        _outbound.emplace_back();
        _outbound_ready.notify_one();
    }

    /// Fails every waiter, so a handshake that threw does not leave the CoAP
    /// handler blocked until its timeout.
    auto fail() -> void {
        const std::lock_guard lock(_mutex);
        _failed = true;
        _outbound_ready.notify_all();
        _inbound_ready.notify_all();
    }

    /// Wakes the responder thread out of receive() at shutdown.
    auto abandon() -> void {
        const std::lock_guard lock(_mutex);
        _abandoned = true;
        _inbound_ready.notify_all();
        _outbound_ready.notify_all();
    }

private:
    std::mutex _mutex;
    std::condition_variable _inbound_ready;
    std::condition_variable _outbound_ready;
    std::deque<std::vector<std::byte>> _inbound;
    std::deque<std::vector<std::byte>> _outbound;
    bool _failed{false};
    bool _abandoned{false};
};

}  // namespace kythira

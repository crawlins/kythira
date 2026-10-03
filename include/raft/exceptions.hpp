// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace kythira {

// Base exception for all Raft-related errors
class raft_exception : public std::runtime_error {
public:
    explicit raft_exception(const std::string& message) : std::runtime_error(message) {}
};

// Exception for network-related errors
class network_exception : public raft_exception {
public:
    explicit network_exception(const std::string& message) : raft_exception(message) {}
};

// A peer answered that it cannot serve an optional extension RPC (PreVote,
// TimeoutNow): HTTP 404/501 or CoAP 4.04/5.01 on the extension's path. It
// means "this peer runs a build without the RPC", not "the call failed", and
// the core treats it differently from every other network error: a pre-vote
// round counts it as a grant and a leadership transfer reports it as
// unsupported (.kiro/specs/http-coap-pre-vote-timeout-now/ Requirement 4).
//
// Mandatory RPCs never produce it: a peer without RequestVote is
// misconfigured, not older, and keeps the transport's ordinary error.
class rpc_not_implemented_exception : public network_exception {
public:
    rpc_not_implemented_exception(std::string rpc, std::uint64_t target)
        : network_exception("peer " + std::to_string(target) + " does not implement RPC '" + rpc +
                            "' (not implemented)"),
          _rpc{std::move(rpc)},
          _target{target} {}

    [[nodiscard]] auto rpc() const -> const std::string& { return _rpc; }
    [[nodiscard]] auto target() const -> std::uint64_t { return _target; }

private:
    std::string _rpc;
    std::uint64_t _target;
};

// Exception for persistence-related errors
class persistence_exception : public raft_exception {
public:
    explicit persistence_exception(const std::string& message) : raft_exception(message) {}
};

// Exception for serialization-related errors
class serialization_exception : public raft_exception {
public:
    explicit serialization_exception(const std::string& message) : raft_exception(message) {}
};

// Exception for election-related errors
class election_exception : public raft_exception {
public:
    explicit election_exception(const std::string& message) : raft_exception(message) {}
};

// Exception for a leadership transfer that could not be carried out.
//
// Derived from `election_exception` because every way a transfer fails is a
// statement about who may become leader: the target is not a voter, it could
// not be caught up before the deadline, or this node stopped being the leader
// while trying. None of them leaves the cluster worse off than before the
// attempt — a failed transfer is a no-op, not a partial move — which is what
// makes the operation safe for a placement driver to retry.
class leader_transfer_exception : public election_exception {
public:
    explicit leader_transfer_exception(const std::string& message) : election_exception(message) {}
};

// The configured transport does not implement TimeoutNow.
//
// A distinct type rather than a flag, because the remedy is different in kind:
// every other transfer failure is transient and worth retrying, and this one
// will still be true in an hour. See `network_client_with_timeout_now`.
class leader_transfer_unsupported_exception : public leader_transfer_exception {
public:
    explicit leader_transfer_unsupported_exception(const std::string& message)
        : leader_transfer_exception(message) {}
};

}  // namespace kythira

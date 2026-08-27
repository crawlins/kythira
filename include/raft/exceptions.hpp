// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stdexcept>
#include <string>

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

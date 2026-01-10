#pragma once

#include <stdexcept>
#include <string>

namespace kythira {

// Base exception for all Raft-related errors
class raft_exception : public std::runtime_error {
public:
    explicit raft_exception(const std::string& message)
        : std::runtime_error(message) {}
};

// Exception for network-related errors
class network_exception : public raft_exception {
public:
    explicit network_exception(const std::string& message)
        : raft_exception(message) {}
};

// Exception for persistence-related errors
class persistence_exception : public raft_exception {
public:
    explicit persistence_exception(const std::string& message)
        : raft_exception(message) {}
};

// Exception for serialization-related errors
class serialization_exception : public raft_exception {
public:
    explicit serialization_exception(const std::string& message)
        : raft_exception(message) {}
};

// Exception for election-related errors
class election_exception : public raft_exception {
public:
    explicit election_exception(const std::string& message)
        : raft_exception(message) {}
};

} // namespace kythira

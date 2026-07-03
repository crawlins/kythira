#pragma once

#include "exceptions.hpp"
#include "types.hpp"
#include <chrono>
#include <string>
#include <cstddef>

namespace kythira {

// Base exception for completion-related errors
class raft_completion_exception : public raft_exception {
public:
    explicit raft_completion_exception(const std::string& message) : raft_exception(message) {}
};

// Commit waiting specific exceptions
template<typename LogIndex = std::uint64_t>
requires log_index<LogIndex>
class commit_timeout_exception : public raft_completion_exception {
public:
    commit_timeout_exception(LogIndex index, std::chrono::milliseconds timeout)
        : raft_completion_exception("Commit timeout for entry " + std::to_string(index) +
                                    " after " + std::to_string(timeout.count()) + "ms"),
          _entry_index(index),
          _timeout(timeout) {}

    [[nodiscard]] auto get_entry_index() const -> LogIndex { return _entry_index; }
    [[nodiscard]] auto get_timeout() const -> std::chrono::milliseconds { return _timeout; }

private:
    LogIndex _entry_index;
    std::chrono::milliseconds _timeout;
};

template<typename TermId = std::uint64_t>
requires term_id<TermId>
class leadership_lost_exception : public raft_completion_exception {
public:
    leadership_lost_exception(TermId old_term, TermId new_term)
        : raft_completion_exception("Leadership lost: term changed from " +
                                    std::to_string(old_term) + " to " + std::to_string(new_term)),
          _old_term(old_term),
          _new_term(new_term) {}

    [[nodiscard]] auto get_old_term() const -> TermId { return _old_term; }
    [[nodiscard]] auto get_new_term() const -> TermId { return _new_term; }

private:
    TermId _old_term;
    TermId _new_term;
};

// Future collection specific exceptions
class future_collection_exception : public raft_completion_exception {
public:
    future_collection_exception(const std::string& operation, std::size_t failed_count)
        : raft_completion_exception("Future collection failed for operation '" + operation +
                                    "': " + std::to_string(failed_count) + " futures failed"),
          _operation(operation),
          _failed_count(failed_count) {}

    [[nodiscard]] auto get_operation() const -> const std::string& { return _operation; }
    [[nodiscard]] auto get_failed_count() const -> std::size_t { return _failed_count; }

private:
    std::string _operation;
    std::size_t _failed_count;
};

// Configuration change specific exceptions
class configuration_change_exception : public raft_completion_exception {
public:
    configuration_change_exception(const std::string& phase, const std::string& reason)
        : raft_completion_exception("Configuration change failed in phase '" + phase +
                                    "': " + reason),
          _phase(phase),
          _reason(reason) {}

    [[nodiscard]] auto get_phase() const -> const std::string& { return _phase; }
    [[nodiscard]] auto get_reason() const -> const std::string& { return _reason; }

private:
    std::string _phase;
    std::string _reason;
};

// Learner placement-capacity criterion exceptions (see .kiro/specs/non-voting-nodes/).
// Distinguishable from each other and from ordinary precondition failures so callers
// can tell "no capacity in this placement group" apart from "not leader" / "not found".
class learner_capacity_exceeded_exception : public raft_completion_exception {
public:
    learner_capacity_exceeded_exception()
        : raft_completion_exception(
              "Learner admission rejected: placement group has no capacity remaining "
              "relative to its desired voting target") {}
};

class voting_capacity_exceeded_exception : public raft_completion_exception {
public:
    voting_capacity_exceeded_exception()
        : raft_completion_exception(
              "Promotion rejected: placement group has no voting capacity remaining "
              "relative to its desired voting target") {}
};

// Type aliases for common instantiations
using commit_timeout_exception_t = commit_timeout_exception<std::uint64_t>;
using leadership_lost_exception_t = leadership_lost_exception<std::uint64_t>;

}  // namespace kythira
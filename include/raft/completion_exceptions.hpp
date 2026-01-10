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
    explicit raft_completion_exception(const std::string& message)
        : raft_exception(message) {}
};

// Commit waiting specific exceptions
template<typename LogIndex = std::uint64_t>
requires log_index<LogIndex>
class commit_timeout_exception : public raft_completion_exception {
public:
    commit_timeout_exception(LogIndex index, std::chrono::milliseconds timeout)
        : raft_completion_exception(
            "Commit timeout for entry " + std::to_string(index) + 
            " after " + std::to_string(timeout.count()) + "ms"
        )
        , _entry_index(index)
        , _timeout(timeout) {}
    
    auto get_entry_index() const -> LogIndex { return _entry_index; }
    auto get_timeout() const -> std::chrono::milliseconds { return _timeout; }
    
private:
    LogIndex _entry_index;
    std::chrono::milliseconds _timeout;
};

template<typename TermId = std::uint64_t>
requires term_id<TermId>
class leadership_lost_exception : public raft_completion_exception {
public:
    leadership_lost_exception(TermId old_term, TermId new_term)
        : raft_completion_exception(
            "Leadership lost: term changed from " + std::to_string(old_term) + 
            " to " + std::to_string(new_term)
        )
        , _old_term(old_term)
        , _new_term(new_term) {}
    
    auto get_old_term() const -> TermId { return _old_term; }
    auto get_new_term() const -> TermId { return _new_term; }
    
private:
    TermId _old_term;
    TermId _new_term;
};

// Future collection specific exceptions
class future_collection_exception : public raft_completion_exception {
public:
    future_collection_exception(const std::string& operation, std::size_t failed_count)
        : raft_completion_exception(
            "Future collection failed for operation '" + operation + 
            "': " + std::to_string(failed_count) + " futures failed"
        )
        , _operation(operation)
        , _failed_count(failed_count) {}
    
    auto get_operation() const -> const std::string& { return _operation; }
    auto get_failed_count() const -> std::size_t { return _failed_count; }
    
private:
    std::string _operation;
    std::size_t _failed_count;
};

// Configuration change specific exceptions
class configuration_change_exception : public raft_completion_exception {
public:
    configuration_change_exception(const std::string& phase, const std::string& reason)
        : raft_completion_exception(
            "Configuration change failed in phase '" + phase + "': " + reason
        )
        , _phase(phase)
        , _reason(reason) {}
    
    auto get_phase() const -> const std::string& { return _phase; }
    auto get_reason() const -> const std::string& { return _reason; }
    
private:
    std::string _phase;
    std::string _reason;
};

// Type aliases for common instantiations
using commit_timeout_exception_t = commit_timeout_exception<std::uint64_t>;
using leadership_lost_exception_t = leadership_lost_exception<std::uint64_t>;

} // namespace kythira
#pragma once

#include <raft/types.hpp>
#include <functional>
#include <chrono>
#include <future>

#include <folly/futures/Future.h>

namespace raft {

// Network client concept - defines interface for sending RPC requests
// Uses folly::Future for async operations
template<typename C>
concept network_client = requires(
    C client,
    std::uint64_t target,
    const request_vote_request<>& rvr,
    const append_entries_request<>& aer,
    const install_snapshot_request<>& isr,
    std::chrono::milliseconds timeout
) {
    // Send RequestVote RPC - returns a folly future
    { client.send_request_vote(target, rvr, timeout) } 
        -> std::same_as<folly::Future<request_vote_response<>>>;
    
    // Send AppendEntries RPC - returns a folly future
    { client.send_append_entries(target, aer, timeout) }
        -> std::same_as<folly::Future<append_entries_response<>>>;
    
    // Send InstallSnapshot RPC - returns a folly future
    { client.send_install_snapshot(target, isr, timeout) }
        -> std::same_as<folly::Future<install_snapshot_response<>>>;
};

// Network server concept - defines interface for receiving RPC requests
template<typename S>
concept network_server = requires(
    S server,
    std::function<request_vote_response<>(const request_vote_request<>&)> rv_handler,
    std::function<append_entries_response<>(const append_entries_request<>&)> ae_handler,
    std::function<install_snapshot_response<>(const install_snapshot_request<>&)> is_handler
) {
    // Register RPC handlers
    { server.register_request_vote_handler(rv_handler) } -> std::same_as<void>;
    { server.register_append_entries_handler(ae_handler) } -> std::same_as<void>;
    { server.register_install_snapshot_handler(is_handler) } -> std::same_as<void>;
    
    // Server lifecycle
    { server.start() } -> std::same_as<void>;
    { server.stop() } -> std::same_as<void>;
    { server.is_running() } -> std::convertible_to<bool>;
};

} // namespace raft

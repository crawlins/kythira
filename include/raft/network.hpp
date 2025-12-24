#pragma once

#include <raft/types.hpp>
#include <concepts/future.hpp>
#include <functional>
#include <chrono>

namespace kythira {

// Network client concept - defines interface for sending RPC requests
// Uses generic future types for async operations
template<typename C, typename FutureType>
concept network_client = requires(
    C client,
    std::uint64_t target,
    const raft::request_vote_request<>& rvr,
    const raft::append_entries_request<>& aer,
    const raft::install_snapshot_request<>& isr,
    std::chrono::milliseconds timeout
) {
    // Send RequestVote RPC - returns a generic future
    { client.send_request_vote(target, rvr, timeout) } 
        -> std::same_as<FutureType>;
    
    // Send AppendEntries RPC - returns a generic future
    { client.send_append_entries(target, aer, timeout) }
        -> std::same_as<FutureType>;
    
    // Send InstallSnapshot RPC - returns a generic future
    { client.send_install_snapshot(target, isr, timeout) }
        -> std::same_as<FutureType>;
};

// Network server concept - defines interface for receiving RPC requests
// Uses generic future types for consistency with client concept
template<typename S, typename FutureType>
concept network_server = requires(
    S server,
    std::function<raft::request_vote_response<>(const raft::request_vote_request<>&)> rv_handler,
    std::function<raft::append_entries_response<>(const raft::append_entries_request<>&)> ae_handler,
    std::function<raft::install_snapshot_response<>(const raft::install_snapshot_request<>&)> is_handler
) {
    // Register RPC handlers
    { server.register_request_vote_handler(rv_handler) } -> std::same_as<void>;
    { server.register_append_entries_handler(ae_handler) } -> std::same_as<void>;
    { server.register_install_snapshot_handler(is_handler) } -> std::same_as<void>;
    
    // Server lifecycle - can be synchronous (void) or asynchronous (FutureType)
    { server.start() } -> std::convertible_to<void>;
    { server.stop() } -> std::convertible_to<void>;
    { server.is_running() } -> std::convertible_to<bool>;
};

} // namespace kythira

#pragma once

#include <raft/network.hpp>
#include <raft/types.hpp>
#include <raft/exceptions.hpp>
#include <raft/json_serializer.hpp>

#include <network_simulator/network_simulator.hpp>

#include <memory>
#include <shared_mutex>
#include <chrono>

namespace kythira {

// Simulator network client implementation
template<typename FutureType, typename Serializer, typename Data>
requires kythira::rpc_serializer<Serializer, Data>
class simulator_network_client {
public:
    // Define a custom types structure for the network simulator
    struct raft_network_types {
        using address_type = std::string;
        using port_type = unsigned short;
        using message_type = network_simulator::Message<raft_network_types>;
        using connection_type = network_simulator::Connection<raft_network_types>;
        using listener_type = network_simulator::Listener<raft_network_types>;
        using node_type = network_simulator::NetworkNode<raft_network_types>;
        
        // Future types using the generic FutureType template parameter
        using future_bool_type = FutureType;
        using future_bytes_type = FutureType;
        using future_connection_type = FutureType;
        using future_message_type = FutureType;
    };
    
    using node_type = std::shared_ptr<network_simulator::NetworkNode<raft_network_types>>;
    using simulator_type = network_simulator::NetworkSimulator<raft_network_types>;
    
    simulator_network_client(node_type node, Serializer serializer = Serializer{})
        : _node(std::move(node))
        , _serializer(std::move(serializer))
        , _rpc_port(5000)  // Default Raft RPC port
    {}
    
    // Send RequestVote RPC
    auto send_request_vote(
        std::uint64_t target,
        const kythira::request_vote_request<>& req,
        std::chrono::milliseconds timeout
    ) -> FutureType {
        // Serialize the request
        auto data = _serializer.serialize(req);
        
        // Convert to byte vector for network transmission
        std::vector<std::byte> payload(data.begin(), data.end());
        
        // Create message
        typename raft_network_types::message_type msg(
            _node->address(),
            0,  // Source port (connectionless)
            target,
            _rpc_port,
            std::move(payload)
        );
        
        // Send message and wait for response
        return _node->send(std::move(msg), timeout)
            .then([this, timeout](bool success) -> FutureType {
                if (!success) {
                    return FutureType(std::make_exception_ptr(
                        kythira::network_exception("Failed to send RequestVote RPC")));
                }
                
                // Wait for response
                return _node->receive(timeout)
                    .then([this](typename raft_network_types::message_type response_msg) -> kythira::request_vote_response<> {
                        // Deserialize response
                        auto payload = response_msg.payload();
                        Data response_data;
                        if constexpr (requires { response_data.resize(0); }) {
                            response_data.resize(payload.size());
                            std::copy(payload.begin(), payload.end(), response_data.begin());
                        }
                        
                        return _serializer.template deserialize_request_vote_response<>(response_data);
                    });
            });
    }
    
    // Send AppendEntries RPC
    auto send_append_entries(
        std::uint64_t target,
        const kythira::append_entries_request<>& req,
        std::chrono::milliseconds timeout
    ) -> FutureType {
        // Serialize the request
        auto data = _serializer.serialize(req);
        
        // Convert to byte vector for network transmission
        std::vector<std::byte> payload(data.begin(), data.end());
        
        // Create message
        typename raft_network_types::message_type msg(
            _node->address(),
            0,  // Source port (connectionless)
            target,
            _rpc_port,
            std::move(payload)
        );
        
        // Send message and wait for response
        return _node->send(std::move(msg), timeout)
            .then([this, timeout](bool success) -> FutureType {
                if (!success) {
                    return FutureType(std::make_exception_ptr(
                        kythira::network_exception("Failed to send AppendEntries RPC")));
                }
                
                // Wait for response
                return _node->receive(timeout)
                    .then([this](typename raft_network_types::message_type response_msg) -> kythira::append_entries_response<> {
                        // Deserialize response
                        auto payload = response_msg.payload();
                        Data response_data;
                        if constexpr (requires { response_data.resize(0); }) {
                            response_data.resize(payload.size());
                            std::copy(payload.begin(), payload.end(), response_data.begin());
                        }
                        
                        return _serializer.template deserialize_append_entries_response<>(response_data);
                    });
            });
    }
    
    // Send InstallSnapshot RPC
    auto send_install_snapshot(
        std::uint64_t target,
        const kythira::install_snapshot_request<>& req,
        std::chrono::milliseconds timeout
    ) -> FutureType {
        // Serialize the request
        auto data = _serializer.serialize(req);
        
        // Convert to byte vector for network transmission
        std::vector<std::byte> payload(data.begin(), data.end());
        
        // Create message
        typename raft_network_types::message_type msg(
            _node->address(),
            0,  // Source port (connectionless)
            target,
            _rpc_port,
            std::move(payload)
        );
        
        // Send message and wait for response
        return _node->send(std::move(msg), timeout)
            .then([this, timeout](bool success) -> FutureType {
                if (!success) {
                    return FutureType(std::make_exception_ptr(
                        kythira::network_exception("Failed to send InstallSnapshot RPC")));
                }
                
                // Wait for response
                return _node->receive(timeout)
                    .then([this](typename raft_network_types::message_type response_msg) -> kythira::install_snapshot_response<> {
                        // Deserialize response
                        auto payload = response_msg.payload();
                        Data response_data;
                        if constexpr (requires { response_data.resize(0); }) {
                            response_data.resize(payload.size());
                            std::copy(payload.begin(), payload.end(), response_data.begin());
                        }
                        
                        return _serializer.template deserialize_install_snapshot_response<>(response_data);
                    });
            });
    }
    
private:
    node_type _node;
    Serializer _serializer;
    unsigned short _rpc_port;
};

// Simulator network server implementation
template<typename FutureType, typename Serializer, typename Data>
requires kythira::rpc_serializer<Serializer, Data>
class simulator_network_server {
public:
    // Define a custom types structure for the network simulator
    struct raft_network_types {
        using address_type = std::string;
        using port_type = unsigned short;
        using message_type = network_simulator::Message<raft_network_types>;
        using connection_type = network_simulator::Connection<raft_network_types>;
        using listener_type = network_simulator::Listener<raft_network_types>;
        using node_type = network_simulator::NetworkNode<raft_network_types>;
        
        // Future types using the generic FutureType template parameter
        using future_bool_type = FutureType;
        using future_bytes_type = FutureType;
        using future_connection_type = FutureType;
        using future_message_type = FutureType;
    };
    
    using node_type = std::shared_ptr<network_simulator::NetworkNode<raft_network_types>>;
    using simulator_type = network_simulator::NetworkSimulator<raft_network_types>;
    
    simulator_network_server(node_type node, Serializer serializer = Serializer{})
        : _node(std::move(node))
        , _serializer(std::move(serializer))
        , _rpc_port(5000)  // Default Raft RPC port
        , _running(false)
    {}
    
    // Move constructor
    simulator_network_server(simulator_network_server&& other) noexcept
        : _node(std::move(other._node))
        , _serializer(std::move(other._serializer))
        , _rpc_port(other._rpc_port)
        , _running(other._running.load())
        , _server_thread(std::move(other._server_thread))
        , _request_vote_handler(std::move(other._request_vote_handler))
        , _append_entries_handler(std::move(other._append_entries_handler))
        , _install_snapshot_handler(std::move(other._install_snapshot_handler))
    {}
    
    // Move assignment
    simulator_network_server& operator=(simulator_network_server&& other) noexcept {
        if (this != &other) {
            // Stop current server if running
            stop();
            
            _node = std::move(other._node);
            _serializer = std::move(other._serializer);
            _rpc_port = other._rpc_port;
            _running = other._running.load();
            _server_thread = std::move(other._server_thread);
            _request_vote_handler = std::move(other._request_vote_handler);
            _append_entries_handler = std::move(other._append_entries_handler);
            _install_snapshot_handler = std::move(other._install_snapshot_handler);
        }
        return *this;
    }
    
    // Destructor
    ~simulator_network_server() {
        stop();
    }
    
    // Delete copy constructor and assignment
    simulator_network_server(const simulator_network_server&) = delete;
    simulator_network_server& operator=(const simulator_network_server&) = delete;
    
    // Register RequestVote handler
    auto register_request_vote_handler(
        std::function<kythira::request_vote_response<>(const kythira::request_vote_request<>&)> handler
    ) -> void {
        std::unique_lock lock(_mutex);
        _request_vote_handler = std::move(handler);
    }
    
    // Register AppendEntries handler
    auto register_append_entries_handler(
        std::function<kythira::append_entries_response<>(const kythira::append_entries_request<>&)> handler
    ) -> void {
        std::unique_lock lock(_mutex);
        _append_entries_handler = std::move(handler);
    }
    
    // Register InstallSnapshot handler
    auto register_install_snapshot_handler(
        std::function<kythira::install_snapshot_response<>(const kythira::install_snapshot_request<>&)> handler
    ) -> void {
        std::unique_lock lock(_mutex);
        _install_snapshot_handler = std::move(handler);
    }
    
    // Start the server
    auto start() -> void {
        std::unique_lock lock(_mutex);
        if (_running) {
            return;
        }
        
        _running = true;
        
        // Start message processing loop in background thread
        _server_thread = std::thread([this]() {
            process_messages();
        });
    }
    
    // Stop the server
    auto stop() -> void {
        {
            std::unique_lock lock(_mutex);
            if (!_running) {
                return;
            }
            _running = false;
        }
        
        // Wait for server thread to finish
        if (_server_thread.joinable()) {
            _server_thread.join();
        }
    }
    
    // Check if server is running
    auto is_running() const -> bool {
        std::shared_lock lock(_mutex);
        return _running;
    }
    
private:
    node_type _node;
    Serializer _serializer;
    unsigned short _rpc_port;
    std::atomic<bool> _running;
    std::thread _server_thread;
    
    // RPC handlers
    std::function<kythira::request_vote_response<>(const kythira::request_vote_request<>&)> _request_vote_handler;
    std::function<kythira::append_entries_response<>(const kythira::append_entries_request<>&)> _append_entries_handler;
    std::function<kythira::install_snapshot_response<>(const kythira::install_snapshot_request<>&)> _install_snapshot_handler;
    
    mutable std::shared_mutex _mutex;
    
    // Message processing loop
    auto process_messages() -> void {
        while (_running) {
            try {
                // Receive message with timeout
                auto msg_future = _node->receive(std::chrono::milliseconds{100});
                
                // Wait for message
                auto msg = std::move(msg_future).get();
                handle_message(std::move(msg));
            } catch (const std::exception& ex) {
                // Timeout or error - continue loop
                // This is expected when no messages are available
            } catch (...) {
                // Unknown error - continue loop
            }
        }
    }
    
    // Handle incoming message
    auto handle_message(typename raft_network_types::message_type msg) -> void {
        try {
            // Extract payload
            auto payload = msg.payload();
            Data request_data;
            if constexpr (requires { request_data.resize(0); }) {
                request_data.resize(payload.size());
                std::copy(payload.begin(), payload.end(), request_data.begin());
            }
            
            // Try to determine message type by attempting deserialization
            // This is a simple approach - in production, you'd want a type field
            
            // Try RequestVote
            try {
                auto request = _serializer.template deserialize_request_vote_request<>(request_data);
                
                std::shared_lock lock(_mutex);
                if (_request_vote_handler) {
                    auto response = _request_vote_handler(request);
                    send_response(msg.source_address(), response);
                }
                return;
            } catch (...) {
                // Not a RequestVote request
            }
            
            // Try AppendEntries
            try {
                auto request = _serializer.template deserialize_append_entries_request<>(request_data);
                
                std::shared_lock lock(_mutex);
                if (_append_entries_handler) {
                    auto response = _append_entries_handler(request);
                    send_response(msg.source_address(), response);
                }
                return;
            } catch (...) {
                // Not an AppendEntries request
            }
            
            // Try InstallSnapshot
            try {
                auto request = _serializer.template deserialize_install_snapshot_request<>(request_data);
                
                std::shared_lock lock(_mutex);
                if (_install_snapshot_handler) {
                    auto response = _install_snapshot_handler(request);
                    send_response(msg.source_address(), response);
                }
                return;
            } catch (...) {
                // Not an InstallSnapshot request
            }
            
            // Unknown message type - ignore
        } catch (...) {
            // Error handling message - ignore
        }
    }
    
    // Send response back to client
    template<typename Response>
    auto send_response(std::uint64_t target, const Response& response) -> void {
        try {
            // Serialize response
            auto data = _serializer.serialize(response);
            
            // Convert to byte vector
            std::vector<std::byte> payload(data.begin(), data.end());
            
            // Create response message
            typename raft_network_types::message_type msg(
                _node->address(),
                _rpc_port,
                target,
                0,  // Destination port (connectionless)
                std::move(payload)
            );
            
            // Send response (fire and forget)
            _node->send(std::move(msg));
        } catch (...) {
            // Error sending response - ignore
        }
    }
};

// Verify that simulator_network_client satisfies the network_client concept
using SimulatorFutureType = kythira::Future<kythira::request_vote_response<>>;
static_assert(kythira::network_client<simulator_network_client<SimulatorFutureType, kythira::json_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>, SimulatorFutureType>,
              "simulator_network_client must satisfy the network_client concept");

// Verify that simulator_network_server satisfies the network_server concept
static_assert(kythira::network_server<simulator_network_server<SimulatorFutureType, kythira::json_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>, SimulatorFutureType>,
              "simulator_network_server must satisfy the network_server concept");

} // namespace kythira

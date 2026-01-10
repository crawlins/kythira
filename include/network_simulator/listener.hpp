#pragma once

#include "concepts.hpp"
#include "types.hpp"
#include "exceptions.hpp"

#include <atomic>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <queue>

namespace network_simulator {

// Forward declarations
template<typename Types>
class NetworkSimulator;

template<typename Types>
class Connection;

// Listener class template
template<typename Types>
class Listener {
public:
    // Type aliases from Types template argument
    using address_type = typename Types::address_type;
    using port_type = typename Types::port_type;
    using endpoint_type = Endpoint<Types>;
    using connection_type = typename Types::connection_type;
    using simulator_type = NetworkSimulator<Types>;
    using future_connection_type = typename Types::future_connection_type;
    
    Listener(endpoint_type local_endpoint,
             simulator_type* simulator)
        : _local(std::move(local_endpoint))
        , _simulator(simulator)
        , _listening(true)
    {}
    
    auto accept() -> future_connection_type;
    auto accept(std::chrono::milliseconds timeout) -> future_connection_type;
    
    auto close() -> void;
    auto is_listening() const -> bool;
    
    auto local_endpoint() const -> endpoint_type { return _local; }
    
    // Internal method for simulator to queue pending connections
    auto queue_pending_connection(std::shared_ptr<connection_type> connection) -> void;
    
private:
    endpoint_type _local;
    simulator_type* _simulator;
    
    std::atomic<bool> _listening;
    
    // Queue of pending connections
    std::queue<std::shared_ptr<connection_type>> _pending_connections;
    
    mutable std::mutex _queue_mutex;
    std::condition_variable _connection_available;
};

} // namespace network_simulator

#include "listener_impl.hpp"
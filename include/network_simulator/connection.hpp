#pragma once

#include "concepts.hpp"
#include "types.hpp"
#include "exceptions.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <vector>
#include <memory>

namespace network_simulator {

// Forward declaration
template<typename Types>
class NetworkSimulator;

// Connection class template
template<typename Types>
class Connection {
public:
    // Type aliases from Types template argument
    using address_type = typename Types::address_type;
    using port_type = typename Types::port_type;
    using endpoint_type = Endpoint<Types>;
    using connection_id_type = ConnectionId<Types>;
    using simulator_type = NetworkSimulator<Types>;
    using future_bool_type = typename Types::future_bool_type;
    using future_bytes_type = typename Types::future_bytes_type;
    
    Connection(endpoint_type local, 
               endpoint_type remote,
               simulator_type* simulator)
        : _local(std::move(local))
        , _remote(std::move(remote))
        , _connection_id(_local.address, _local.port, _remote.address, _remote.port)
        , _simulator(simulator)
        , _open(true)
    {}
    
    auto read() -> future_bytes_type;
    auto read(std::chrono::milliseconds timeout) -> future_bytes_type;
    
    auto write(std::vector<std::byte> data) -> future_bool_type;
    auto write(std::vector<std::byte> data, std::chrono::milliseconds timeout) -> future_bool_type;
    
    auto close() -> void;
    auto is_open() const -> bool;
    
    auto local_endpoint() const -> endpoint_type { return _local; }
    auto remote_endpoint() const -> endpoint_type { return _remote; }
    auto connection_id() const -> connection_id_type { return _connection_id; }
    
    // Internal method for simulator to deliver data
    auto deliver_data(std::vector<std::byte> data) -> void;
    
private:
    endpoint_type _local;
    endpoint_type _remote;
    connection_id_type _connection_id;
    simulator_type* _simulator;
    
    std::atomic<bool> _open;
    
    // Buffered data for this connection
    std::queue<std::vector<std::byte>> _read_buffer;
    mutable std::mutex _buffer_mutex;
    std::condition_variable _data_available;
};

} // namespace network_simulator

#include "connection_impl.hpp"
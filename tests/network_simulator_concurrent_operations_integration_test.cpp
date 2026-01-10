#define BOOST_TEST_MODULE NetworkSimulatorConcurrentOperationsIntegrationTest
#include <boost/test/unit_test.hpp>

#include <network_simulator/simulator.hpp>
#include <network_simulator/types.hpp>
#include <network_simulator/exceptions.hpp>

#include <chrono>
#include <string>
#include <vector>
#include <thread>
#include <memory>
#include <future>
#include <atomic>
#include <random>

using namespace network_simulator;

namespace {
    constexpr const char* node_prefix = "node_";
    constexpr unsigned short base_port = 8080;
    constexpr std::chrono::milliseconds network_latency{5};
    constexpr double network_reliability = 1.0;  // Perfect reliability for integration tests
    constexpr std::chrono::seconds test_timeout{10};
    constexpr const char* test_message_prefix = "Message from ";
    constexpr std::size_t num_nodes = 4;
    constexpr std::size_t messages_per_node = 10;
    constexpr std::size_t concurrent_connections = 5;
}

/**
 * Integration test for concurrent connectionless operations
 * Tests: multiple nodes sending/receiving simultaneously
 * _Requirements: 14.1-14.5_
 */
BOOST_AUTO_TEST_CASE(concurrent_connectionless_operations, * boost::unit_test::timeout(60)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    // Create a fully connected mesh topology for maximum concurrency
    NetworkEdge edge(network_latency, network_reliability);
    
    std::vector<std::string> node_ids;
    std::vector<std::shared_ptr<DefaultNetworkTypes::node_type>> nodes;
    
    // Create nodes
    for (std::size_t i = 0; i < num_nodes; ++i) {
        std::string node_id = node_prefix + std::to_string(i);
        node_ids.push_back(node_id);
        
        sim.add_node(node_id);
        auto node = sim.create_node(node_id);
        nodes.push_back(node);
    }
    
    // Create full mesh connectivity
    for (std::size_t i = 0; i < num_nodes; ++i) {
        for (std::size_t j = 0; j < num_nodes; ++j) {
            if (i != j) {
                sim.add_edge(node_ids[i], node_ids[j], edge);
            }
        }
    }
    
    // Start simulation
    sim.start();
    
    // === CONCURRENT SENDING ===
    
    std::vector<std::future<void>> send_futures;
    std::atomic<std::size_t> successful_sends{0};
    std::atomic<std::size_t> failed_sends{0};
    
    // Each node sends messages to all other nodes concurrently
    for (std::size_t sender_idx = 0; sender_idx < num_nodes; ++sender_idx) {
        auto send_task = std::async(std::launch::async, [&, sender_idx]() {
            auto sender_node = nodes[sender_idx];
            std::string sender_id = node_ids[sender_idx];
            
            for (std::size_t target_idx = 0; target_idx < num_nodes; ++target_idx) {
                if (sender_idx == target_idx) continue;  // Don't send to self
                
                std::string target_id = node_ids[target_idx];
                
                for (std::size_t msg_num = 0; msg_num < messages_per_node; ++msg_num) {
                    try {
                        // Create unique message
                        std::string message_content = test_message_prefix + sender_id + 
                                                    " to " + target_id + 
                                                    " #" + std::to_string(msg_num);
                        
                        std::vector<std::byte> payload;
                        for (char c : message_content) {
                            payload.push_back(static_cast<std::byte>(c));
                        }
                        
                        DefaultNetworkTypes::message_type msg(
                            sender_id, base_port + static_cast<unsigned short>(sender_idx),
                            target_id, base_port + static_cast<unsigned short>(target_idx),
                            payload
                        );
                        
                        // Send message
                        auto send_future = sender_node->send(msg);
                        bool send_success = std::move(send_future).get();
                        
                        if (send_success) {
                            successful_sends.fetch_add(1);
                        } else {
                            failed_sends.fetch_add(1);
                        }
                        
                        // Small delay to avoid overwhelming the system
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        
                    } catch (const std::exception& e) {
                        failed_sends.fetch_add(1);
                    }
                }
            }
        });
        
        send_futures.push_back(std::move(send_task));
    }
    
    // Wait for all sending to complete
    for (auto& future : send_futures) {
        future.wait();
    }
    
    // Allow time for message delivery
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // === CONCURRENT RECEIVING ===
    
    std::vector<std::future<void>> receive_futures;
    std::atomic<std::size_t> successful_receives{0};
    std::atomic<std::size_t> failed_receives{0};
    
    // Each node tries to receive messages concurrently
    for (std::size_t receiver_idx = 0; receiver_idx < num_nodes; ++receiver_idx) {
        auto receive_task = std::async(std::launch::async, [&, receiver_idx]() {
            auto receiver_node = nodes[receiver_idx];
            std::string receiver_id = node_ids[receiver_idx];
            
            // Try to receive messages (we expect messages from other nodes)
            std::size_t expected_messages = (num_nodes - 1) * messages_per_node;
            
            for (std::size_t i = 0; i < expected_messages; ++i) {
                try {
                    auto receive_future = receiver_node->receive(std::chrono::milliseconds{100});
                    auto received_msg = std::move(receive_future).get();
                    
                    // Check if we actually received a message (not empty)
                    if (!received_msg.source_address().empty()) {
                        // Verify the message is addressed to this node
                        if (received_msg.destination_address() == receiver_id) {
                            successful_receives.fetch_add(1);
                        } else {
                            failed_receives.fetch_add(1);
                        }
                    } else {
                        // No message available
                        break;
                    }
                    
                } catch (const TimeoutException&) {
                    // No more messages available
                    break;
                } catch (const std::exception&) {
                    failed_receives.fetch_add(1);
                }
            }
        });
        
        receive_futures.push_back(std::move(receive_task));
    }
    
    // Wait for all receiving to complete
    for (auto& future : receive_futures) {
        future.wait();
    }
    
    // === VERIFY CONCURRENT OPERATIONS ===
    
    std::size_t total_expected_sends = num_nodes * (num_nodes - 1) * messages_per_node;
    
    // We should have attempted to send all messages
    BOOST_CHECK_EQUAL(successful_sends + failed_sends, total_expected_sends);
    
    // Most sends should succeed (allowing for some failures due to concurrency)
    BOOST_CHECK_GT(successful_sends.load(), total_expected_sends / 2);
    
    // We should have received some messages (exact count depends on timing and implementation)
    BOOST_CHECK_GT(successful_receives.load(), 0);
    
    sim.stop();
}

/**
 * Integration test for concurrent connection-oriented operations
 * Tests: multiple clients connecting to multiple servers simultaneously
 * _Requirements: 14.1-14.5_
 */
BOOST_AUTO_TEST_CASE(concurrent_connection_oriented_operations, * boost::unit_test::timeout(60)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    // Create topology with multiple server and client nodes
    NetworkEdge edge(network_latency, network_reliability);
    
    std::vector<std::string> server_ids;
    std::vector<std::string> client_ids;
    std::vector<std::shared_ptr<DefaultNetworkTypes::node_type>> server_nodes;
    std::vector<std::shared_ptr<DefaultNetworkTypes::node_type>> client_nodes;
    
    // Create server nodes
    for (std::size_t i = 0; i < num_nodes / 2; ++i) {
        std::string server_id = "server_" + std::to_string(i);
        server_ids.push_back(server_id);
        
        sim.add_node(server_id);
        auto server_node = sim.create_node(server_id);
        server_nodes.push_back(server_node);
    }
    
    // Create client nodes
    for (std::size_t i = 0; i < num_nodes / 2; ++i) {
        std::string client_id = "client_" + std::to_string(i);
        client_ids.push_back(client_id);
        
        sim.add_node(client_id);
        auto client_node = sim.create_node(client_id);
        client_nodes.push_back(client_node);
    }
    
    // Create bidirectional connectivity between all nodes
    for (const auto& server_id : server_ids) {
        for (const auto& client_id : client_ids) {
            sim.add_edge(server_id, client_id, edge);
            sim.add_edge(client_id, server_id, edge);
        }
    }
    
    // Start simulation
    sim.start();
    
    // === CONCURRENT SERVER SETUP ===
    
    std::vector<std::future<std::shared_ptr<DefaultNetworkTypes::listener_type>>> listener_futures;
    
    // All servers bind to ports concurrently
    for (std::size_t i = 0; i < server_nodes.size(); ++i) {
        auto bind_task = std::async(std::launch::async, [&, i]() {
            unsigned short port = base_port + static_cast<unsigned short>(i);
            auto bind_future = server_nodes[i]->bind(port);
            return std::move(bind_future).get();
        });
        
        listener_futures.push_back(std::move(bind_task));
    }
    
    // Collect all listeners
    std::vector<std::shared_ptr<DefaultNetworkTypes::listener_type>> listeners;
    for (auto& future : listener_futures) {
        auto listener = future.get();
        BOOST_REQUIRE(listener != nullptr);
        BOOST_CHECK(listener->is_listening());
        listeners.push_back(listener);
    }
    
    // === CONCURRENT CLIENT CONNECTIONS ===
    
    std::vector<std::future<std::shared_ptr<DefaultNetworkTypes::connection_type>>> connection_futures;
    std::atomic<std::size_t> successful_connections{0};
    std::atomic<std::size_t> failed_connections{0};
    
    // Each client connects to each server concurrently
    for (std::size_t client_idx = 0; client_idx < client_nodes.size(); ++client_idx) {
        for (std::size_t server_idx = 0; server_idx < server_nodes.size(); ++server_idx) {
            auto connect_task = std::async(std::launch::async, [&, client_idx, server_idx]() -> std::shared_ptr<DefaultNetworkTypes::connection_type> {
                try {
                    std::string server_id = server_ids[server_idx];
                    unsigned short server_port = base_port + static_cast<unsigned short>(server_idx);
                    unsigned short client_port = base_port + 1000 + static_cast<unsigned short>(client_idx * 10 + server_idx);
                    
                    auto connect_future = client_nodes[client_idx]->connect(server_id, server_port, client_port);
                    auto connection = std::move(connect_future).get();
                    
                    if (connection != nullptr && connection->is_open()) {
                        successful_connections.fetch_add(1);
                        return connection;
                    } else {
                        failed_connections.fetch_add(1);
                        return nullptr;
                    }
                } catch (const std::exception&) {
                    failed_connections.fetch_add(1);
                    return nullptr;
                }
            });
            
            connection_futures.push_back(std::move(connect_task));
        }
    }
    
    // === CONCURRENT SERVER ACCEPTS ===
    
    std::vector<std::future<std::shared_ptr<DefaultNetworkTypes::connection_type>>> accept_futures;
    
    // Each server accepts connections concurrently
    for (std::size_t server_idx = 0; server_idx < listeners.size(); ++server_idx) {
        // Each server expects connections from all clients
        for (std::size_t client_idx = 0; client_idx < client_nodes.size(); ++client_idx) {
            auto accept_task = std::async(std::launch::async, [&, server_idx]() -> std::shared_ptr<DefaultNetworkTypes::connection_type> {
                try {
                    auto accept_future = listeners[server_idx]->accept(test_timeout);
                    auto connection = std::move(accept_future).get();
                    
                    if (connection != nullptr && connection->is_open()) {
                        return connection;
                    } else {
                        return nullptr;
                    }
                } catch (const std::exception&) {
                    return nullptr;
                }
            });
            
            accept_futures.push_back(std::move(accept_task));
        }
    }
    
    // === COLLECT CONNECTIONS ===
    
    std::vector<std::shared_ptr<DefaultNetworkTypes::connection_type>> client_connections;
    std::vector<std::shared_ptr<DefaultNetworkTypes::connection_type>> server_connections;
    
    // Collect client connections
    for (auto& future : connection_futures) {
        auto connection = future.get();
        if (connection != nullptr) {
            client_connections.push_back(connection);
        }
    }
    
    // Collect server connections
    for (auto& future : accept_futures) {
        auto connection = future.get();
        if (connection != nullptr) {
            server_connections.push_back(connection);
        }
    }
    
    // === CONCURRENT DATA TRANSFER ===
    
    std::atomic<std::size_t> successful_writes{0};
    std::atomic<std::size_t> successful_reads{0};
    std::vector<std::future<void>> data_futures;
    
    // Concurrent writes from clients
    for (std::size_t i = 0; i < client_connections.size(); ++i) {
        auto write_task = std::async(std::launch::async, [&, i]() {
            try {
                std::string message = "Data from client " + std::to_string(i);
                std::vector<std::byte> data;
                for (char c : message) {
                    data.push_back(static_cast<std::byte>(c));
                }
                
                auto write_future = client_connections[i]->write(data);
                bool write_success = std::move(write_future).get();
                
                if (write_success) {
                    successful_writes.fetch_add(1);
                }
            } catch (const std::exception&) {
                // Write failed
            }
        });
        
        data_futures.push_back(std::move(write_task));
    }
    
    // Concurrent reads from servers
    for (std::size_t i = 0; i < server_connections.size(); ++i) {
        auto read_task = std::async(std::launch::async, [&, i]() {
            try {
                auto read_future = server_connections[i]->read(test_timeout);
                auto data = std::move(read_future).get();
                
                if (!data.empty()) {
                    successful_reads.fetch_add(1);
                }
            } catch (const std::exception&) {
                // Read failed or timed out
            }
        });
        
        data_futures.push_back(std::move(read_task));
    }
    
    // Wait for all data operations to complete
    for (auto& future : data_futures) {
        future.wait();
    }
    
    // === VERIFY CONCURRENT OPERATIONS ===
    
    // We should have established some connections
    BOOST_CHECK_GT(successful_connections.load(), 0);
    BOOST_CHECK_GT(client_connections.size(), 0);
    BOOST_CHECK_GT(server_connections.size(), 0);
    
    // We should have some successful data transfers
    BOOST_CHECK_GT(successful_writes.load(), 0);
    BOOST_CHECK_GT(successful_reads.load(), 0);
    
    // === CLEANUP ===
    
    // Close all connections
    for (auto& conn : client_connections) {
        if (conn != nullptr) {
            conn->close();
        }
    }
    
    for (auto& conn : server_connections) {
        if (conn != nullptr) {
            conn->close();
        }
    }
    
    // Close all listeners
    for (auto& listener : listeners) {
        if (listener != nullptr) {
            listener->close();
        }
    }
    
    sim.stop();
}

/**
 * Integration test for concurrent topology modifications
 * Tests: adding/removing nodes and edges while operations are ongoing
 * _Requirements: 14.1-14.5_
 */
BOOST_AUTO_TEST_CASE(concurrent_topology_modifications, * boost::unit_test::timeout(60)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    NetworkEdge edge(network_latency, network_reliability);
    
    // Start with initial topology
    std::string node_a = "node_a";
    std::string node_b = "node_b";
    
    sim.add_node(node_a);
    sim.add_node(node_b);
    sim.add_edge(node_a, node_b, edge);
    sim.add_edge(node_b, node_a, edge);
    
    auto node_a_ptr = sim.create_node(node_a);
    auto node_b_ptr = sim.create_node(node_b);
    
    sim.start();
    
    // === CONCURRENT OPERATIONS AND TOPOLOGY CHANGES ===
    
    std::atomic<bool> stop_operations{false};
    std::atomic<std::size_t> operations_completed{0};
    std::atomic<std::size_t> topology_changes{0};
    
    // Background task: continuous message sending
    auto messaging_task = std::async(std::launch::async, [&]() {
        std::size_t message_count = 0;
        
        while (!stop_operations.load()) {
            try {
                std::string message = "Message " + std::to_string(message_count++);
                std::vector<std::byte> payload;
                for (char c : message) {
                    payload.push_back(static_cast<std::byte>(c));
                }
                
                DefaultNetworkTypes::message_type msg(
                    node_a, base_port,
                    node_b, base_port,
                    payload
                );
                
                auto send_future = node_a_ptr->send(msg);
                bool send_success = std::move(send_future).get();
                
                if (send_success) {
                    operations_completed.fetch_add(1);
                }
                
                // Small delay
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                
            } catch (const std::exception&) {
                // Operation failed - continue
            }
        }
    });
    
    // Background task: topology modifications
    auto topology_task = std::async(std::launch::async, [&]() {
        std::size_t node_count = 2;
        
        for (int i = 0; i < 5; ++i) {  // Perform 5 topology changes
            try {
                std::string new_node = "dynamic_node_" + std::to_string(i);
                
                // Add new node
                sim.add_node(new_node);
                sim.add_edge(node_a, new_node, edge);
                sim.add_edge(new_node, node_a, edge);
                
                auto new_node_ptr = sim.create_node(new_node);
                topology_changes.fetch_add(1);
                
                // Let it exist for a while
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                
                // Remove the node
                sim.remove_node(new_node);
                topology_changes.fetch_add(1);
                
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                
            } catch (const std::exception&) {
                // Topology change failed - continue
            }
        }
    });
    
    // Let operations run for a while
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // Stop operations
    stop_operations.store(true);
    
    // Wait for tasks to complete
    messaging_task.wait();
    topology_task.wait();
    
    // === VERIFY CONCURRENT SAFETY ===
    
    // We should have completed some operations despite topology changes
    BOOST_CHECK_GT(operations_completed.load(), 0);
    
    // We should have made some topology changes
    BOOST_CHECK_GT(topology_changes.load(), 0);
    
    // The simulator should still be in a valid state
    BOOST_CHECK(sim.has_node(node_a));
    BOOST_CHECK(sim.has_node(node_b));
    BOOST_CHECK(sim.has_edge(node_a, node_b));
    
    sim.stop();
}

/**
 * Integration test for concurrent simulator lifecycle operations
 * Tests: starting/stopping simulator while operations are ongoing
 * _Requirements: 14.1-14.5_
 */
BOOST_AUTO_TEST_CASE(concurrent_lifecycle_operations, * boost::unit_test::timeout(60)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    NetworkEdge edge(network_latency, network_reliability);
    
    // Set up topology
    std::string node_a = "node_a";
    std::string node_b = "node_b";
    
    sim.add_node(node_a);
    sim.add_node(node_b);
    sim.add_edge(node_a, node_b, edge);
    sim.add_edge(node_b, node_a, edge);
    
    auto node_a_ptr = sim.create_node(node_a);
    auto node_b_ptr = sim.create_node(node_b);
    
    // === TEST CONCURRENT START/STOP CYCLES ===
    
    std::atomic<std::size_t> start_stop_cycles{0};
    std::atomic<std::size_t> operations_attempted{0};
    std::atomic<std::size_t> operations_succeeded{0};
    
    // Background task: continuous start/stop cycles
    auto lifecycle_task = std::async(std::launch::async, [&]() {
        for (int i = 0; i < 5; ++i) {
            try {
                sim.start();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                
                sim.stop();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                
                start_stop_cycles.fetch_add(1);
                
            } catch (const std::exception&) {
                // Lifecycle operation failed - continue
            }
        }
    });
    
    // Background task: attempt operations during lifecycle changes
    auto operations_task = std::async(std::launch::async, [&]() {
        for (int i = 0; i < 20; ++i) {
            try {
                operations_attempted.fetch_add(1);
                
                std::string message = "Lifecycle test message " + std::to_string(i);
                std::vector<std::byte> payload;
                for (char c : message) {
                    payload.push_back(static_cast<std::byte>(c));
                }
                
                DefaultNetworkTypes::message_type msg(
                    node_a, base_port,
                    node_b, base_port,
                    payload
                );
                
                auto send_future = node_a_ptr->send(msg);
                bool send_success = std::move(send_future).get();
                
                if (send_success) {
                    operations_succeeded.fetch_add(1);
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                
            } catch (const std::exception&) {
                // Operation failed during lifecycle change - expected
            }
        }
    });
    
    // Wait for both tasks to complete
    lifecycle_task.wait();
    operations_task.wait();
    
    // === VERIFY CONCURRENT SAFETY ===
    
    // We should have completed some start/stop cycles
    BOOST_CHECK_GT(start_stop_cycles.load(), 0);
    
    // We should have attempted operations
    BOOST_CHECK_GT(operations_attempted.load(), 0);
    
    // Some operations may have succeeded (when simulator was started)
    // Some may have failed (when simulator was stopped)
    // Both outcomes are acceptable for concurrent safety
    
    // Final state should be consistent
    sim.start();  // Ensure we can start again
    BOOST_CHECK(sim.has_node(node_a));
    BOOST_CHECK(sim.has_node(node_b));
    
    sim.stop();
}

/**
 * Integration test for thread safety with high contention
 * Tests: many threads performing operations simultaneously
 * _Requirements: 14.1-14.5_
 */
BOOST_AUTO_TEST_CASE(high_contention_thread_safety, * boost::unit_test::timeout(60)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    NetworkEdge edge(network_latency, network_reliability);
    
    // Create a small topology for high contention
    std::string node_a = "node_a";
    std::string node_b = "node_b";
    
    sim.add_node(node_a);
    sim.add_node(node_b);
    sim.add_edge(node_a, node_b, edge);
    sim.add_edge(node_b, node_a, edge);
    
    auto node_a_ptr = sim.create_node(node_a);
    auto node_b_ptr = sim.create_node(node_b);
    
    sim.start();
    
    // === HIGH CONTENTION TEST ===
    
    constexpr std::size_t num_threads = 10;
    constexpr std::size_t operations_per_thread = 20;
    
    std::atomic<std::size_t> total_operations{0};
    std::atomic<std::size_t> successful_operations{0};
    std::vector<std::future<void>> thread_futures;
    
    // Launch many threads performing operations simultaneously
    for (std::size_t thread_id = 0; thread_id < num_threads; ++thread_id) {
        auto thread_task = std::async(std::launch::async, [&, thread_id]() {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> delay_dist(1, 10);
            
            for (std::size_t op = 0; op < operations_per_thread; ++op) {
                try {
                    total_operations.fetch_add(1);
                    
                    std::string message = "Thread " + std::to_string(thread_id) + 
                                        " Operation " + std::to_string(op);
                    std::vector<std::byte> payload;
                    for (char c : message) {
                        payload.push_back(static_cast<std::byte>(c));
                    }
                    
                    // Alternate between sending from A to B and B to A
                    if (op % 2 == 0) {
                        DefaultNetworkTypes::message_type msg(
                            node_a, base_port + static_cast<unsigned short>(thread_id),
                            node_b, base_port,
                            payload
                        );
                        
                        auto send_future = node_a_ptr->send(msg);
                        bool send_success = std::move(send_future).get();
                        
                        if (send_success) {
                            successful_operations.fetch_add(1);
                        }
                    } else {
                        DefaultNetworkTypes::message_type msg(
                            node_b, base_port + static_cast<unsigned short>(thread_id),
                            node_a, base_port,
                            payload
                        );
                        
                        auto send_future = node_b_ptr->send(msg);
                        bool send_success = std::move(send_future).get();
                        
                        if (send_success) {
                            successful_operations.fetch_add(1);
                        }
                    }
                    
                    // Random small delay to increase contention
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_dist(gen)));
                    
                } catch (const std::exception&) {
                    // Operation failed - continue
                }
            }
        });
        
        thread_futures.push_back(std::move(thread_task));
    }
    
    // Wait for all threads to complete
    for (auto& future : thread_futures) {
        future.wait();
    }
    
    // === VERIFY THREAD SAFETY ===
    
    std::size_t expected_operations = num_threads * operations_per_thread;
    
    // All operations should have been attempted
    BOOST_CHECK_EQUAL(total_operations.load(), expected_operations);
    
    // Most operations should have succeeded (allowing for some contention failures)
    BOOST_CHECK_GT(successful_operations.load(), expected_operations / 2);
    
    // The simulator should still be in a consistent state
    BOOST_CHECK(sim.has_node(node_a));
    BOOST_CHECK(sim.has_node(node_b));
    BOOST_CHECK(sim.has_edge(node_a, node_b));
    BOOST_CHECK(sim.has_edge(node_b, node_a));
    
    sim.stop();
}
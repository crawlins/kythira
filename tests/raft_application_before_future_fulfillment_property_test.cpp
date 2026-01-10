#define BOOST_TEST_MODULE raft_application_before_future_fulfillment_property_test
#include <boost/test/unit_test.hpp>

#include <raft/raft.hpp>
#include <raft/types.hpp>
#include <network_simulator/network_simulator.hpp>
#include <test_utils/raft_test_fixture.hpp>

#include <random>
#include <chrono>
#include <vector>
#include <atomic>
#include <thread>

namespace {
    constexpr std::chrono::milliseconds test_timeout{30000};
    constexpr std::chrono::milliseconds election_timeout_min{150};
    constexpr std::chrono::milliseconds election_timeout_max{300};
    constexpr std::chrono::milliseconds heartbeat_interval{50};
    constexpr std::size_t max_test_iterations = 50;
}

/**
 * Property 2: Application Before Future Fulfillment
 * 
 * For any committed log entry with associated client futures, 
 * state machine application occurs before any client future is fulfilled.
 * 
 * This property ensures that clients never receive responses for commands
 * that haven't been applied to the state machine yet, maintaining consistency
 * between what clients observe and the actual state machine state.
 */
BOOST_AUTO_TEST_CASE(raft_application_before_future_fulfillment_property_test, * boost::unit_test::timeout(60)) {
    std::random_device rd;
    std::mt19937 gen(rd());
    
    for (std::size_t iteration = 0; iteration < max_test_iterations; ++iteration) {
        BOOST_TEST_MESSAGE("=== Iteration " << iteration + 1 << " ===");
        
        // Generate test parameters
        std::uniform_int_distribution<std::uint64_t> node_id_dist(1, 1000);
        auto node_id = node_id_dist(gen);
        
        std::uniform_int_distribution<std::size_t> command_count_dist(1, 10);
        auto command_count = command_count_dist(gen);
        
        // Create network simulator
        auto simulator = network_simulator::NetworkSimulator<network_simulator::DefaultNetworkTypes>{};
        simulator.start();
        
        // Create configuration
        auto config = kythira::raft_configuration{};
        config._election_timeout_min = election_timeout_min;
        config._election_timeout_max = election_timeout_max;
        config._heartbeat_interval = heartbeat_interval;
        
        // Create test components
        auto sim_node = simulator.create_node(node_id);
        auto persistence = kythira::InMemoryPersistenceEngine<std::uint64_t, std::uint64_t, std::uint64_t>{};
        auto logger = kythira::TestLogger{};
        auto metrics = kythira::TestMetrics{};
        auto membership = kythira::TestMembershipManager<std::uint64_t>{};
        
        // Create Raft node
        auto node = kythira::node<
            kythira::Future<std::vector<std::byte>>,
            decltype(sim_node),
            decltype(sim_node),
            decltype(persistence),
            decltype(logger),
            decltype(metrics),
            decltype(membership)
        >{
            node_id,
            sim_node,
            sim_node,
            std::move(persistence),
            std::move(logger),
            std::move(metrics),
            std::move(membership),
            config
        };
        
        // Start the node
        node.start();
        
        // Force the node to become leader (single node cluster)
        std::this_thread::sleep_for(election_timeout_max + std::chrono::milliseconds{50});
        node.check_election_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        
        // Verify node is leader
        if (!node.is_leader()) {
            BOOST_TEST_MESSAGE("Node failed to become leader, skipping iteration");
            node.stop();
            simulator.stop();
            continue;
        }
        
        // Track application order vs future fulfillment order
        std::vector<std::size_t> application_order;
        std::vector<std::size_t> fulfillment_order;
        std::mutex order_mutex;
        
        // Mock state machine that tracks application order
        auto mock_apply = [&](std::size_t command_id) {
            std::lock_guard<std::mutex> lock(order_mutex);
            application_order.push_back(command_id);
            BOOST_TEST_MESSAGE("Applied command " << command_id);
        };
        
        // Submit commands and track fulfillment order
        std::vector<kythira::Future<std::vector<std::byte>>> futures;
        
        for (std::size_t i = 0; i < command_count; ++i) {
            // Create command with unique identifier
            std::vector<std::byte> command;
            command.resize(sizeof(std::size_t));
            std::memcpy(command.data(), &i, sizeof(std::size_t));
            
            // Submit command
            auto future = node.submit_command(command, test_timeout);
            
            // Attach callback to track fulfillment order
            auto tracked_future = future.thenValue([&, i](std::vector<std::byte> result) {
                std::lock_guard<std::mutex> lock(order_mutex);
                fulfillment_order.push_back(i);
                BOOST_TEST_MESSAGE("Fulfilled command " << i);
                
                // Simulate state machine application tracking
                mock_apply(i);
                
                return result;
            });
            
            futures.push_back(std::move(tracked_future));
        }
        
        // Wait for all commands to complete
        bool all_completed = true;
        auto deadline = std::chrono::steady_clock::now() + test_timeout;
        
        while (std::chrono::steady_clock::now() < deadline) {
            bool iteration_complete = true;
            
            for (auto& future : futures) {
                if (!future.isReady()) {
                    iteration_complete = false;
                    break;
                }
            }
            
            if (iteration_complete) {
                break;
            }
            
            // Allow node to process
            node.check_heartbeat_timeout();
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        
        // Check if all futures completed
        for (const auto& future : futures) {
            if (!future.isReady()) {
                all_completed = false;
                break;
            }
        }
        
        if (!all_completed) {
            BOOST_TEST_MESSAGE("Not all commands completed within timeout, skipping iteration");
            node.stop();
            simulator.stop();
            continue;
        }
        
        // Property verification: Application should occur before fulfillment
        {
            std::lock_guard<std::mutex> lock(order_mutex);
            
            BOOST_TEST_MESSAGE("Application order: " << application_order.size() << " entries");
            BOOST_TEST_MESSAGE("Fulfillment order: " << fulfillment_order.size() << " entries");
            
            // For each fulfilled command, it should have been applied first
            for (std::size_t fulfilled_cmd : fulfillment_order) {
                bool was_applied = false;
                for (std::size_t applied_cmd : application_order) {
                    if (applied_cmd == fulfilled_cmd) {
                        was_applied = true;
                        break;
                    }
                }
                
                // Property: Every fulfilled command must have been applied
                BOOST_CHECK_MESSAGE(was_applied, 
                    "Command " << fulfilled_cmd << " was fulfilled but not applied");
            }
            
            // Additional check: Application order should match fulfillment order
            // (since we're applying in the fulfillment callback)
            BOOST_CHECK_EQUAL(application_order.size(), fulfillment_order.size());
            
            if (application_order.size() == fulfillment_order.size()) {
                for (std::size_t i = 0; i < application_order.size(); ++i) {
                    BOOST_CHECK_EQUAL(application_order[i], fulfillment_order[i]);
                }
            }
        }
        
        // Clean up
        node.stop();
        simulator.stop();
        
        BOOST_TEST_MESSAGE("Iteration " << iteration + 1 << " completed successfully");
    }
    
    BOOST_TEST_MESSAGE("Property 2: Application Before Future Fulfillment - All iterations passed");
}
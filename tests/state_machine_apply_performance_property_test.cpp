#define BOOST_TEST_MODULE state_machine_apply_performance_property_test
#include <boost/test/unit_test.hpp>
#include "raft/test_state_machine.hpp"
#include "raft/examples/counter_state_machine.hpp"
#include "state_machine_test_utilities.hpp"
#include <chrono>

BOOST_AUTO_TEST_CASE(property_apply_completes_quickly, * boost::unit_test::timeout(30)) {
    kythira::test::command_generator gen;
    constexpr auto max_latency_us = 1000; // 1ms
    
    for (int iteration = 0; iteration < 100; ++iteration) {
        kythira::test_key_value_state_machine sm;
        auto cmd = gen.random_command();
        
        auto start = std::chrono::high_resolution_clock::now();
        try {
            sm.apply(cmd, 1);
        } catch (...) {}
        auto end = std::chrono::high_resolution_clock::now();
        
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        BOOST_CHECK_LT(duration.count(), max_latency_us);
    }
}

BOOST_AUTO_TEST_CASE(property_apply_scales_linearly, * boost::unit_test::timeout(60)) {
    kythira::test_key_value_state_machine sm;
    
    std::vector<long long> durations;
    
    for (int batch = 0; batch < 5; ++batch) {
        auto start = std::chrono::high_resolution_clock::now();
        
        for (int i = 0; i < 100; ++i) {
            std::string key = "key" + std::to_string(batch * 100 + i);
            std::string value = "value";
            auto cmd = kythira::test_key_value_state_machine<>::make_put_command(key, value);
            sm.apply(cmd, batch * 100 + i + 1);
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        durations.push_back(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
    }
    
    // Check that performance doesn't degrade significantly
    // Later batches should not be more than 2x slower than first batch
    for (std::size_t i = 1; i < durations.size(); ++i) {
        BOOST_CHECK_LT(durations[i], durations[0] * 2);
    }
}

BOOST_AUTO_TEST_CASE(property_counter_performance, * boost::unit_test::timeout(30)) {
    kythira::examples::counter_state_machine sm;
    constexpr auto max_latency_us = 100; // Counter should be very fast
    
    std::string cmd = "INC";
    std::vector<std::byte> bytes(reinterpret_cast<const std::byte*>(cmd.data()),
                                 reinterpret_cast<const std::byte*>(cmd.data() + cmd.size()));
    
    for (int i = 0; i < 1000; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        sm.apply(bytes, i + 1);
        auto end = std::chrono::high_resolution_clock::now();
        
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        BOOST_CHECK_LT(duration.count(), max_latency_us);
    }
}

BOOST_AUTO_TEST_CASE(property_concurrent_load_performance, * boost::unit_test::timeout(60)) {
    kythira::test_key_value_state_machine sm;
    
    // Simulate high load
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < 10000; ++i) {
        std::string key = "key" + std::to_string(i % 100);
        std::string value = "value" + std::to_string(i);
        auto cmd = kythira::test_key_value_state_machine<>::make_put_command(key, value);
        sm.apply(cmd, i + 1);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    // Should complete 10k operations in under 1 second
    BOOST_CHECK_LT(duration.count(), 1000);
}

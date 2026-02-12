#define BOOST_TEST_MODULE state_machine_performance_benchmark
#include <boost/test/unit_test.hpp>
#include "raft/test_state_machine.hpp"
#include "raft/examples/counter_state_machine.hpp"
#include "raft/examples/register_state_machine.hpp"
#include <chrono>
#include <iostream>

template<typename StateMachine, typename CommandGen>
auto benchmark_apply(const std::string& name, CommandGen gen, int iterations) -> void {
    StateMachine sm;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < iterations; ++i) {
        try {
            sm.apply(gen(i), i + 1);
        } catch (...) {}
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    double avg_us = static_cast<double>(duration.count()) / iterations;
    double ops_per_sec = 1000000.0 / avg_us;
    
    std::cout << name << ":\n";
    std::cout << "  Total: " << duration.count() << " us\n";
    std::cout << "  Avg: " << avg_us << " us/op\n";
    std::cout << "  Throughput: " << static_cast<int>(ops_per_sec) << " ops/sec\n\n";
}

BOOST_AUTO_TEST_CASE(benchmark_kv_put, * boost::unit_test::timeout(60)) {
    auto gen = [](int i) {
        std::string key = "key" + std::to_string(i);
        std::string value = "value" + std::to_string(i);
        return kythira::test_key_value_state_machine<>::make_put_command(key, value);
    };
    
    benchmark_apply<kythira::test_key_value_state_machine<>>("KV PUT", gen, 10000);
}

BOOST_AUTO_TEST_CASE(benchmark_counter_inc, * boost::unit_test::timeout(60)) {
    auto gen = [](int) {
        std::string cmd = "INC";
        return std::vector<std::byte>(reinterpret_cast<const std::byte*>(cmd.data()),
                                      reinterpret_cast<const std::byte*>(cmd.data() + cmd.size()));
    };
    
    benchmark_apply<kythira::examples::counter_state_machine>("Counter INC", gen, 100000);
}

BOOST_AUTO_TEST_CASE(benchmark_register_write, * boost::unit_test::timeout(60)) {
    auto gen = [](int i) {
        std::string cmd = "WRITE " + std::to_string(i);
        return std::vector<std::byte>(reinterpret_cast<const std::byte*>(cmd.data()),
                                      reinterpret_cast<const std::byte*>(cmd.data() + cmd.size()));
    };
    
    benchmark_apply<kythira::examples::register_state_machine>("Register WRITE", gen, 100000);
}

BOOST_AUTO_TEST_CASE(benchmark_snapshot_operations, * boost::unit_test::timeout(60)) {
    kythira::test_key_value_state_machine sm;
    
    // Populate using binary format
    for (int i = 0; i < 1000; ++i) {
        std::string key = "key" + std::to_string(i);
        std::string value = "value" + std::to_string(i);
        auto cmd = kythira::test_key_value_state_machine<>::make_put_command(key, value);
        sm.apply(cmd, i + 1);
    }
    
    // Benchmark get_state
    auto start = std::chrono::high_resolution_clock::now();
    auto state = sm.get_state();
    auto end = std::chrono::high_resolution_clock::now();
    auto get_duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    // Benchmark restore_from_snapshot
    start = std::chrono::high_resolution_clock::now();
    kythira::test_key_value_state_machine sm2;
    sm2.restore_from_snapshot(state, 1000);
    end = std::chrono::high_resolution_clock::now();
    auto restore_duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    std::cout << "Snapshot operations (1000 entries):\n";
    std::cout << "  get_state: " << get_duration.count() << " us\n";
    std::cout << "  restore_from_snapshot: " << restore_duration.count() << " us\n\n";
}

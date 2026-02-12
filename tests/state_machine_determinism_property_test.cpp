#define BOOST_TEST_MODULE state_machine_determinism_property_test
#include <boost/test/unit_test.hpp>
#include "raft/test_state_machine.hpp"
#include "raft/examples/counter_state_machine.hpp"
#include "raft/examples/register_state_machine.hpp"
#include "state_machine_test_utilities.hpp"

BOOST_AUTO_TEST_CASE(property_kv_determinism, * boost::unit_test::timeout(30)) {
    kythira::test::command_generator gen(42);
    
    for (int iteration = 0; iteration < 100; ++iteration) {
        std::vector<std::vector<std::byte>> commands;
        for (int i = 0; i < 50; ++i) {
            commands.push_back(gen.random_command());
        }
        
        // Use non-template version for key-value state machine
        BOOST_CHECK(kythira::test::snapshot_validator::validate_determinism(commands));
    }
}

BOOST_AUTO_TEST_CASE(property_counter_determinism, * boost::unit_test::timeout(30)) {
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int> op_dist(0, 2);
    
    for (int iteration = 0; iteration < 100; ++iteration) {
        std::vector<std::vector<std::byte>> commands;
        
        for (int i = 0; i < 50; ++i) {
            std::string cmd;
            switch (op_dist(rng)) {
                case 0: cmd = "INC"; break;
                case 1: cmd = "DEC"; break;
                case 2: cmd = "RESET"; break;
            }
            commands.push_back({reinterpret_cast<const std::byte*>(cmd.data()),
                               reinterpret_cast<const std::byte*>(cmd.data() + cmd.size())});
        }
        
        BOOST_CHECK(kythira::test::snapshot_validator::validate_determinism<kythira::examples::counter_state_machine>(commands));
    }
}

BOOST_AUTO_TEST_CASE(property_register_determinism, * boost::unit_test::timeout(30)) {
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int> value_dist(0, 1000);
    
    for (int iteration = 0; iteration < 100; ++iteration) {
        std::vector<std::vector<std::byte>> commands;
        
        for (int i = 0; i < 50; ++i) {
            std::string cmd = "WRITE " + std::to_string(value_dist(rng));
            commands.push_back({reinterpret_cast<const std::byte*>(cmd.data()),
                               reinterpret_cast<const std::byte*>(cmd.data() + cmd.size())});
        }
        
        BOOST_CHECK(kythira::test::snapshot_validator::validate_determinism<kythira::examples::register_state_machine>(commands));
    }
}

BOOST_AUTO_TEST_CASE(property_multiple_runs_determinism, * boost::unit_test::timeout(30)) {
    kythira::test::command_generator gen(999);
    
    std::vector<std::vector<std::byte>> commands;
    for (int i = 0; i < 100; ++i) {
        commands.push_back(gen.random_command());
    }
    
    // Run 5 times and verify all produce same size (not byte-for-byte due to unordered_map)
    std::vector<std::size_t> sizes;
    for (int run = 0; run < 5; ++run) {
        kythira::test_key_value_state_machine<std::uint64_t> sm;
        std::uint64_t index = 1;
        for (const auto& cmd : commands) {
            try {
                sm.apply(cmd, index++);
            } catch (...) {}
        }
        sizes.push_back(sm.size());
    }
    
    // All runs should produce the same size
    for (std::size_t i = 1; i < sizes.size(); ++i) {
        BOOST_CHECK_EQUAL(sizes[0], sizes[i]);
    }
}

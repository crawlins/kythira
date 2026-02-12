#define BOOST_TEST_MODULE state_machine_snapshot_round_trip_property_test
#include <boost/test/unit_test.hpp>
#include "raft/test_state_machine.hpp"
#include "raft/examples/counter_state_machine.hpp"
#include "raft/examples/register_state_machine.hpp"
#include "state_machine_test_utilities.hpp"

BOOST_AUTO_TEST_CASE(property_kv_snapshot_round_trip, * boost::unit_test::timeout(30)) {
    kythira::test::command_generator gen;
    
    for (int iteration = 0; iteration < 100; ++iteration) {
        kythira::test_key_value_state_machine sm;
        
        // Apply random commands
        for (int i = 0; i < 50; ++i) {
            try {
                sm.apply(gen.random_command(), i + 1);
            } catch (...) {} // Ignore errors (e.g., GET on non-existent key)
        }
        
        // Validate round-trip
        BOOST_CHECK(kythira::test::snapshot_validator::validate_round_trip(sm, 50));
    }
}

BOOST_AUTO_TEST_CASE(property_counter_snapshot_round_trip, * boost::unit_test::timeout(30)) {
    for (int iteration = 0; iteration < 100; ++iteration) {
        kythira::examples::counter_state_machine sm;
        
        // Apply random operations
        std::mt19937_64 rng(iteration);
        std::uniform_int_distribution<int> op_dist(0, 2);
        
        for (int i = 0; i < 50; ++i) {
            std::string cmd;
            switch (op_dist(rng)) {
                case 0: cmd = "INC"; break;
                case 1: cmd = "DEC"; break;
                case 2: cmd = "RESET"; break;
            }
            std::vector<std::byte> cmd_bytes(reinterpret_cast<const std::byte*>(cmd.data()),
                                             reinterpret_cast<const std::byte*>(cmd.data() + cmd.size()));
            sm.apply(cmd_bytes, i + 1);
        }
        
        // Validate round-trip
        BOOST_CHECK(kythira::test::snapshot_validator::validate_round_trip(sm, 50));
    }
}

BOOST_AUTO_TEST_CASE(property_register_snapshot_round_trip, * boost::unit_test::timeout(30)) {
    for (int iteration = 0; iteration < 100; ++iteration) {
        kythira::examples::register_state_machine sm;
        
        // Apply random writes
        std::mt19937_64 rng(iteration);
        std::uniform_int_distribution<int> value_dist(0, 1000);
        
        for (int i = 0; i < 50; ++i) {
            std::string cmd = "WRITE " + std::to_string(value_dist(rng));
            std::vector<std::byte> cmd_bytes(reinterpret_cast<const std::byte*>(cmd.data()),
                                             reinterpret_cast<const std::byte*>(cmd.data() + cmd.size()));
            sm.apply(cmd_bytes, i + 1);
        }
        
        // Validate round-trip
        BOOST_CHECK(kythira::test::snapshot_validator::validate_round_trip(sm, 50));
    }
}

BOOST_AUTO_TEST_CASE(property_empty_state_snapshot, * boost::unit_test::timeout(30)) {
    kythira::test_key_value_state_machine sm;
    BOOST_CHECK(kythira::test::snapshot_validator::validate_round_trip(sm, 0));
}

BOOST_AUTO_TEST_CASE(property_large_state_snapshot, * boost::unit_test::timeout(30)) {
    kythira::test_key_value_state_machine sm;
    
    // Create large state using binary format
    for (int i = 0; i < 1000; ++i) {
        std::string key = "key" + std::to_string(i);
        std::string value(1000, 'x'); // 1KB value
        auto cmd = kythira::test_key_value_state_machine<>::make_put_command(key, value);
        sm.apply(cmd, i + 1);
    }
    
    BOOST_CHECK(kythira::test::snapshot_validator::validate_round_trip(sm, 1000));
}

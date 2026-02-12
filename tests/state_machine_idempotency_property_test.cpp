#define BOOST_TEST_MODULE state_machine_idempotency_property_test
#include <boost/test/unit_test.hpp>
#include "raft/test_state_machine.hpp"
#include "raft/examples/counter_state_machine.hpp"
#include "state_machine_test_utilities.hpp"

BOOST_AUTO_TEST_CASE(property_put_idempotency, * boost::unit_test::timeout(30)) {
    kythira::test::command_generator gen(42);
    
    for (int iteration = 0; iteration < 100; ++iteration) {
        auto cmd = gen.random_put();
        
        kythira::test_key_value_state_machine sm;
        auto result1 = sm.apply(cmd, 1);
        auto state1 = sm.get_state();
        
        auto result2 = sm.apply(cmd, 1); // Apply same command again
        auto state2 = sm.get_state();
        
        BOOST_CHECK(result1 == result2);
        BOOST_CHECK(state1 == state2);
    }
}

BOOST_AUTO_TEST_CASE(property_get_idempotency, * boost::unit_test::timeout(30)) {
    kythira::test_key_value_state_machine sm;
    
    // Setup: PUT a key using binary format
    auto put_cmd = kythira::test_key_value_state_machine<>::make_put_command("testkey", "testvalue");
    sm.apply(put_cmd, 1);
    
    // Test: GET multiple times using binary format
    auto get_cmd = kythira::test_key_value_state_machine<>::make_get_command("testkey");
    
    auto result1 = sm.apply(get_cmd, 2);
    auto state1 = sm.get_state();
    
    auto result2 = sm.apply(get_cmd, 2);
    auto state2 = sm.get_state();
    
    auto result3 = sm.apply(get_cmd, 2);
    auto state3 = sm.get_state();
    
    BOOST_CHECK(result1 == result2);
    BOOST_CHECK(result2 == result3);
    BOOST_CHECK(state1 == state2);
    BOOST_CHECK(state2 == state3);
}

BOOST_AUTO_TEST_CASE(property_counter_idempotency, * boost::unit_test::timeout(30)) {
    kythira::examples::counter_state_machine sm;
    
    std::string inc_cmd = "INC";
    std::vector<std::byte> inc_bytes(reinterpret_cast<const std::byte*>(inc_cmd.data()),
                                     reinterpret_cast<const std::byte*>(inc_cmd.data() + inc_cmd.size()));
    
    // Apply INC command at index 1
    auto result1 = sm.apply(inc_bytes, 1);
    auto state1 = sm.get_state();
    
    // Apply INC command at index 2 (different index, should increment again)
    auto result2 = sm.apply(inc_bytes, 2);
    auto state2 = sm.get_state();
    
    // Results should be different (counter incremented twice)
    BOOST_CHECK(result1 != result2);
    BOOST_CHECK(state1 != state2);
    
    // Verify counter value increased
    BOOST_CHECK_EQUAL(sm.get_value(), 2);
}

BOOST_AUTO_TEST_CASE(property_sequence_idempotency, * boost::unit_test::timeout(30)) {
    kythira::test::command_generator gen(123);
    
    for (int iteration = 0; iteration < 50; ++iteration) {
        std::vector<std::vector<std::byte>> commands;
        for (int i = 0; i < 10; ++i) {
            commands.push_back(gen.random_command());
        }
        
        // Apply sequence twice
        kythira::test_key_value_state_machine sm1, sm2;
        
        for (std::size_t i = 0; i < commands.size(); ++i) {
            try {
                sm1.apply(commands[i], i + 1);
                sm2.apply(commands[i], i + 1);
            } catch (...) {}
        }
        
        BOOST_CHECK(sm1.get_state() == sm2.get_state());
    }
}

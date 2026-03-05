#define BOOST_TEST_MODULE counter_state_machine_test
#include <boost/test/unit_test.hpp>
#include <raft/examples/counter_state_machine.hpp>

using namespace kythira::examples;

namespace {
    auto make_command(const std::string& cmd) -> std::vector<std::byte> {
        return {reinterpret_cast<const std::byte*>(cmd.data()),
                reinterpret_cast<const std::byte*>(cmd.data() + cmd.size())};
    }
    
    auto parse_result(const std::vector<std::byte>& result) -> std::int64_t {
        std::string str(reinterpret_cast<const char*>(result.data()), result.size());
        return std::stoll(str);
    }
}

BOOST_AUTO_TEST_CASE(test_counter_increment, * boost::unit_test::timeout(10)) {
    counter_state_machine sm;
    
    // Increment
    auto cmd = make_command("INC");
    auto result = sm.apply(cmd, 1);
    BOOST_CHECK_EQUAL(sm.get_value(), 1);
    BOOST_CHECK_EQUAL(parse_result(result), 1);
    
    // Increment again
    result = sm.apply(cmd, 2);
    BOOST_CHECK_EQUAL(sm.get_value(), 2);
    BOOST_CHECK_EQUAL(parse_result(result), 2);
}

BOOST_AUTO_TEST_CASE(test_counter_decrement, * boost::unit_test::timeout(10)) {
    counter_state_machine sm;
    
    // Start with some value
    auto inc_cmd = make_command("INC");
    for (int i = 0; i < 10; ++i) {
        sm.apply(inc_cmd, i + 1);
    }
    BOOST_CHECK_EQUAL(sm.get_value(), 10);
    
    // Decrement
    auto dec_cmd = make_command("DEC");
    auto result = sm.apply(dec_cmd, 11);
    BOOST_CHECK_EQUAL(sm.get_value(), 9);
    BOOST_CHECK_EQUAL(parse_result(result), 9);
    
    // Decrement again
    result = sm.apply(dec_cmd, 12);
    BOOST_CHECK_EQUAL(sm.get_value(), 8);
    BOOST_CHECK_EQUAL(parse_result(result), 8);
}

BOOST_AUTO_TEST_CASE(test_counter_reset, * boost::unit_test::timeout(10)) {
    counter_state_machine sm;
    
    // Set counter to some value
    auto inc_cmd = make_command("INC");
    for (int i = 0; i < 42; ++i) {
        sm.apply(inc_cmd, i + 1);
    }
    BOOST_CHECK_EQUAL(sm.get_value(), 42);
    
    // Reset counter
    auto reset_cmd = make_command("RESET");
    auto result = sm.apply(reset_cmd, 43);
    BOOST_CHECK_EQUAL(sm.get_value(), 0);
    BOOST_CHECK_EQUAL(parse_result(result), 0);
}

BOOST_AUTO_TEST_CASE(test_counter_get, * boost::unit_test::timeout(10)) {
    counter_state_machine sm;
    
    // Set counter to some value
    auto inc_cmd = make_command("INC");
    for (int i = 0; i < 100; ++i) {
        sm.apply(inc_cmd, i + 1);
    }
    BOOST_CHECK_EQUAL(sm.get_value(), 100);
    
    // Get current value without modifying
    auto get_cmd = make_command("GET");
    auto result = sm.apply(get_cmd, 101);
    BOOST_CHECK_EQUAL(parse_result(result), 100);
    BOOST_CHECK_EQUAL(sm.get_value(), 100);
}

BOOST_AUTO_TEST_CASE(test_counter_negative_values, * boost::unit_test::timeout(10)) {
    counter_state_machine sm;
    
    // Decrement from zero
    auto dec_cmd = make_command("DEC");
    for (int i = 0; i < 5; ++i) {
        sm.apply(dec_cmd, i + 1);
    }
    BOOST_CHECK_EQUAL(sm.get_value(), -5);
    
    // Increment back to positive
    auto inc_cmd = make_command("INC");
    for (int i = 0; i < 10; ++i) {
        sm.apply(inc_cmd, i + 6);
    }
    BOOST_CHECK_EQUAL(sm.get_value(), 5);
}

BOOST_AUTO_TEST_CASE(test_counter_snapshot_round_trip, * boost::unit_test::timeout(10)) {
    counter_state_machine sm1;
    
    // Set counter to some value
    auto inc_cmd = make_command("INC");
    for (int i = 0; i < 42; ++i) {
        sm1.apply(inc_cmd, i + 1);
    }
    auto dec_cmd = make_command("DEC");
    for (int i = 0; i < 7; ++i) {
        sm1.apply(dec_cmd, i + 43);
    }
    
    BOOST_CHECK_EQUAL(sm1.get_value(), 35);
    
    // Create snapshot
    auto snapshot = sm1.get_state();
    
    // Restore to new state machine
    counter_state_machine sm2;
    sm2.restore_from_snapshot(snapshot, 49);
    
    BOOST_CHECK_EQUAL(sm2.get_value(), 35);
}

BOOST_AUTO_TEST_CASE(test_counter_empty_snapshot, * boost::unit_test::timeout(10)) {
    counter_state_machine sm;
    
    // Restore from empty snapshot
    std::vector<std::byte> empty_snapshot;
    sm.restore_from_snapshot(empty_snapshot, 0);
    
    BOOST_CHECK_EQUAL(sm.get_value(), 0);
}

BOOST_AUTO_TEST_CASE(test_counter_invalid_command, * boost::unit_test::timeout(10)) {
    counter_state_machine sm;
    
    // Empty command
    std::vector<std::byte> empty_cmd;
    BOOST_CHECK_THROW(sm.apply(empty_cmd, 1), std::invalid_argument);
    
    // Invalid command type
    auto invalid_cmd = make_command("INVALID");
    BOOST_CHECK_THROW(sm.apply(invalid_cmd, 1), std::invalid_argument);
}

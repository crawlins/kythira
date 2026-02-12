#define BOOST_TEST_MODULE counter_state_machine_test
#include <boost/test/unit_test.hpp>
#include <raft/examples/counter_state_machine.hpp>

using namespace kythira::examples;

BOOST_AUTO_TEST_CASE(test_counter_increment, * boost::unit_test::timeout(10)) {
    counter_state_machine<std::uint64_t> sm;
    
    // Increment by 1 (default)
    auto cmd = counter_state_machine<std::uint64_t>::make_increment_command();
    auto result = sm.apply(cmd, 1);
    BOOST_CHECK_EQUAL(counter_state_machine<std::uint64_t>::parse_result(result), 1);
    BOOST_CHECK_EQUAL(sm.get_counter(), 1);
    
    // Increment by 5
    cmd = counter_state_machine<std::uint64_t>::make_increment_command(5);
    result = sm.apply(cmd, 2);
    BOOST_CHECK_EQUAL(counter_state_machine<std::uint64_t>::parse_result(result), 6);
    BOOST_CHECK_EQUAL(sm.get_counter(), 6);
}

BOOST_AUTO_TEST_CASE(test_counter_decrement, * boost::unit_test::timeout(10)) {
    counter_state_machine<std::uint64_t> sm;
    
    // Start with some value
    auto cmd = counter_state_machine<std::uint64_t>::make_increment_command(10);
    sm.apply(cmd, 1);
    
    // Decrement by 1 (default)
    cmd = counter_state_machine<std::uint64_t>::make_decrement_command();
    auto result = sm.apply(cmd, 2);
    BOOST_CHECK_EQUAL(counter_state_machine<std::uint64_t>::parse_result(result), 9);
    BOOST_CHECK_EQUAL(sm.get_counter(), 9);
    
    // Decrement by 3
    cmd = counter_state_machine<std::uint64_t>::make_decrement_command(3);
    result = sm.apply(cmd, 3);
    BOOST_CHECK_EQUAL(counter_state_machine<std::uint64_t>::parse_result(result), 6);
    BOOST_CHECK_EQUAL(sm.get_counter(), 6);
}

BOOST_AUTO_TEST_CASE(test_counter_reset, * boost::unit_test::timeout(10)) {
    counter_state_machine<std::uint64_t> sm;
    
    // Set counter to some value
    auto cmd = counter_state_machine<std::uint64_t>::make_increment_command(42);
    sm.apply(cmd, 1);
    BOOST_CHECK_EQUAL(sm.get_counter(), 42);
    
    // Reset counter
    cmd = counter_state_machine<std::uint64_t>::make_reset_command();
    auto result = sm.apply(cmd, 2);
    BOOST_CHECK_EQUAL(counter_state_machine<std::uint64_t>::parse_result(result), 0);
    BOOST_CHECK_EQUAL(sm.get_counter(), 0);
}

BOOST_AUTO_TEST_CASE(test_counter_get, * boost::unit_test::timeout(10)) {
    counter_state_machine<std::uint64_t> sm;
    
    // Set counter to some value
    auto cmd = counter_state_machine<std::uint64_t>::make_increment_command(100);
    sm.apply(cmd, 1);
    
    // Get current value without modifying
    cmd = counter_state_machine<std::uint64_t>::make_get_command();
    auto result = sm.apply(cmd, 2);
    BOOST_CHECK_EQUAL(counter_state_machine<std::uint64_t>::parse_result(result), 100);
    BOOST_CHECK_EQUAL(sm.get_counter(), 100);
}

BOOST_AUTO_TEST_CASE(test_counter_negative_values, * boost::unit_test::timeout(10)) {
    counter_state_machine<std::uint64_t> sm;
    
    // Decrement from zero
    auto cmd = counter_state_machine<std::uint64_t>::make_decrement_command(5);
    auto result = sm.apply(cmd, 1);
    BOOST_CHECK_EQUAL(counter_state_machine<std::uint64_t>::parse_result(result), -5);
    BOOST_CHECK_EQUAL(sm.get_counter(), -5);
    
    // Increment back to positive
    cmd = counter_state_machine<std::uint64_t>::make_increment_command(10);
    result = sm.apply(cmd, 2);
    BOOST_CHECK_EQUAL(counter_state_machine<std::uint64_t>::parse_result(result), 5);
    BOOST_CHECK_EQUAL(sm.get_counter(), 5);
}

BOOST_AUTO_TEST_CASE(test_counter_snapshot_round_trip, * boost::unit_test::timeout(10)) {
    counter_state_machine<std::uint64_t> sm1;
    
    // Set counter to some value
    auto cmd = counter_state_machine<std::uint64_t>::make_increment_command(42);
    sm1.apply(cmd, 1);
    cmd = counter_state_machine<std::uint64_t>::make_decrement_command(7);
    sm1.apply(cmd, 2);
    
    BOOST_CHECK_EQUAL(sm1.get_counter(), 35);
    BOOST_CHECK_EQUAL(sm1.get_last_applied_index(), 2);
    
    // Create snapshot
    auto snapshot = sm1.get_state();
    
    // Restore to new state machine
    counter_state_machine<std::uint64_t> sm2;
    sm2.restore_from_snapshot(snapshot, 2);
    
    BOOST_CHECK_EQUAL(sm2.get_counter(), 35);
    BOOST_CHECK_EQUAL(sm2.get_last_applied_index(), 2);
}

BOOST_AUTO_TEST_CASE(test_counter_empty_snapshot, * boost::unit_test::timeout(10)) {
    counter_state_machine<std::uint64_t> sm;
    
    // Restore from empty snapshot
    std::vector<std::byte> empty_snapshot;
    sm.restore_from_snapshot(empty_snapshot, 0);
    
    BOOST_CHECK_EQUAL(sm.get_counter(), 0);
    BOOST_CHECK_EQUAL(sm.get_last_applied_index(), 0);
}

BOOST_AUTO_TEST_CASE(test_counter_last_applied_index, * boost::unit_test::timeout(10)) {
    counter_state_machine<std::uint64_t> sm;
    
    BOOST_CHECK_EQUAL(sm.get_last_applied_index(), 0);
    
    auto cmd = counter_state_machine<std::uint64_t>::make_increment_command();
    sm.apply(cmd, 5);
    BOOST_CHECK_EQUAL(sm.get_last_applied_index(), 5);
    
    cmd = counter_state_machine<std::uint64_t>::make_increment_command();
    sm.apply(cmd, 10);
    BOOST_CHECK_EQUAL(sm.get_last_applied_index(), 10);
}

BOOST_AUTO_TEST_CASE(test_counter_invalid_command, * boost::unit_test::timeout(10)) {
    counter_state_machine<std::uint64_t> sm;
    
    // Empty command
    std::vector<std::byte> empty_cmd;
    BOOST_CHECK_THROW(sm.apply(empty_cmd, 1), std::invalid_argument);
    
    // Invalid command type
    std::vector<std::byte> invalid_cmd = {static_cast<std::byte>(99)};
    BOOST_CHECK_THROW(sm.apply(invalid_cmd, 1), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(test_counter_invalid_snapshot, * boost::unit_test::timeout(10)) {
    counter_state_machine<std::uint64_t> sm;
    
    // Invalid snapshot size
    std::vector<std::byte> invalid_snapshot = {std::byte{1}, std::byte{2}, std::byte{3}};
    BOOST_CHECK_THROW(sm.restore_from_snapshot(invalid_snapshot, 1), std::invalid_argument);
}

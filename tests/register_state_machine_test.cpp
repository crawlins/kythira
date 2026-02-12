#define BOOST_TEST_MODULE register_state_machine_test
#include <boost/test/unit_test.hpp>
#include <raft/examples/register_state_machine.hpp>

using namespace kythira::examples;

BOOST_AUTO_TEST_CASE(test_register_initial_state, * boost::unit_test::timeout(10)) {
    register_state_machine<std::uint64_t> sm;
    
    BOOST_CHECK_EQUAL(sm.get_value(), "");
    BOOST_CHECK_EQUAL(sm.get_version(), 0);
    BOOST_CHECK_EQUAL(sm.get_last_applied_index(), 0);
}

BOOST_AUTO_TEST_CASE(test_register_write, * boost::unit_test::timeout(10)) {
    register_state_machine<std::uint64_t> sm;
    
    auto cmd = register_state_machine<std::uint64_t>::make_write_command("hello");
    auto result = sm.apply(cmd, 1);
    
    auto parsed = register_state_machine<std::uint64_t>::parse_result(result);
    BOOST_CHECK_EQUAL(parsed.value, "hello");
    BOOST_CHECK_EQUAL(parsed.version, 1);
    BOOST_CHECK_EQUAL(sm.get_value(), "hello");
    BOOST_CHECK_EQUAL(sm.get_version(), 1);
}

BOOST_AUTO_TEST_CASE(test_register_read, * boost::unit_test::timeout(10)) {
    register_state_machine<std::uint64_t> sm;
    
    // Write a value
    auto cmd = register_state_machine<std::uint64_t>::make_write_command("test");
    sm.apply(cmd, 1);
    
    // Read the value
    cmd = register_state_machine<std::uint64_t>::make_read_command();
    auto result = sm.apply(cmd, 2);
    
    auto parsed = register_state_machine<std::uint64_t>::parse_result(result);
    BOOST_CHECK_EQUAL(parsed.value, "test");
    BOOST_CHECK_EQUAL(parsed.version, 1); // Version doesn't change on read
}

BOOST_AUTO_TEST_CASE(test_register_multiple_writes, * boost::unit_test::timeout(10)) {
    register_state_machine<std::uint64_t> sm;
    
    auto cmd = register_state_machine<std::uint64_t>::make_write_command("value1");
    sm.apply(cmd, 1);
    BOOST_CHECK_EQUAL(sm.get_version(), 1);
    
    cmd = register_state_machine<std::uint64_t>::make_write_command("value2");
    sm.apply(cmd, 2);
    BOOST_CHECK_EQUAL(sm.get_version(), 2);
    
    cmd = register_state_machine<std::uint64_t>::make_write_command("value3");
    sm.apply(cmd, 3);
    BOOST_CHECK_EQUAL(sm.get_version(), 3);
    
    BOOST_CHECK_EQUAL(sm.get_value(), "value3");
}

BOOST_AUTO_TEST_CASE(test_register_cas_success, * boost::unit_test::timeout(10)) {
    register_state_machine<std::uint64_t> sm;
    
    // Write initial value
    auto cmd = register_state_machine<std::uint64_t>::make_write_command("old");
    sm.apply(cmd, 1);
    BOOST_CHECK_EQUAL(sm.get_version(), 1);
    
    // CAS with correct expected value
    cmd = register_state_machine<std::uint64_t>::make_cas_command("old", "new");
    auto result = sm.apply(cmd, 2);
    
    auto parsed = register_state_machine<std::uint64_t>::parse_result(result);
    BOOST_CHECK_EQUAL(parsed.value, "new");
    BOOST_CHECK_EQUAL(parsed.version, 2);
    BOOST_CHECK_EQUAL(sm.get_value(), "new");
    BOOST_CHECK_EQUAL(sm.get_version(), 2);
}

BOOST_AUTO_TEST_CASE(test_register_cas_failure, * boost::unit_test::timeout(10)) {
    register_state_machine<std::uint64_t> sm;
    
    // Write initial value
    auto cmd = register_state_machine<std::uint64_t>::make_write_command("current");
    sm.apply(cmd, 1);
    BOOST_CHECK_EQUAL(sm.get_version(), 1);
    
    // CAS with incorrect expected value
    cmd = register_state_machine<std::uint64_t>::make_cas_command("wrong", "new");
    auto result = sm.apply(cmd, 2);
    
    auto parsed = register_state_machine<std::uint64_t>::parse_result(result);
    BOOST_CHECK_EQUAL(parsed.value, "current"); // Value unchanged
    BOOST_CHECK_EQUAL(parsed.version, 1); // Version unchanged
    BOOST_CHECK_EQUAL(sm.get_value(), "current");
    BOOST_CHECK_EQUAL(sm.get_version(), 1);
}

BOOST_AUTO_TEST_CASE(test_register_cas_empty_to_value, * boost::unit_test::timeout(10)) {
    register_state_machine<std::uint64_t> sm;
    
    // CAS from empty to value
    auto cmd = register_state_machine<std::uint64_t>::make_cas_command("", "first");
    auto result = sm.apply(cmd, 1);
    
    auto parsed = register_state_machine<std::uint64_t>::parse_result(result);
    BOOST_CHECK_EQUAL(parsed.value, "first");
    BOOST_CHECK_EQUAL(parsed.version, 1);
}

BOOST_AUTO_TEST_CASE(test_register_snapshot_round_trip, * boost::unit_test::timeout(10)) {
    register_state_machine<std::uint64_t> sm1;
    
    // Write some values
    auto cmd = register_state_machine<std::uint64_t>::make_write_command("value1");
    sm1.apply(cmd, 1);
    cmd = register_state_machine<std::uint64_t>::make_write_command("value2");
    sm1.apply(cmd, 2);
    cmd = register_state_machine<std::uint64_t>::make_write_command("final");
    sm1.apply(cmd, 3);
    
    BOOST_CHECK_EQUAL(sm1.get_value(), "final");
    BOOST_CHECK_EQUAL(sm1.get_version(), 3);
    
    // Create snapshot
    auto snapshot = sm1.get_state();
    
    // Restore to new state machine
    register_state_machine<std::uint64_t> sm2;
    sm2.restore_from_snapshot(snapshot, 3);
    
    BOOST_CHECK_EQUAL(sm2.get_value(), "final");
    BOOST_CHECK_EQUAL(sm2.get_version(), 3);
    BOOST_CHECK_EQUAL(sm2.get_last_applied_index(), 3);
}

BOOST_AUTO_TEST_CASE(test_register_empty_snapshot, * boost::unit_test::timeout(10)) {
    register_state_machine<std::uint64_t> sm;
    
    // Restore from empty snapshot
    std::vector<std::byte> empty_snapshot;
    sm.restore_from_snapshot(empty_snapshot, 0);
    
    BOOST_CHECK_EQUAL(sm.get_value(), "");
    BOOST_CHECK_EQUAL(sm.get_version(), 0);
}

BOOST_AUTO_TEST_CASE(test_register_version_tracking, * boost::unit_test::timeout(10)) {
    register_state_machine<std::uint64_t> sm;
    
    // Version starts at 0
    BOOST_CHECK_EQUAL(sm.get_version(), 0);
    
    // Write increments version
    auto cmd = register_state_machine<std::uint64_t>::make_write_command("v1");
    sm.apply(cmd, 1);
    BOOST_CHECK_EQUAL(sm.get_version(), 1);
    
    // Read doesn't increment version
    cmd = register_state_machine<std::uint64_t>::make_read_command();
    sm.apply(cmd, 2);
    BOOST_CHECK_EQUAL(sm.get_version(), 1);
    
    // Successful CAS increments version
    cmd = register_state_machine<std::uint64_t>::make_cas_command("v1", "v2");
    sm.apply(cmd, 3);
    BOOST_CHECK_EQUAL(sm.get_version(), 2);
    
    // Failed CAS doesn't increment version
    cmd = register_state_machine<std::uint64_t>::make_cas_command("wrong", "v3");
    sm.apply(cmd, 4);
    BOOST_CHECK_EQUAL(sm.get_version(), 2);
}

BOOST_AUTO_TEST_CASE(test_register_concurrent_access_simulation, * boost::unit_test::timeout(10)) {
    register_state_machine<std::uint64_t> sm;
    
    // Simulate concurrent writes (applied in order by Raft)
    auto cmd1 = register_state_machine<std::uint64_t>::make_write_command("client1");
    auto cmd2 = register_state_machine<std::uint64_t>::make_write_command("client2");
    auto cmd3 = register_state_machine<std::uint64_t>::make_write_command("client3");
    
    sm.apply(cmd1, 1);
    sm.apply(cmd2, 2);
    sm.apply(cmd3, 3);
    
    // Last write wins (linearizable)
    BOOST_CHECK_EQUAL(sm.get_value(), "client3");
    BOOST_CHECK_EQUAL(sm.get_version(), 3);
}

BOOST_AUTO_TEST_CASE(test_register_invalid_command, * boost::unit_test::timeout(10)) {
    register_state_machine<std::uint64_t> sm;
    
    // Empty command
    std::vector<std::byte> empty_cmd;
    BOOST_CHECK_THROW(sm.apply(empty_cmd, 1), std::invalid_argument);
    
    // Invalid command type
    std::vector<std::byte> invalid_cmd = {static_cast<std::byte>(99)};
    BOOST_CHECK_THROW(sm.apply(invalid_cmd, 1), std::invalid_argument);
}

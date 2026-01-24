#define BOOST_TEST_MODULE raft_state_machine_concept_test
#include <boost/test/unit_test.hpp>

#include <raft/types.hpp>
#include <raft/test_state_machine.hpp>
#include <string>
#include <vector>
#include <cstddef>

namespace {
    constexpr std::uint64_t test_index_1 = 1;
    constexpr std::uint64_t test_index_2 = 2;
    constexpr std::uint64_t test_index_3 = 3;
    constexpr std::uint64_t test_index_10 = 10;
    constexpr std::uint64_t test_index_100 = 100;
    
    constexpr const char* test_key_foo = "foo";
    constexpr const char* test_key_bar = "bar";
    constexpr const char* test_key_baz = "baz";
    constexpr const char* test_key_missing = "missing";
    
    constexpr const char* test_value_hello = "hello";
    constexpr const char* test_value_world = "world";
    constexpr const char* test_value_test = "test";
    constexpr const char* test_value_updated = "updated";
}

// Test suite for state machine concept validation
BOOST_AUTO_TEST_SUITE(state_machine_concept_validation)

// Test 1: Concept satisfaction
// Validates: Requirements 1.1, 7.4, 10.1-10.4, 15.2, 19.1-19.5, 31.1-31.2
BOOST_AUTO_TEST_CASE(test_concept_satisfaction, * boost::unit_test::timeout(30)) {
    // Verify that test_key_value_state_machine satisfies the state_machine concept
    static_assert(kythira::state_machine<kythira::test_key_value_state_machine<std::uint64_t>, std::uint64_t>,
                  "test_key_value_state_machine must satisfy state_machine concept");
    
    // Create an instance to verify it compiles
    kythira::test_key_value_state_machine<std::uint64_t> sm;
    
    BOOST_CHECK(true); // If we get here, concept is satisfied
}

// Test 2: Apply method signature
// Validates: Requirements 7.4, 15.2, 19.1-19.5
BOOST_AUTO_TEST_CASE(test_apply_method_signature, * boost::unit_test::timeout(30)) {
    kythira::test_key_value_state_machine<std::uint64_t> sm;
    
    // Create a simple PUT command
    auto command = kythira::test_key_value_state_machine<>::make_put_command(test_key_foo, test_value_hello);
    
    // Apply should return std::vector<std::byte>
    auto result = sm.apply(command, test_index_1);
    
    // Verify return type
    static_assert(std::is_same_v<decltype(result), std::vector<std::byte>>,
                  "apply must return std::vector<std::byte>");
    
    BOOST_CHECK(sm.contains(test_key_foo));
    BOOST_CHECK_EQUAL(sm.get_value(test_key_foo).value(), test_value_hello);
}

// Test 3: Get state method signature
// Validates: Requirements 10.1-10.4, 31.1-31.2
BOOST_AUTO_TEST_CASE(test_get_state_method_signature, * boost::unit_test::timeout(30)) {
    kythira::test_key_value_state_machine<std::uint64_t> sm;
    
    // Add some data
    auto put_cmd = kythira::test_key_value_state_machine<>::make_put_command(test_key_foo, test_value_hello);
    sm.apply(put_cmd, test_index_1);
    
    // Get state should return std::vector<std::byte>
    auto state = sm.get_state();
    
    // Verify return type
    static_assert(std::is_same_v<decltype(state), std::vector<std::byte>>,
                  "get_state must return std::vector<std::byte>");
    
    BOOST_CHECK(!state.empty());
}

// Test 4: Restore from snapshot method signature
// Validates: Requirements 10.1-10.4, 31.1-31.2
BOOST_AUTO_TEST_CASE(test_restore_from_snapshot_method_signature, * boost::unit_test::timeout(30)) {
    kythira::test_key_value_state_machine<std::uint64_t> sm1;
    kythira::test_key_value_state_machine<std::uint64_t> sm2;
    
    // Add data to sm1
    auto put_cmd = kythira::test_key_value_state_machine<>::make_put_command(test_key_foo, test_value_hello);
    sm1.apply(put_cmd, test_index_1);
    
    // Get snapshot
    auto snapshot = sm1.get_state();
    
    // Restore sm2 from snapshot - should return void
    sm2.restore_from_snapshot(snapshot, test_index_1);
    
    // Verify return type is void
    static_assert(std::is_same_v<decltype(sm2.restore_from_snapshot(snapshot, test_index_1)), void>,
                  "restore_from_snapshot must return void");
    
    BOOST_CHECK(sm2.contains(test_key_foo));
    BOOST_CHECK_EQUAL(sm2.get_value(test_key_foo).value(), test_value_hello);
}

BOOST_AUTO_TEST_SUITE_END()

// Test suite for state machine functionality
BOOST_AUTO_TEST_SUITE(state_machine_functionality)

// Test 5: Basic PUT operation
// Validates: Requirements 7.4, 19.1-19.5
BOOST_AUTO_TEST_CASE(test_basic_put_operation, * boost::unit_test::timeout(30)) {
    kythira::test_key_value_state_machine<std::uint64_t> sm;
    
    auto put_cmd = kythira::test_key_value_state_machine<>::make_put_command(test_key_foo, test_value_hello);
    auto result = sm.apply(put_cmd, test_index_1);
    
    BOOST_CHECK(result.empty()); // PUT returns empty
    BOOST_CHECK(sm.contains(test_key_foo));
    BOOST_CHECK_EQUAL(sm.get_value(test_key_foo).value(), test_value_hello);
    BOOST_CHECK_EQUAL(sm.get_last_applied_index(), test_index_1);
}

// Test 6: Basic GET operation
// Validates: Requirements 7.4, 19.1-19.5
BOOST_AUTO_TEST_CASE(test_basic_get_operation, * boost::unit_test::timeout(30)) {
    kythira::test_key_value_state_machine<std::uint64_t> sm;
    
    // First PUT a value
    auto put_cmd = kythira::test_key_value_state_machine<>::make_put_command(test_key_foo, test_value_hello);
    sm.apply(put_cmd, test_index_1);
    
    // Then GET it
    auto get_cmd = kythira::test_key_value_state_machine<>::make_get_command(test_key_foo);
    auto result = sm.apply(get_cmd, test_index_2);
    
    BOOST_CHECK(!result.empty());
    std::string value(reinterpret_cast<const char*>(result.data()), result.size());
    BOOST_CHECK_EQUAL(value, test_value_hello);
    BOOST_CHECK_EQUAL(sm.get_last_applied_index(), test_index_2);
}

// Test 7: GET missing key
// Validates: Requirements 7.4, 19.1-19.5
BOOST_AUTO_TEST_CASE(test_get_missing_key, * boost::unit_test::timeout(30)) {
    kythira::test_key_value_state_machine<std::uint64_t> sm;
    
    auto get_cmd = kythira::test_key_value_state_machine<>::make_get_command(test_key_missing);
    auto result = sm.apply(get_cmd, test_index_1);
    
    BOOST_CHECK(result.empty()); // Missing key returns empty
}

// Test 8: Basic DEL operation
// Validates: Requirements 7.4, 19.1-19.5
BOOST_AUTO_TEST_CASE(test_basic_del_operation, * boost::unit_test::timeout(30)) {
    kythira::test_key_value_state_machine<std::uint64_t> sm;
    
    // First PUT a value
    auto put_cmd = kythira::test_key_value_state_machine<>::make_put_command(test_key_foo, test_value_hello);
    sm.apply(put_cmd, test_index_1);
    BOOST_CHECK(sm.contains(test_key_foo));
    
    // Then DEL it
    auto del_cmd = kythira::test_key_value_state_machine<>::make_del_command(test_key_foo);
    auto result = sm.apply(del_cmd, test_index_2);
    
    BOOST_CHECK(result.empty()); // DEL returns empty
    BOOST_CHECK(!sm.contains(test_key_foo));
    BOOST_CHECK_EQUAL(sm.get_last_applied_index(), test_index_2);
}

// Test 9: Multiple operations
// Validates: Requirements 7.4, 19.1-19.5
BOOST_AUTO_TEST_CASE(test_multiple_operations, * boost::unit_test::timeout(30)) {
    kythira::test_key_value_state_machine<std::uint64_t> sm;
    
    // PUT multiple keys
    auto put1 = kythira::test_key_value_state_machine<>::make_put_command(test_key_foo, test_value_hello);
    sm.apply(put1, test_index_1);
    
    auto put2 = kythira::test_key_value_state_machine<>::make_put_command(test_key_bar, test_value_world);
    sm.apply(put2, test_index_2);
    
    auto put3 = kythira::test_key_value_state_machine<>::make_put_command(test_key_baz, test_value_test);
    sm.apply(put3, test_index_3);
    
    BOOST_CHECK_EQUAL(sm.size(), 3);
    BOOST_CHECK(sm.contains(test_key_foo));
    BOOST_CHECK(sm.contains(test_key_bar));
    BOOST_CHECK(sm.contains(test_key_baz));
    BOOST_CHECK_EQUAL(sm.get_last_applied_index(), test_index_3);
}

// Test 10: Update existing key
// Validates: Requirements 7.4, 19.1-19.5
BOOST_AUTO_TEST_CASE(test_update_existing_key, * boost::unit_test::timeout(30)) {
    kythira::test_key_value_state_machine<std::uint64_t> sm;
    
    // PUT initial value
    auto put1 = kythira::test_key_value_state_machine<>::make_put_command(test_key_foo, test_value_hello);
    sm.apply(put1, test_index_1);
    BOOST_CHECK_EQUAL(sm.get_value(test_key_foo).value(), test_value_hello);
    
    // PUT updated value
    auto put2 = kythira::test_key_value_state_machine<>::make_put_command(test_key_foo, test_value_updated);
    sm.apply(put2, test_index_2);
    BOOST_CHECK_EQUAL(sm.get_value(test_key_foo).value(), test_value_updated);
    BOOST_CHECK_EQUAL(sm.size(), 1); // Still only one key
}

BOOST_AUTO_TEST_SUITE_END()

// Test suite for snapshot operations
BOOST_AUTO_TEST_SUITE(snapshot_operations)

// Test 11: Snapshot empty state machine
// Validates: Requirements 10.1-10.4, 31.1-31.2
BOOST_AUTO_TEST_CASE(test_snapshot_empty_state, * boost::unit_test::timeout(30)) {
    kythira::test_key_value_state_machine<std::uint64_t> sm;
    
    auto snapshot = sm.get_state();
    
    // Empty state machine should produce a valid snapshot
    BOOST_CHECK(!snapshot.empty()); // Contains at least the entry count (0)
}

// Test 12: Snapshot with data
// Validates: Requirements 10.1-10.4, 31.1-31.2
BOOST_AUTO_TEST_CASE(test_snapshot_with_data, * boost::unit_test::timeout(30)) {
    kythira::test_key_value_state_machine<std::uint64_t> sm;
    
    // Add some data
    auto put1 = kythira::test_key_value_state_machine<>::make_put_command(test_key_foo, test_value_hello);
    sm.apply(put1, test_index_1);
    
    auto put2 = kythira::test_key_value_state_machine<>::make_put_command(test_key_bar, test_value_world);
    sm.apply(put2, test_index_2);
    
    auto snapshot = sm.get_state();
    
    BOOST_CHECK(!snapshot.empty());
    // Snapshot should contain: entry count + 2 key-value pairs
}

// Test 13: Restore from empty snapshot
// Validates: Requirements 10.1-10.4, 31.1-31.2
BOOST_AUTO_TEST_CASE(test_restore_from_empty_snapshot, * boost::unit_test::timeout(30)) {
    kythira::test_key_value_state_machine<std::uint64_t> sm1;
    kythira::test_key_value_state_machine<std::uint64_t> sm2;
    
    // Get snapshot of empty state machine
    auto snapshot = sm1.get_state();
    
    // Add data to sm2
    auto put = kythira::test_key_value_state_machine<>::make_put_command(test_key_foo, test_value_hello);
    sm2.apply(put, test_index_1);
    BOOST_CHECK_EQUAL(sm2.size(), 1);
    
    // Restore sm2 from empty snapshot
    sm2.restore_from_snapshot(snapshot, test_index_10);
    
    BOOST_CHECK_EQUAL(sm2.size(), 0);
    BOOST_CHECK_EQUAL(sm2.get_last_applied_index(), test_index_10);
}

// Test 14: Snapshot round-trip
// Validates: Requirements 10.1-10.4, 31.1-31.2
BOOST_AUTO_TEST_CASE(test_snapshot_round_trip, * boost::unit_test::timeout(30)) {
    kythira::test_key_value_state_machine<std::uint64_t> sm1;
    kythira::test_key_value_state_machine<std::uint64_t> sm2;
    
    // Add data to sm1
    auto put1 = kythira::test_key_value_state_machine<>::make_put_command(test_key_foo, test_value_hello);
    sm1.apply(put1, test_index_1);
    
    auto put2 = kythira::test_key_value_state_machine<>::make_put_command(test_key_bar, test_value_world);
    sm1.apply(put2, test_index_2);
    
    auto put3 = kythira::test_key_value_state_machine<>::make_put_command(test_key_baz, test_value_test);
    sm1.apply(put3, test_index_3);
    
    // Create snapshot
    auto snapshot = sm1.get_state();
    
    // Restore to sm2
    sm2.restore_from_snapshot(snapshot, test_index_100);
    
    // Verify sm2 has same data as sm1
    BOOST_CHECK_EQUAL(sm2.size(), sm1.size());
    BOOST_CHECK(sm2.contains(test_key_foo));
    BOOST_CHECK(sm2.contains(test_key_bar));
    BOOST_CHECK(sm2.contains(test_key_baz));
    BOOST_CHECK_EQUAL(sm2.get_value(test_key_foo).value(), test_value_hello);
    BOOST_CHECK_EQUAL(sm2.get_value(test_key_bar).value(), test_value_world);
    BOOST_CHECK_EQUAL(sm2.get_value(test_key_baz).value(), test_value_test);
    BOOST_CHECK_EQUAL(sm2.get_last_applied_index(), test_index_100);
}

// Test 15: Restore clears existing state
// Validates: Requirements 10.1-10.4, 31.1-31.2
BOOST_AUTO_TEST_CASE(test_restore_clears_existing_state, * boost::unit_test::timeout(30)) {
    kythira::test_key_value_state_machine<std::uint64_t> sm1;
    kythira::test_key_value_state_machine<std::uint64_t> sm2;
    
    // Add data to sm1
    auto put1 = kythira::test_key_value_state_machine<>::make_put_command(test_key_foo, test_value_hello);
    sm1.apply(put1, test_index_1);
    
    // Add different data to sm2
    auto put2 = kythira::test_key_value_state_machine<>::make_put_command(test_key_bar, test_value_world);
    sm2.apply(put2, test_index_1);
    
    auto put3 = kythira::test_key_value_state_machine<>::make_put_command(test_key_baz, test_value_test);
    sm2.apply(put3, test_index_2);
    
    BOOST_CHECK_EQUAL(sm2.size(), 2);
    
    // Restore sm2 from sm1's snapshot
    auto snapshot = sm1.get_state();
    sm2.restore_from_snapshot(snapshot, test_index_10);
    
    // sm2 should now match sm1, not have its old data
    BOOST_CHECK_EQUAL(sm2.size(), 1);
    BOOST_CHECK(sm2.contains(test_key_foo));
    BOOST_CHECK(!sm2.contains(test_key_bar));
    BOOST_CHECK(!sm2.contains(test_key_baz));
    BOOST_CHECK_EQUAL(sm2.get_value(test_key_foo).value(), test_value_hello);
}

BOOST_AUTO_TEST_SUITE_END()

// Test suite for error handling
BOOST_AUTO_TEST_SUITE(error_handling)

// Test 16: Apply with empty command
// Validates: Requirements 19.4
BOOST_AUTO_TEST_CASE(test_apply_empty_command, * boost::unit_test::timeout(30)) {
    kythira::test_key_value_state_machine<std::uint64_t> sm;
    
    std::vector<std::byte> empty_command;
    
    BOOST_CHECK_THROW(sm.apply(empty_command, test_index_1), std::invalid_argument);
}

// Test 17: Apply with invalid command format
// Validates: Requirements 19.4
BOOST_AUTO_TEST_CASE(test_apply_invalid_command_format, * boost::unit_test::timeout(30)) {
    kythira::test_key_value_state_machine<std::uint64_t> sm;
    
    // Command with only command type, no key
    std::vector<std::byte> invalid_command = {std::byte{1}};
    
    BOOST_CHECK_THROW(sm.apply(invalid_command, test_index_1), std::invalid_argument);
}

// Test 18: Restore from invalid snapshot
// Validates: Requirements 31.4
BOOST_AUTO_TEST_CASE(test_restore_from_invalid_snapshot, * boost::unit_test::timeout(30)) {
    kythira::test_key_value_state_machine<std::uint64_t> sm;
    
    // Invalid snapshot with incomplete data
    std::vector<std::byte> invalid_snapshot = {std::byte{1}, std::byte{2}};
    
    BOOST_CHECK_THROW(sm.restore_from_snapshot(invalid_snapshot, test_index_1), std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

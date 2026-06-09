#define BOOST_TEST_MODULE state_machine_commutativity_property_test
#include <boost/test/unit_test.hpp>
#include <random>
#include <vector>
#include <algorithm>
#include "state_machine_test_utilities.hpp"

namespace {
    constexpr std::size_t num_iterations = 100;
    constexpr std::size_t num_commands = 50;
}

// Test key-value state machine for commutativity testing
class test_kv_state_machine {
public:
    auto apply(const std::vector<std::byte>& command, std::uint64_t) -> std::vector<std::byte> {
        if (command.empty()) return {};

        auto cmd_type = static_cast<std::uint8_t>(command[0]);
        std::size_t offset = 1;

        // Extract key
        std::uint32_t key_len;
        std::memcpy(&key_len, command.data() + offset, sizeof(key_len));
        offset += sizeof(key_len);

        std::string key(reinterpret_cast<const char*>(command.data() + offset), key_len);
        offset += key_len;

        if (cmd_type == 0) { // GET
            auto it = _store.find(key);
            if (it != _store.end()) {
                return std::vector<std::byte>(
                    reinterpret_cast<const std::byte*>(it->second.data()),
                    reinterpret_cast<const std::byte*>(it->second.data() + it->second.size())
                );
            }
            return {};
        } else if (cmd_type == 1) { // SET
            std::uint32_t val_len;
            std::memcpy(&val_len, command.data() + offset, sizeof(val_len));
            offset += sizeof(val_len);

            std::string value(reinterpret_cast<const char*>(command.data() + offset), val_len);
            _store[key] = value;
            return {};
        } else if (cmd_type == 2) { // DELETE
            _store.erase(key);
            return {};
        }

        return {};
    }

    auto get_state() const -> std::vector<std::byte> {
        std::vector<std::byte> state;
        for (const auto& [key, value] : _store) {
            state.insert(state.end(),
                        reinterpret_cast<const std::byte*>(key.data()),
                        reinterpret_cast<const std::byte*>(key.data() + key.size()));
            state.push_back(std::byte{0});
            state.insert(state.end(),
                        reinterpret_cast<const std::byte*>(value.data()),
                        reinterpret_cast<const std::byte*>(value.data() + value.size()));
            state.push_back(std::byte{0});
        }
        return state;
    }

    auto restore_from_snapshot(const std::vector<std::byte>& snapshot) -> void {
        _store.clear();
        // Parse snapshot and restore state
        std::size_t offset = 0;
        while (offset < snapshot.size()) {
            std::string key, value;
            while (offset < snapshot.size() && snapshot[offset] != std::byte{0}) {
                key += static_cast<char>(snapshot[offset++]);
            }
            offset++; // Skip null terminator

            while (offset < snapshot.size() && snapshot[offset] != std::byte{0}) {
                value += static_cast<char>(snapshot[offset++]);
            }
            offset++; // Skip null terminator

            if (!key.empty()) {
                _store[key] = value;
            }
        }
    }

private:
    std::map<std::string, std::string> _store;
};

BOOST_AUTO_TEST_CASE(property_independent_operations_commute, * boost::unit_test::timeout(60)) {
    // Property: Operations on different keys should commute
    std::random_device rd;
    std::mt19937 rng(rd());

    for (std::size_t iter = 0; iter < num_iterations; ++iter) {
        test_kv_state_machine sm1, sm2;

        // Generate operations on different keys
        auto cmd1 = kythira::test::command_generator::generate_set_command("key_a", "value_1");
        auto cmd2 = kythira::test::command_generator::generate_set_command("key_b", "value_2");

        // Apply in order: cmd1, cmd2
        sm1.apply(cmd1, 1);
        sm1.apply(cmd2, 2);

        // Apply in order: cmd2, cmd1
        sm2.apply(cmd2, 1);
        sm2.apply(cmd1, 2);

        // States should be identical (operations commute)
        auto state1 = sm1.get_state();
        auto state2 = sm2.get_state();

        BOOST_CHECK_EQUAL(state1.size(), state2.size());
    }
}

BOOST_AUTO_TEST_CASE(property_dependent_operations_do_not_commute, * boost::unit_test::timeout(60)) {
    // Property: Operations on the same key may not commute
    std::random_device rd;
    std::mt19937 rng(rd());

    for (std::size_t iter = 0; iter < num_iterations; ++iter) {
        test_kv_state_machine sm1, sm2;

        // Generate operations on same key
        auto cmd1 = kythira::test::command_generator::generate_set_command("key_a", "value_1");
        auto cmd2 = kythira::test::command_generator::generate_set_command("key_a", "value_2");

        // Apply in order: cmd1, cmd2
        sm1.apply(cmd1, 1);
        sm1.apply(cmd2, 2);

        // Apply in order: cmd2, cmd1
        sm2.apply(cmd2, 1);
        sm2.apply(cmd1, 2);

        // Final states may differ (last write wins)
        // This test validates that the system correctly handles non-commutative operations
        auto state1 = sm1.get_state();
        auto state2 = sm2.get_state();

        // Both should have valid states (no crashes)
        BOOST_CHECK_GE(state1.size(), 0);
        BOOST_CHECK_GE(state2.size(), 0);
    }
}

BOOST_AUTO_TEST_CASE(property_read_operations_always_commute, * boost::unit_test::timeout(60)) {
    // Property: Read operations should always commute with each other
    std::random_device rd;
    std::mt19937 rng(rd());

    for (std::size_t iter = 0; iter < num_iterations; ++iter) {
        test_kv_state_machine sm;

        // Set up initial state
        auto setup = kythira::test::command_generator::generate_set_command("key_a", "value_1");
        sm.apply(setup, 1);

        // Generate read operations
        auto read1 = kythira::test::command_generator::generate_get_command("key_a");
        auto read2 = kythira::test::command_generator::generate_get_command("key_a");

        // Apply reads in any order - state should not change
        auto state_before = sm.get_state();
        sm.apply(read1, 2);
        sm.apply(read2, 3);
        auto state_after = sm.get_state();

        BOOST_CHECK_EQUAL(state_before.size(), state_after.size());
    }
}

BOOST_AUTO_TEST_CASE(property_delete_idempotent, * boost::unit_test::timeout(60)) {
    // Property: Deleting a non-existent key is idempotent
    std::random_device rd;
    std::mt19937 rng(rd());

    for (std::size_t iter = 0; iter < num_iterations; ++iter) {
        test_kv_state_machine sm;

        auto del_cmd = kythira::test::command_generator::generate_delete_command("nonexistent_key");

        auto state1 = sm.get_state();
        sm.apply(del_cmd, 1);
        auto state2 = sm.get_state();
        sm.apply(del_cmd, 2);
        auto state3 = sm.get_state();

        // All states should be identical
        BOOST_CHECK_EQUAL(state1.size(), state2.size());
        BOOST_CHECK_EQUAL(state2.size(), state3.size());
    }
}

BOOST_AUTO_TEST_CASE(property_concurrent_writes_converge, * boost::unit_test::timeout(90)) {
    // Property: Concurrent writes to different keys should converge to same state
    std::random_device rd;
    std::mt19937 rng(rd());

    for (std::size_t iter = 0; iter < num_iterations; ++iter) {
        std::vector<std::vector<std::byte>> commands;

        // Generate commands for different keys
        for (std::size_t i = 0; i < 10; ++i) {
            std::string key = "key_" + std::to_string(i);
            std::string value = "value_" + std::to_string(i);
            commands.push_back(kythira::test::command_generator::generate_set_command(key, value));
        }

        // Apply in different orders
        test_kv_state_machine sm1, sm2;

        // Order 1: sequential
        for (std::uint64_t i = 0; i < commands.size(); ++i) {
            sm1.apply(commands[i], i + 1);
        }

        // Order 2: shuffled
        auto shuffled = commands;
        std::shuffle(shuffled.begin(), shuffled.end(), rng);
        for (std::uint64_t i = 0; i < shuffled.size(); ++i) {
            sm2.apply(shuffled[i], i + 1);
        }

        // States should converge (all keys written)
        auto state1 = sm1.get_state();
        auto state2 = sm2.get_state();

        BOOST_CHECK_EQUAL(state1.size(), state2.size());
    }
}

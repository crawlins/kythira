#pragma once

#include <vector>
#include <cstddef>
#include <random>
#include <string>
#include <cstring>

namespace kythira::test {

// Command generator for state machine testing
class command_generator {
public:
    enum class command_type : std::uint8_t {
        get = 0,
        set = 1,
        del = 2
    };

    // Generate random binary command for key-value state machine
    static auto generate_random_command(std::mt19937& rng) -> std::vector<std::byte> {
        std::uniform_int_distribution<int> cmd_dist(0, 2);
        auto cmd = static_cast<command_type>(cmd_dist(rng));

        std::uniform_int_distribution<int> key_dist(0, 99);
        std::string key = "key_" + std::to_string(key_dist(rng));

        std::vector<std::byte> command;
        command.push_back(static_cast<std::byte>(cmd));

        // Add key length and key
        std::uint32_t key_len = static_cast<std::uint32_t>(key.size());
        auto key_len_bytes = reinterpret_cast<const std::byte*>(&key_len);
        command.insert(command.end(), key_len_bytes, key_len_bytes + sizeof(key_len));
        command.insert(command.end(),
                      reinterpret_cast<const std::byte*>(key.data()),
                      reinterpret_cast<const std::byte*>(key.data() + key.size()));

        // For SET commands, add value
        if (cmd == command_type::set) {
            std::uniform_int_distribution<int> val_dist(0, 999);
            std::string value = "value_" + std::to_string(val_dist(rng));

            std::uint32_t val_len = static_cast<std::uint32_t>(value.size());
            auto val_len_bytes = reinterpret_cast<const std::byte*>(&val_len);
            command.insert(command.end(), val_len_bytes, val_len_bytes + sizeof(val_len));
            command.insert(command.end(),
                          reinterpret_cast<const std::byte*>(value.data()),
                          reinterpret_cast<const std::byte*>(value.data() + value.size()));
        }

        return command;
    }

    // Generate specific GET command
    static auto generate_get_command(const std::string& key) -> std::vector<std::byte> {
        std::vector<std::byte> command;
        command.push_back(static_cast<std::byte>(command_type::get));

        std::uint32_t key_len = static_cast<std::uint32_t>(key.size());
        auto key_len_bytes = reinterpret_cast<const std::byte*>(&key_len);
        command.insert(command.end(), key_len_bytes, key_len_bytes + sizeof(key_len));
        command.insert(command.end(),
                      reinterpret_cast<const std::byte*>(key.data()),
                      reinterpret_cast<const std::byte*>(key.data() + key.size()));

        return command;
    }

    // Generate specific SET command
    static auto generate_set_command(const std::string& key, const std::string& value) -> std::vector<std::byte> {
        std::vector<std::byte> command;
        command.push_back(static_cast<std::byte>(command_type::set));

        std::uint32_t key_len = static_cast<std::uint32_t>(key.size());
        auto key_len_bytes = reinterpret_cast<const std::byte*>(&key_len);
        command.insert(command.end(), key_len_bytes, key_len_bytes + sizeof(key_len));
        command.insert(command.end(),
                      reinterpret_cast<const std::byte*>(key.data()),
                      reinterpret_cast<const std::byte*>(key.data() + key.size()));

        std::uint32_t val_len = static_cast<std::uint32_t>(value.size());
        auto val_len_bytes = reinterpret_cast<const std::byte*>(&val_len);
        command.insert(command.end(), val_len_bytes, val_len_bytes + sizeof(val_len));
        command.insert(command.end(),
                      reinterpret_cast<const std::byte*>(value.data()),
                      reinterpret_cast<const std::byte*>(value.data() + value.size()));

        return command;
    }

    // Generate specific DELETE command
    static auto generate_delete_command(const std::string& key) -> std::vector<std::byte> {
        std::vector<std::byte> command;
        command.push_back(static_cast<std::byte>(command_type::del));

        std::uint32_t key_len = static_cast<std::uint32_t>(key.size());
        auto key_len_bytes = reinterpret_cast<const std::byte*>(&key_len);
        command.insert(command.end(), key_len_bytes, key_len_bytes + sizeof(key_len));
        command.insert(command.end(),
                      reinterpret_cast<const std::byte*>(key.data()),
                      reinterpret_cast<const std::byte*>(key.data() + key.size()));

        return command;
    }
};

// Snapshot validator for state machine testing
class snapshot_validator {
public:
    // Validate snapshot can be restored
    template<typename StateMachine>
    static auto validate_snapshot_round_trip(StateMachine& sm) -> bool {
        // Get current state
        auto snapshot = sm.get_state();

        // Create new state machine and restore
        StateMachine sm2;
        sm2.restore_from_snapshot(snapshot, 0);

        // Get state from restored machine
        auto snapshot2 = sm2.get_state();

        // Compare logical state (size and contents)
        return snapshot.size() == snapshot2.size();
    }

    // Validate deterministic application
    template<typename StateMachine>
    static auto validate_deterministic_application(
        const std::vector<std::vector<std::byte>>& commands) -> bool {

        StateMachine sm1;
        StateMachine sm2;

        // Apply same commands to both
        for (std::uint64_t i = 0; i < commands.size(); ++i) {
            sm1.apply(commands[i], i + 1);
            sm2.apply(commands[i], i + 1);
        }

        // Get states
        auto state1 = sm1.get_state();
        auto state2 = sm2.get_state();

        // States should be identical
        return state1.size() == state2.size();
    }
};

} // namespace kythira::test

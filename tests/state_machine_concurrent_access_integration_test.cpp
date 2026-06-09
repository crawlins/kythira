#define BOOST_TEST_MODULE state_machine_concurrent_access_integration_test
#include <boost/test/unit_test.hpp>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include "state_machine_test_utilities.hpp"

namespace {
    constexpr std::size_t num_threads = 4;
    constexpr std::size_t operations_per_thread = 100;
}

// Thread-safe test state machine
class concurrent_state_machine {
public:
    auto apply(const std::vector<std::byte>& command, std::uint64_t index) -> std::vector<std::byte> {
        std::unique_lock lock(_mutex);

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
            _operations_applied++;
            return {};
        } else if (cmd_type == 2) { // DELETE
            _store.erase(key);
            _operations_applied++;
            return {};
        }

        return {};
    }

    auto get_state() const -> std::vector<std::byte> {
        std::shared_lock lock(_mutex);
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
        std::unique_lock lock(_mutex);
        _store.clear();

        std::size_t offset = 0;
        while (offset < snapshot.size()) {
            std::string key, value;
            while (offset < snapshot.size() && snapshot[offset] != std::byte{0}) {
                key += static_cast<char>(snapshot[offset++]);
            }
            offset++;

            if (offset >= snapshot.size()) break;

            while (offset < snapshot.size() && snapshot[offset] != std::byte{0}) {
                value += static_cast<char>(snapshot[offset++]);
            }
            offset++;

            if (!key.empty()) {
                _store[key] = value;
            }
        }
    }

    auto get_operations_applied() const -> std::size_t {
        std::shared_lock lock(_mutex);
        return _operations_applied;
    }

private:
    mutable std::shared_mutex _mutex;
    std::map<std::string, std::string> _store;
    std::size_t _operations_applied = 0;
};

BOOST_AUTO_TEST_CASE(test_concurrent_writes, * boost::unit_test::timeout(60)) {
    concurrent_state_machine sm;
    std::vector<std::thread> threads;
    std::atomic<std::uint64_t> index_counter{0};

    // Launch multiple threads writing concurrently
    for (std::size_t t = 0; t < num_threads; ++t) {
        threads.emplace_back([&sm, &index_counter, t]() {
            std::random_device rd;
            std::mt19937 rng(rd() + t);

            for (std::size_t i = 0; i < operations_per_thread; ++i) {
                std::string key = "thread_" + std::to_string(t) + "_key_" + std::to_string(i);
                std::string value = "value_" + std::to_string(i);
                auto cmd = kythira::test::command_generator::generate_set_command(key, value);
                sm.apply(cmd, ++index_counter);
            }
        });
    }

    // Wait for all threads
    for (auto& thread : threads) {
        thread.join();
    }

    // Verify all operations applied
    BOOST_CHECK_EQUAL(sm.get_operations_applied(), num_threads * operations_per_thread);
}

BOOST_AUTO_TEST_CASE(test_concurrent_reads_and_writes, * boost::unit_test::timeout(60)) {
    concurrent_state_machine sm;
    std::atomic<std::uint64_t> index_counter{0};
    std::atomic<bool> stop{false};

    // Pre-populate with some data
    for (std::size_t i = 0; i < 50; ++i) {
        auto cmd = kythira::test::command_generator::generate_set_command(
            "key_" + std::to_string(i), "value_" + std::to_string(i));
        sm.apply(cmd, ++index_counter);
    }

    std::vector<std::thread> threads;

    // Writer threads
    for (std::size_t t = 0; t < num_threads / 2; ++t) {
        threads.emplace_back([&sm, &index_counter, &stop, t]() {
            std::random_device rd;
            std::mt19937 rng(rd() + t);

            while (!stop) {
                auto cmd = kythira::test::command_generator::generate_random_command(rng);
                sm.apply(cmd, ++index_counter);
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
    }

    // Reader threads
    std::atomic<std::size_t> read_count{0};
    for (std::size_t t = 0; t < num_threads / 2; ++t) {
        threads.emplace_back([&sm, &stop, &read_count]() {
            while (!stop) {
                auto state = sm.get_state();
                read_count++;
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
    }

    // Run for a short time
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    stop = true;

    // Wait for all threads
    for (auto& thread : threads) {
        thread.join();
    }

    // Verify reads occurred
    BOOST_CHECK_GT(read_count.load(), 0);
}

BOOST_AUTO_TEST_CASE(test_snapshot_during_concurrent_operations, * boost::unit_test::timeout(60)) {
    concurrent_state_machine sm;
    std::atomic<std::uint64_t> index_counter{0};
    std::atomic<bool> stop{false};

    std::vector<std::thread> threads;

    // Writer threads
    for (std::size_t t = 0; t < num_threads; ++t) {
        threads.emplace_back([&sm, &index_counter, &stop, t]() {
            std::random_device rd;
            std::mt19937 rng(rd() + t);

            while (!stop) {
                auto cmd = kythira::test::command_generator::generate_random_command(rng);
                sm.apply(cmd, ++index_counter);
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
    }

    // Snapshot thread
    std::vector<std::vector<std::byte>> snapshots;
    threads.emplace_back([&sm, &stop, &snapshots]() {
        while (!stop) {
            snapshots.push_back(sm.get_state());
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });

    // Run for a short time
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    stop = true;

    // Wait for all threads
    for (auto& thread : threads) {
        thread.join();
    }

    // Verify snapshots were taken
    BOOST_CHECK_GT(snapshots.size(), 0);

    // Verify snapshots are valid (can be restored)
    for (const auto& snapshot : snapshots) {
        concurrent_state_machine test_sm;
        BOOST_CHECK_NO_THROW(test_sm.restore_from_snapshot(snapshot));
    }
}

BOOST_AUTO_TEST_CASE(test_high_contention_same_keys, * boost::unit_test::timeout(60)) {
    concurrent_state_machine sm;
    std::atomic<std::uint64_t> index_counter{0};
    std::vector<std::thread> threads;

    // All threads write to same keys (high contention)
    for (std::size_t t = 0; t < num_threads; ++t) {
        threads.emplace_back([&sm, &index_counter, t]() {
            for (std::size_t i = 0; i < operations_per_thread; ++i) {
                // Use only 10 keys for high contention
                std::string key = "shared_key_" + std::to_string(i % 10);
                std::string value = "thread_" + std::to_string(t) + "_value_" + std::to_string(i);
                auto cmd = kythira::test::command_generator::generate_set_command(key, value);
                sm.apply(cmd, ++index_counter);
            }
        });
    }

    // Wait for all threads
    for (auto& thread : threads) {
        thread.join();
    }

    // Verify all operations applied
    BOOST_CHECK_EQUAL(sm.get_operations_applied(), num_threads * operations_per_thread);
}

BOOST_AUTO_TEST_CASE(test_mixed_operations_stress, * boost::unit_test::timeout(90)) {
    concurrent_state_machine sm;
    std::atomic<std::uint64_t> index_counter{0};
    std::atomic<bool> stop{false};

    std::vector<std::thread> threads;
    std::atomic<std::size_t> total_operations{0};

    // Mixed operation threads
    for (std::size_t t = 0; t < num_threads; ++t) {
        threads.emplace_back([&sm, &index_counter, &stop, &total_operations, t]() {
            std::random_device rd;
            std::mt19937 rng(rd() + t);
            std::uniform_int_distribution<int> op_dist(0, 2);

            while (!stop) {
                int op_type = op_dist(rng);
                std::string key = "key_" + std::to_string(rng() % 50);

                if (op_type == 0) {
                    // GET
                    auto cmd = kythira::test::command_generator::generate_get_command(key);
                    sm.apply(cmd, ++index_counter);
                } else if (op_type == 1) {
                    // SET
                    std::string value = "value_" + std::to_string(rng());
                    auto cmd = kythira::test::command_generator::generate_set_command(key, value);
                    sm.apply(cmd, ++index_counter);
                } else {
                    // DELETE
                    auto cmd = kythira::test::command_generator::generate_delete_command(key);
                    sm.apply(cmd, ++index_counter);
                }

                total_operations++;
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        });
    }

    // Run for a short time
    std::this_thread::sleep_for(std::chrono::seconds(1));
    stop = true;

    // Wait for all threads
    for (auto& thread : threads) {
        thread.join();
    }

    // Verify operations occurred
    BOOST_CHECK_GT(total_operations.load(), 0);
}

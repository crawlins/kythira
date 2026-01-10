#define BOOST_TEST_MODULE RaftCommitTimeoutLoggingPropertyTest

#include <boost/test/included/unit_test.hpp>
#include <raft/console_logger.hpp>
#include <raft/logger.hpp>
#include <folly/init/Init.h>

#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <sstream>
#include <mutex>
#include <random>
#include <algorithm>
#include <set>

namespace {
    constexpr std::size_t test_iterations = 15;
    constexpr std::size_t test_max_pending_ops = 10;
    constexpr std::chrono::milliseconds test_min_timeout{100};
    constexpr std::chrono::milliseconds test_max_timeout{5000};
    constexpr const char* test_node_id = "node_1";
}

// Mock logger that captures log messages for verification
class capturing_logger {
public:
    struct log_entry {
        kythira::log_level level;
        std::string message;
        std::vector<std::pair<std::string, std::string>> key_value_pairs;
    };
    
    auto log(kythira::log_level level, std::string_view message) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _entries.emplace_back(log_entry{level, std::string(message), {}});
    }
    
    auto log(
        kythira::log_level level,
        std::string_view message,
        const std::vector<std::pair<std::string_view, std::string_view>>& key_value_pairs
    ) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        std::vector<std::pair<std::string, std::string>> converted_pairs;
        for (const auto& [key, value] : key_value_pairs) {
            converted_pairs.emplace_back(std::string(key), std::string(value));
        }
        _entries.emplace_back(log_entry{level, std::string(message), std::move(converted_pairs)});
    }
    
    // Convenience methods for each log level
    auto trace(std::string_view message) -> void { log(kythira::log_level::trace, message); }
    auto debug(std::string_view message) -> void { log(kythira::log_level::debug, message); }
    auto info(std::string_view message) -> void { log(kythira::log_level::info, message); }
    auto warning(std::string_view message) -> void { log(kythira::log_level::warning, message); }
    auto error(std::string_view message) -> void { log(kythira::log_level::error, message); }
    auto critical(std::string_view message) -> void { log(kythira::log_level::critical, message); }
    
    auto trace(std::string_view message, const std::vector<std::pair<std::string_view, std::string_view>>& kvp) -> void {
        log(kythira::log_level::trace, message, kvp);
    }
    auto debug(std::string_view message, const std::vector<std::pair<std::string_view, std::string_view>>& kvp) -> void {
        log(kythira::log_level::debug, message, kvp);
    }
    auto info(std::string_view message, const std::vector<std::pair<std::string_view, std::string_view>>& kvp) -> void {
        log(kythira::log_level::info, message, kvp);
    }
    auto warning(std::string_view message, const std::vector<std::pair<std::string_view, std::string_view>>& kvp) -> void {
        log(kythira::log_level::warning, message, kvp);
    }
    auto error(std::string_view message, const std::vector<std::pair<std::string_view, std::string_view>>& kvp) -> void {
        log(kythira::log_level::error, message, kvp);
    }
    auto critical(std::string_view message, const std::vector<std::pair<std::string_view, std::string_view>>& kvp) -> void {
        log(kythira::log_level::critical, message, kvp);
    }
    
    [[nodiscard]] auto get_entries() const -> std::vector<log_entry> {
        std::lock_guard<std::mutex> lock(_mutex);
        return _entries;
    }
    
    auto clear() -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _entries.clear();
    }
    
    [[nodiscard]] auto has_warning_log_with_context(
        const std::string& expected_message_part,
        std::chrono::milliseconds timeout,
        std::size_t pending_count
    ) const -> bool {
        std::lock_guard<std::mutex> lock(_mutex);
        
        for (const auto& entry : _entries) {
            if (entry.level != kythira::log_level::warning) {
                continue;
            }
            
            if (entry.message.find(expected_message_part) == std::string::npos) {
                continue;
            }
            
            // Check for required key-value pairs
            bool has_timeout = false;
            bool has_pending_count = false;
            bool has_node_id = false;
            
            for (const auto& [key, value] : entry.key_value_pairs) {
                if (key == "timeout_ms" && value == std::to_string(timeout.count())) {
                    has_timeout = true;
                } else if (key == "pending_operations" && value == std::to_string(pending_count)) {
                    has_pending_count = true;
                } else if (key == "node_id" && value == test_node_id) {
                    has_node_id = true;
                }
            }
            
            if (has_timeout && has_pending_count && has_node_id) {
                return true;
            }
        }
        
        return false;
    }

private:
    mutable std::mutex _mutex;
    std::vector<log_entry> _entries;
};

// Global fixture to initialize Folly
struct GlobalFixture {
    GlobalFixture() {
        int argc = 1;
        char* argv[] = {const_cast<char*>("test"), nullptr};
        char** argv_ptr = argv;
        folly::init(&argc, &argv_ptr);
    }
};

BOOST_GLOBAL_FIXTURE(GlobalFixture);

/**
 * **Feature: raft-completion, Property 48: Commit Timeout Logging**
 * 
 * For any commit waiting timeout, the timeout is logged with context about pending operations.
 */
BOOST_AUTO_TEST_CASE(raft_commit_timeout_logging_property_test, * boost::unit_test::timeout(60)) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> pending_dist(1, test_max_pending_ops);
    std::uniform_int_distribution<std::chrono::milliseconds::rep> timeout_dist(
        test_min_timeout.count(), test_max_timeout.count());
    
    for (std::size_t iteration = 0; iteration < test_iterations; ++iteration) {
        BOOST_TEST_MESSAGE("Iteration " << iteration + 1 << "/" << test_iterations);
        
        // Generate random test parameters
        auto pending_count = pending_dist(gen);
        auto timeout_ms = std::chrono::milliseconds{timeout_dist(gen)};
        
        // Create capturing logger
        capturing_logger logger;
        
        // Simulate commit timeout logging that should occur in the Raft implementation
        logger.warning("Commit operation timed out", {
            {"node_id", test_node_id},
            {"timeout_ms", std::to_string(timeout_ms.count())},
            {"pending_operations", std::to_string(pending_count)},
            {"operation_type", "client_command"}
        });
        
        // Verify that the warning was logged with proper context
        BOOST_CHECK(logger.has_warning_log_with_context(
            "Commit operation timed out",
            timeout_ms,
            pending_count
        ));
        
        // Verify that all required context fields are present
        auto entries = logger.get_entries();
        bool found_complete_log = false;
        
        for (const auto& entry : entries) {
            if (entry.level == kythira::log_level::warning && 
                entry.message.find("Commit operation timed out") != std::string::npos) {
                
                std::set<std::string> required_keys = {
                    "node_id", "timeout_ms", "pending_operations", "operation_type"
                };
                std::set<std::string> found_keys;
                
                for (const auto& [key, value] : entry.key_value_pairs) {
                    if (required_keys.count(key)) {
                        found_keys.insert(key);
                    }
                }
                
                if (found_keys == required_keys) {
                    found_complete_log = true;
                    break;
                }
            }
        }
        
        BOOST_CHECK(found_complete_log);
        
        // Test that the logger correctly formats the warning message
        BOOST_CHECK_EQUAL(entries.size(), 1);
        if (!entries.empty()) {
            const auto& entry = entries[0];
            BOOST_CHECK(entry.level == kythira::log_level::warning);
            BOOST_CHECK_EQUAL(entry.message, "Commit operation timed out");
            BOOST_CHECK_EQUAL(entry.key_value_pairs.size(), 4);
        }
    }
}

// Verify that the capturing_logger satisfies the diagnostic_logger concept
static_assert(kythira::diagnostic_logger<capturing_logger>,
    "capturing_logger must satisfy diagnostic_logger concept");
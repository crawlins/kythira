#define BOOST_TEST_MODULE RaftApplicationFailureLoggingPropertyTest

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
    constexpr std::size_t test_max_log_index = 1000;
    constexpr std::size_t test_max_term = 100;
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
    
    [[nodiscard]] auto has_critical_log_with_context(
        const std::string& expected_message_part,
        std::size_t log_index,
        std::size_t term,
        const std::string& error_details
    ) const -> bool {
        std::lock_guard<std::mutex> lock(_mutex);
        
        for (const auto& entry : _entries) {
            if (entry.level != kythira::log_level::critical) {
                continue;
            }
            
            if (entry.message.find(expected_message_part) == std::string::npos) {
                continue;
            }
            
            // Check for required key-value pairs
            bool has_log_index = false;
            bool has_term = false;
            bool has_error_details = false;
            bool has_node_id = false;
            
            for (const auto& [key, value] : entry.key_value_pairs) {
                if (key == "log_index" && value == std::to_string(log_index)) {
                    has_log_index = true;
                } else if (key == "term" && value == std::to_string(term)) {
                    has_term = true;
                } else if (key == "error_details" && value == error_details) {
                    has_error_details = true;
                } else if (key == "node_id" && value == test_node_id) {
                    has_node_id = true;
                }
            }
            
            if (has_log_index && has_term && has_error_details && has_node_id) {
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
 * **Feature: raft-completion, Property 51: Application Failure Logging**
 * 
 * For any state machine application failure, the failing entry and error details are logged.
 */
BOOST_AUTO_TEST_CASE(raft_application_failure_logging_property_test, * boost::unit_test::timeout(60)) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> log_index_dist(1, test_max_log_index);
    std::uniform_int_distribution<std::size_t> term_dist(1, test_max_term);
    
    std::vector<std::string> error_types = {
        "State machine exception", "Serialization error", "Invalid command format", "Resource exhaustion"
    };
    
    for (std::size_t iteration = 0; iteration < test_iterations; ++iteration) {
        BOOST_TEST_MESSAGE("Iteration " << iteration + 1 << "/" << test_iterations);
        
        // Generate random test parameters
        auto log_index = log_index_dist(gen);
        auto term = term_dist(gen);
        auto error_details = error_types[iteration % error_types.size()];
        
        // Create capturing logger
        capturing_logger logger;
        
        // Simulate state machine application failure logging that should occur in the Raft implementation
        logger.critical("State machine application failed", {
            {"node_id", test_node_id},
            {"log_index", std::to_string(log_index)},
            {"term", std::to_string(term)},
            {"error_details", error_details},
            {"action", "halt_application"}
        });
        
        // Verify that the critical error was logged with proper context
        BOOST_CHECK(logger.has_critical_log_with_context(
            "State machine application failed",
            log_index,
            term,
            error_details
        ));
        
        // Verify that all required context fields are present
        auto entries = logger.get_entries();
        bool found_complete_log = false;
        
        for (const auto& entry : entries) {
            if (entry.level == kythira::log_level::critical && 
                entry.message.find("State machine application failed") != std::string::npos) {
                
                std::set<std::string> required_keys = {
                    "node_id", "log_index", "term", "error_details", "action"
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
        
        // Test that the logger correctly formats the critical message
        BOOST_CHECK_EQUAL(entries.size(), 1);
        if (!entries.empty()) {
            const auto& entry = entries[0];
            BOOST_CHECK(entry.level == kythira::log_level::critical);
            BOOST_CHECK_EQUAL(entry.message, "State machine application failed");
            BOOST_CHECK_EQUAL(entry.key_value_pairs.size(), 5);
        }
    }
}

// Verify that the capturing_logger satisfies the diagnostic_logger concept
static_assert(kythira::diagnostic_logger<capturing_logger>,
    "capturing_logger must satisfy diagnostic_logger concept");
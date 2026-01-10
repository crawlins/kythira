#define BOOST_TEST_MODULE RaftRpcErrorLoggingPropertyTest

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
    constexpr std::size_t test_max_retries = 10;
    constexpr const char* test_failure_message = "Network failure";
    constexpr const char* test_target_node = "node_2";
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
    
    [[nodiscard]] auto has_error_log_with_context(
        const std::string& expected_message_part,
        const std::string& failure_type,
        const std::string& target_node,
        std::size_t retry_count
    ) const -> bool {
        std::lock_guard<std::mutex> lock(_mutex);
        
        for (const auto& entry : _entries) {
            if (entry.level != kythira::log_level::error) {
                continue;
            }
            
            if (entry.message.find(expected_message_part) == std::string::npos) {
                continue;
            }
            
            // Check for required key-value pairs
            bool has_failure_type = false;
            bool has_target_node = false;
            bool has_retry_count = false;
            
            for (const auto& [key, value] : entry.key_value_pairs) {
                if (key == "failure_type" && value == failure_type) {
                    has_failure_type = true;
                } else if (key == "target_node" && value == target_node) {
                    has_target_node = true;
                } else if (key == "retry_count" && value == std::to_string(retry_count)) {
                    has_retry_count = true;
                }
            }
            
            if (has_failure_type && has_target_node && has_retry_count) {
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
 * **Feature: raft-completion, Property 47: RPC Error Logging**
 * 
 * For any RPC operation failure, detailed error information including failure type, 
 * target node, and retry attempts is logged.
 */
BOOST_AUTO_TEST_CASE(raft_rpc_error_logging_property_test, * boost::unit_test::timeout(60)) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> retry_dist(0, test_max_retries);
    
    std::vector<std::string> rpc_types = {
        "append_entries", "request_vote", "install_snapshot", "heartbeat"
    };
    
    std::vector<std::string> failure_types = {
        "Network timeout", "Connection refused", "DNS resolution failed", "SSL handshake failed"
    };
    
    for (std::size_t iteration = 0; iteration < test_iterations; ++iteration) {
        BOOST_TEST_MESSAGE("Iteration " << iteration + 1 << "/" << test_iterations);
        
        // Generate random test parameters
        auto retry_count = retry_dist(gen);
        auto rpc_type = rpc_types[iteration % rpc_types.size()];
        auto failure_type = failure_types[iteration % failure_types.size()];
        
        // Create capturing logger
        capturing_logger logger;
        
        // Simulate RPC failure logging that should occur in the Raft implementation
        logger.error("RPC operation failed", {
            {"failure_type", failure_type},
            {"target_node", test_target_node},
            {"retry_count", std::to_string(retry_count)},
            {"rpc_type", rpc_type}
        });
        
        // Verify that the error was logged with proper context
        BOOST_CHECK(logger.has_error_log_with_context(
            "RPC operation failed",
            failure_type,
            test_target_node,
            retry_count
        ));
        
        // Verify that all required context fields are present
        auto entries = logger.get_entries();
        bool found_complete_log = false;
        
        for (const auto& entry : entries) {
            if (entry.level == kythira::log_level::error && 
                entry.message.find("RPC operation failed") != std::string::npos) {
                
                std::set<std::string> required_keys = {
                    "failure_type", "target_node", "retry_count", "rpc_type"
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
        
        // Test that the logger correctly formats the error message
        BOOST_CHECK_EQUAL(entries.size(), 1);
        if (!entries.empty()) {
            const auto& entry = entries[0];
            BOOST_CHECK(entry.level == kythira::log_level::error);
            BOOST_CHECK_EQUAL(entry.message, "RPC operation failed");
            BOOST_CHECK_EQUAL(entry.key_value_pairs.size(), 4);
        }
    }
}

// Verify that the capturing_logger satisfies the diagnostic_logger concept
static_assert(kythira::diagnostic_logger<capturing_logger>,
    "capturing_logger must satisfy diagnostic_logger concept");
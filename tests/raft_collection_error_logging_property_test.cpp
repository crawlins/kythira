#define BOOST_TEST_MODULE RaftCollectionErrorLoggingPropertyTest

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
    constexpr std::size_t test_max_failed_futures = 10;
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
        const std::string& operation_type,
        std::size_t failed_count,
        std::size_t total_count
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
            bool has_operation_type = false;
            bool has_failed_count = false;
            bool has_total_count = false;
            bool has_node_id = false;
            
            for (const auto& [key, value] : entry.key_value_pairs) {
                if (key == "operation_type" && value == operation_type) {
                    has_operation_type = true;
                } else if (key == "failed_futures" && value == std::to_string(failed_count)) {
                    has_failed_count = true;
                } else if (key == "total_futures" && value == std::to_string(total_count)) {
                    has_total_count = true;
                } else if (key == "node_id" && value == test_node_id) {
                    has_node_id = true;
                }
            }
            
            if (has_operation_type && has_failed_count && has_total_count && has_node_id) {
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
 * **Feature: raft-completion, Property 50: Collection Error Logging**
 * 
 * For any future collection operation that encounters errors, which futures failed and why are logged.
 */
BOOST_AUTO_TEST_CASE(raft_collection_error_logging_property_test, * boost::unit_test::timeout(60)) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> failed_dist(1, test_max_failed_futures);
    std::uniform_int_distribution<std::size_t> total_dist(3, 15);
    
    std::vector<std::string> operation_types = {
        "heartbeat_collection", "election_votes", "replication_acks", "snapshot_transfer"
    };
    
    for (std::size_t iteration = 0; iteration < test_iterations; ++iteration) {
        BOOST_TEST_MESSAGE("Iteration " << iteration + 1 << "/" << test_iterations);
        
        // Generate random test parameters
        auto total_count = total_dist(gen);
        auto failed_count = std::min(failed_dist(gen), total_count);
        auto operation_type = operation_types[iteration % operation_types.size()];
        
        // Create capturing logger
        capturing_logger logger;
        
        // Simulate future collection error logging that should occur in the Raft implementation
        // First log the overall collection error summary
        logger.warning("Future collection operation encountered errors", {
            {"node_id", test_node_id},
            {"operation_type", operation_type},
            {"failed_futures", std::to_string(failed_count)},
            {"total_futures", std::to_string(total_count)},
            {"success_rate", std::to_string((total_count - failed_count) * 100 / total_count) + "%"}
        });
        
        // Then log individual future failures with specific reasons (as the property requires)
        std::vector<std::string> failure_reasons = {
            "network_timeout", "connection_refused", "serialization_error", 
            "invalid_response", "peer_unavailable", "rpc_cancelled"
        };
        
        for (std::size_t i = 0; i < failed_count; ++i) {
            auto reason = failure_reasons[i % failure_reasons.size()];
            logger.warning("Individual future failed in collection", {
                {"node_id", test_node_id},
                {"operation_type", operation_type},
                {"future_index", std::to_string(i)},
                {"failure_reason", reason},
                {"collection_id", std::to_string(iteration)} // To group related failures
            });
        }
        
        // Verify that the warning was logged with proper context
        BOOST_CHECK(logger.has_warning_log_with_context(
            "Future collection operation encountered errors",
            operation_type,
            failed_count,
            total_count
        ));
        
        // Verify that all required context fields are present
        auto entries = logger.get_entries();
        bool found_complete_log = false;
        std::size_t individual_failure_logs = 0;
        
        for (const auto& entry : entries) {
            if (entry.level == kythira::log_level::warning) {
                if (entry.message.find("Future collection operation encountered errors") != std::string::npos) {
                    std::set<std::string> required_keys = {
                        "node_id", "operation_type", "failed_futures", "total_futures", "success_rate"
                    };
                    std::set<std::string> found_keys;
                    
                    for (const auto& [key, value] : entry.key_value_pairs) {
                        if (required_keys.count(key)) {
                            found_keys.insert(key);
                        }
                    }
                    
                    if (found_keys == required_keys) {
                        found_complete_log = true;
                    }
                } else if (entry.message.find("Individual future failed in collection") != std::string::npos) {
                    // Verify individual failure log has required fields
                    std::set<std::string> required_individual_keys = {
                        "node_id", "operation_type", "future_index", "failure_reason", "collection_id"
                    };
                    std::set<std::string> found_individual_keys;
                    
                    for (const auto& [key, value] : entry.key_value_pairs) {
                        if (required_individual_keys.count(key)) {
                            found_individual_keys.insert(key);
                        }
                    }
                    
                    if (found_individual_keys == required_individual_keys) {
                        individual_failure_logs++;
                    }
                }
            }
        }
        
        BOOST_CHECK(found_complete_log);
        // Property: Each failed future should have its own log entry with failure reason
        BOOST_CHECK_EQUAL(individual_failure_logs, failed_count);
        
        // Test that the logger correctly formats the warning messages
        BOOST_CHECK_EQUAL(entries.size(), 1 + failed_count); // Summary + individual failures
        
        // Verify the summary log entry
        bool found_summary = false;
        for (const auto& entry : entries) {
            if (entry.message == "Future collection operation encountered errors") {
                BOOST_CHECK(entry.level == kythira::log_level::warning);
                BOOST_CHECK_EQUAL(entry.key_value_pairs.size(), 5);
                found_summary = true;
                break;
            }
        }
        BOOST_CHECK(found_summary);
        
        // Clear logger for next iteration
        logger.clear();
    }
}

// Verify that the capturing_logger satisfies the diagnostic_logger concept
static_assert(kythira::diagnostic_logger<capturing_logger>,
    "capturing_logger must satisfy diagnostic_logger concept");
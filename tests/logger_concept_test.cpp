#include <raft/logger.hpp>
#include <iostream>
#include <format>

// Simple console logger implementation for testing the concept
class console_logger {
public:
    auto log(kythira::log_level level, std::string_view message) -> void {
        std::cout << level_to_string(level) << ": " << message << "\n";
    }
    
    auto log(
        kythira::log_level level,
        std::string_view message,
        const std::vector<std::pair<std::string_view, std::string_view>>& key_value_pairs
    ) -> void {
        std::cout << level_to_string(level) << ": " << message;
        for (const auto& [key, value] : key_value_pairs) {
            std::cout << " [" << key << "=" << value << "]";
        }
        std::cout << "\n";
    }
    
    auto trace(std::string_view message) -> void {
        log(kythira::log_level::trace, message);
    }
    
    auto debug(std::string_view message) -> void {
        log(kythira::log_level::debug, message);
    }
    
    auto info(std::string_view message) -> void {
        log(kythira::log_level::info, message);
    }
    
    auto warning(std::string_view message) -> void {
        log(kythira::log_level::warning, message);
    }
    
    auto error(std::string_view message) -> void {
        log(kythira::log_level::error, message);
    }
    
    auto critical(std::string_view message) -> void {
        log(kythira::log_level::critical, message);
    }

private:
    auto level_to_string(kythira::log_level level) -> std::string_view {
        switch (level) {
            case kythira::log_level::trace: return "TRACE";
            case kythira::log_level::debug: return "DEBUG";
            case kythira::log_level::info: return "INFO";
            case kythira::log_level::warning: return "WARNING";
            case kythira::log_level::error: return "ERROR";
            case kythira::log_level::critical: return "CRITICAL";
        }
        return "UNKNOWN";
    }
};

// Verify that console_logger satisfies the diagnostic_logger concept
static_assert(kythira::diagnostic_logger<console_logger>, 
    "console_logger must satisfy diagnostic_logger concept");

auto main() -> int {
    console_logger logger;
    
    std::cout << "Testing diagnostic_logger concept implementation\n";
    std::cout << std::string(60, '=') << "\n\n";
    
    // Test basic logging
    std::cout << "Test 1: Basic logging methods\n";
    logger.trace("This is a trace message");
    logger.debug("This is a debug message");
    logger.info("This is an info message");
    logger.warning("This is a warning message");
    logger.error("This is an error message");
    logger.critical("This is a critical message");
    std::cout << "  ✓ Basic logging passed\n\n";
    
    // Test structured logging
    std::cout << "Test 2: Structured logging with key-value pairs\n";
    logger.log(
        kythira::log_level::info,
        "Leader election started",
        {
            {"term", "42"},
            {"candidate_id", "node_1"},
            {"timeout_ms", "150"}
        }
    );
    
    logger.log(
        kythira::log_level::warning,
        "Network partition detected",
        {
            {"affected_nodes", "3"},
            {"partition_id", "p1"}
        }
    );
    std::cout << "  ✓ Structured logging passed\n\n";
    
    // Test log level enum
    std::cout << "Test 3: Log level enum values\n";
    static_assert(static_cast<std::uint8_t>(kythira::log_level::trace) == 0);
    static_assert(static_cast<std::uint8_t>(kythira::log_level::debug) == 1);
    static_assert(static_cast<std::uint8_t>(kythira::log_level::info) == 2);
    static_assert(static_cast<std::uint8_t>(kythira::log_level::warning) == 3);
    static_assert(static_cast<std::uint8_t>(kythira::log_level::error) == 4);
    static_assert(static_cast<std::uint8_t>(kythira::log_level::critical) == 5);
    std::cout << "  ✓ Log level enum passed\n\n";
    
    std::cout << std::string(60, '=') << "\n";
    std::cout << "All tests passed!\n";
    
    return 0;
}

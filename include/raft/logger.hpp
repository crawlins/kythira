#pragma once

#include <concepts>
#include <string_view>
#include <vector>
#include <utility>
#include <cstdint>

namespace raft {

// Log severity levels
enum class log_level : std::uint8_t {
    trace,
    debug,
    info,
    warning,
    error,
    critical
};

// Diagnostic logger concept for structured logging
template<typename L>
concept diagnostic_logger = requires(
    L logger,
    log_level level,
    std::string_view message,
    std::vector<std::pair<std::string_view, std::string_view>> key_value_pairs
) {
    // Basic logging with level and message
    { logger.log(level, message) } -> std::same_as<void>;
    
    // Structured logging with key-value pairs
    { logger.log(level, message, key_value_pairs) } -> std::same_as<void>;
    
    // Convenience methods for each log level
    { logger.trace(message) } -> std::same_as<void>;
    { logger.debug(message) } -> std::same_as<void>;
    { logger.info(message) } -> std::same_as<void>;
    { logger.warning(message) } -> std::same_as<void>;
    { logger.error(message) } -> std::same_as<void>;
    { logger.critical(message) } -> std::same_as<void>;
};

} // namespace raft

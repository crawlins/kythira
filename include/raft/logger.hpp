#pragma once

/// @file logger.hpp
/// @brief Logging severity levels and the `diagnostic_logger` concept.

#include <concepts>
#include <string_view>
#include <vector>
#include <utility>
#include <cstdint>

namespace kythira {

/// @brief Log severity levels in increasing order of importance.
enum class log_level : std::uint8_t {
    trace,    ///< Fine-grained execution traces; very high volume.
    debug,    ///< Diagnostic information useful during development.
    info,     ///< Noteworthy normal-operation events.
    warning,  ///< Potentially harmful situations that do not stop execution.
    error,    ///< Errors that are recoverable at the subsystem level.
    critical  ///< Severe errors that may cause the node to stop functioning.
};

/// @brief Concept for a structured diagnostic logger.
///
/// Implementations must support both plain-message and key-value structured logging
/// at each severity level, plus convenience methods for each level.
///
/// @tparam L Concrete logger type.
template<typename L>
concept diagnostic_logger =
    requires(L logger, log_level level, std::string_view message,
             std::vector<std::pair<std::string_view, std::string_view>> key_value_pairs) {
        /// Emit a message at the given severity level.
        { logger.log(level, message) } -> std::same_as<void>;

        /// Emit a structured message with key-value context pairs.
        { logger.log(level, message, key_value_pairs) } -> std::same_as<void>;

        /// Convenience shorthands for each level.
        { logger.trace(message) } -> std::same_as<void>;
        { logger.debug(message) } -> std::same_as<void>;
        { logger.info(message) } -> std::same_as<void>;
        { logger.warning(message) } -> std::same_as<void>;
        { logger.error(message) } -> std::same_as<void>;
        { logger.critical(message) } -> std::same_as<void>;
    };

}  // namespace kythira

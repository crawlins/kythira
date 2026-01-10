#pragma once

#include <raft/logger.hpp>
#include <iostream>
#include <string_view>
#include <vector>
#include <utility>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace kythira {

// Console logger implementation for development and testing
// Provides thread-safe structured logging to stdout/stderr
class console_logger {
public:
    // Constructor with optional minimum log level filter
    explicit console_logger(log_level min_level = log_level::trace)
        : _min_level(min_level) {}
    
    // Move constructor
    console_logger(console_logger&& other) noexcept
        : _min_level(other._min_level) {}
    
    // Move assignment
    console_logger& operator=(console_logger&& other) noexcept {
        if (this != &other) {
            _min_level = other._min_level;
        }
        return *this;
    }
    
    // Delete copy constructor and assignment
    console_logger(const console_logger&) = delete;
    console_logger& operator=(const console_logger&) = delete;
    
    // Basic logging with level and message
    auto log(log_level level, std::string_view message) -> void {
        if (level < _min_level) {
            return;
        }
        
        std::lock_guard<std::mutex> lock(_mutex);
        auto& stream = get_stream(level);
        stream << format_timestamp() << " "
               << level_to_string(level) << ": "
               << message << "\n";
        stream.flush();
    }
    
    // Structured logging with key-value pairs
    auto log(
        log_level level,
        std::string_view message,
        const std::vector<std::pair<std::string_view, std::string_view>>& key_value_pairs
    ) -> void {
        if (level < _min_level) {
            return;
        }
        
        std::lock_guard<std::mutex> lock(_mutex);
        auto& stream = get_stream(level);
        stream << format_timestamp() << " "
               << level_to_string(level) << ": "
               << message;
        
        for (const auto& [key, value] : key_value_pairs) {
            stream << " [" << key << "=" << value << "]";
        }
        
        stream << "\n";
        stream.flush();
    }
    
    // Convenience methods for each log level
    auto trace(std::string_view message) -> void {
        log(log_level::trace, message);
    }
    
    auto trace(std::string_view message, const std::vector<std::pair<std::string_view, std::string_view>>& key_value_pairs) -> void {
        log(log_level::trace, message, key_value_pairs);
    }
    
    auto debug(std::string_view message) -> void {
        log(log_level::debug, message);
    }
    
    auto debug(std::string_view message, const std::vector<std::pair<std::string_view, std::string_view>>& key_value_pairs) -> void {
        log(log_level::debug, message, key_value_pairs);
    }
    
    auto info(std::string_view message) -> void {
        log(log_level::info, message);
    }
    
    auto info(std::string_view message, const std::vector<std::pair<std::string_view, std::string_view>>& key_value_pairs) -> void {
        log(log_level::info, message, key_value_pairs);
    }
    
    auto warning(std::string_view message) -> void {
        log(log_level::warning, message);
    }
    
    auto warning(std::string_view message, const std::vector<std::pair<std::string_view, std::string_view>>& key_value_pairs) -> void {
        log(log_level::warning, message, key_value_pairs);
    }
    
    auto error(std::string_view message) -> void {
        log(log_level::error, message);
    }
    
    auto error(std::string_view message, const std::vector<std::pair<std::string_view, std::string_view>>& key_value_pairs) -> void {
        log(log_level::error, message, key_value_pairs);
    }
    
    auto critical(std::string_view message) -> void {
        log(log_level::critical, message);
    }
    
    auto critical(std::string_view message, const std::vector<std::pair<std::string_view, std::string_view>>& key_value_pairs) -> void {
        log(log_level::critical, message, key_value_pairs);
    }
    
    // Set minimum log level filter
    auto set_min_level(log_level level) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _min_level = level;
    }
    
    // Get current minimum log level
    [[nodiscard]] auto get_min_level() const -> log_level {
        return _min_level;
    }

private:
    log_level _min_level;
    mutable std::mutex _mutex;
    
    // Convert log level to string representation
    [[nodiscard]] auto level_to_string(log_level level) const -> std::string_view {
        switch (level) {
            case log_level::trace:    return "TRACE";
            case log_level::debug:    return "DEBUG";
            case log_level::info:     return "INFO";
            case log_level::warning:  return "WARNING";
            case log_level::error:    return "ERROR";
            case log_level::critical: return "CRITICAL";
        }
        return "UNKNOWN";
    }
    
    // Get appropriate output stream based on log level
    [[nodiscard]] auto get_stream(log_level level) const -> std::ostream& {
        // Error and critical messages go to stderr, others to stdout
        if (level >= log_level::error) {
            return std::cerr;
        }
        return std::cout;
    }
    
    // Format current timestamp for log messages
    [[nodiscard]] auto format_timestamp() const -> std::string {
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ) % 1000;
        
        std::ostringstream oss;
        oss << std::put_time(std::localtime(&time_t_now), "%Y-%m-%d %H:%M:%S")
            << '.' << std::setfill('0') << std::setw(3) << ms.count();
        return oss.str();
    }
};

// Verify that console_logger satisfies the diagnostic_logger concept
static_assert(diagnostic_logger<console_logger>,
    "console_logger must satisfy diagnostic_logger concept");

} // namespace kythira

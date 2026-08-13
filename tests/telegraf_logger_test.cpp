/// @file telegraf_logger_test.cpp
/// @brief Behavior tests for the Telegraf logger backend
///        (include/raft/telegraf_logger.hpp) with the injected sender seam:
///        line-protocol log-event shape (level tag, constant tags, msg and
///        structured string fields, trailing timestamp), string-field
///        escaping, min-level filtering, and concept conformance.

#include "metrics_line_test_support.hpp"

#include <raft/logger.hpp>
#include <raft/telegraf_logger.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>

using namespace std::chrono_literals;
using metrics_line_test_support::fast_exporter_config;
using metrics_line_test_support::make_recording_sender;
using metrics_line_test_support::recording_sender_state;
using metrics_line_test_support::wait_for_payloads;

namespace {

auto make_logger(std::shared_ptr<recording_sender_state> state,
                 kythira::log_level min_level = kythira::log_level::trace)
    -> kythira::telegraf_logger {
    kythira::telegraf_logger_config config;
    config.host = "unused-with-injected-sender";
    config.constant_tags = {{"node_id", "1"}};
    config.exporter = fast_exporter_config();
    return kythira::telegraf_logger(config, min_level, make_recording_sender(state));
}

/// A line is `<prefix> <timestamp_ns>`; validate the split and that the
/// timestamp is all digits.
auto check_line(const std::string& line, const std::string& expected_prefix, const char* what)
    -> bool {
    auto space = line.rfind(' ');
    if (space == std::string::npos || line.substr(0, space) != expected_prefix) {
        std::cerr << "  x " << what << ": expected `" << expected_prefix << "`, got `" << line
                  << "`\n";
        return false;
    }
    auto timestamp = line.substr(space + 1);
    if (timestamp.empty() ||
        !std::ranges::all_of(timestamp, [](unsigned char c) { return std::isdigit(c); })) {
        std::cerr << "  x " << what << ": bad timestamp `" << timestamp << "`\n";
        return false;
    }
    std::cout << "  OK\n";
    return true;
}

auto take_payload(std::shared_ptr<recording_sender_state> state, std::size_t nth) -> std::string {
    if (!wait_for_payloads(*state, nth, 2000ms)) return {};
    std::lock_guard<std::mutex> lock(state->mu);
    return state->payloads.at(nth - 1);
}

auto test_log_event_line() -> bool {
    std::cout << "Test: structured log -> level tag, constant tags, string fields\n";
    auto state = std::make_shared<recording_sender_state>();
    auto logger = make_logger(state);

    logger.warning("replication slow", {{"peer", "3"}});

    auto line = take_payload(state, 1);
    if (line.empty()) {
        std::cerr << "  x timed out\n";
        return false;
    }
    return check_line(line,
                      R"(kythira_log,level=warning,node_id=1 msg="replication slow",peer="3")",
                      "log event line");
}

auto test_string_field_escaping() -> bool {
    std::cout << "Test: quotes/backslashes escaped, newlines replaced\n";
    auto state = std::make_shared<recording_sender_state>();
    auto logger = make_logger(state);

    logger.error("say \"hi\"\\now\nnewline");

    auto line = take_payload(state, 1);
    if (line.empty()) {
        std::cerr << "  x timed out\n";
        return false;
    }
    return check_line(line, R"(kythira_log,level=error,node_id=1 msg="say \"hi\"\\now newline")",
                      "escaped line");
}

auto test_min_level_filters() -> bool {
    std::cout << "Test: below-min-level records never reach the sender\n";
    auto state = std::make_shared<recording_sender_state>();
    {
        auto logger = make_logger(state, kythira::log_level::error);
        logger.info("suppressed");
        logger.warning("also suppressed");
    }
    std::lock_guard<std::mutex> lock(state->mu);
    if (!state->payloads.empty()) {
        std::cerr << "  x expected no payloads, got " << state->payloads.size() << "\n";
        return false;
    }
    std::cout << "  OK\n";
    return true;
}

auto test_all_levels_and_overloads() -> bool {
    std::cout << "Test: every level's convenience methods map to the right level tag\n";
    auto state = std::make_shared<recording_sender_state>();
    auto logger = make_logger(state);

    const std::vector<std::pair<std::string_view, std::string_view>> kv{{"k", "v"}};
    logger.trace("m");
    logger.trace("m", kv);
    logger.debug("m");
    logger.debug("m", kv);
    logger.info("m");
    logger.info("m", kv);
    logger.warning("m");
    logger.warning("m", kv);
    logger.error("m");
    logger.error("m", kv);
    logger.critical("m");
    logger.critical("m", kv);
    logger.set_min_level(kythira::log_level::info);
    if (logger.get_min_level() != kythira::log_level::info) {
        std::cerr << "  x get_min_level mismatch\n";
        return false;
    }

    if (!wait_for_payloads(*state, 12, 4000ms)) {
        std::cerr << "  x timed out waiting for 12 payloads\n";
        return false;
    }
    std::lock_guard<std::mutex> lock(state->mu);
    std::string all;
    for (const auto& payload : state->payloads) all += payload;
    for (const char* level : {"trace", "debug", "info", "warning", "error", "critical"}) {
        if (all.find(",level=" + std::string(level) + ",") == std::string::npos) {
            std::cerr << "  x level " << level << " never appeared\n";
            return false;
        }
    }
    if (all.find("k=\"v\"") == std::string::npos) {
        std::cerr << "  x kv overload never rendered\n";
        return false;
    }
    std::cout << "  OK\n";
    return true;
}

auto test_concept_conformance() -> bool {
    std::cout << "Test: concept conformance\n";
    static_assert(kythira::diagnostic_logger<kythira::telegraf_logger>,
                  "telegraf_logger must satisfy diagnostic_logger concept");
    std::cout << "  OK\n";
    return true;
}

}  // namespace

auto main() -> int {
    std::cout << "Testing telegraf_logger implementation\n" << std::string(60, '=') << "\n\n";

    int failed = 0;
    auto run = [&](bool (*test)()) {
        if (!test()) ++failed;
    };

    run(test_log_event_line);
    run(test_string_field_escaping);
    run(test_min_level_filters);
    run(test_all_levels_and_overloads);
    run(test_concept_conformance);

    std::cout << std::string(60, '=') << "\n";
    if (failed > 0) {
        std::cerr << failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "All tests passed!\n";
    return 0;
}

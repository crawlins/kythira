// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file victorialogs_logger_test.cpp
/// @brief Behavior tests for the VictoriaLogs logger backend
///        (include/raft/victorialogs_logger.hpp) with the injected
///        line_sender_fn seam: ND-JSON line shape (_time/_msg/level plus
///        stream-identity and structured fields), min-level filtering,
///        constructor validation, and concept conformance.

#include "metrics_line_test_support.hpp"

#include <raft/logger.hpp>
#include <raft/victorialogs_logger.hpp>

#include <boost/json.hpp>

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
    -> kythira::victorialogs_logger {
    kythira::victorialogs_config config;
    config.job = "kythira-test";
    config.instance = "7";
    config.exporter = fast_exporter_config();
    return kythira::victorialogs_logger(config, min_level, make_recording_sender(state));
}

auto test_jsonline_shape() -> bool {
    std::cout << "Test: one log() -> one ND-JSON line with all fields\n";
    auto state = std::make_shared<recording_sender_state>();
    auto logger = make_logger(state);

    logger.error("replication stalled", {{"peer", "3"}, {"term", "12"}});

    if (!wait_for_payloads(*state, 1, 2000ms)) {
        std::cerr << "  x timed out waiting for the sender\n";
        return false;
    }
    std::lock_guard<std::mutex> lock(state->mu);
    auto parsed = boost::json::parse(state->payloads.back()).as_object();
    if (parsed.at("_msg").as_string() != "replication stalled" ||
        parsed.at("level").as_string() != "error" ||
        parsed.at("job").as_string() != "kythira-test" ||
        parsed.at("instance").as_string() != "7" || parsed.at("peer").as_string() != "3" ||
        parsed.at("term").as_string() != "12") {
        std::cerr << "  x wrong line: " << state->payloads.back() << "\n";
        return false;
    }
    // _time is RFC3339 UTC with nanosecond fraction — the shape measured
    // to work against a real VictoriaLogs (a bare nanosecond integer is
    // parsed as milliseconds and rejected as out of range).
    auto time_str = std::string(parsed.at("_time").as_string());
    if (time_str.find('T') == std::string::npos || !time_str.ends_with("Z") ||
        time_str.find('.') == std::string::npos ||
        time_str.size() != std::string("2026-08-13T06:46:54.123456789Z").size()) {
        std::cerr << "  x implausible _time: " << time_str << "\n";
        return false;
    }
    std::cout << "  OK\n";
    return true;
}

auto test_json_escaping_is_structural() -> bool {
    std::cout << "Test: quotes/newlines in messages survive JSON round-trip\n";
    auto state = std::make_shared<recording_sender_state>();
    auto logger = make_logger(state);

    logger.info("say \"hi\"\nnewline");

    if (!wait_for_payloads(*state, 1, 2000ms)) {
        std::cerr << "  x timed out\n";
        return false;
    }
    std::lock_guard<std::mutex> lock(state->mu);
    auto parsed = boost::json::parse(state->payloads.back()).as_object();
    if (parsed.at("_msg").as_string() != "say \"hi\"\nnewline") {
        std::cerr << "  x round-trip mangled the message\n";
        return false;
    }
    std::cout << "  OK\n";
    return true;
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

auto test_empty_endpoint_without_sender_throws() -> bool {
    std::cout << "Test: empty endpoint without an injected sender is rejected\n";
    try {
        kythira::victorialogs_config config;  // endpoint_base_url left empty.
        kythira::victorialogs_logger logger(config);
        std::cerr << "  x expected std::invalid_argument\n";
        return false;
    } catch (const std::invalid_argument&) {
        std::cout << "  OK\n";
        return true;
    }
}

auto test_all_levels_and_overloads() -> bool {
    std::cout << "Test: every level's convenience methods map to the right level field\n";
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
        if (all.find("\"level\":\"" + std::string(level) + "\"") == std::string::npos) {
            std::cerr << "  x level " << level << " never appeared\n";
            return false;
        }
    }
    if (all.find("\"k\":\"v\"") == std::string::npos) {
        std::cerr << "  x kv overload never rendered\n";
        return false;
    }
    std::cout << "  OK\n";
    return true;
}

auto test_concept_conformance() -> bool {
    std::cout << "Test: concept conformance\n";
    static_assert(kythira::diagnostic_logger<kythira::victorialogs_logger>,
                  "victorialogs_logger must satisfy diagnostic_logger concept");
    std::cout << "  OK\n";
    return true;
}

}  // namespace

auto main() -> int {
    std::cout << "Testing victorialogs_logger implementation\n" << std::string(60, '=') << "\n\n";

    int failed = 0;
    auto run = [&](bool (*test)()) {
        if (!test()) ++failed;
    };

    run(test_jsonline_shape);
    run(test_json_escaping_is_structural);
    run(test_min_level_filters);
    run(test_empty_endpoint_without_sender_throws);
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

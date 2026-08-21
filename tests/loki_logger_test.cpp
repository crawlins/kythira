// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file loki_logger_test.cpp
/// @brief Behavior tests for the Loki logger backend
///        (include/raft/loki_logger.hpp) with the injected http_poster_fn
///        seam from otlp_test_support.hpp: push path, stream labels from
///        the resource, per-severity stream grouping, logfmt rendering and
///        escaping, min-level filtering, and concept conformance.

#include "otlp_test_support.hpp"

#include <raft/logger.hpp>
#include <raft/loki_logger.hpp>

#include <boost/json.hpp>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>

using namespace std::chrono_literals;
using otlp_test_support::fast_test_config;
using otlp_test_support::make_recording_poster;
using otlp_test_support::recording_poster_state;
using otlp_test_support::test_resource;
using otlp_test_support::wait_for_calls;

namespace {

auto first_stream(const std::string& body) -> boost::json::object {
    auto parsed = boost::json::parse(body);
    return parsed.as_object().at("streams").as_array().at(0).as_object();
}

auto test_push_path_and_stream_labels() -> bool {
    std::cout << "Test: push path and stream labels from the resource\n";
    auto state = std::make_shared<recording_poster_state>();
    kythira::loki_logger logger(fast_test_config(), test_resource(), kythira::log_level::trace,
                                make_recording_poster(state));

    logger.info("Starting Raft node", {{"node_id", "1"}});

    if (!wait_for_calls(*state, 1, 2000ms)) {
        std::cerr << "  x timed out waiting for the exporter to POST\n";
        return false;
    }

    std::lock_guard<std::mutex> lock(state->mu);
    if (state->paths.back() != "/loki/api/v1/push") {
        std::cerr << "  x wrong path: " << state->paths.back() << "\n";
        return false;
    }
    auto stream = first_stream(state->bodies.back());
    const auto& labels = stream.at("stream").as_object();
    if (labels.at("job").as_string() != "otlp-test" || labels.at("instance").as_string() != "1" ||
        labels.at("level").as_string() != "info") {
        std::cerr << "  x wrong labels: " << boost::json::serialize(labels) << "\n";
        return false;
    }
    const auto& value = stream.at("values").as_array().at(0).as_array();
    if (value.at(1).as_string() != R"(msg="Starting Raft node" node_id="1")") {
        std::cerr << "  x wrong logfmt line: " << value.at(1).as_string() << "\n";
        return false;
    }
    std::cout << "  OK\n";
    return true;
}

auto test_levels_group_into_separate_streams() -> bool {
    std::cout << "Test: one batch groups severities into separate streams\n";
    auto state = std::make_shared<recording_poster_state>();
    auto config = fast_test_config();
    config.max_batch_size = 2;  // Both records flush in one POST.
    kythira::loki_logger logger(config, test_resource(), kythira::log_level::trace,
                                make_recording_poster(state));

    logger.info("normal");
    logger.error("broken");

    if (!wait_for_calls(*state, 1, 2000ms)) {
        std::cerr << "  x timed out\n";
        return false;
    }
    std::lock_guard<std::mutex> lock(state->mu);
    auto parsed = boost::json::parse(state->bodies.back());
    const auto& streams = parsed.as_object().at("streams").as_array();
    if (streams.size() != 2) {
        std::cerr << "  x expected 2 streams, got " << streams.size() << ": "
                  << state->bodies.back() << "\n";
        return false;
    }
    std::cout << "  OK\n";
    return true;
}

auto test_logfmt_escaping() -> bool {
    std::cout << "Test: logfmt escaping of quotes, backslashes, newlines\n";
    auto state = std::make_shared<recording_poster_state>();
    kythira::loki_logger logger(fast_test_config(), test_resource(), kythira::log_level::trace,
                                make_recording_poster(state));

    logger.warning("say \"hi\"\\now", {{"detail", "line1\nline2"}});

    if (!wait_for_calls(*state, 1, 2000ms)) {
        std::cerr << "  x timed out\n";
        return false;
    }
    std::lock_guard<std::mutex> lock(state->mu);
    auto stream = first_stream(state->bodies.back());
    auto line = std::string(stream.at("values").as_array().at(0).as_array().at(1).as_string());
    if (line != R"(msg="say \"hi\"\\now" detail="line1 line2")") {
        std::cerr << "  x wrong escaped line: " << line << "\n";
        return false;
    }
    std::cout << "  OK\n";
    return true;
}

auto test_min_level_filters() -> bool {
    std::cout << "Test: below-min-level records never reach the exporter\n";
    auto state = std::make_shared<recording_poster_state>();
    {
        kythira::loki_logger logger(fast_test_config(), test_resource(),
                                    kythira::log_level::warning, make_recording_poster(state));
        logger.info("suppressed");
        logger.debug("also suppressed");
    }  // Destruction flushes anything queued.
    std::lock_guard<std::mutex> lock(state->mu);
    if (!state->bodies.empty()) {
        std::cerr << "  x expected no POSTs, got " << state->bodies.size() << "\n";
        return false;
    }
    std::cout << "  OK\n";
    return true;
}

auto test_all_levels_and_overloads() -> bool {
    std::cout << "Test: every level's convenience methods map to the right stream label\n";
    auto state = std::make_shared<recording_poster_state>();
    kythira::loki_logger logger(fast_test_config(), test_resource(), kythira::log_level::trace,
                                make_recording_poster(state));

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

    if (!wait_for_calls(*state, 12, 4000ms)) {
        std::cerr << "  x timed out waiting for 12 POSTs\n";
        return false;
    }
    std::lock_guard<std::mutex> lock(state->mu);
    std::string all;
    for (const auto& body : state->bodies) all += body;
    for (const char* level : {"trace", "debug", "info", "warning", "error", "critical"}) {
        if (all.find("\"level\":\"" + std::string(level) + "\"") == std::string::npos) {
            std::cerr << "  x level " << level << " never appeared\n";
            return false;
        }
    }
    // The logfmt line rides inside a JSON string, so the quotes arrive
    // JSON-escaped in the raw body.
    if (all.find(R"(k=\"v\")") == std::string::npos) {
        std::cerr << "  x kv overload never rendered\n";
        return false;
    }
    std::cout << "  OK\n";
    return true;
}

auto test_concept_conformance() -> bool {
    std::cout << "Test: concept conformance\n";
    static_assert(kythira::diagnostic_logger<kythira::loki_logger>,
                  "loki_logger must satisfy diagnostic_logger concept");
    std::cout << "  OK\n";
    return true;
}

}  // namespace

auto main() -> int {
    std::cout << "Testing loki_logger implementation\n" << std::string(60, '=') << "\n\n";

    int failed = 0;
    auto run = [&](bool (*test)()) {
        if (!test()) ++failed;
    };

    run(test_push_path_and_stream_labels);
    run(test_levels_group_into_separate_streams);
    run(test_logfmt_escaping);
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

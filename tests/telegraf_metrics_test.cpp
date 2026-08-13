/// @file telegraf_metrics_test.cpp
/// @brief Behavior tests for the Telegraf metrics backend
///        (include/raft/telegraf_metrics.hpp): InfluxDB line-protocol
///        rendering through an injected sender — measurement/tag escaping,
///        field mapping per concept method, integer counter suffix, and the
///        trailing nanosecond timestamp.

#include "metrics_line_test_support.hpp"

#include <raft/metrics.hpp>
#include <raft/telegraf_metrics.hpp>

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

auto make_backend(std::shared_ptr<recording_sender_state> state) -> kythira::telegraf_metrics {
    kythira::telegraf_metrics_config config;
    config.host = "unused-with-injected-sender";
    config.exporter = fast_exporter_config();
    return kythira::telegraf_metrics(config, make_recording_sender(state));
}

/// A line is `<prefix> <timestamp_ns>`; validate the split and that the
/// timestamp is all digits (its exact value is wall-clock).
auto check_line(const std::string& line, const std::string& expected_prefix, const char* what)
    -> bool {
    auto space = line.rfind(' ');
    if (space == std::string::npos) {
        std::cerr << "  x " << what << ": no timestamp separator in `" << line << "`\n";
        return false;
    }
    if (line.substr(0, space) != expected_prefix) {
        std::cerr << "  x " << what << ": expected `" << expected_prefix << "`, got `"
                  << line.substr(0, space) << "`\n";
        return false;
    }
    auto timestamp = line.substr(space + 1);
    if (timestamp.empty() ||
        !std::ranges::all_of(timestamp, [](unsigned char c) { return std::isdigit(c); })) {
        std::cerr << "  x " << what << ": bad timestamp `" << timestamp << "`\n";
        return false;
    }
    return true;
}

auto emit_and_take_line(kythira::telegraf_metrics& metrics,
                        std::shared_ptr<recording_sender_state> state, std::size_t nth)
    -> std::string {
    metrics.emit();
    if (!wait_for_payloads(*state, nth, 2000ms)) return {};
    std::lock_guard<std::mutex> lock(state->mu);
    return state->payloads.at(nth - 1);
}

auto test_counter_line() -> bool {
    std::cout << "Test: add_count -> integer count field with tags\n";
    auto state = std::make_shared<recording_sender_state>();
    auto metrics = make_backend(state);

    metrics.set_metric_name("http.client.request.sent");
    metrics.add_dimension("rpc_type", "request_vote");
    metrics.add_dimension("target_node_id", "3");
    metrics.add_count(2);
    metrics.add_one();

    auto line = emit_and_take_line(metrics, state, 1);
    if (line.empty()) {
        std::cerr << "  x timed out\n";
        return false;
    }
    return check_line(line,
                      "http.client.request.sent,rpc_type=request_vote,target_node_id=3 count=3i",
                      "counter line");
}

auto test_gauge_and_duration_lines() -> bool {
    std::cout << "Test: add_value -> value field; add_duration -> duration_ms field\n";
    auto state = std::make_shared<recording_sender_state>();
    auto metrics = make_backend(state);

    metrics.set_metric_name("http.client.request.size");
    metrics.add_value(1024.5);
    auto gauge_line = emit_and_take_line(metrics, state, 1);
    if (gauge_line.empty()) {
        std::cerr << "  x timed out (gauge)\n";
        return false;
    }
    if (!check_line(gauge_line, "http.client.request.size value=1024.5", "gauge line"))
        return false;

    metrics.set_metric_name("rpc.latency");
    metrics.add_duration(1500us);
    auto duration_line = emit_and_take_line(metrics, state, 2);
    if (duration_line.empty()) {
        std::cerr << "  x timed out (duration)\n";
        return false;
    }
    return check_line(duration_line, "rpc.latency duration_ms=1.5", "duration line");
}

auto test_escaping() -> bool {
    std::cout << "Test: measurement and tag escaping\n";
    auto state = std::make_shared<recording_sender_state>();
    auto metrics = make_backend(state);

    metrics.set_metric_name("weird name,with comma");
    metrics.add_dimension("tag key", "value with space,comma=eq");
    metrics.add_one();

    auto line = emit_and_take_line(metrics, state, 1);
    if (line.empty()) {
        std::cerr << "  x timed out\n";
        return false;
    }
    return check_line(line,
                      R"(weird\ name\,with\ comma,tag\ key=value\ with\ space\,comma\=eq count=1i)",
                      "escaped line");
}

auto test_emit_without_recording_is_noop() -> bool {
    std::cout << "Test: emit() without a recording call sends nothing\n";
    auto state = std::make_shared<recording_sender_state>();
    {
        auto metrics = make_backend(state);
        metrics.set_metric_name("never_recorded");
        metrics.add_dimension("node_id", "1");
        metrics.emit();
    }  // Destruction flushes anything queued.

    std::lock_guard<std::mutex> lock(state->mu);
    if (!state->payloads.empty()) {
        std::cerr << "  x expected no payloads, got " << state->payloads.size() << "\n";
        return false;
    }
    std::cout << "  OK\n";
    return true;
}

auto test_copies_share_the_exporter() -> bool {
    std::cout << "Test: copied handles share one exporter\n";
    auto state = std::make_shared<recording_sender_state>();
    auto metrics = make_backend(state);

    auto copy = metrics;
    copy.set_metric_name("requests");
    copy.add_one();
    copy.emit();

    if (!wait_for_payloads(*state, 1, 2000ms)) {
        std::cerr << "  x the copy's emission never reached the shared sender\n";
        return false;
    }
    std::cout << "  OK\n";
    return true;
}

auto test_concept_conformance() -> bool {
    std::cout << "Test: concept conformance\n";
    static_assert(kythira::metrics<kythira::telegraf_metrics>,
                  "telegraf_metrics must satisfy metrics concept");
    std::cout << "  OK\n";
    return true;
}

}  // namespace

auto main() -> int {
    std::cout << "Testing telegraf_metrics implementation\n" << std::string(60, '=') << "\n\n";

    int failed = 0;
    auto run = [&](bool (*test)()) {
        if (!test()) ++failed;
    };

    run(test_counter_line);
    run(test_gauge_and_duration_lines);
    run(test_escaping);
    run(test_emit_without_recording_is_noop);
    run(test_copies_share_the_exporter);
    run(test_concept_conformance);

    std::cout << std::string(60, '=') << "\n";
    if (failed > 0) {
        std::cerr << failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "All tests passed!\n";
    return 0;
}

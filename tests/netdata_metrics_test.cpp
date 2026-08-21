// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file netdata_metrics_test.cpp
/// @brief Behavior tests for the NetData metrics backend
///        (include/raft/netdata_metrics.hpp): StatsD line rendering through
///        an injected sender — type mapping per concept method, DataDog-style
///        tag rendering, the include_tags=false switch, and sanitization of
///        the protocol's unescapable delimiter characters.

#include "metrics_line_test_support.hpp"

#include <raft/metrics.hpp>
#include <raft/netdata_metrics.hpp>

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

auto make_backend(std::shared_ptr<recording_sender_state> state, bool include_tags = true)
    -> kythira::netdata_metrics {
    kythira::netdata_metrics_config config;
    config.host = "unused-with-injected-sender";
    config.include_tags = include_tags;
    config.exporter = fast_exporter_config();
    return kythira::netdata_metrics(config, make_recording_sender(state));
}

auto take_payload(std::shared_ptr<recording_sender_state> state, std::size_t nth) -> std::string {
    if (!wait_for_payloads(*state, nth, 2000ms)) return {};
    std::lock_guard<std::mutex> lock(state->mu);
    return state->payloads.at(nth - 1);
}

auto expect_payload(const std::string& got, const std::string& want, const char* what) -> bool {
    if (got != want) {
        std::cerr << "  x " << what << ": expected `" << want << "`, got `" << got << "`\n";
        return false;
    }
    std::cout << "  OK\n";
    return true;
}

auto test_counter_with_tags() -> bool {
    std::cout << "Test: add_count -> |c with DataDog-style tags\n";
    auto state = std::make_shared<recording_sender_state>();
    auto metrics = make_backend(state);

    metrics.set_metric_name("http.client.request.sent");
    metrics.add_dimension("rpc_type", "request_vote");
    metrics.add_dimension("target_node_id", "3");
    metrics.add_count(5);
    metrics.emit();

    return expect_payload(take_payload(state, 1),
                          "http.client.request.sent:5|c|#rpc_type:request_vote,target_node_id:3",
                          "counter line");
}

auto test_gauge_and_timer() -> bool {
    std::cout << "Test: add_value -> |g; add_duration -> |ms\n";
    auto state = std::make_shared<recording_sender_state>();
    auto metrics = make_backend(state);

    metrics.set_metric_name("http.client.request.size");
    metrics.add_value(1024.5);
    metrics.emit();
    if (!expect_payload(take_payload(state, 1), "http.client.request.size:1024.5|g", "gauge line"))
        return false;

    metrics.set_metric_name("rpc.latency");
    metrics.add_duration(2500us);
    metrics.emit();
    return expect_payload(take_payload(state, 2), "rpc.latency:2.5|ms", "timer line");
}

auto test_tags_disabled() -> bool {
    std::cout << "Test: include_tags=false drops dimensions from the wire\n";
    auto state = std::make_shared<recording_sender_state>();
    auto metrics = make_backend(state, /*include_tags=*/false);

    metrics.set_metric_name("requests");
    metrics.add_dimension("rpc_type", "request_vote");
    metrics.add_one();
    metrics.emit();

    return expect_payload(take_payload(state, 1), "requests:1|c", "untagged line");
}

auto test_sanitization() -> bool {
    std::cout << "Test: protocol delimiters are replaced, not escaped\n";
    auto state = std::make_shared<recording_sender_state>();
    auto metrics = make_backend(state);

    metrics.set_metric_name("bad:name|with delims");
    metrics.add_dimension("key:colon", "value,comma|pipe");
    metrics.add_one();
    metrics.emit();

    return expect_payload(take_payload(state, 1),
                          "bad_name_with_delims:1|c|#key_colon:value_comma_pipe", "sanitized line");
}

auto test_emit_without_recording_is_noop() -> bool {
    std::cout << "Test: emit() without a recording call sends nothing\n";
    auto state = std::make_shared<recording_sender_state>();
    {
        auto metrics = make_backend(state);
        metrics.set_metric_name("never_recorded");
        metrics.emit();
    }
    std::lock_guard<std::mutex> lock(state->mu);
    if (!state->payloads.empty()) {
        std::cerr << "  x expected no payloads, got " << state->payloads.size() << "\n";
        return false;
    }
    std::cout << "  OK\n";
    return true;
}

auto test_concept_conformance() -> bool {
    std::cout << "Test: concept conformance\n";
    static_assert(kythira::metrics<kythira::netdata_metrics>,
                  "netdata_metrics must satisfy metrics concept");
    std::cout << "  OK\n";
    return true;
}

}  // namespace

auto main() -> int {
    std::cout << "Testing netdata_metrics implementation\n" << std::string(60, '=') << "\n\n";

    int failed = 0;
    auto run = [&](bool (*test)()) {
        if (!test()) ++failed;
    };

    run(test_counter_with_tags);
    run(test_gauge_and_timer);
    run(test_tags_disabled);
    run(test_sanitization);
    run(test_emit_without_recording_is_noop);
    run(test_concept_conformance);

    std::cout << std::string(60, '=') << "\n";
    if (failed > 0) {
        std::cerr << failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "All tests passed!\n";
    return 0;
}

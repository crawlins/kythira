/// @file victoriametrics_metrics_test.cpp
/// @brief Behavior tests for the VictoriaMetrics metrics backend
///        (include/raft/victoriametrics_metrics.hpp) with the injected
///        http_poster_fn seam from otlp_test_support.hpp: import path,
///        cumulative body content across pushes, constant labels, the final
///        best-effort push at destruction, and failed-push accounting.

#include "otlp_test_support.hpp"

#include <raft/metrics.hpp>
#include <raft/victoriametrics_metrics.hpp>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using otlp_test_support::make_recording_poster;
using otlp_test_support::recording_poster_state;
using otlp_test_support::wait_for_calls;

namespace {

auto fast_config() -> kythira::victoriametrics_config {
    kythira::victoriametrics_config config;
    config.endpoint_base_url = "http://victoriametrics:8428";
    config.push_interval = 20ms;
    return config;
}

auto contains(const std::string& haystack, const std::string& needle) -> bool {
    return haystack.find(needle) != std::string::npos;
}

auto test_push_carries_cumulative_exposition() -> bool {
    std::cout << "Test: pushes POST the cumulative text exposition to the import path\n";
    auto state = std::make_shared<recording_poster_state>();
    kythira::victoriametrics_metrics metrics(fast_config(), make_recording_poster(state));

    metrics.set_metric_name("command_received");
    metrics.add_dimension("node_id", "1");
    metrics.add_one();
    metrics.emit();

    if (!wait_for_calls(*state, 1, 2000ms)) {
        std::cerr << "  x timed out waiting for the first push\n";
        return false;
    }

    metrics.set_metric_name("command_received");
    metrics.add_dimension("node_id", "1");
    metrics.add_one();
    metrics.emit();

    if (!wait_for_calls(*state, 3, 2000ms)) {
        std::cerr << "  x timed out waiting for a later push\n";
        return false;
    }

    std::lock_guard<std::mutex> lock(state->mu);
    if (state->paths.back() != "/api/v1/import/prometheus") {
        std::cerr << "  x wrong path: " << state->paths.back() << "\n";
        return false;
    }
    if (!contains(state->bodies.front(), "command_received_total{node_id=\"1\"} 1")) {
        std::cerr << "  x first push missing count 1:\n" << state->bodies.front() << "\n";
        return false;
    }
    if (!contains(state->bodies.back(), "command_received_total{node_id=\"1\"} 2")) {
        std::cerr << "  x later push missing cumulative count 2:\n" << state->bodies.back() << "\n";
        return false;
    }
    std::cout << "  OK\n";
    return true;
}

auto test_constant_labels_apply_to_every_series() -> bool {
    std::cout << "Test: constant labels are attached to every series\n";
    auto state = std::make_shared<recording_poster_state>();
    auto config = fast_config();
    config.constant_labels = {{"job", "kythira"}, {"instance", "node-42"}};
    kythira::victoriametrics_metrics metrics(config, make_recording_poster(state));

    metrics.set_metric_name("requests");
    metrics.add_dimension("rpc_type", "request_vote");
    metrics.add_one();
    metrics.emit();

    if (!wait_for_calls(*state, 1, 2000ms)) {
        std::cerr << "  x timed out\n";
        return false;
    }
    std::lock_guard<std::mutex> lock(state->mu);
    // Labels render sorted, so the constant labels interleave alphabetically.
    if (!contains(state->bodies.back(),
                  "requests_total{instance=\"node-42\",job=\"kythira\",rpc_type=\"request_vote\"}"
                  " 1")) {
        std::cerr << "  x constant labels missing:\n" << state->bodies.back() << "\n";
        return false;
    }
    std::cout << "  OK\n";
    return true;
}

auto test_no_push_while_registry_empty() -> bool {
    std::cout << "Test: an empty registry produces no POSTs\n";
    auto state = std::make_shared<recording_poster_state>();
    {
        kythira::victoriametrics_metrics metrics(fast_config(), make_recording_poster(state));
        // Give the push loop a few intervals with nothing recorded.
        std::this_thread::sleep_for(100ms);
    }
    std::lock_guard<std::mutex> lock(state->mu);
    if (!state->bodies.empty()) {
        std::cerr << "  x expected no pushes, got " << state->bodies.size() << "\n";
        return false;
    }
    std::cout << "  OK\n";
    return true;
}

auto test_final_push_on_destruction() -> bool {
    std::cout << "Test: destruction performs one final push\n";
    auto state = std::make_shared<recording_poster_state>();
    {
        auto config = fast_config();
        config.push_interval = 3600s;  // Only the final push can deliver.
        kythira::victoriametrics_metrics metrics(config, make_recording_poster(state));
        metrics.set_metric_name("requests");
        metrics.add_one();
        metrics.emit();
    }
    std::lock_guard<std::mutex> lock(state->mu);
    if (state->bodies.empty() || !contains(state->bodies.back(), "requests_total 1")) {
        std::cerr << "  x final push missing\n";
        return false;
    }
    std::cout << "  OK\n";
    return true;
}

auto test_failed_push_is_counted() -> bool {
    std::cout << "Test: failed pushes are counted, not retried\n";
    auto state = std::make_shared<recording_poster_state>();
    state->fail_first_n_calls = 1;
    kythira::victoriametrics_metrics metrics(fast_config(), make_recording_poster(state));

    metrics.set_metric_name("requests");
    metrics.add_one();
    metrics.emit();

    if (!wait_for_calls(*state, 2, 2000ms)) {
        std::cerr << "  x timed out waiting for the post-failure push\n";
        return false;
    }
    if (metrics.failed_push_count() < 1) {
        std::cerr << "  x failed push not counted\n";
        return false;
    }
    std::cout << "  OK\n";
    return true;
}

auto test_empty_endpoint_throws() -> bool {
    std::cout << "Test: an empty endpoint is rejected at construction\n";
    try {
        kythira::victoriametrics_config config;  // endpoint_base_url left empty.
        kythira::victoriametrics_metrics metrics(config);
        std::cerr << "  x expected std::invalid_argument\n";
        return false;
    } catch (const std::invalid_argument&) {
        std::cout << "  OK\n";
        return true;
    }
}

auto test_concept_conformance() -> bool {
    std::cout << "Test: concept conformance\n";
    static_assert(kythira::metrics<kythira::victoriametrics_metrics>,
                  "victoriametrics_metrics must satisfy metrics concept");
    std::cout << "  OK\n";
    return true;
}

}  // namespace

auto main() -> int {
    std::cout << "Testing victoriametrics_metrics implementation\n"
              << std::string(60, '=') << "\n\n";

    int failed = 0;
    auto run = [&](bool (*test)()) {
        if (!test()) ++failed;
    };

    run(test_push_carries_cumulative_exposition);
    run(test_constant_labels_apply_to_every_series);
    run(test_no_push_while_registry_empty);
    run(test_final_push_on_destruction);
    run(test_failed_push_is_counted);
    run(test_empty_endpoint_throws);
    run(test_concept_conformance);

    std::cout << std::string(60, '=') << "\n";
    if (failed > 0) {
        std::cerr << failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "All tests passed!\n";
    return 0;
}

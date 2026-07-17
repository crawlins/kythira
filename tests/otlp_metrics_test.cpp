#include "otlp_test_support.hpp"

#include <raft/otlp_metrics.hpp>

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

auto first_metric(const std::string& body) -> boost::json::object {
    auto parsed = boost::json::parse(body);
    const auto& root = parsed.as_object();
    const auto& resource_metrics = root.at("resourceMetrics").as_array();
    const auto& scope_metrics = resource_metrics.at(0).as_object().at("scopeMetrics").as_array();
    const auto& metrics_arr = scope_metrics.at(0).as_object().at("metrics").as_array();
    return metrics_arr.at(0).as_object();
}

auto first_data_point(const boost::json::object& metric,
                      std::string_view shape_key) -> boost::json::object {
    return metric.at(shape_key).as_object().at("dataPoints").as_array().at(0).as_object();
}

auto test_add_one_produces_delta_sum() -> bool {
    std::cout << "Test: add_one() -> delta Sum\n";
    auto state = std::make_shared<recording_poster_state>();
    kythira::otlp_metrics metrics(fast_test_config(), test_resource(),
                                  make_recording_poster(state));

    metrics.set_metric_name("command_received");
    metrics.add_dimension("node_id", "1");
    metrics.add_one();
    metrics.emit();

    if (!wait_for_calls(*state, 1, 2000ms)) {
        std::cerr << "  x timed out waiting for the exporter to POST\n";
        return false;
    }

    std::lock_guard<std::mutex> lock(state->mu);
    if (state->paths.back() != "/v1/metrics") {
        std::cerr << "  x wrong signal path: " << state->paths.back() << "\n";
        return false;
    }

    auto metric = first_metric(state->bodies.back());
    if (metric.at("name").as_string() != "command_received") {
        std::cerr << "  x wrong metric name\n";
        return false;
    }
    auto dp = first_data_point(metric, "sum");
    if (metric.at("sum").at("aggregationTemporality").as_string() !=
        "AGGREGATION_TEMPORALITY_DELTA") {
        std::cerr << "  x expected delta aggregation temporality\n";
        return false;
    }
    if (!metric.at("sum").at("isMonotonic").as_bool()) {
        std::cerr << "  x expected isMonotonic true\n";
        return false;
    }
    if (dp.at("asInt").as_string() != "1") {
        std::cerr << "  x expected asInt \"1\", got " << dp.at("asInt").as_string() << "\n";
        return false;
    }

    std::cout << "  OK\n";
    return true;
}

auto test_add_count_and_add_duration_and_add_value() -> bool {
    std::cout << "Test: add_count/add_duration/add_value produce Sum/Histogram/Gauge\n";
    auto state = std::make_shared<recording_poster_state>();
    kythira::otlp_metrics metrics(fast_test_config(), test_resource(),
                                  make_recording_poster(state));

    metrics.set_metric_name("batch_counter");
    metrics.add_count(7);
    metrics.emit();
    if (!wait_for_calls(*state, 1, 2000ms)) {
        std::cerr << "  x timed out waiting for add_count export\n";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(state->mu);
        auto metric = first_metric(state->bodies.back());
        auto dp = first_data_point(metric, "sum");
        if (dp.at("asInt").as_string() != "7") {
            std::cerr << "  x expected asInt \"7\"\n";
            return false;
        }
    }

    metrics.set_metric_name("append_entries_latency");
    metrics.add_duration(15ms);
    metrics.emit();
    if (!wait_for_calls(*state, 2, 2000ms)) {
        std::cerr << "  x timed out waiting for add_duration export\n";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(state->mu);
        auto metric = first_metric(state->bodies.back());
        auto dp = first_data_point(metric, "histogram");
        if (dp.at("count").as_string() != "1") {
            std::cerr << "  x expected histogram count \"1\"\n";
            return false;
        }
        if (dp.at("sum").as_double() != 15.0) {
            std::cerr << "  x expected histogram sum 15.0 ms, got " << dp.at("sum").as_double()
                      << "\n";
            return false;
        }
        if (!dp.if_contains("explicitBounds") || !dp.if_contains("bucketCounts")) {
            std::cerr << "  x missing explicitBounds/bucketCounts\n";
            return false;
        }
    }

    metrics.set_metric_name("queue_depth");
    metrics.add_value(42.5);
    metrics.emit();
    if (!wait_for_calls(*state, 3, 2000ms)) {
        std::cerr << "  x timed out waiting for add_value export\n";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(state->mu);
        auto metric = first_metric(state->bodies.back());
        auto dp = first_data_point(metric, "gauge");
        if (dp.at("asDouble").as_double() != 42.5) {
            std::cerr << "  x expected asDouble 42.5\n";
            return false;
        }
        if (dp.if_contains("startTimeUnixNano")) {
            std::cerr << "  x Gauge data points must not carry startTimeUnixNano\n";
            return false;
        }
    }

    std::cout << "  OK\n";
    return true;
}

// Requirement 1.3 / Property 3: the k-th delta data point's
// startTimeUnixNano must equal the (k-1)-th's timeUnixNano.
auto test_delta_start_time_chains_across_emits() -> bool {
    std::cout << "Test: delta Sum start_time chains across successive emit() calls\n";
    auto state = std::make_shared<recording_poster_state>();
    kythira::otlp_metrics metrics(fast_test_config(), test_resource(),
                                  make_recording_poster(state));

    metrics.set_metric_name("log_entry_appended");
    metrics.add_dimension("node_id", "1");
    metrics.add_one();
    metrics.emit();
    if (!wait_for_calls(*state, 1, 2000ms)) {
        std::cerr << "  x timed out waiting for first export\n";
        return false;
    }
    std::string first_time;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        auto dp = first_data_point(first_metric(state->bodies.back()), "sum");
        first_time = std::string(dp.at("timeUnixNano").as_string());
    }

    metrics.set_metric_name("log_entry_appended");
    metrics.add_dimension("node_id", "1");
    metrics.add_one();
    metrics.emit();
    if (!wait_for_calls(*state, 2, 2000ms)) {
        std::cerr << "  x timed out waiting for second export\n";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(state->mu);
        auto dp = first_data_point(first_metric(state->bodies.back()), "sum");
        auto second_start = std::string(dp.at("startTimeUnixNano").as_string());
        if (second_start != first_time) {
            std::cerr << "  x expected second start_time_unix_nano (" << second_start
                      << ") == first time_unix_nano (" << first_time << ")\n";
            return false;
        }
    }

    std::cout << "  OK\n";
    return true;
}

// Requirement 1.2: emit() with no recording method called since
// set_metric_name is a no-op — nothing pushed, nothing exported.
auto test_emit_without_recording_is_noop() -> bool {
    std::cout << "Test: emit() with no add_*() call pushes nothing\n";
    auto state = std::make_shared<recording_poster_state>();
    kythira::otlp_metrics metrics(fast_test_config(), test_resource(),
                                  make_recording_poster(state));

    metrics.set_metric_name("never_recorded");
    metrics.emit();

    if (wait_for_calls(*state, 1, 150ms)) {
        std::cerr << "  x exporter POSTed something for a metric with no recorded value\n";
        return false;
    }

    std::cout << "  OK\n";
    return true;
}

auto test_concept_conformance() -> bool {
    std::cout << "Test: otlp_metrics satisfies kythira::metrics\n";
    static_assert(kythira::metrics<kythira::otlp_metrics>,
                  "otlp_metrics must satisfy metrics concept");
    std::cout << "  OK\n";
    return true;
}

}  // namespace

auto main() -> int {
    std::cout << "Testing otlp_metrics implementation\n" << std::string(60, '=') << "\n\n";

    int failed = 0;
    auto run = [&](bool (*test)()) {
        if (!test()) ++failed;
    };

    run(test_add_one_produces_delta_sum);
    run(test_add_count_and_add_duration_and_add_value);
    run(test_delta_start_time_chains_across_emits);
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

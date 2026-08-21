// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file prometheus_metrics_test.cpp
/// @brief Behavior tests for the Prometheus metrics backend
///        (include/raft/prometheus_metrics.hpp): concept protocol mapping,
///        registry aggregation across copied handles, text exposition
///        rendering, sanitization/escaping, and a real scrape over HTTP
///        against prometheus_scrape_server on an ephemeral port.

#include <raft/metrics.hpp>
#include <raft/prometheus_metrics.hpp>

#include <httplib.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>

using namespace std::chrono_literals;

namespace {

auto contains(const std::string& haystack, const std::string& needle) -> bool {
    return haystack.find(needle) != std::string::npos;
}

auto expect_line(const std::string& text, const std::string& line, const char* what) -> bool {
    if (!contains(text, line + "\n")) {
        std::cerr << "  x missing " << what << ": expected line `" << line << "` in:\n"
                  << text << "\n";
        return false;
    }
    return true;
}

auto test_counter_accumulates_across_copies() -> bool {
    std::cout << "Test: counters accumulate across copied handles\n";
    auto registry = std::make_shared<kythira::prometheus_registry>();
    kythira::prometheus_metrics metrics(registry);

    // The transports' copy-per-emission idiom (http_transport_impl.hpp).
    auto copy = metrics;
    copy.set_metric_name("http.client.request.sent");
    copy.add_dimension("rpc_type", "request_vote");
    copy.add_one();
    copy.emit();

    copy = metrics;
    copy.set_metric_name("http.client.request.sent");
    copy.add_dimension("rpc_type", "request_vote");
    copy.add_count(2);
    copy.emit();

    auto text = registry->render_text();
    if (!expect_line(text, "# TYPE http_client_request_sent_total counter", "TYPE line"))
        return false;
    if (!expect_line(text, "http_client_request_sent_total{rpc_type=\"request_vote\"} 3",
                     "accumulated counter"))
        return false;

    std::cout << "  OK\n";
    return true;
}

auto test_gauge_last_value_wins() -> bool {
    std::cout << "Test: gauge keeps the last value\n";
    auto registry = std::make_shared<kythira::prometheus_registry>();
    kythira::prometheus_metrics metrics(registry);

    metrics.set_metric_name("http.client.request.size");
    metrics.add_value(100.0);
    metrics.emit();
    metrics.set_metric_name("http.client.request.size");
    metrics.add_value(250.5);
    metrics.emit();

    auto text = registry->render_text();
    if (!expect_line(text, "# TYPE http_client_request_size gauge", "TYPE line")) return false;
    if (!expect_line(text, "http_client_request_size 250.5", "last gauge value")) return false;
    if (contains(text, "100")) {
        std::cerr << "  x stale gauge value still rendered:\n" << text << "\n";
        return false;
    }

    std::cout << "  OK\n";
    return true;
}

auto test_histogram_buckets_sum_count() -> bool {
    std::cout << "Test: add_duration -> histogram with cumulative buckets\n";
    kythira::prometheus_metrics_config config;
    config.histogram_bounds_ms = {1, 10, 100};
    auto registry = std::make_shared<kythira::prometheus_registry>(config);
    kythira::prometheus_metrics metrics(registry);

    metrics.set_metric_name("rpc.latency");
    metrics.add_duration(5ms);  // -> le=10 bucket
    metrics.emit();
    metrics.set_metric_name("rpc.latency");
    metrics.add_duration(50ms);  // -> le=100 bucket
    metrics.emit();
    metrics.set_metric_name("rpc.latency");
    metrics.add_duration(2s);  // -> +Inf only
    metrics.emit();

    auto text = registry->render_text();
    if (!expect_line(text, "# TYPE rpc_latency histogram", "TYPE line")) return false;
    if (!expect_line(text, "rpc_latency_bucket{le=\"1\"} 0", "le=1 bucket")) return false;
    if (!expect_line(text, "rpc_latency_bucket{le=\"10\"} 1", "le=10 bucket")) return false;
    if (!expect_line(text, "rpc_latency_bucket{le=\"100\"} 2", "le=100 cumulative bucket"))
        return false;
    if (!expect_line(text, "rpc_latency_bucket{le=\"+Inf\"} 3", "+Inf bucket")) return false;
    if (!expect_line(text, "rpc_latency_sum 2055", "sum")) return false;
    if (!expect_line(text, "rpc_latency_count 3", "count")) return false;

    std::cout << "  OK\n";
    return true;
}

auto test_histogram_le_merges_into_existing_labels() -> bool {
    std::cout << "Test: histogram le label merges with recorded dimensions\n";
    kythira::prometheus_metrics_config config;
    config.histogram_bounds_ms = {10};
    auto registry = std::make_shared<kythira::prometheus_registry>(config);
    kythira::prometheus_metrics metrics(registry);

    metrics.set_metric_name("rpc.latency");
    metrics.add_dimension("rpc_type", "append_entries");
    metrics.add_duration(1ms);
    metrics.emit();

    auto text = registry->render_text();
    if (!expect_line(text, "rpc_latency_bucket{rpc_type=\"append_entries\",le=\"10\"} 1",
                     "merged label block"))
        return false;
    if (!expect_line(text, "rpc_latency_sum{rpc_type=\"append_entries\"} 1", "labeled sum"))
        return false;

    std::cout << "  OK\n";
    return true;
}

auto test_sanitization_and_escaping() -> bool {
    std::cout << "Test: name sanitization and label value escaping\n";
    auto registry = std::make_shared<kythira::prometheus_registry>();
    kythira::prometheus_metrics metrics(registry);

    metrics.set_metric_name("http.client.tls-reload.failed");
    metrics.add_dimension("error detail", "line1\nline\"2\"\\end");
    metrics.add_one();
    metrics.emit();

    auto text = registry->render_text();
    if (!expect_line(text,
                     "http_client_tls_reload_failed_total{error_detail=\"line1\\nline\\\"2\\\"\\\\"
                     "end\"} 1",
                     "sanitized+escaped series"))
        return false;

    std::cout << "  OK\n";
    return true;
}

auto test_dimension_order_does_not_split_series() -> bool {
    std::cout << "Test: dimension order does not split a series\n";
    auto registry = std::make_shared<kythira::prometheus_registry>();
    kythira::prometheus_metrics metrics(registry);

    metrics.set_metric_name("requests");
    metrics.add_dimension("a", "1");
    metrics.add_dimension("b", "2");
    metrics.add_one();
    metrics.emit();
    metrics.set_metric_name("requests");
    metrics.add_dimension("b", "2");
    metrics.add_dimension("a", "1");
    metrics.add_one();
    metrics.emit();

    auto text = registry->render_text();
    if (!expect_line(text, "requests_total{a=\"1\",b=\"2\"} 2", "single merged series"))
        return false;

    std::cout << "  OK\n";
    return true;
}

auto test_emit_without_recording_is_noop() -> bool {
    std::cout << "Test: emit() without a recording call renders nothing\n";
    auto registry = std::make_shared<kythira::prometheus_registry>();
    kythira::prometheus_metrics metrics(registry);

    metrics.set_metric_name("never_recorded");
    metrics.add_dimension("node_id", "1");
    metrics.emit();

    if (!registry->render_text().empty()) {
        std::cerr << "  x expected empty exposition, got:\n" << registry->render_text() << "\n";
        return false;
    }

    std::cout << "  OK\n";
    return true;
}

auto test_emit_resets_staging() -> bool {
    std::cout << "Test: emit() resets staged dimensions and sample\n";
    auto registry = std::make_shared<kythira::prometheus_registry>();
    kythira::prometheus_metrics metrics(registry);

    metrics.set_metric_name("requests");
    metrics.add_dimension("rpc_type", "request_vote");
    metrics.add_one();
    metrics.emit();
    metrics.emit();  // Nothing staged: must not double-count.

    auto text = registry->render_text();
    if (!expect_line(text, "requests_total{rpc_type=\"request_vote\"} 1", "single count"))
        return false;

    std::cout << "  OK\n";
    return true;
}

auto test_scrape_server_end_to_end() -> bool {
    std::cout << "Test: real HTTP scrape against prometheus_scrape_server\n";
    auto registry = std::make_shared<kythira::prometheus_registry>();
    kythira::prometheus_metrics metrics(registry);

    metrics.set_metric_name("command_received");
    metrics.add_dimension("node_id", "7");
    metrics.add_one();
    metrics.emit();

    kythira::prometheus_scrape_server server(registry, "127.0.0.1", 0);
    if (server.port() == 0) {
        std::cerr << "  x ephemeral port not reported\n";
        return false;
    }

    httplib::Client client("127.0.0.1", server.port());
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(2, 0);
    auto res = client.Get("/metrics");
    if (!res || res->status != 200) {
        std::cerr << "  x scrape failed: " << (res ? std::to_string(res->status) : "no response")
                  << "\n";
        return false;
    }
    if (!contains(res->get_header_value("Content-Type"), "text/plain")) {
        std::cerr << "  x wrong content type: " << res->get_header_value("Content-Type") << "\n";
        return false;
    }
    if (!expect_line(res->body, "command_received_total{node_id=\"7\"} 1", "scraped series"))
        return false;

    server.stop();
    std::cout << "  OK\n";
    return true;
}

auto test_concept_conformance() -> bool {
    std::cout << "Test: concept conformance\n";
    static_assert(kythira::metrics<kythira::prometheus_metrics>,
                  "prometheus_metrics must satisfy metrics concept");
    std::cout << "  OK\n";
    return true;
}

}  // namespace

auto main() -> int {
    std::cout << "Testing prometheus_metrics implementation\n" << std::string(60, '=') << "\n\n";

    int failed = 0;
    auto run = [&](bool (*test)()) {
        if (!test()) ++failed;
    };

    run(test_counter_accumulates_across_copies);
    run(test_gauge_last_value_wins);
    run(test_histogram_buckets_sum_count);
    run(test_histogram_le_merges_into_existing_labels);
    run(test_sanitization_and_escaping);
    run(test_dimension_order_does_not_split_series);
    run(test_emit_without_recording_is_noop);
    run(test_emit_resets_staging);
    run(test_scrape_server_end_to_end);
    run(test_concept_conformance);

    std::cout << std::string(60, '=') << "\n";
    if (failed > 0) {
        std::cerr << failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "All tests passed!\n";
    return 0;
}

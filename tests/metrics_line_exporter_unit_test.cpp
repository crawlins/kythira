/// @file metrics_line_exporter_unit_test.cpp
/// @brief Mechanics tests for the shared line-protocol export engine
///        (include/raft/metrics_line_exporter.hpp) with an injected sender —
///        no real sockets: payload packing under max_payload_bytes,
///        drop-oldest overflow accounting, send-failure accounting, and the
///        final best-effort flush at destruction.

#include "metrics_line_test_support.hpp"

#include <raft/metrics_line_exporter.hpp>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>

using namespace std::chrono_literals;
using metrics_line_test_support::fast_exporter_config;
using metrics_line_test_support::make_recording_sender;
using metrics_line_test_support::recording_sender_state;
using metrics_line_test_support::split_lines;
using metrics_line_test_support::wait_for_payloads;

namespace {

auto test_lines_are_delivered() -> bool {
    std::cout << "Test: pushed lines reach the sender\n";
    auto state = std::make_shared<recording_sender_state>();
    kythira::metrics_line_exporter exporter(fast_exporter_config(), make_recording_sender(state));

    exporter.push("a:1|c");
    if (!wait_for_payloads(*state, 1, 2000ms)) {
        std::cerr << "  x timed out waiting for the sender\n";
        return false;
    }

    std::lock_guard<std::mutex> lock(state->mu);
    if (state->payloads.front() != "a:1|c") {
        std::cerr << "  x wrong payload: " << state->payloads.front() << "\n";
        return false;
    }
    std::cout << "  OK\n";
    return true;
}

auto test_payload_packing_respects_max_bytes() -> bool {
    std::cout << "Test: batched lines pack into payloads under max_payload_bytes\n";
    auto state = std::make_shared<recording_sender_state>();
    kythira::metrics_line_exporter_config config;
    config.max_payload_bytes = 12;  // Fits two 5-byte lines + '\n', not three.
    config.max_batch_lines = 512;
    config.flush_interval = 3600s;  // Only the destructor's final flush fires.
    {
        kythira::metrics_line_exporter exporter(config, make_recording_sender(state));
        exporter.push("aaa:1");
        exporter.push("bbb:2");
        exporter.push("ccc:3");
    }  // Destruction flushes.

    std::lock_guard<std::mutex> lock(state->mu);
    if (state->payloads.size() != 2) {
        std::cerr << "  x expected 2 payloads, got " << state->payloads.size() << "\n";
        return false;
    }
    if (state->payloads[0] != "aaa:1\nbbb:2" || state->payloads[1] != "ccc:3") {
        std::cerr << "  x wrong packing: [" << state->payloads[0] << "] [" << state->payloads[1]
                  << "]\n";
        return false;
    }
    std::cout << "  OK\n";
    return true;
}

auto test_oversized_single_line_still_sent() -> bool {
    std::cout << "Test: a single line longer than max_payload_bytes is sent alone\n";
    auto state = std::make_shared<recording_sender_state>();
    kythira::metrics_line_exporter_config config;
    config.max_payload_bytes = 4;
    config.flush_interval = 3600s;
    {
        kythira::metrics_line_exporter exporter(config, make_recording_sender(state));
        exporter.push("longer-than-four-bytes:1|c");
    }

    std::lock_guard<std::mutex> lock(state->mu);
    if (state->payloads.size() != 1 || state->payloads[0] != "longer-than-four-bytes:1|c") {
        std::cerr << "  x oversized line mishandled\n";
        return false;
    }
    std::cout << "  OK\n";
    return true;
}

auto test_overflow_drops_oldest() -> bool {
    std::cout << "Test: queue overflow drops oldest and counts it\n";
    auto state = std::make_shared<recording_sender_state>();
    kythira::metrics_line_exporter_config config;
    config.max_queue_size = 2;
    config.max_batch_lines = 512;
    config.flush_interval = 3600s;
    std::uint64_t dropped = 0;
    {
        kythira::metrics_line_exporter exporter(config, make_recording_sender(state));
        exporter.push("first");
        exporter.push("second");
        exporter.push("third");  // Evicts "first".
        dropped = exporter.dropped_line_count();
    }

    if (dropped != 1) {
        std::cerr << "  x expected 1 dropped line, got " << dropped << "\n";
        return false;
    }
    std::lock_guard<std::mutex> lock(state->mu);
    auto lines = split_lines(state->payloads.at(0));
    if (lines.size() != 2 || lines[0] != "second" || lines[1] != "third") {
        std::cerr << "  x wrong survivors in payload: " << state->payloads.at(0) << "\n";
        return false;
    }
    std::cout << "  OK\n";
    return true;
}

auto test_send_failure_counts_dropped_lines() -> bool {
    std::cout << "Test: a failed send counts the payload's lines as dropped\n";
    auto state = std::make_shared<recording_sender_state>();
    state->fail_first_n_calls = 1;
    kythira::metrics_line_exporter exporter(fast_exporter_config(), make_recording_sender(state));

    exporter.push("doomed:1|c");
    exporter.push("survivor:1|c");
    if (!wait_for_payloads(*state, 1, 2000ms)) {
        std::cerr << "  x timed out waiting for the surviving payload\n";
        return false;
    }
    if (exporter.dropped_line_count() != 1) {
        std::cerr << "  x expected 1 dropped, got " << exporter.dropped_line_count() << "\n";
        return false;
    }
    std::cout << "  OK\n";
    return true;
}

auto test_empty_sender_throws() -> bool {
    std::cout << "Test: an empty sender is rejected at construction\n";
    try {
        kythira::metrics_line_exporter exporter(fast_exporter_config(), kythira::line_sender_fn{});
        std::cerr << "  x expected std::invalid_argument\n";
        return false;
    } catch (const std::invalid_argument&) {
        std::cout << "  OK\n";
        return true;
    }
}

}  // namespace

auto main() -> int {
    std::cout << "Testing metrics_line_exporter mechanics\n" << std::string(60, '=') << "\n\n";

    int failed = 0;
    auto run = [&](bool (*test)()) {
        if (!test()) ++failed;
    };

    run(test_lines_are_delivered);
    run(test_payload_packing_respects_max_bytes);
    run(test_oversized_single_line_still_sent);
    run(test_overflow_drops_oldest);
    run(test_send_failure_counts_dropped_lines);
    run(test_empty_sender_throws);

    std::cout << std::string(60, '=') << "\n";
    if (failed > 0) {
        std::cerr << failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "All tests passed!\n";
    return 0;
}

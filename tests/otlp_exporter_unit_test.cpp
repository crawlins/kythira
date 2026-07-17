#include "otlp_test_support.hpp"

#include <raft/otlp_exporter.hpp>

#include <boost/json.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using otlp_test_support::make_recording_poster;
using otlp_test_support::recording_poster_state;
using otlp_test_support::test_resource;
using otlp_test_support::wait_for_calls;

namespace {

struct test_record {
    int value = 0;
};

[[nodiscard]] auto encode(const kythira::otlp_resource&, std::span<const test_record> records)
    -> boost::json::object {
    boost::json::array arr;
    for (const auto& r : records) arr.push_back(boost::json::value(r.value));
    return boost::json::object{{"records", arr}};
}

using test_exporter = kythira::otlp_http_batch_exporter<test_record>;

// Requirement 3.3: overflow bounds memory via drop-oldest, independent of
// whether the background thread ever gets a chance to drain — flush_interval
// and max_batch_size are both set far beyond this test's window so no drain
// happens before the assertion.
auto test_drop_oldest_overflow() -> bool {
    std::cout << "Test: push() drop-oldest overflow bounds the queue\n";
    auto state = std::make_shared<recording_poster_state>();

    kythira::otlp_export_config cfg;
    cfg.endpoint_base_url = "http://localhost:1";
    cfg.max_queue_size = 10;
    cfg.max_batch_size = 1000;  // never reached by this test's pushes
    cfg.flush_interval = 60s;   // background thread parked throughout

    test_exporter exporter(cfg, test_resource(), "/v1/test", &encode, make_recording_poster(state));

    for (int i = 0; i < 15; ++i) exporter.push(test_record{i});

    if (exporter.dropped_record_count() != 5) {
        std::cerr << "  x expected 5 dropped records (15 pushed, capacity 10), got "
                  << exporter.dropped_record_count() << "\n";
        return false;
    }

    std::cout << "  OK\n";
    return true;
}

// Requirement 3.4: batch flushes once max_batch_size records are queued,
// without waiting for flush_interval.
auto test_batch_triggers_on_size() -> bool {
    std::cout << "Test: batch flushes on reaching max_batch_size\n";
    auto state = std::make_shared<recording_poster_state>();

    kythira::otlp_export_config cfg;
    cfg.endpoint_base_url = "http://localhost:1";
    cfg.max_batch_size = 3;
    cfg.flush_interval = 10s;  // must NOT be what triggers the flush below

    test_exporter exporter(cfg, test_resource(), "/v1/test", &encode, make_recording_poster(state));

    exporter.push(test_record{1});
    exporter.push(test_record{2});
    exporter.push(test_record{3});

    if (!wait_for_calls(*state, 1, 2000ms)) {
        std::cerr << "  x size-triggered flush did not happen within 2s (flush_interval is 10s)\n";
        return false;
    }

    std::cout << "  OK\n";
    return true;
}

// Requirement 3.4: batch also flushes on flush_interval even below
// max_batch_size.
auto test_batch_triggers_on_interval() -> bool {
    std::cout << "Test: batch flushes on flush_interval even with one record\n";
    auto state = std::make_shared<recording_poster_state>();

    kythira::otlp_export_config cfg;
    cfg.endpoint_base_url = "http://localhost:1";
    cfg.max_batch_size = 1000;  // must NOT be what triggers the flush below
    cfg.flush_interval = 50ms;

    test_exporter exporter(cfg, test_resource(), "/v1/test", &encode, make_recording_poster(state));

    exporter.push(test_record{1});

    if (!wait_for_calls(*state, 1, 2000ms)) {
        std::cerr << "  x interval-triggered flush did not happen\n";
        return false;
    }

    std::cout << "  OK\n";
    return true;
}

// Requirement 3.5: a retryable failure (503) is retried with backoff up to
// max_retries before giving up; a batch that eventually succeeds is not
// counted as dropped.
auto test_retry_then_success() -> bool {
    std::cout << "Test: retryable failures are retried, then succeed\n";
    auto state = std::make_shared<recording_poster_state>();
    state->fail_first_n_calls = 2;  // 3rd call succeeds

    kythira::otlp_export_config cfg;
    cfg.endpoint_base_url = "http://localhost:1";
    cfg.max_batch_size = 1;
    cfg.flush_interval = 10s;
    cfg.max_retries = 3;
    cfg.retry_backoff_base = 10ms;

    test_exporter exporter(cfg, test_resource(), "/v1/test", &encode, make_recording_poster(state));

    exporter.push(test_record{1});

    if (!wait_for_calls(*state, 3, 2000ms)) {
        std::cerr << "  x expected 3 poster calls (2 failures + 1 success), got "
                  << state->call_count << "\n";
        return false;
    }
    if (exporter.dropped_record_count() != 0) {
        std::cerr << "  x a batch that eventually succeeded must not be counted as dropped\n";
        return false;
    }

    std::cout << "  OK\n";
    return true;
}

// Requirement 3.5: a non-retryable status is dropped after exactly one
// attempt, no retry traffic generated.
auto test_non_retryable_status_dropped_without_retry() -> bool {
    std::cout << "Test: non-retryable status (400) dropped without retry\n";
    auto call_count = std::make_shared<std::atomic<unsigned>>(0);

    kythira::otlp_export_config cfg;
    cfg.endpoint_base_url = "http://localhost:1";
    cfg.max_batch_size = 1;
    cfg.flush_interval = 10s;
    cfg.max_retries = 3;
    cfg.retry_backoff_base = 10ms;

    kythira::http_poster_fn poster =
        [call_count](std::string_view, std::string_view,
                     const std::vector<std::pair<std::string, std::string>>&, std::string_view,
                     std::chrono::milliseconds) -> kythira::http_post_result {
        call_count->fetch_add(1, std::memory_order_relaxed);
        return {.ok = false, .status = 400};
    };

    test_exporter exporter(cfg, test_resource(), "/v1/test", &encode, poster);
    exporter.push(test_record{1});

    // Give it well beyond what a (mistaken) retry loop would need.
    std::this_thread::sleep_for(300ms);

    if (call_count->load() != 1) {
        std::cerr << "  x expected exactly 1 poster call for a non-retryable status, got "
                  << call_count->load() << "\n";
        return false;
    }
    if (exporter.dropped_record_count() != 1) {
        std::cerr << "  x expected the record to be counted as dropped\n";
        return false;
    }

    std::cout << "  OK\n";
    return true;
}

// Requirement 3.6: destructor performs one final best-effort flush of
// whatever is still queued, even though neither max_batch_size nor
// flush_interval would otherwise have triggered it yet.
auto test_destructor_flushes_and_joins() -> bool {
    std::cout << "Test: destructor performs a final flush and joins cleanly\n";
    auto state = std::make_shared<recording_poster_state>();

    kythira::otlp_export_config cfg;
    cfg.endpoint_base_url = "http://localhost:1";
    cfg.max_batch_size = 1000;  // would never trigger on its own
    cfg.flush_interval = 60s;   // would never trigger within this test either

    {
        test_exporter exporter(cfg, test_resource(), "/v1/test", &encode,
                               make_recording_poster(state));
        exporter.push(test_record{1});
        exporter.push(test_record{2});
    }  // destructor runs here — must flush before returning.

    std::lock_guard<std::mutex> lock(state->mu);
    if (state->call_count != 1) {
        std::cerr << "  x expected exactly 1 final-flush POST, got " << state->call_count << "\n";
        return false;
    }

    std::cout << "  OK\n";
    return true;
}

}  // namespace

auto main() -> int {
    std::cout << "Testing otlp_http_batch_exporter mechanics\n" << std::string(60, '=') << "\n\n";

    int failed = 0;
    auto run = [&](bool (*test)()) {
        if (!test()) ++failed;
    };

    run(test_drop_oldest_overflow);
    run(test_batch_triggers_on_size);
    run(test_batch_triggers_on_interval);
    run(test_retry_then_success);
    run(test_non_retryable_status_dropped_without_retry);
    run(test_destructor_flushes_and_joins);

    std::cout << std::string(60, '=') << "\n";
    if (failed > 0) {
        std::cerr << failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "All tests passed!\n";
    return 0;
}

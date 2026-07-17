#include "otlp_test_support.hpp"

#include <raft/otlp_logger.hpp>

#include <boost/json.hpp>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using otlp_test_support::fast_test_config;
using otlp_test_support::make_recording_poster;
using otlp_test_support::recording_poster_state;
using otlp_test_support::test_resource;
using otlp_test_support::wait_for_calls;

namespace {

auto first_log_record(const std::string& body) -> boost::json::object {
    auto parsed = boost::json::parse(body);
    const auto& root = parsed.as_object();
    const auto& resource_logs = root.at("resourceLogs").as_array();
    const auto& scope_logs = resource_logs.at(0).as_object().at("scopeLogs").as_array();
    const auto& log_records = scope_logs.at(0).as_object().at("logRecords").as_array();
    return log_records.at(0).as_object();
}

auto test_severity_mapping() -> bool {
    std::cout << "Test: log_level -> OTLP SeverityNumber/SeverityText\n";
    auto state = std::make_shared<recording_poster_state>();
    kythira::otlp_logger logger(fast_test_config(), test_resource(), kythira::log_level::trace,
                                make_recording_poster(state));

    struct expectation {
        kythira::log_level level;
        int number;
        const char* text;
    };
    const expectation cases[] = {
        {kythira::log_level::trace, 1, "TRACE"},   {kythira::log_level::debug, 5, "DEBUG"},
        {kythira::log_level::info, 9, "INFO"},     {kythira::log_level::warning, 13, "WARN"},
        {kythira::log_level::error, 17, "ERROR"},  {kythira::log_level::critical, 21, "FATAL"},
    };

    std::size_t expected_calls = 0;
    for (const auto& c : cases) {
        logger.log(c.level, "test message");
        ++expected_calls;
        if (!wait_for_calls(*state, expected_calls, 2000ms)) {
            std::cerr << "  x timed out waiting for level " << c.text << "\n";
            return false;
        }
        std::lock_guard<std::mutex> lock(state->mu);
        auto record = first_log_record(state->bodies.back());
        if (record.at("severityNumber").as_int64() != c.number) {
            std::cerr << "  x wrong severityNumber for " << c.text << "\n";
            return false;
        }
        if (record.at("severityText").as_string() != c.text) {
            std::cerr << "  x wrong severityText for " << c.text << "\n";
            return false;
        }
        if (record.at("body").at("stringValue").as_string() != "test message") {
            std::cerr << "  x wrong body\n";
            return false;
        }
    }

    std::cout << "  OK\n";
    return true;
}

auto test_structured_attributes_preserve_order() -> bool {
    std::cout << "Test: structured log() maps key_value_pairs to attributes, order preserved\n";
    auto state = std::make_shared<recording_poster_state>();
    kythira::otlp_logger logger(fast_test_config(), test_resource(), kythira::log_level::trace,
                                make_recording_poster(state));

    std::vector<std::pair<std::string_view, std::string_view>> kv{
        {"node_id", "1"}, {"term", "3"}, {"reason", "not_leader"}};
    logger.info("command rejected", kv);

    if (!wait_for_calls(*state, 1, 2000ms)) {
        std::cerr << "  x timed out waiting for structured log export\n";
        return false;
    }

    std::lock_guard<std::mutex> lock(state->mu);
    auto record = first_log_record(state->bodies.back());
    const auto& attrs = record.at("attributes").as_array();
    if (attrs.size() != kv.size()) {
        std::cerr << "  x expected " << kv.size() << " attributes, got " << attrs.size() << "\n";
        return false;
    }
    for (std::size_t i = 0; i < kv.size(); ++i) {
        const auto& attr = attrs.at(i).as_object();
        if (attr.at("key").as_string() != kv[i].first ||
            attr.at("value").at("stringValue").as_string() != kv[i].second) {
            std::cerr << "  x attribute " << i << " mismatch or out of order\n";
            return false;
        }
    }

    std::cout << "  OK\n";
    return true;
}

auto test_min_level_filter_suppresses_below_threshold() -> bool {
    std::cout << "Test: set_min_level suppresses lower-severity calls\n";
    auto state = std::make_shared<recording_poster_state>();
    kythira::otlp_logger logger(fast_test_config(), test_resource(), kythira::log_level::warning,
                                make_recording_poster(state));

    logger.debug("should be suppressed");
    logger.info("should be suppressed too");
    if (wait_for_calls(*state, 1, 150ms)) {
        std::cerr << "  x a call below min_level was exported\n";
        return false;
    }

    logger.error("should go through");
    if (!wait_for_calls(*state, 1, 2000ms)) {
        std::cerr << "  x a call at/above min_level was not exported\n";
        return false;
    }

    std::cout << "  OK\n";
    return true;
}

auto test_concept_conformance() -> bool {
    std::cout << "Test: otlp_logger satisfies kythira::diagnostic_logger\n";
    static_assert(kythira::diagnostic_logger<kythira::otlp_logger>,
                  "otlp_logger must satisfy diagnostic_logger concept");
    std::cout << "  OK\n";
    return true;
}

}  // namespace

auto main() -> int {
    std::cout << "Testing otlp_logger implementation\n" << std::string(60, '=') << "\n\n";

    int failed = 0;
    auto run = [&](bool (*test)()) {
        if (!test()) ++failed;
    };

    run(test_severity_mapping);
    run(test_structured_attributes_preserve_order);
    run(test_min_level_filter_suppresses_below_threshold);
    run(test_concept_conformance);

    std::cout << std::string(60, '=') << "\n";
    if (failed > 0) {
        std::cerr << failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "All tests passed!\n";
    return 0;
}

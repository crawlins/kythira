// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// serialized_metrics (include/raft/metrics.hpp): many threads building and
// emitting metrics on one object, over a back-end that is not thread-safe,
// each emitted metric whole and unmixed.

#define BOOST_TEST_MODULE serialized_metrics_test
#include <boost/test/unit_test.hpp>

#include <raft/metrics.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

struct recorded {
    std::string name;
    std::map<std::string, std::string> dimensions;
    std::int64_t count = 0;
    double value = 0;
    std::chrono::nanoseconds duration{0};
};

// Builds one pending metric with no locking at all, like the test
// recorders: two threads building at once on it would corrupt it.
class unsafe_backend {
public:
    explicit unsafe_backend(std::shared_ptr<std::vector<recorded>> out) : _out(std::move(out)) {}
    auto set_metric_name(std::string_view n) -> void { _pending.name = std::string(n); }
    auto add_dimension(std::string_view k, std::string_view v) -> void {
        _pending.dimensions[std::string(k)] = std::string(v);
    }
    auto add_one() -> void { ++_pending.count; }
    auto add_count(std::int64_t c) -> void { _pending.count += c; }
    auto add_duration(std::chrono::nanoseconds d) -> void { _pending.duration = d; }
    auto add_value(double v) -> void { _pending.value = v; }
    auto emit() -> void {
        _out->push_back(_pending);
        _pending = {};
    }

private:
    std::shared_ptr<std::vector<recorded>> _out;
    recorded _pending;
};

static_assert(kythira::metrics<unsafe_backend>);

}  // namespace

BOOST_AUTO_TEST_CASE(noop_metrics_stays_unwrapped) {
    static_assert(
        std::is_same_v<kythira::node_metrics_t<kythira::noop_metrics>, kythira::noop_metrics>);
    static_assert(std::is_same_v<kythira::node_metrics_t<unsafe_backend>,
                                 kythira::serialized_metrics<unsafe_backend>>);
}

BOOST_AUTO_TEST_CASE(every_kind_of_value_reaches_the_backend) {
    auto out = std::make_shared<std::vector<recorded>>();
    kythira::serialized_metrics<unsafe_backend> m{unsafe_backend{out}};
    m.set_metric_name("a");
    m.add_dimension("k", "v");
    m.add_one();
    m.add_count(4);
    m.add_value(2.5);
    m.add_duration(std::chrono::nanoseconds{7});
    m.emit();
    BOOST_REQUIRE_EQUAL(out->size(), 1U);
    BOOST_TEST(out->at(0).name == "a");
    BOOST_TEST(out->at(0).dimensions.at("k") == "v");
    BOOST_TEST(out->at(0).count == 5);
    BOOST_TEST(out->at(0).value == 2.5);
    BOOST_TEST(out->at(0).duration.count() == 7);
}

BOOST_AUTO_TEST_CASE(an_unemitted_metric_is_dropped_by_the_next_one) {
    auto out = std::make_shared<std::vector<recorded>>();
    kythira::serialized_metrics<unsafe_backend> m{unsafe_backend{out}};
    m.set_metric_name("abandoned");
    m.add_dimension("stale", "yes");
    m.set_metric_name("b");
    m.add_one();
    m.emit();
    BOOST_REQUIRE_EQUAL(out->size(), 1U);
    BOOST_TEST(out->at(0).name == "b");
    BOOST_TEST(out->at(0).dimensions.empty());
}

BOOST_AUTO_TEST_CASE(concurrent_threads_emit_whole_unmixed_metrics,
                     *boost::unit_test::timeout(60)) {
    constexpr int threads = 8;
    constexpr int per_thread = 5000;
    auto out = std::make_shared<std::vector<recorded>>();
    kythira::serialized_metrics<unsafe_backend> m{unsafe_backend{out}};
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&m, t] {
            const auto me = std::to_string(t);
            for (int i = 0; i < per_thread; ++i) {
                m.set_metric_name("thread_" + me);
                m.add_dimension("thread", me);
                m.add_dimension("seq", std::to_string(i));
                m.add_count(t);
                m.emit();
            }
        });
    }
    for (auto& th : pool) {
        th.join();
    }
    BOOST_REQUIRE_EQUAL(out->size(), static_cast<std::size_t>(threads * per_thread));
    std::vector<int> next(threads, 0);
    for (const auto& r : *out) {
        const auto t = std::stoi(r.dimensions.at("thread"));
        BOOST_REQUIRE(r.name == "thread_" + std::to_string(t));
        BOOST_REQUIRE_EQUAL(r.dimensions.size(), 2U);
        BOOST_REQUIRE_EQUAL(r.count, t);
        // Each thread's metrics arrive in the order it emitted them.
        BOOST_REQUIRE_EQUAL(std::stoi(r.dimensions.at("seq")), next[t]);
        ++next[t];
    }
}

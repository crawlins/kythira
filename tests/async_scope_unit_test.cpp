/// @file async_scope_unit_test.cpp
/// @brief Unit tests for `kythira::async_scope`.
///
/// The property under test is not "the counter goes up and down" — it is the
/// one thing a stop flag cannot promise: **after `close_and_drain()` returns,
/// no guarded body is executing and none can start.** Most of these cases
/// therefore assert on ordering across threads rather than on return values,
/// because a scope that merely counted correctly would still be useless.

#define BOOST_TEST_MODULE async_scope_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/async_scope.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using kythira::async_scope;

BOOST_AUTO_TEST_SUITE(async_scope_unit)

BOOST_AUTO_TEST_CASE(an_open_scope_admits) {
    async_scope scope;
    BOOST_CHECK(!scope.closed());
    BOOST_CHECK_EQUAL(scope.in_flight(), 0u);

    {
        auto t = scope.enter();
        BOOST_REQUIRE(t.has_value());
        BOOST_CHECK_EQUAL(scope.in_flight(), 1u);
    }
    BOOST_CHECK_EQUAL(scope.in_flight(), 0u);
}

BOOST_AUTO_TEST_CASE(tickets_nest_and_unwind_in_any_order) {
    async_scope scope;
    auto a = scope.enter();
    auto b = scope.enter();
    BOOST_REQUIRE(a.has_value());
    BOOST_REQUIRE(b.has_value());
    BOOST_CHECK_EQUAL(scope.in_flight(), 2u);

    a.reset();
    BOOST_CHECK_EQUAL(scope.in_flight(), 1u);
    b.reset();
    BOOST_CHECK_EQUAL(scope.in_flight(), 0u);
}

/// The refusal half of the contract. A continuation scheduled before shutdown
/// but running after it must be turned away rather than allowed to touch a
/// destroyed owner — this is the retry case that produced the original bug.
BOOST_AUTO_TEST_CASE(a_closed_scope_refuses) {
    async_scope scope;
    scope.close_and_drain();

    BOOST_CHECK(scope.closed());
    auto t = scope.enter();
    BOOST_CHECK(!t.has_value());
}

BOOST_AUTO_TEST_CASE(closing_twice_is_not_an_error) {
    async_scope scope;
    scope.close_and_drain();
    scope.close_and_drain();  // a stop() that runs twice must not hang or throw
    BOOST_CHECK(scope.closed());
}

/// **The load-bearing test.** A drain must not return while a body is running.
/// Asserted by having the body set a flag *after* a sleep and checking it from
/// the draining thread the instant the drain returns: if the drain returned
/// early, the flag is still false and the owner would have been destroyed under
/// a live callback.
BOOST_AUTO_TEST_CASE(drain_waits_for_a_body_already_running) {
    async_scope scope;
    std::atomic<bool> body_finished{false};
    std::atomic<bool> body_entered{false};

    // No Boost.Test assertion runs off the main thread anywhere in this file:
    // the framework's assertion state is not thread-safe, and a REQUIRE that
    // fires on a worker aborts the process instead of reporting a failure —
    // which is how a real regression would arrive as an unreadable SIGABRT.
    // Workers record into atomics; the main thread does the asserting.
    std::atomic<bool> body_was_admitted{false};
    std::thread worker([&] {
        auto t = scope.enter();
        body_was_admitted.store(t.has_value());
        if (!t) {
            body_entered.store(true);
            return;
        }
        body_entered.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds{300});
        body_finished.store(true);
    });

    // Only start draining once the body is genuinely inside, so this tests the
    // drain rather than a race to be first.
    while (!body_entered.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    scope.close_and_drain();
    BOOST_CHECK(body_was_admitted.load());
    BOOST_CHECK(body_finished.load());
    BOOST_CHECK_EQUAL(scope.in_flight(), 0u);

    worker.join();
}

/// The race the whole type exists for: `enter()` and `close_and_drain()` called
/// concurrently. Each worker either is refused, or is admitted *and* held
/// inside long enough that a drain which failed to wait would demonstrably
/// return while it was still there.
///
/// The invariant is asserted as `in_flight() == 0` **the instant the drain
/// returns**, which is the property an owner relies on when it destroys itself
/// next. An earlier version of this test instead counted admissions that
/// happened after the drain returned; it passed against a deliberately
/// non-waiting drain, because that window is vanishingly small and the bodies
/// held nothing. Mutation testing is what caught it — the barrier was removed
/// and this case still went green.
BOOST_AUTO_TEST_CASE(drain_returns_only_when_nothing_is_inside) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        async_scope scope;
        std::atomic<int> bodies_running{0};
        std::atomic<int> observed_inside_at_drain{-1};

        std::vector<std::thread> workers;
        workers.reserve(4);
        for (int i = 0; i < 4; ++i) {
            workers.emplace_back([&] {
                auto t = scope.enter();
                if (!t.has_value()) {
                    return;  // refused: the other acceptable outcome
                }
                bodies_running.fetch_add(1);
                // Long enough that a non-waiting drain returns mid-body.
                std::this_thread::sleep_for(std::chrono::milliseconds{20});
                bodies_running.fetch_sub(1);
            });
        }

        scope.close_and_drain();
        // Sampled before joining, so this reflects the moment the drain
        // returned rather than the moment the workers happened to finish.
        observed_inside_at_drain.store(static_cast<int>(scope.in_flight()));
        const auto still_running = bodies_running.load();

        for (auto& w : workers) {
            w.join();
        }

        BOOST_REQUIRE_EQUAL(observed_inside_at_drain.load(), 0);
        BOOST_REQUIRE_EQUAL(still_running, 0);
    }
}

/// A ticket must not be able to outlive the drain by being moved somewhere
/// else. Moving transfers the admission; it does not mint a second one.
BOOST_AUTO_TEST_CASE(moving_a_ticket_transfers_rather_than_duplicates) {
    async_scope scope;
    auto a = scope.enter();
    BOOST_REQUIRE(a.has_value());
    BOOST_CHECK_EQUAL(scope.in_flight(), 1u);

    auto b = std::move(a);
    BOOST_CHECK_EQUAL(scope.in_flight(), 1u);

    b.reset();
    BOOST_CHECK_EQUAL(scope.in_flight(), 0u);
}

/// A slow drain has to be *visible*. Silence during a stuck shutdown is
/// indistinguishable from a hang, and this codebase's recurring failure is
/// machinery that reports nothing while doing nothing.
BOOST_AUTO_TEST_CASE(a_slow_drain_reports_while_it_is_still_stuck) {
    async_scope scope;
    std::atomic<bool> reported{false};
    std::atomic<bool> release_body{false};

    std::atomic<bool> admitted{false};
    std::thread worker([&] {
        auto t = scope.enter();
        admitted.store(t.has_value());
        if (!t) {
            return;
        }
        while (!release_body.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    });

    // The scope must be occupied before the drain starts, or this measures a
    // race to be first rather than the reporting behaviour.
    while (scope.in_flight() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    std::thread releaser([&] {
        // Long enough to trip the threshold below, then let the body go.
        std::this_thread::sleep_for(std::chrono::milliseconds{250});
        release_body.store(true);
    });

    scope.close_and_drain([&](std::chrono::milliseconds) { reported.store(true); },
                          std::chrono::milliseconds{50});

    BOOST_CHECK(admitted.load());
    BOOST_CHECK(reported.load());

    worker.join();
    releaser.join();
}

/// A drain with nothing in flight must not block on its own threshold.
BOOST_AUTO_TEST_CASE(an_idle_drain_returns_immediately_and_reports_nothing) {
    async_scope scope;
    std::atomic<bool> reported{false};

    const auto started = std::chrono::steady_clock::now();
    scope.close_and_drain([&](std::chrono::milliseconds) { reported.store(true); },
                          std::chrono::milliseconds{500});
    const auto elapsed = std::chrono::steady_clock::now() - started;

    BOOST_CHECK(!reported.load());
    BOOST_CHECK(elapsed < std::chrono::milliseconds{400});
}

BOOST_AUTO_TEST_SUITE_END()

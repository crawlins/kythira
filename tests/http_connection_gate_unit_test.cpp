// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE http_connection_gate_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/http_connection_gate.hpp>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

// The counter behind max_concurrent_connections on the Beast and Proxygen
// servers (.kiro/specs/http-server-request-limits/, task 2.2). No transport
// dependency, so it runs on every leg.

using kythira::http_detail::connection_gate;

BOOST_AUTO_TEST_SUITE(http_connection_gate_tests)

BOOST_AUTO_TEST_CASE(zero_limit_is_rejected) {
    BOOST_CHECK_THROW(connection_gate{0}, std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(acquires_up_to_the_limit_then_refuses) {
    auto gate = std::make_shared<connection_gate>(2);
    auto a = gate->try_acquire();
    auto b = gate->try_acquire();
    BOOST_TEST(static_cast<bool>(a));
    BOOST_TEST(static_cast<bool>(b));
    BOOST_TEST(gate->live() == 2u);

    auto c = gate->try_acquire();
    BOOST_TEST(!static_cast<bool>(c));
    BOOST_TEST(gate->live() == 2u);
    BOOST_TEST(gate->refused() == 1u);
}

BOOST_AUTO_TEST_CASE(releasing_a_slot_admits_the_next) {
    auto gate = std::make_shared<connection_gate>(1);
    {
        auto a = gate->try_acquire();
        BOOST_TEST(static_cast<bool>(a));
        BOOST_TEST(!static_cast<bool>(gate->try_acquire()));
    }
    BOOST_TEST(gate->live() == 0u);
    auto b = gate->try_acquire();
    BOOST_TEST(static_cast<bool>(b));
}

BOOST_AUTO_TEST_CASE(release_is_idempotent) {
    auto gate = std::make_shared<connection_gate>(2);
    auto a = gate->try_acquire();
    a.release();
    a.release();
    BOOST_TEST(!static_cast<bool>(a));
    BOOST_TEST(gate->live() == 0u);
}

BOOST_AUTO_TEST_CASE(moving_a_slot_transfers_it) {
    auto gate = std::make_shared<connection_gate>(2);
    auto a = gate->try_acquire();
    auto b = std::move(a);
    BOOST_TEST(!static_cast<bool>(a));
    BOOST_TEST(static_cast<bool>(b));
    BOOST_TEST(gate->live() == 1u);

    // Move-assigning over a held slot releases the old one first.
    auto c = gate->try_acquire();
    BOOST_TEST(gate->live() == 2u);
    c = std::move(b);
    BOOST_TEST(gate->live() == 1u);
    c = {};
    BOOST_TEST(gate->live() == 0u);
}

// Requirement 2.6: a connection torn down after its server is gone must not
// touch freed memory. The slot owns the gate, so dropping every other
// reference first is safe.
BOOST_AUTO_TEST_CASE(slot_keeps_its_gate_alive) {
    std::weak_ptr<connection_gate> watch;
    connection_gate::slot held;
    {
        auto gate = std::make_shared<connection_gate>(1);
        watch = gate;
        held = gate->try_acquire();
    }
    BOOST_TEST(!watch.expired());
    BOOST_TEST(watch.lock()->live() == 1u);
    held = {};
    BOOST_TEST(watch.expired());
}

// The limit holds under contention: many threads acquiring and releasing at
// once never see more than `limit` slots live.
BOOST_AUTO_TEST_CASE(concurrent_acquire_never_exceeds_the_limit) {
    constexpr std::size_t limit = 4;
    auto gate = std::make_shared<connection_gate>(limit);
    std::atomic<std::size_t> holding{0};
    std::atomic<std::size_t> high_water{0};
    std::atomic<std::size_t> admitted{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 20000; ++i) {
                auto s = gate->try_acquire();
                if (!s) {
                    continue;
                }
                admitted.fetch_add(1);
                auto now = holding.fetch_add(1) + 1;
                auto seen = high_water.load();
                while (now > seen && !high_water.compare_exchange_weak(seen, now)) {
                }
                holding.fetch_sub(1);
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    BOOST_TEST(high_water.load() <= limit);
    BOOST_TEST(admitted.load() > 0u);
    BOOST_TEST(gate->live() == 0u);
}

BOOST_AUTO_TEST_SUITE_END()

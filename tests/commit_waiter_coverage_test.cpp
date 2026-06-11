#define BOOST_TEST_MODULE commit_waiter_coverage_test
#include <boost/test/unit_test.hpp>

#include <raft/commit_waiter.hpp>
#include <raft/completion_exceptions.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

BOOST_AUTO_TEST_SUITE(basic_operations)

BOOST_AUTO_TEST_CASE(register_and_fulfill) {
    kythira::commit_waiter<std::uint64_t> cw;
    BOOST_CHECK(!cw.has_pending_operations());
    BOOST_CHECK_EQUAL(cw.get_pending_count(), 0u);

    std::vector<std::byte> received;
    std::exception_ptr rejected;

    cw.register_operation(
        1, [&](std::vector<std::byte> r) { received = std::move(r); },
        [&](const std::exception_ptr& e) { rejected = e; });

    BOOST_CHECK(cw.has_pending_operations());
    BOOST_CHECK_EQUAL(cw.get_pending_count(), 1u);
    BOOST_CHECK_EQUAL(cw.get_pending_count_for_index(1), 1u);
    BOOST_CHECK_EQUAL(cw.get_pending_count_for_index(99), 0u);

    cw.notify_committed_and_applied(1);

    BOOST_CHECK(!cw.has_pending_operations());
    BOOST_CHECK_EQUAL(cw.get_pending_count(), 0u);
    BOOST_CHECK(!rejected);
    BOOST_CHECK(received.empty());  // default notify returns empty result
}

BOOST_AUTO_TEST_CASE(fulfill_with_result_function) {
    kythira::commit_waiter<std::uint64_t> cw;

    std::vector<std::byte> received;

    cw.register_operation(
        5, [&](std::vector<std::byte> r) { received = std::move(r); },
        [&](const std::exception_ptr&) {});

    std::vector<std::byte> expected_result = {std::byte{0x41}, std::byte{0x42}};
    cw.notify_committed_and_applied(5, [&](std::uint64_t idx) -> std::vector<std::byte> {
        BOOST_CHECK_EQUAL(idx, 5u);
        return expected_result;
    });

    BOOST_CHECK(received == expected_result);
}

BOOST_AUTO_TEST_CASE(fulfill_with_result_function_throws) {
    kythira::commit_waiter<std::uint64_t> cw;

    std::exception_ptr rejected;

    cw.register_operation(
        3, [](const std::vector<std::byte>&) {},
        [&](const std::exception_ptr& e) { rejected = e; });

    cw.notify_committed_and_applied(3, [](std::uint64_t) -> std::vector<std::byte> {
        throw std::runtime_error("state machine error");
    });

    BOOST_CHECK(rejected != nullptr);
    BOOST_CHECK_THROW(std::rethrow_exception(rejected), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(multiple_ops_same_index) {
    kythira::commit_waiter<std::uint64_t> cw;

    int fulfilled = 0;
    for (int i = 0; i < 3; ++i) {
        cw.register_operation(
            10, [&](const std::vector<std::byte>&) { ++fulfilled; },
            [](const std::exception_ptr&) {});
    }

    BOOST_CHECK_EQUAL(cw.get_pending_count(), 3u);
    BOOST_CHECK_EQUAL(cw.get_pending_count_for_index(10), 3u);

    cw.notify_committed_and_applied(10);

    BOOST_CHECK_EQUAL(fulfilled, 3);
    BOOST_CHECK_EQUAL(cw.get_pending_count(), 0u);
}

BOOST_AUTO_TEST_CASE(notify_covers_multiple_indices) {
    kythira::commit_waiter<std::uint64_t> cw;

    int fulfilled = 0;
    cw.register_operation(1, [&](auto) { ++fulfilled; }, [](auto) {});
    cw.register_operation(2, [&](auto) { ++fulfilled; }, [](auto) {});
    cw.register_operation(3, [&](auto) { ++fulfilled; }, [](auto) {});

    // Notify up to index 2 — should fulfill indices 1 and 2 but not 3
    cw.notify_committed_and_applied(2);
    BOOST_CHECK_EQUAL(fulfilled, 2);
    BOOST_CHECK_EQUAL(cw.get_pending_count(), 1u);

    cw.notify_committed_and_applied(3);
    BOOST_CHECK_EQUAL(fulfilled, 3);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(cancel_operations)

BOOST_AUTO_TEST_CASE(cancel_all) {
    kythira::commit_waiter<std::uint64_t> cw;

    int rejected = 0;
    cw.register_operation(1, [](auto) {}, [&](auto) { ++rejected; });
    cw.register_operation(2, [](auto) {}, [&](auto) { ++rejected; });
    cw.register_operation(2, [](auto) {}, [&](auto) { ++rejected; });

    cw.cancel_all_operations("shutdown");

    BOOST_CHECK_EQUAL(rejected, 3);
    BOOST_CHECK(!cw.has_pending_operations());
}

BOOST_AUTO_TEST_CASE(cancel_all_leadership_lost) {
    kythira::commit_waiter<std::uint64_t> cw;

    std::exception_ptr captured;
    cw.register_operation(5, [](auto) {}, [&](const std::exception_ptr& e) { captured = e; });

    cw.cancel_all_operations_leadership_lost<std::uint64_t>(3, 4);

    BOOST_CHECK(captured != nullptr);
    bool is_leadership_lost = false;
    try {
        std::rethrow_exception(captured);
    } catch (const kythira::leadership_lost_exception<std::uint64_t>&) {
        is_leadership_lost = true;
    } catch (...) {  // NOLINT(bugprone-empty-catch)
    }
    BOOST_CHECK(is_leadership_lost);
}

BOOST_AUTO_TEST_CASE(cancel_timed_out_operations) {
    kythira::commit_waiter<std::uint64_t> cw;

    int rejected = 0;
    int fulfilled = 0;

    // Register one that immediately times out (1ms timeout)
    cw.register_operation(
        1, [&](auto) { ++fulfilled; }, [&](auto) { ++rejected; }, std::chrono::milliseconds{1});

    // Register one with no timeout
    cw.register_operation(2, [&](auto) { ++fulfilled; }, [&](auto) { ++rejected; }, std::nullopt);

    // Register one with long timeout (not timed out yet)
    cw.register_operation(
        3, [&](auto) { ++fulfilled; }, [&](auto) { ++rejected; }, std::chrono::milliseconds{60000});

    std::this_thread::sleep_for(std::chrono::milliseconds{10});

    auto cancelled = cw.cancel_timed_out_operations();
    BOOST_CHECK_EQUAL(cancelled, 1u);
    BOOST_CHECK_EQUAL(rejected, 1);
    BOOST_CHECK_EQUAL(fulfilled, 0);
    BOOST_CHECK_EQUAL(cw.get_pending_count(), 2u);
}

BOOST_AUTO_TEST_CASE(cancel_timed_out_no_timeouts) {
    kythira::commit_waiter<std::uint64_t> cw;

    cw.register_operation(1, [](auto) {}, [](auto) {}, std::chrono::milliseconds{60000});

    auto cancelled = cw.cancel_timed_out_operations();
    BOOST_CHECK_EQUAL(cancelled, 0u);
    BOOST_CHECK_EQUAL(cw.get_pending_count(), 1u);
}

BOOST_AUTO_TEST_CASE(cancel_timed_out_all_expire) {
    kythira::commit_waiter<std::uint64_t> cw;

    int rejected = 0;
    cw.register_operation(1, [](auto) {}, [&](auto) { ++rejected; }, std::chrono::milliseconds{1});
    cw.register_operation(2, [](auto) {}, [&](auto) { ++rejected; }, std::chrono::milliseconds{1});

    std::this_thread::sleep_for(std::chrono::milliseconds{10});

    auto cancelled = cw.cancel_timed_out_operations();
    BOOST_CHECK_EQUAL(cancelled, 2u);
    BOOST_CHECK_EQUAL(rejected, 2);
    BOOST_CHECK(!cw.has_pending_operations());
}

BOOST_AUTO_TEST_CASE(cancel_operations_for_index_found) {
    kythira::commit_waiter<std::uint64_t> cw;

    int rejected = 0;
    cw.register_operation(7, [](auto) {}, [&](auto) { ++rejected; });
    cw.register_operation(7, [](auto) {}, [&](auto) { ++rejected; });
    cw.register_operation(8, [](auto) {}, [&](auto) { ++rejected; });

    auto cancelled = cw.cancel_operations_for_index(7, "test cancel");
    BOOST_CHECK_EQUAL(cancelled, 2u);
    BOOST_CHECK_EQUAL(rejected, 2);
    BOOST_CHECK_EQUAL(cw.get_pending_count(), 1u);
}

BOOST_AUTO_TEST_CASE(cancel_operations_for_index_not_found) {
    kythira::commit_waiter<std::uint64_t> cw;

    cw.register_operation(5, [](auto) {}, [](auto) {});

    auto cancelled = cw.cancel_operations_for_index(99, "not found");
    BOOST_CHECK_EQUAL(cancelled, 0u);
    BOOST_CHECK_EQUAL(cw.get_pending_count(), 1u);
}

BOOST_AUTO_TEST_CASE(cancel_operations_after_index) {
    kythira::commit_waiter<std::uint64_t> cw;

    int rejected = 0;
    cw.register_operation(1, [](auto) {}, [&](auto) { ++rejected; });
    cw.register_operation(2, [](auto) {}, [&](auto) { ++rejected; });
    cw.register_operation(5, [](auto) {}, [&](auto) { ++rejected; });
    cw.register_operation(10, [](auto) {}, [&](auto) { ++rejected; });

    // Cancel all ops with index > 2
    auto cancelled = cw.cancel_operations_after_index(2, "log truncation");
    BOOST_CHECK_EQUAL(cancelled, 2u);  // indices 5 and 10
    BOOST_CHECK_EQUAL(rejected, 2);
    BOOST_CHECK_EQUAL(cw.get_pending_count(), 2u);  // indices 1 and 2 remain
}

BOOST_AUTO_TEST_CASE(cancel_operations_after_index_nothing_after) {
    kythira::commit_waiter<std::uint64_t> cw;

    cw.register_operation(1, [](auto) {}, [](auto) {});
    cw.register_operation(2, [](auto) {}, [](auto) {});

    auto cancelled = cw.cancel_operations_after_index(100, "no-op");
    BOOST_CHECK_EQUAL(cancelled, 0u);
    BOOST_CHECK_EQUAL(cw.get_pending_count(), 2u);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(empty_state)

BOOST_AUTO_TEST_CASE(cancel_all_empty) {
    kythira::commit_waiter<std::uint64_t> cw;
    BOOST_CHECK_NO_THROW(cw.cancel_all_operations("nothing"));
}

BOOST_AUTO_TEST_CASE(cancel_timed_out_empty) {
    kythira::commit_waiter<std::uint64_t> cw;
    auto cancelled = cw.cancel_timed_out_operations();
    BOOST_CHECK_EQUAL(cancelled, 0u);
}

BOOST_AUTO_TEST_CASE(notify_empty) {
    kythira::commit_waiter<std::uint64_t> cw;
    BOOST_CHECK_NO_THROW(cw.notify_committed_and_applied(100));
}

BOOST_AUTO_TEST_SUITE_END()

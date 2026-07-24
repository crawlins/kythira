/**
 * @file migration_guide_example.cpp
 * @brief Side-by-side comparison of the Folly-backed (default) and
 *     boost::thread-backed future implementations, for anyone deciding
 *     whether to reach for `kythira::boost_backend` types in new code.
 *
 * This is a comparison, not a migration path: no existing kythira::Future
 * call site is converted to kythira::boost_backend::Future as a result of
 * this example, or of this spec (.kiro/specs/boost-future-backend/) at
 * all — neither the Folly nor stdexec dependency is removed or made
 * optional-only. This file exists purely to make the backends' shape
 * differences concrete for someone choosing between them for new,
 * boost::thread-specific code.
 *
 * Demonstrates, for both backends: basic future creation, chaining,
 * promise/future pairs, error handling, collective operations, and the
 * boost backend's timer-backed within() (no Folly-neutral equivalent
 * demonstrated here, since kythira::Future already has its own via a
 * different mechanism — this shows the boost::asio-backed shape
 * specifically).
 */

#include <raft/future.hpp>
#include <raft/future_boost.hpp>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
constexpr int example_value = 42;
constexpr const char* error_message = "example error";
}  // namespace

auto demonstrate_basic_future_creation() -> bool {
    std::cout << "=== Basic Future Creation ===\n";

    std::cout << "  FOLLY BACKEND (kythira::FutureFactory):\n";
    auto folly_future = kythira::FutureFactory::makeFuture(example_value);
    int folly_result = std::move(folly_future).get();
    std::cout << "    kythira::Future<int> result: " << folly_result << "\n";

    std::cout << "  BOOST BACKEND (kythira::boost_backend::FutureFactory):\n";
    auto boost_future = kythira::boost_backend::FutureFactory::makeFuture(example_value);
    int boost_result = std::move(boost_future).get();
    std::cout << "    kythira::boost_backend::Future<int> result: " << boost_result << "\n";

    bool ok = folly_result == boost_result;
    std::cout << (ok ? "  ✓ Both backends produce equivalent results\n" : "  ✗ Results diverged\n");
    return ok;
}

auto demonstrate_chaining() -> bool {
    std::cout << "\n=== Continuation Chaining ===\n";

    std::cout << "  FOLLY BACKEND (thenValue):\n";
    int folly_result = kythira::FutureFactory::makeFuture(example_value)
                           .thenValue([](int v) { return v * 2; })
                           .get();
    std::cout << "    chained result: " << folly_result << "\n";

    std::cout << "  BOOST BACKEND (thenValue):\n";
    int boost_result = std::move(kythira::boost_backend::FutureFactory::makeFuture(example_value)
                                     .thenValue([](int v) { return v * 2; }))
                           .get();
    std::cout << "    chained result: " << boost_result << "\n";

    bool ok = folly_result == boost_result;
    std::cout << (ok ? "  ✓ Both backends chain equivalently\n" : "  ✗ Results diverged\n");
    return ok;
}

auto demonstrate_promise_future_pair() -> bool {
    std::cout << "\n=== Promise/Future Pair ===\n";

    std::cout << "  FOLLY BACKEND (kythira::Promise):\n";
    kythira::Promise<int> folly_promise;
    auto folly_future = folly_promise.getFuture();
    folly_promise.setValue(example_value);
    int folly_result = folly_future.get();
    std::cout << "    fulfilled result: " << folly_result << "\n";

    std::cout << "  BOOST BACKEND (kythira::boost_backend::Promise):\n";
    kythira::boost_backend::Promise<int> boost_promise;
    auto boost_future = boost_promise.getFuture();
    // boost::promise::set_value(), like folly::Promise, is push-model:
    // callable from arbitrary code, on any thread, at any later time. Here
    // it happens inline for the example, but need not.
    boost_promise.setValue(example_value);
    int boost_result = std::move(boost_future).get();
    std::cout << "    fulfilled result: " << boost_result << "\n";

    bool ok = folly_result == boost_result;
    std::cout << (ok ? "  ✓ Both backends' promise/future pairs match\n"
                     : "  ✗ Results diverged\n");
    return ok;
}

auto demonstrate_error_handling() -> bool {
    std::cout << "\n=== Error Handling ===\n";

    std::cout << "  FOLLY BACKEND (thenError):\n";
    int folly_result = kythira::FutureFactory::makeExceptionalFuture<int>(
                           std::make_exception_ptr(std::runtime_error(error_message)))
                           .thenError([](std::exception_ptr) { return -1; })
                           .get();
    std::cout << "    recovered result: " << folly_result << "\n";

    std::cout << "  BOOST BACKEND (thenError):\n";
    int boost_result = std::move(kythira::boost_backend::FutureFactory::makeExceptionalFuture<int>(
                                     std::make_exception_ptr(std::runtime_error(error_message)))
                                     .thenError([](std::exception_ptr) { return -1; }))
                           .get();
    std::cout << "    recovered result: " << boost_result << "\n";

    bool ok = folly_result == boost_result;
    std::cout << (ok ? "  ✓ Both backends recover from errors equivalently\n"
                     : "  ✗ Results diverged\n");
    return ok;
}

auto demonstrate_collective_operations() -> bool {
    std::cout << "\n=== Collective Operations (collectAll) ===\n";

    std::cout << "  FOLLY BACKEND (kythira::FutureCollector):\n";
    std::vector<kythira::Future<int>> folly_futures;
    folly_futures.push_back(kythira::FutureFactory::makeFuture(1));
    folly_futures.push_back(kythira::FutureFactory::makeFuture(2));
    auto folly_results = kythira::FutureCollector::collectAll(std::move(folly_futures)).get();
    std::cout << "    collected " << folly_results.size() << " results\n";

    std::cout << "  BOOST BACKEND (kythira::boost_backend::FutureCollector):\n";
    std::vector<kythira::boost_backend::Future<int>> boost_futures;
    boost_futures.push_back(kythira::boost_backend::FutureFactory::makeFuture(1));
    boost_futures.push_back(kythira::boost_backend::FutureFactory::makeFuture(2));
    auto boost_results =
        std::move(kythira::boost_backend::FutureCollector::collectAll(std::move(boost_futures)))
            .get();
    std::cout << "    collected " << boost_results.size() << " results\n";

    bool ok = folly_results.size() == boost_results.size() &&
              folly_results[0].value() == boost_results[0].value() &&
              folly_results[1].value() == boost_results[1].value();
    std::cout << (ok ? "  ✓ Both backends collect equivalently\n" : "  ✗ Results diverged\n");
    return ok;
}

auto demonstrate_boost_timer_backed_within() -> bool {
    // within() is timer_service-backed (a shared boost::asio::io_context +
    // background thread), racing the original future's completion against
    // a timeout — code that wants this specific timing primitive without
    // pulling in Folly's Timekeeper is the case where reaching for the
    // boost backend in new code makes sense.
    std::cout << "\n=== boost-Native within() (asio-backed timeout) ===\n";

    int result = std::move(kythira::boost_backend::FutureFactory::makeFuture(example_value)
                               .within(std::chrono::milliseconds(500)))
                     .get();
    std::cout << "    result before timeout: " << result << "\n";

    bool timed_out = false;
    try {
        kythira::boost_backend::Promise<int> never_fulfilled;
        std::move(never_fulfilled.getFuture()).within(std::chrono::milliseconds(50)).get();
    } catch (const std::exception&) {
        timed_out = true;
    }
    std::cout << "    slow future timed out as expected: " << (timed_out ? "yes" : "no") << "\n";

    bool ok = result == example_value && timed_out;
    std::cout << (ok ? "  ✓ within() behaved correctly on both paths\n"
                     : "  ✗ Unexpected result\n");
    return ok;
}

auto main() -> int {
    std::cout << "kythira Future Backend Comparison: Folly vs boost\n";
    std::cout << "====================================================\n";

    bool all_ok = true;
    all_ok &= demonstrate_basic_future_creation();
    all_ok &= demonstrate_chaining();
    all_ok &= demonstrate_promise_future_pair();
    all_ok &= demonstrate_error_handling();
    all_ok &= demonstrate_collective_operations();
    all_ok &= demonstrate_boost_timer_backed_within();

    std::cout << "\n====================================================\n";
    std::cout << (all_ok ? "All comparisons produced equivalent results.\n"
                         : "Some comparisons diverged — see output above.\n");
    return all_ok ? 0 : 1;
}

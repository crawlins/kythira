/**
 * @file migration_guide_example.cpp
 * @brief Side-by-side comparison of the Folly-backed (default) and
 *     stdexec-backed future implementations, for anyone deciding whether
 *     to reach for `kythira::stdexec_backend` types in new code.
 *
 * This is a comparison, not a migration path: unlike
 * examples/migration_guide_example.cpp (old std::future/folly::Future
 * patterns -> kythira::Future), no existing kythira::Future call site is
 * expected to move to kythira::stdexec_backend::Future as a result of this
 * example, or of this spec (.kiro/specs/stdexec-future-backend/) at all —
 * see include/raft/future_stdexec_README.md for the exact scope
 * boundaries. This file exists purely to make the two backends' shape
 * differences concrete for someone choosing between them for new,
 * stdexec-specific code (e.g. code that wants direct access to stdexec
 * schedulers/algorithms rather than going through Folly's executor model).
 *
 * Demonstrates, for both backends: basic future creation, chaining,
 * promise/future pairs, error handling, and collective operations.
 */

#include <raft/future.hpp>
#include <raft/future_stdexec.hpp>

#include <exec/single_thread_context.hpp>

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

    std::cout << "  STDEXEC BACKEND (kythira::stdexec_backend::FutureFactory):\n";
    auto stdexec_future = kythira::stdexec_backend::FutureFactory::makeFuture(example_value);
    int stdexec_result = std::move(stdexec_future).get();
    std::cout << "    kythira::stdexec_backend::Future<int> result: " << stdexec_result << "\n";

    bool ok = folly_result == stdexec_result;
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

    std::cout << "  STDEXEC BACKEND (thenValue):\n";
    int stdexec_result =
        std::move(
            kythira::stdexec_backend::FutureFactory::makeFuture(example_value).thenValue([](int v) {
                return v * 2;
            }))
            .get();
    std::cout << "    chained result: " << stdexec_result << "\n";

    bool ok = folly_result == stdexec_result;
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

    std::cout << "  STDEXEC BACKEND (kythira::stdexec_backend::Promise):\n";
    kythira::stdexec_backend::Promise<int> stdexec_promise;
    auto stdexec_future = stdexec_promise.getFuture();
    // Promise::setValue may be called from arbitrary external code, on any
    // thread, at an arbitrary later time — the entire reason
    // single_shot_channel exists (design.md's central design problem).
    // Here it happens inline for the example, but need not.
    stdexec_promise.setValue(example_value);
    int stdexec_result = std::move(stdexec_future).get();
    std::cout << "    fulfilled result: " << stdexec_result << "\n";

    bool ok = folly_result == stdexec_result;
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

    std::cout << "  STDEXEC BACKEND (thenError):\n";
    int stdexec_result =
        std::move(kythira::stdexec_backend::FutureFactory::makeExceptionalFuture<int>(
                      std::make_exception_ptr(std::runtime_error(error_message)))
                      .thenError([](std::exception_ptr) { return -1; }))
            .get();
    std::cout << "    recovered result: " << stdexec_result << "\n";

    bool ok = folly_result == stdexec_result;
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

    std::cout << "  STDEXEC BACKEND (kythira::stdexec_backend::FutureCollector):\n";
    std::vector<kythira::stdexec_backend::Future<int>> stdexec_futures;
    stdexec_futures.push_back(kythira::stdexec_backend::FutureFactory::makeFuture(1));
    stdexec_futures.push_back(kythira::stdexec_backend::FutureFactory::makeFuture(2));
    auto stdexec_results =
        std::move(kythira::stdexec_backend::FutureCollector::collectAll(std::move(stdexec_futures)))
            .get();
    std::cout << "    collected " << stdexec_results.size() << " results\n";

    bool ok = folly_results.size() == stdexec_results.size() &&
              folly_results[0].value() == stdexec_results[0].value() &&
              folly_results[1].value() == stdexec_results[1].value();
    std::cout << (ok ? "  ✓ Both backends collect equivalently\n" : "  ✗ Results diverged\n");
    return ok;
}

auto demonstrate_stdexec_native_scheduler() -> bool {
    // Code that genuinely wants direct access to an stdexec scheduler
    // (rather than going through Folly's Executor model) is the one case
    // where reaching for the stdexec backend in new code makes sense —
    // via() takes a scheduler_handle wrapping any concrete stdexec
    // scheduler directly, with no Folly Executor equivalent required.
    std::cout << "\n=== stdexec-Native Scheduler (no Folly equivalent) ===\n";

    exec::single_thread_context ctx;
    kythira::stdexec_backend::scheduler_handle handle(ctx.get_scheduler());
    int result = std::move(kythira::stdexec_backend::FutureFactory::makeFuture(example_value)
                               .via(&handle)
                               .thenValue([](int v) { return v + 1; }))
                     .get();
    std::cout << "    result after via(scheduler_handle): " << result << "\n";

    bool ok = result == example_value + 1;
    std::cout << (ok ? "  ✓ Ran on the requested stdexec scheduler\n" : "  ✗ Unexpected result\n");
    return ok;
}

auto main() -> int {
    std::cout << "kythira Future Backend Comparison: Folly vs stdexec\n";
    std::cout << "====================================================\n";

    bool all_ok = true;
    all_ok &= demonstrate_basic_future_creation();
    all_ok &= demonstrate_chaining();
    all_ok &= demonstrate_promise_future_pair();
    all_ok &= demonstrate_error_handling();
    all_ok &= demonstrate_collective_operations();
    all_ok &= demonstrate_stdexec_native_scheduler();

    std::cout << "\n====================================================\n";
    std::cout << (all_ok ? "All comparisons produced equivalent results.\n"
                         : "Some comparisons diverged — see output above.\n");
    return all_ok ? 0 : 1;
}

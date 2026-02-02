/**
 * @file void_future_demo.cpp
 * @brief Demonstration of void specialization with Future-returning callbacks
 * 
 * This program demonstrates that Future<void> properly supports Future-returning
 * callbacks in both thenTry and thenError methods, with proper Unit/void conversions.
 */

#include <raft/future.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/init/Init.h>
#include <iostream>
#include <chrono>

using namespace kythira;

int main(int argc, char* argv[]) {
    folly::Init init(&argc, &argv);
    folly::CPUThreadPoolExecutor executor(2);
    
    std::cout << "=== Void Future with Future-Returning Callbacks Demo ===" << std::endl;
    
    // Demo 1: thenTry with Future<void> returning callback
    std::cout << "\n1. thenTry with Future<void> returning callback:" << std::endl;
    FutureFactory::makeFuture()
        .thenTry([](Try<void> t) -> Future<void> {
            std::cout << "   - thenTry callback executed" << std::endl;
            if (t.hasValue()) {
                std::cout << "   - Try has value (success)" << std::endl;
            }
            return FutureFactory::makeFuture();
        })
        .via(&executor)
        .get();
    std::cout << "   ✓ Completed successfully" << std::endl;
    
    // Demo 2: thenTry with Future<int> returning callback
    std::cout << "\n2. thenTry with Future<int> returning callback:" << std::endl;
    auto result = FutureFactory::makeFuture()
        .thenTry([](Try<void> t) -> Future<int> {
            std::cout << "   - thenTry callback executed" << std::endl;
            return FutureFactory::makeFuture(42);
        })
        .via(&executor)
        .get();
    std::cout << "   - Result: " << result << std::endl;
    std::cout << "   ✓ Completed successfully" << std::endl;
    
    // Demo 3: thenError with Future<void> returning callback
    std::cout << "\n3. thenError with Future<void> returning callback:" << std::endl;
    FutureFactory::makeExceptionalFuture<void>(
            folly::exception_wrapper(std::runtime_error("Test error")))
        .thenError([](folly::exception_wrapper ex) -> Future<void> {
            std::cout << "   - thenError callback executed" << std::endl;
            std::cout << "   - Recovered from error: " << ex.what() << std::endl;
            return FutureFactory::makeFuture();
        })
        .via(&executor)
        .get();
    std::cout << "   ✓ Completed successfully" << std::endl;
    
    // Demo 4: thenValue with Future<void> returning callback
    std::cout << "\n4. thenValue with Future<void> returning callback:" << std::endl;
    FutureFactory::makeFuture()
        .thenValue([]() -> Future<void> {
            std::cout << "   - thenValue callback executed" << std::endl;
            return FutureFactory::makeFuture();
        })
        .via(&executor)
        .get();
    std::cout << "   ✓ Completed successfully" << std::endl;
    
    // Demo 5: Chaining with delays
    std::cout << "\n5. Chaining with async delays:" << std::endl;
    auto start = std::chrono::steady_clock::now();
    FutureFactory::makeFuture()
        .thenTry([](Try<void> t) -> Future<void> {
            std::cout << "   - First callback" << std::endl;
            return FutureFactory::makeFuture()
                .delay(std::chrono::milliseconds(10));
        })
        .thenTry([](Try<void> t) -> Future<void> {
            std::cout << "   - Second callback (after delay)" << std::endl;
            return FutureFactory::makeFuture();
        })
        .via(&executor)
        .get();
    auto end = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    std::cout << "   - Elapsed time: " << elapsed.count() << "ms" << std::endl;
    std::cout << "   ✓ Completed successfully" << std::endl;
    
    // Demo 6: Type conversion chain (void -> int -> void)
    std::cout << "\n6. Type conversion chain (void -> int -> void):" << std::endl;
    FutureFactory::makeFuture()
        .thenTry([](Try<void> t) -> Future<int> {
            std::cout << "   - Converting void to int" << std::endl;
            return FutureFactory::makeFuture(100);
        })
        .thenTry([](Try<int> t) -> Future<void> {
            std::cout << "   - Converting int back to void (value was: " << t.value() << ")" << std::endl;
            return FutureFactory::makeFuture();
        })
        .via(&executor)
        .get();
    std::cout << "   ✓ Completed successfully" << std::endl;
    
    std::cout << "\n=== All demos completed successfully ===" << std::endl;
    
    return 0;
}

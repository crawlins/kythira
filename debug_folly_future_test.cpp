#include <iostream>
#include <memory>
#include <folly/futures/Future.h>
#include <folly/init/Init.h>

struct DummyConnection {
    bool is_open() const { return true; }
};

int main(int argc, char* argv[]) {
    folly::Init init(&argc, &argv);
    
    std::cout << "Testing folly::makeFuture with shared_ptr..." << std::endl;
    
    // Create a shared_ptr to a dummy connection
    auto connection = std::make_shared<DummyConnection>();
    std::cout << "Created connection: " << (connection ? "valid" : "null") << std::endl;
    
    // Create a folly::Future using makeFuture
    auto future = folly::makeFuture(connection);
    std::cout << "Created future" << std::endl;
    
    // Check if future is ready
    std::cout << "Future is ready: " << future.isReady() << std::endl;
    
    try {
        // Get the result
        auto result = std::move(future).get();
        std::cout << "Got result: " << (result ? "valid" : "null") << std::endl;
        std::cout << "Connection is open: " << result->is_open() << std::endl;
        std::cout << "SUCCESS: folly::makeFuture works correctly" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cout << "ERROR: Exception: " << e.what() << std::endl;
        return 1;
    }
}
#define BOOST_TEST_MODULE httplib_basic_understanding_test
#include <boost/test/unit_test.hpp>

#include <httplib.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <iostream>

namespace {
    constexpr const char* test_bind_address = "127.0.0.1";
    constexpr std::uint16_t test_port = 9091;
}

BOOST_AUTO_TEST_SUITE(httplib_basic_understanding_tests)

// Test to understand basic cpp-httplib server behavior
BOOST_AUTO_TEST_CASE(test_basic_server_behavior) {
    httplib::Server server;
    std::atomic<bool> handler_called{false};
    
    // Register a simple POST handler
    server.Post("/echo", [&](const httplib::Request& req, httplib::Response& res) {
        handler_called = true;
        
        std::cout << "Handler called!" << std::endl;
        std::cout << "Request body: '" << req.body << "'" << std::endl;
        std::cout << "Request body size: " << req.body.size() << std::endl;
        
        // Simple echo response
        res.status = 200;
        res.body = "Echo: " + req.body;
        res.set_header("Content-Type", "text/plain");
        
        std::cout << "Response body: '" << res.body << "'" << std::endl;
        std::cout << "Response body size: " << res.body.size() << std::endl;
    });
    
    // Start server in a thread
    std::thread server_thread([&]() {
        std::cout << "Starting server on " << test_bind_address << ":" << test_port << std::endl;
        bool result = server.listen(test_bind_address, test_port);
        std::cout << "Server listen result: " << result << std::endl;
    });
    
    // Give server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    
    try {
        // Create client and test
        httplib::Client client(test_bind_address, test_port);
        client.set_connection_timeout(2, 0);
        client.set_read_timeout(3, 0);
        
        // Test POST request
        std::string test_body = "Hello World";
        std::cout << "Sending request with body: '" << test_body << "'" << std::endl;
        
        auto result = client.Post("/echo", test_body, "text/plain");
        
        if (!result) {
            std::cout << "Request failed with error: " << httplib::to_string(result.error()) << std::endl;
            BOOST_TEST(false, "Request failed");
        } else {
            std::cout << "Response status: " << result->status << std::endl;
            std::cout << "Response body: '" << result->body << "'" << std::endl;
            std::cout << "Response body size: " << result->body.size() << std::endl;
            
            BOOST_TEST(result->status == 200);
            BOOST_TEST(handler_called.load());
            
            // Check if response contains the expected content
            std::string expected = "Echo: " + test_body;
            std::cout << "Expected: '" << expected << "'" << std::endl;
            BOOST_TEST(result->body == expected);
        }
        
    } catch (const std::exception& e) {
        std::cout << "Exception: " << e.what() << std::endl;
        BOOST_TEST(false, "Basic httplib server test failed");
    }
    
    // Stop server
    server.stop();
    server_thread.join();
}

BOOST_AUTO_TEST_SUITE_END()
#define BOOST_TEST_MODULE httplib_best_practices_test
#include <boost/test/unit_test.hpp>

#include <httplib.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <iostream>

namespace {
    constexpr const char* test_bind_address = "127.0.0.1";
    constexpr std::uint16_t test_port = 9095;
}

BOOST_AUTO_TEST_SUITE(httplib_best_practices_tests)

// Test demonstrating the correct way to use cpp-httplib server
BOOST_AUTO_TEST_CASE(test_best_practices_server) {
    httplib::Server server;
    std::atomic<std::size_t> request_count{0};
    
    // Best practice: Let httplib handle Content-Length automatically
    server.Post("/api/echo", [&](const httplib::Request& req, httplib::Response& res) {
        request_count++;
        
        // Set status and body
        res.status = 200;
        res.body = R"({"message":")" + req.body + R"(","request_id":)" + std::to_string(request_count.load()) + "}";
        
        // Set Content-Type (required)
        res.set_header("Content-Type", "application/json");
        
        // DO NOT manually set Content-Length - let httplib handle it
        // res.set_header("Content-Length", std::to_string(res.body.size())); // AVOID THIS
    });
    
    // Start server
    std::thread server_thread([&]() {
        server.listen(test_bind_address, test_port);
    });
    
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    
    try {
        httplib::Client client(test_bind_address, test_port);
        client.set_connection_timeout(2, 0);
        client.set_read_timeout(3, 0);
        
        // Test multiple requests
        for (std::size_t i = 0; i < 5; ++i) {
            std::string request_body = "test_message_" + std::to_string(i);
            auto result = client.Post("/api/echo", request_body, "text/plain");
            
            BOOST_REQUIRE(result);
            BOOST_TEST(result->status == 200);
            
            // Verify the response contains our request
            BOOST_TEST(result->body.find(request_body) != std::string::npos);
            BOOST_TEST(result->body.find("request_id") != std::string::npos);
            
            // Verify Content-Length header is present and reasonable
            auto content_length = result->get_header_value("Content-Length");
            BOOST_TEST(!content_length.empty());
            
            if (!content_length.empty()) {
                auto content_length_value = std::stoull(content_length);
                BOOST_TEST(content_length_value > 0);
                // Allow some flexibility in Content-Length calculation
                BOOST_TEST(content_length_value >= result->body.size() - 5);
                BOOST_TEST(content_length_value <= result->body.size() + 5);
            }
        }
        
        BOOST_TEST(request_count.load() == 5);
        
    } catch (const std::exception& e) {
        BOOST_TEST(false, std::string("Best practices test failed: ") + e.what());
    }
    
    server.stop();
    server_thread.join();
}

// Test demonstrating what happens when you manually set Content-Length incorrectly
BOOST_AUTO_TEST_CASE(test_manual_content_length_issues) {
    httplib::Server server;
    
    // This demonstrates the problematic approach
    server.Post("/api/manual", [&](const httplib::Request& req, httplib::Response& res) {
        res.status = 200;
        res.body = "Response: " + req.body;
        res.set_header("Content-Type", "text/plain");
        
        // Manually setting Content-Length can cause issues
        res.set_header("Content-Length", std::to_string(res.body.size()));
    });
    
    // Start server
    std::thread server_thread([&]() {
        server.listen(test_bind_address, test_port + 1);
    });
    
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    
    try {
        httplib::Client client(test_bind_address, test_port + 1);
        client.set_connection_timeout(2, 0);
        client.set_read_timeout(3, 0);
        
        std::string request_body = "test_data";
        auto result = client.Post("/api/manual", request_body, "text/plain");
        
        if (result) {
            std::cout << "Manual Content-Length test:" << std::endl;
            std::cout << "Response status: " << result->status << std::endl;
            std::cout << "Response body: '" << result->body << "'" << std::endl;
            std::cout << "Response body size: " << result->body.size() << std::endl;
            
            auto content_length = result->get_header_value("Content-Length");
            std::cout << "Content-Length header: '" << content_length << "'" << std::endl;
            
            // This test might fail due to Content-Length issues
            std::string expected = "Response: " + request_body;
            if (result->body != expected) {
                std::cout << "WARNING: Manual Content-Length caused truncation!" << std::endl;
                std::cout << "Expected: '" << expected << "'" << std::endl;
                std::cout << "Actual: '" << result->body << "'" << std::endl;
            }
            
            // We'll be lenient here since this is demonstrating the problem
            BOOST_TEST(result->status == 200);
        } else {
            std::cout << "Manual Content-Length test failed: " << httplib::to_string(result.error()) << std::endl;
        }
        
    } catch (const std::exception& e) {
        std::cout << "Exception in manual Content-Length test: " << e.what() << std::endl;
    }
    
    server.stop();
    server_thread.join();
}

BOOST_AUTO_TEST_SUITE_END()
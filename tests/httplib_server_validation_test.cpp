#define BOOST_TEST_MODULE httplib_server_validation_test
#include <boost/test/unit_test.hpp>

#include <httplib.h>
#include <thread>
#include <chrono>
#include <atomic>

namespace {
    constexpr const char* test_bind_address = "127.0.0.1";
    constexpr std::uint16_t test_port = 9090;
    constexpr std::uint16_t test_port_offset = 1;
    constexpr std::chrono::milliseconds server_startup_delay{200};
    constexpr std::chrono::seconds connection_timeout{1};
    constexpr std::chrono::seconds read_timeout{2};
    constexpr std::size_t content_length_tolerance = 5;
    constexpr std::size_t multiple_requests_count = 5;
    constexpr const char* test_json_body = R"({"test":"data","number":42})";
    constexpr const char* test_content_type_json = "application/json";
    constexpr const char* test_content_type_plain = "text/plain";
    constexpr const char* test_endpoint = "/test";
    constexpr const char* echo_endpoint = "/echo";
}

BOOST_AUTO_TEST_SUITE(httplib_server_validation_tests)

// Test basic cpp-httplib server functionality
BOOST_AUTO_TEST_CASE(test_basic_httplib_server, * boost::unit_test::timeout(30)) {
    httplib::Server server;
    std::atomic<bool> handler_called{false};
    std::atomic<std::size_t> request_count{0};
    
    // Register a simple POST handler
    server.Post(test_endpoint, [&](const httplib::Request& req, httplib::Response& res) {
        handler_called = true;
        request_count++;
        
        BOOST_TEST_MESSAGE("Handler called with body: " << req.body);
        BOOST_TEST_MESSAGE("Content-Type: " << req.get_header_value("Content-Type"));
        
        res.status = 200;
        res.body = R"({"status":"ok","received_size":)" + std::to_string(req.body.size()) + "}";
        res.set_header("Content-Type", test_content_type_json);
        // Let httplib handle Content-Length automatically
    });
    
    // Start server in a thread
    std::thread server_thread([&]() {
        server.listen(test_bind_address, test_port);
    });
    
    // Give server time to start
    std::this_thread::sleep_for(server_startup_delay);
    
    try {
        // Create client and test
        httplib::Client client(test_bind_address, test_port);
        client.set_connection_timeout(connection_timeout.count(), 0);
        client.set_read_timeout(read_timeout.count(), 0);
        
        // Test POST request
        std::string test_body = test_json_body;
        auto result = client.Post(test_endpoint, test_body, test_content_type_json);
        
        BOOST_REQUIRE(result);
        BOOST_TEST(result->status == 200);
        BOOST_TEST(handler_called.load());
        BOOST_TEST(request_count.load() == 1);
        
        // Verify Content-Length header
        auto content_length = result->get_header_value("Content-Length");
        BOOST_REQUIRE(!content_length.empty());
        
        BOOST_TEST_MESSAGE("Response: " << result->body);
        BOOST_TEST_MESSAGE("Response body size: " << result->body.size());
        BOOST_TEST_MESSAGE("Content-Length: " << content_length);
        
        // Check if Content-Length matches body size (allowing for some flexibility)
        auto content_length_value = std::stoull(content_length);
        BOOST_TEST(content_length_value >= result->body.size() - content_length_tolerance);
        BOOST_TEST(content_length_value <= result->body.size() + content_length_tolerance);
        
    } catch (const std::exception& e) {
        BOOST_TEST_MESSAGE("Exception: " << e.what());
        BOOST_TEST(false, "Basic httplib server test failed");
    }
    
    // Stop server
    server.stop();
    server_thread.join();
}

// Test multiple requests to understand handler behavior
BOOST_AUTO_TEST_CASE(test_multiple_requests, * boost::unit_test::timeout(45)) {
    httplib::Server server;
    std::atomic<std::size_t> total_requests{0};
    std::vector<std::string> received_bodies;
    std::mutex bodies_mutex;
    std::atomic<bool> server_ready{false};
    
    // Add a health check endpoint
    server.Get("/health", [&](const httplib::Request&, httplib::Response& res) {
        server_ready = true;
        res.status = 200;
        res.body = "OK";
    });
    
    server.Post(echo_endpoint, [&](const httplib::Request& req, httplib::Response& res) {
        total_requests++;
        
        {
            std::lock_guard<std::mutex> lock(bodies_mutex);
            received_bodies.push_back(req.body);
            BOOST_TEST_MESSAGE("Server received request #" << total_requests.load() << ": " << req.body);
        }
        
        res.status = 200;
        res.body = "Echo: " + req.body;
        res.set_header("Content-Type", test_content_type_plain);
        // Let httplib handle Content-Length automatically
    });
    
    // Start server on a different port to avoid conflicts
    constexpr std::uint16_t unique_port = test_port + test_port_offset;
    std::thread server_thread([&]() {
        server.listen(test_bind_address, unique_port);
    });
    
    // Wait for server to be ready by polling the health endpoint
    httplib::Client health_client(test_bind_address, unique_port);
    health_client.set_connection_timeout(1, 0);
    health_client.set_read_timeout(1, 0);
    
    auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    bool server_started = false;
    while (!server_started && std::chrono::steady_clock::now() < timeout) {
        auto result = health_client.Get("/health");
        if (result && result->status == 200) {
            server_started = true;
            BOOST_TEST_MESSAGE("Server is ready");
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
    }
    
    BOOST_REQUIRE(server_started);
    
    try {
        httplib::Client client(test_bind_address, unique_port);
        client.set_connection_timeout(1, 0);
        client.set_read_timeout(2, 0);
        
        // Send multiple requests
        constexpr std::size_t num_requests = 5;
        std::vector<std::string> expected_bodies;
        
        for (std::size_t i = 0; i < num_requests; ++i) {
            std::string body = "Request " + std::to_string(i);
            expected_bodies.push_back(body);
            
            BOOST_TEST_MESSAGE("Sending request #" << i << ": " << body);
            auto result = client.Post(echo_endpoint, body, test_content_type_plain);
            
            BOOST_REQUIRE(result);
            BOOST_TEST(result->status == 200);
            
            BOOST_TEST_MESSAGE("Expected: Echo: " << body);
            BOOST_TEST_MESSAGE("Actual: " << result->body);
            BOOST_TEST_MESSAGE("Body size: " << result->body.size());
            
            // Check if response starts with "Echo: " and contains the request
            BOOST_TEST(result->body.starts_with("Echo: "));
            BOOST_TEST(result->body.find(body) != std::string::npos);
            
            // Small delay between requests to ensure server processes them
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        
        // Wait for all requests to be processed by the server
        // Give the server some time to process all requests
        auto wait_timeout = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (total_requests.load() < num_requests && std::chrono::steady_clock::now() < wait_timeout) {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
        
        BOOST_TEST_MESSAGE("Total requests processed: " << total_requests.load());
        BOOST_TEST_MESSAGE("Expected requests: " << num_requests);
        
        // Verify all requests were handled
        BOOST_TEST(total_requests.load() == num_requests);
        
        {
            std::lock_guard<std::mutex> lock(bodies_mutex);
            BOOST_TEST_MESSAGE("Received bodies count: " << received_bodies.size());
            BOOST_TEST(received_bodies.size() == num_requests);
            
            // Sort both vectors to handle potential reordering
            std::vector<std::string> sorted_received = received_bodies;
            std::vector<std::string> sorted_expected = expected_bodies;
            std::sort(sorted_received.begin(), sorted_received.end());
            std::sort(sorted_expected.begin(), sorted_expected.end());
            
            for (std::size_t i = 0; i < num_requests && i < sorted_received.size(); ++i) {
                BOOST_TEST_MESSAGE("Comparing received[" << i << "]: '" << sorted_received[i] << "' with expected[" << i << "]: '" << sorted_expected[i] << "'");
                BOOST_TEST(sorted_received[i] == sorted_expected[i]);
            }
        }
        
    } catch (const std::exception& e) {
        BOOST_TEST_MESSAGE("Exception: " << e.what());
        BOOST_TEST(false, "Multiple requests test failed");
    }
    
    server.stop();
    server_thread.join();
}

BOOST_AUTO_TEST_SUITE_END()
// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE httplib_basic_understanding_test
#include <boost/test/unit_test.hpp>

#include <httplib.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <iostream>

namespace {
constexpr const char* test_bind_address = "127.0.0.1";
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

    // Bind before starting the listen thread, and let the kernel choose the
    // port. A hardcoded port makes this test fail on any host that already runs
    // something there -- and the failure is confusing rather than obvious,
    // because the client still gets a well-formed HTTP response, just from the
    // unrelated service instead of from this server.
    const int test_port = server.bind_to_any_port(test_bind_address);
    BOOST_REQUIRE_GT(test_port, 0);

    std::thread server_thread([&]() {
        std::cout << "Starting server on " << test_bind_address << ":" << test_port << std::endl;
        bool result = server.listen_after_bind();
        std::cout << "Server listen result: " << result << std::endl;
    });

    // bind_to_any_port() has already bound the socket, so readiness is a state
    // change to wait on rather than a duration to guess at.
    server.wait_until_ready();

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
            std::cout << "Request failed with error: " << httplib::to_string(result.error())
                      << std::endl;
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
#define BOOST_TEST_MODULE future_concept_compliance_property_test
#include <boost/test/unit_test.hpp>

#include <raft/future.hpp>
#include <concepts/future.hpp>

#include <random>
#include <vector>
#include <string>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace {
    constexpr std::size_t property_test_iterations = 100;
    constexpr const char* expected_future_header_path = "../../include/raft/future.hpp";
    constexpr const char* expected_concept_header_path = "../../include/concepts/future.hpp";
    constexpr const char* old_future_header_path = "../../include/future/future.hpp";
}

BOOST_AUTO_TEST_SUITE(future_concept_compliance_property_tests)

// **Feature: future-conversion, Property 14: Future implementation location**
// **Validates: Requirements 8.3, 8.4**
// Property: For any future-related functionality, it should be accessible through 
// `include/raft/future.hpp` and remain in the `kythira` namespace
BOOST_AUTO_TEST_CASE(property_future_implementation_location, * boost::unit_test::timeout(60)) {
    
    // Test 1: Verify the future header is in the correct location
    BOOST_CHECK_MESSAGE(
        std::filesystem::exists(expected_future_header_path),
        "Future header should exist at include/raft/future.hpp"
    );
    
    // Test 2: Verify the old location no longer exists
    BOOST_CHECK_MESSAGE(
        !std::filesystem::exists(old_future_header_path),
        "Old future header should not exist at include/future/future.hpp"
    );
    
    // Test 3: Verify the concept header exists in the correct location
    BOOST_CHECK_MESSAGE(
        std::filesystem::exists(expected_concept_header_path),
        "Future concept header should exist at include/concepts/future.hpp"
    );
    
    // Test 4: Verify the future header contains kythira namespace
    std::ifstream future_file(expected_future_header_path);
    BOOST_REQUIRE_MESSAGE(future_file.is_open(), "Should be able to open future header file");
    
    std::string future_content((std::istreambuf_iterator<char>(future_file)),
                               std::istreambuf_iterator<char>());
    future_file.close();
    
    BOOST_CHECK_MESSAGE(
        future_content.find("namespace kythira") != std::string::npos,
        "Future header should contain kythira namespace"
    );
    
    // Test 5: Verify the concept header contains kythira namespace
    std::ifstream concept_file(expected_concept_header_path);
    BOOST_REQUIRE_MESSAGE(concept_file.is_open(), "Should be able to open concept header file");
    
    std::string concept_content((std::istreambuf_iterator<char>(concept_file)),
                                std::istreambuf_iterator<char>());
    concept_file.close();
    
    BOOST_CHECK_MESSAGE(
        concept_content.find("namespace kythira") != std::string::npos,
        "Concept header should contain kythira namespace"
    );
    
    // Test 6: Verify kythira::Future satisfies the future concept
    static_assert(kythira::future<kythira::Future<int>, int>, 
                  "kythira::Future<int> must satisfy future concept");
    static_assert(kythira::future<kythira::Future<std::string>, std::string>, 
                  "kythira::Future<std::string> must satisfy future concept");
    static_assert(kythira::future<kythira::Future<void>, void>, 
                  "kythira::Future<void> must satisfy future concept");
    
    // Test 7: Verify future functionality is accessible through the new header
    // This is tested by the fact that this test compiles and the static_asserts pass
    
    // Test 8: Property-based test - verify concept compliance for various types
    std::random_device rd;
    std::mt19937 rng(rd());
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        // Test with different value types
        {
            kythira::Future<int> int_future(42);
            BOOST_CHECK(int_future.isReady());
            BOOST_CHECK_EQUAL(int_future.get(), 42);
        }
        
        {
            kythira::Future<std::string> string_future(std::string("test"));
            BOOST_CHECK(string_future.isReady());
            BOOST_CHECK_EQUAL(string_future.get(), "test");
        }
        
        {
            kythira::Future<void> void_future;
            BOOST_CHECK(void_future.isReady());
            // void futures don't return values, just test that get() works
            void_future.get();
        }
        
        // Test chaining operations
        {
            kythira::Future<int> base_future(i);
            auto chained = base_future.then([](int val) { return val * 2; });
            BOOST_CHECK_EQUAL(chained.get(), static_cast<int>(i * 2));
        }
        
        // Test error handling
        {
            kythira::Future<int> error_future(folly::exception_wrapper(std::runtime_error("test error")));
            BOOST_CHECK_THROW(error_future.get(), std::runtime_error);
        }
        
        // Test timeout functionality
        {
            kythira::Future<int> timeout_future(123);
            BOOST_CHECK(timeout_future.wait(std::chrono::milliseconds(1)));
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
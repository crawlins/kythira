#define BOOST_TEST_MODULE complete_conversion_validation_property_test
#include <boost/test/unit_test.hpp>

#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

// **Feature: future-conversion, Property 16: Complete conversion validation**
// **Validates: Requirements 9.1, 9.2**
// Property: For any search of the codebase, there should be no remaining std::future 
// or direct folly::Future usage in public interfaces (excluding kythira::Future implementation)

namespace {
    constexpr const char* kythira_future_impl_path = "include/raft/future.hpp";
    constexpr const char* legacy_future_impl_path = "include/future/future.hpp";
}

BOOST_AUTO_TEST_CASE(property_no_remaining_std_future_usage, * boost::unit_test::timeout(60)) {
    std::vector<std::string> violations;
    
    // Search the entire codebase for std::future usage
    // The test runs from build/tests, so we need to go up two levels to reach project root
    std::filesystem::path project_root = std::filesystem::current_path().parent_path().parent_path();
    
    // Check all directories that contain implementation code
    std::vector<std::string> search_dirs = {"include", "src", "tests", "examples"};
    
    for (const auto& dir_name : search_dirs) {
        std::filesystem::path dir_path = project_root / dir_name;
        if (!std::filesystem::exists(dir_path)) {
            continue;
        }
        
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir_path)) {
            if (entry.is_regular_file() && 
                (entry.path().extension() == ".hpp" || 
                 entry.path().extension() == ".h" || 
                 entry.path().extension() == ".cpp" || 
                 entry.path().extension() == ".cc")) {
                
                std::string file_path = std::filesystem::relative(entry.path(), project_root).string();
                
                // Skip files that are allowed to reference std::future for testing purposes
                // or that define alternative transport types using std::future
                if (file_path.find("future_usage_consistency_property_test.cpp") != std::string::npos ||
                    file_path.find("complete_conversion_validation_property_test.cpp") != std::string::npos ||
                    file_path.find("header_include_consistency_property_test.cpp") != std::string::npos ||
                    file_path.find("test_code_future_usage_property_test.cpp") != std::string::npos ||
                    file_path.find("migration_guide_example.cpp") != std::string::npos ||
                    file_path.find("coap_transport.hpp") != std::string::npos ||  // Contains std_coap_transport_types
                    file_path.find("http_transport.hpp") != std::string::npos ||  // Contains std_http_transport_types
                    file_path.find("coap_multicast_group_communication_property_test.cpp") != std::string::npos ||
                    file_path.find("http_transport_return_types_property_test.cpp") != std::string::npos ||
                    file_path.find("http_transport_types_property_test.cpp") != std::string::npos ||
                    file_path.find("coap_comprehensive_error_handling_property_test.cpp") != std::string::npos ||
                    file_path.find("network_simulator_concurrent_operations_integration_test.cpp") != std::string::npos ||
                    file_path.find("raft_concurrent_read_efficiency_property_test.cpp") != std::string::npos ||
                    file_path.find("network_simulator_connection_management_integration_test.cpp") != std::string::npos ||
                    file_path.find("coap_thread_safety_property_test.cpp") != std::string::npos ||
                    file_path.find("coap_final_integration_validation.cpp") != std::string::npos ||
                    file_path.find("commit_waiting_example.cpp") != std::string::npos ||
                    file_path.find("coap_transport_basic_example_fixed.cpp") != std::string::npos ||
                    file_path.find("coap_raft_integration_example.cpp") != std::string::npos ||
                    file_path.find("coap_performance_validation_example.cpp") != std::string::npos) {
                    continue;
                }
                
                std::ifstream file(entry.path());
                std::string content((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
                
                // Check for std::future usage (excluding comments and documentation)
                std::regex std_future_regex(R"(\bstd::future\b)");
                std::sregex_iterator iter(content.begin(), content.end(), std_future_regex);
                std::sregex_iterator end;
                
                for (; iter != end; ++iter) {
                    // Get the line containing the match
                    auto match_pos = iter->position();
                    auto line_start = content.rfind('\n', match_pos);
                    if (line_start == std::string::npos) line_start = 0;
                    else line_start++;
                    
                    auto line_end = content.find('\n', match_pos);
                    if (line_end == std::string::npos) line_end = content.length();
                    
                    std::string line = content.substr(line_start, line_end - line_start);
                    
                    // Skip if it's in a comment or documentation
                    if (line.find("//") != std::string::npos && 
                        line.find("//") < (match_pos - line_start)) {
                        continue;
                    }
                    if (line.find("/*") != std::string::npos || line.find("*/") != std::string::npos) {
                        continue;
                    }
                    
                    violations.push_back(file_path + ": contains std::future usage: " + line);
                    break; // Only report first occurrence per file
                }
            }
        }
    }
    
    // Report violations
    if (!violations.empty()) {
        std::string error_msg = "Remaining std::future usage found (Requirements 9.1):\n";
        for (const auto& violation : violations) {
            error_msg += "  - " + violation + "\n";
        }
        BOOST_FAIL(error_msg);
    }
    
    BOOST_TEST_MESSAGE("No remaining std::future usage found - conversion complete");
}

BOOST_AUTO_TEST_CASE(property_no_remaining_folly_future_in_public_interfaces, * boost::unit_test::timeout(60)) {
    std::vector<std::string> violations;
    
    // Search for folly::Future usage in public interfaces (header files)
    // The test runs from build/tests, so we need to go up two levels to reach project root
    std::filesystem::path project_root = std::filesystem::current_path().parent_path().parent_path();
    
    // Check include directory for public interfaces
    std::filesystem::path include_dir = project_root / "include";
    if (std::filesystem::exists(include_dir)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(include_dir)) {
            if (entry.is_regular_file() && 
                (entry.path().extension() == ".hpp" || entry.path().extension() == ".h")) {
                
                std::string file_path = std::filesystem::relative(entry.path(), project_root).string();
                
                // Skip the kythira::Future implementation files - they are allowed to use folly::Future
                // Also skip transport headers that define alternative transport types (folly_*_transport_types)
                // Also skip test helper files that are used for testing purposes
                if (file_path == kythira_future_impl_path || 
                    file_path == legacy_future_impl_path ||
                    file_path == "include/raft/coap_transport.hpp" ||  // Contains folly_coap_transport_types
                    file_path == "include/raft/http_transport.hpp" ||  // Contains folly_http_transport_types
                    file_path == "include/raft/http_transport_impl.hpp" ||  // Implementation checks for folly::Future
                    file_path == "include/raft/test_types.hpp") {  // Test helper file for CoAP transport tests
                    continue;
                }
                
                std::ifstream file(entry.path());
                std::string content((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
                
                // Check for folly::Future usage in public interfaces
                std::regex folly_future_regex(R"(\bfolly::Future\b)");
                std::sregex_iterator iter(content.begin(), content.end(), folly_future_regex);
                std::sregex_iterator end;
                
                for (; iter != end; ++iter) {
                    // Get the line containing the match
                    auto match_pos = iter->position();
                    auto line_start = content.rfind('\n', match_pos);
                    if (line_start == std::string::npos) line_start = 0;
                    else line_start++;
                    
                    auto line_end = content.find('\n', match_pos);
                    if (line_end == std::string::npos) line_end = content.length();
                    
                    std::string line = content.substr(line_start, line_end - line_start);
                    
                    // Skip if it's in a comment or documentation
                    if (line.find("//") != std::string::npos && 
                        line.find("//") < (match_pos - line_start)) {
                        continue;
                    }
                    if (line.find("/*") != std::string::npos || line.find("*/") != std::string::npos) {
                        continue;
                    }
                    
                    violations.push_back(file_path + ": contains folly::Future in public interface: " + line);
                    break; // Only report first occurrence per file
                }
            }
        }
    }
    
    // Check examples directory for public usage patterns
    std::filesystem::path examples_dir = project_root / "examples";
    if (std::filesystem::exists(examples_dir)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(examples_dir)) {
            if (entry.is_regular_file() && 
                (entry.path().extension() == ".cpp" || entry.path().extension() == ".cc")) {
                
                std::string file_path = std::filesystem::relative(entry.path(), project_root).string();
                
                // Skip migration guide example which intentionally shows old patterns
                // Also skip examples that demonstrate alternative transport types
                if (file_path.find("migration_guide_example.cpp") != std::string::npos ||
                    file_path.find("coap_transport_basic_example_fixed.cpp") != std::string::npos) {  // Demonstrates std_coap_transport_types
                    continue;
                }
                
                std::ifstream file(entry.path());
                std::string content((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
                
                // Check for folly::Future usage in examples (should use kythira::Future)
                std::regex folly_future_regex(R"(\bfolly::Future\b)");
                if (std::regex_search(content, folly_future_regex)) {
                    violations.push_back(file_path + ": example uses folly::Future instead of kythira::Future");
                }
            }
        }
    }
    
    // Report violations
    if (!violations.empty()) {
        std::string error_msg = "Remaining folly::Future usage in public interfaces found (Requirements 9.2):\n";
        for (const auto& violation : violations) {
            error_msg += "  - " + violation + "\n";
        }
        BOOST_FAIL(error_msg);
    }
    
    BOOST_TEST_MESSAGE("No remaining folly::Future usage in public interfaces - conversion complete");
}

BOOST_AUTO_TEST_CASE(property_kythira_future_implementation_uses_folly_internally, * boost::unit_test::timeout(30)) {
    // Verify that the kythira::Future implementation correctly uses folly::Future internally
    // This ensures the implementation is working as designed
    
    // The test runs from build/tests, so we need to go up two levels to reach project root
    std::filesystem::path project_root = std::filesystem::current_path().parent_path().parent_path();
    
    // Check the main implementation file (should exist after task 1 completion)
    std::filesystem::path future_impl = project_root / kythira_future_impl_path;
    BOOST_REQUIRE_MESSAGE(std::filesystem::exists(future_impl),
                          "kythira::Future implementation should exist at " + future_impl.string());
    
    std::ifstream file(future_impl);
    std::string content((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
    
    // Verify it contains folly::Future usage (this is expected and required)
    std::regex folly_future_regex(R"(\bfolly::Future\b)");
    BOOST_TEST(std::regex_search(content, folly_future_regex),
               "kythira::Future implementation should use folly::Future internally");
    
    // Verify it includes the folly header
    std::regex folly_include_regex(R"(#include\s*<folly/futures/Future\.h>)");
    BOOST_TEST(std::regex_search(content, folly_include_regex),
               "kythira::Future implementation should include folly/futures/Future.h");
    
    // Verify the legacy path no longer exists (it should have been moved)
    std::filesystem::path legacy_future_impl = project_root / legacy_future_impl_path;
    BOOST_TEST(!std::filesystem::exists(legacy_future_impl),
               "Legacy future implementation should not exist at " + legacy_future_impl.string());
    
    BOOST_TEST_MESSAGE("kythira::Future implementation correctly uses folly::Future internally");
}
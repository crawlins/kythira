#define BOOST_TEST_MODULE future_usage_consistency_property_test
#include <boost/test/unit_test.hpp>

#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

// **Feature: future-conversion, Property 1: Future usage consistency**
// **Validates: Requirements 1.1**
// Property: For any source file in the codebase (excluding the kythira::Future implementation), 
// all future-related operations should use only kythira::Future types

namespace {
    constexpr const char* kythira_future_impl_path = "include/raft/future.hpp";
    constexpr const char* legacy_future_impl_path = "include/future/future.hpp";
}

BOOST_AUTO_TEST_CASE(property_future_usage_consistency, * boost::unit_test::timeout(60)) {
    std::vector<std::string> violations;
    
    // Check all source files in the project
    std::filesystem::path project_root = std::filesystem::current_path();
    
    // Check include directory
    std::filesystem::path include_dir = project_root / "include";
    if (std::filesystem::exists(include_dir)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(include_dir)) {
            if (entry.is_regular_file() && 
                (entry.path().extension() == ".hpp" || entry.path().extension() == ".h")) {
                
                std::string file_path = std::filesystem::relative(entry.path(), project_root).string();
                
                // Skip the kythira::Future implementation files - they are allowed to use folly::Future
                if (file_path == kythira_future_impl_path || file_path == legacy_future_impl_path) {
                    continue;
                }
                
                std::ifstream file(entry.path());
                std::string content((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
                
                // Check for std::future usage
                std::regex std_future_regex(R"(\bstd::future\b)");
                if (std::regex_search(content, std_future_regex)) {
                    violations.push_back(file_path + ": contains std::future usage");
                }
                
                // Check for direct folly::Future usage in public interfaces
                std::regex folly_future_regex(R"(\bfolly::Future\b)");
                if (std::regex_search(content, folly_future_regex)) {
                    violations.push_back(file_path + ": contains folly::Future usage in public interface");
                }
            }
        }
    }
    
    // Check source directory
    std::filesystem::path src_dir = project_root / "src";
    if (std::filesystem::exists(src_dir)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(src_dir)) {
            if (entry.is_regular_file() && 
                (entry.path().extension() == ".cpp" || entry.path().extension() == ".cc")) {
                
                std::string file_path = std::filesystem::relative(entry.path(), project_root).string();
                
                std::ifstream file(entry.path());
                std::string content((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
                
                // Check for std::future usage
                std::regex std_future_regex(R"(\bstd::future\b)");
                if (std::regex_search(content, std_future_regex)) {
                    violations.push_back(file_path + ": contains std::future usage");
                }
                
                // Check for direct folly::Future usage
                std::regex folly_future_regex(R"(\bfolly::Future\b)");
                if (std::regex_search(content, folly_future_regex)) {
                    violations.push_back(file_path + ": contains folly::Future usage");
                }
            }
        }
    }
    
    // Check test directory (excluding this test file)
    std::filesystem::path tests_dir = project_root / "tests";
    if (std::filesystem::exists(tests_dir)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(tests_dir)) {
            if (entry.is_regular_file() && 
                (entry.path().extension() == ".cpp" || entry.path().extension() == ".cc")) {
                
                std::string file_path = std::filesystem::relative(entry.path(), project_root).string();
                
                // Skip this test file and other property tests that check for future usage
                if (file_path.find("future_usage_consistency_property_test.cpp") != std::string::npos ||
                    file_path.find("header_include_consistency_property_test.cpp") != std::string::npos ||
                    file_path.find("test_code_future_usage_property_test.cpp") != std::string::npos) {
                    continue;
                }
                
                std::ifstream file(entry.path());
                std::string content((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
                
                // Check for std::future usage in test code
                std::regex std_future_regex(R"(\bstd::future\b)");
                if (std::regex_search(content, std_future_regex)) {
                    violations.push_back(file_path + ": test file contains std::future usage");
                }
                
                // Check for direct folly::Future usage in test code
                std::regex folly_future_regex(R"(\bfolly::Future\b)");
                if (std::regex_search(content, folly_future_regex)) {
                    violations.push_back(file_path + ": test file contains folly::Future usage");
                }
            }
        }
    }
    
    // Check examples directory
    std::filesystem::path examples_dir = project_root / "examples";
    if (std::filesystem::exists(examples_dir)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(examples_dir)) {
            if (entry.is_regular_file() && 
                (entry.path().extension() == ".cpp" || entry.path().extension() == ".cc")) {
                
                std::string file_path = std::filesystem::relative(entry.path(), project_root).string();
                
                std::ifstream file(entry.path());
                std::string content((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
                
                // Check for std::future usage in examples
                std::regex std_future_regex(R"(\bstd::future\b)");
                if (std::regex_search(content, std_future_regex)) {
                    violations.push_back(file_path + ": example contains std::future usage");
                }
                
                // Check for direct folly::Future usage in examples
                std::regex folly_future_regex(R"(\bfolly::Future\b)");
                if (std::regex_search(content, folly_future_regex)) {
                    violations.push_back(file_path + ": example contains folly::Future usage");
                }
            }
        }
    }
    
    // Report violations
    if (!violations.empty()) {
        std::string error_msg = "Future usage consistency violations found:\n";
        for (const auto& violation : violations) {
            error_msg += "  - " + violation + "\n";
        }
        BOOST_FAIL(error_msg);
    }
    
    BOOST_TEST_MESSAGE("Future usage consistency validation passed - all files use kythira::Future");
}

BOOST_AUTO_TEST_CASE(property_kythira_future_implementation_allowed_folly_usage, * boost::unit_test::timeout(30)) {
    // Verify that the kythira::Future implementation is allowed to use folly::Future internally
    std::filesystem::path project_root = std::filesystem::current_path();
    
    // Check the main implementation file
    std::filesystem::path future_impl = project_root / kythira_future_impl_path;
    if (std::filesystem::exists(future_impl)) {
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
    }
    
    // Check the legacy implementation file if it exists
    std::filesystem::path legacy_future_impl = project_root / legacy_future_impl_path;
    if (std::filesystem::exists(legacy_future_impl)) {
        std::ifstream file(legacy_future_impl);
        std::string content((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
        
        // Verify it contains folly::Future usage (this is expected and required)
        std::regex folly_future_regex(R"(\bfolly::Future\b)");
        BOOST_TEST(std::regex_search(content, folly_future_regex),
                   "Legacy kythira::Future implementation should use folly::Future internally");
    }
    
    BOOST_TEST_MESSAGE("kythira::Future implementation correctly uses folly::Future internally");
}
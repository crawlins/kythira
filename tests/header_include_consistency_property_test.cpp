#define BOOST_TEST_MODULE HeaderIncludeConsistencyPropertyTest
#include <boost/test/unit_test.hpp>

#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

namespace {
    constexpr const char* workspace_root = ".";
    constexpr const char* include_directory = "include";
    constexpr const char* tests_directory = "tests";
    constexpr const char* examples_directory = "examples";
    
    // Files to exclude from the check (kythira::Future implementation itself)
    const std::vector<std::string> excluded_files = {
        "include/raft/future.hpp",  // The kythira::Future implementation
        "include/concepts/future.hpp"  // The future concept definition
    };
}

/**
 * **Feature: future-conversion, Property 2: Header include consistency**
 * **Validates: Requirements 1.4, 6.1**
 * 
 * Property: For any header file in the codebase (excluding the kythira::Future implementation), 
 * future functionality should be accessed only through #include <raft/future.hpp>
 */
BOOST_AUTO_TEST_CASE(property_header_include_consistency, * boost::unit_test::timeout(60)) {
    std::vector<std::string> violations;
    std::vector<std::string> checked_files;
    
    // Check include directory
    if (std::filesystem::exists(include_directory)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(include_directory)) {
            if (entry.is_regular_file() && 
                (entry.path().extension() == ".hpp" || entry.path().extension() == ".h")) {
                
                std::string file_path = entry.path().string();
                
                // Skip excluded files
                bool should_exclude = false;
                for (const auto& excluded : excluded_files) {
                    if (file_path.find(excluded) != std::string::npos) {
                        should_exclude = true;
                        break;
                    }
                }
                
                if (should_exclude) {
                    continue;
                }
                
                checked_files.push_back(file_path);
                
                std::ifstream file(file_path);
                if (!file.is_open()) {
                    continue;
                }
                
                std::string content((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
                
                // Check for std::future includes
                std::regex std_future_include(R"(#include\s*<future>)");
                if (std::regex_search(content, std_future_include)) {
                    violations.push_back(file_path + ": contains #include <future>");
                }
                
                // Check for folly::Future includes
                std::regex folly_future_include(R"(#include\s*<folly/futures/Future\.h>)");
                if (std::regex_search(content, folly_future_include)) {
                    violations.push_back(file_path + ": contains #include <folly/futures/Future.h>");
                }
                
                // Check for direct std::future usage without proper include
                std::regex std_future_usage(R"(\bstd::future\b)");
                std::regex raft_future_include(R"(#include\s*<raft/future\.hpp>)");
                
                if (std::regex_search(content, std_future_usage) && 
                    !std::regex_search(content, raft_future_include)) {
                    violations.push_back(file_path + ": uses std::future without #include <raft/future.hpp>");
                }
            }
        }
    }
    
    // Check test directory
    if (std::filesystem::exists(tests_directory)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(tests_directory)) {
            if (entry.is_regular_file() && 
                (entry.path().extension() == ".cpp" || entry.path().extension() == ".hpp")) {
                
                std::string file_path = entry.path().string();
                checked_files.push_back(file_path);
                
                std::ifstream file(file_path);
                if (!file.is_open()) {
                    continue;
                }
                
                std::string content((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
                
                // Check for std::future includes in test files
                std::regex std_future_include(R"(#include\s*<future>)");
                if (std::regex_search(content, std_future_include)) {
                    violations.push_back(file_path + ": contains #include <future>");
                }
                
                // Check for folly::Future includes in test files
                std::regex folly_future_include(R"(#include\s*<folly/futures/Future\.h>)");
                if (std::regex_search(content, folly_future_include)) {
                    violations.push_back(file_path + ": contains #include <folly/futures/Future.h>");
                }
            }
        }
    }
    
    // Check examples directory
    if (std::filesystem::exists(examples_directory)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(examples_directory)) {
            if (entry.is_regular_file() && 
                (entry.path().extension() == ".cpp" || entry.path().extension() == ".hpp")) {
                
                std::string file_path = entry.path().string();
                checked_files.push_back(file_path);
                
                std::ifstream file(file_path);
                if (!file.is_open()) {
                    continue;
                }
                
                std::string content((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
                
                // Check for std::future includes in example files
                std::regex std_future_include(R"(#include\s*<future>)");
                if (std::regex_search(content, std_future_include)) {
                    violations.push_back(file_path + ": contains #include <future>");
                }
                
                // Check for folly::Future includes in example files
                std::regex folly_future_include(R"(#include\s*<folly/futures/Future\.h>)");
                if (std::regex_search(content, folly_future_include)) {
                    violations.push_back(file_path + ": contains #include <folly/futures/Future.h>");
                }
            }
        }
    }
    
    // Report results
    BOOST_TEST_MESSAGE("Checked " << checked_files.size() << " files for header include consistency");
    
    if (!violations.empty()) {
        BOOST_TEST_MESSAGE("Header include consistency violations found:");
        for (const auto& violation : violations) {
            BOOST_TEST_MESSAGE("  " << violation);
        }
    }
    
    // Property: No violations should be found
    BOOST_TEST(violations.empty(), 
               "Header include consistency violations found. All files should use #include <raft/future.hpp> for future functionality.");
    
    BOOST_TEST_MESSAGE("Header include consistency property test passed");
}

BOOST_AUTO_TEST_CASE(property_future_include_path_consistency, * boost::unit_test::timeout(30)) {
    // Property: Files that include future functionality should use the correct path
    std::vector<std::string> incorrect_paths;
    std::vector<std::string> checked_files;
    
    // Check for incorrect future include paths
    std::vector<std::string> directories = {include_directory, tests_directory, examples_directory};
    
    for (const auto& dir : directories) {
        if (!std::filesystem::exists(dir)) {
            continue;
        }
        
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
            if (entry.is_regular_file() && 
                (entry.path().extension() == ".cpp" || entry.path().extension() == ".hpp")) {
                
                std::string file_path = entry.path().string();
                
                // Skip excluded files
                bool should_exclude = false;
                for (const auto& excluded : excluded_files) {
                    if (file_path.find(excluded) != std::string::npos) {
                        should_exclude = true;
                        break;
                    }
                }
                
                if (should_exclude) {
                    continue;
                }
                
                checked_files.push_back(file_path);
                
                std::ifstream file(file_path);
                if (!file.is_open()) {
                    continue;
                }
                
                std::string content((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
                
                // Check for incorrect future include paths
                std::regex old_future_path(R"(#include\s*<future/future\.hpp>)");
                if (std::regex_search(content, old_future_path)) {
                    incorrect_paths.push_back(file_path + ": uses old path #include <future/future.hpp>");
                }
                
                // Check for correct future include path when kythira::Future is used
                std::regex kythira_future_usage(R"(\bkythira::Future\b)");
                std::regex correct_future_include(R"(#include\s*<raft/future\.hpp>)");
                
                if (std::regex_search(content, kythira_future_usage) && 
                    !std::regex_search(content, correct_future_include)) {
                    incorrect_paths.push_back(file_path + ": uses kythira::Future without #include <raft/future.hpp>");
                }
            }
        }
    }
    
    // Report results
    BOOST_TEST_MESSAGE("Checked " << checked_files.size() << " files for future include path consistency");
    
    if (!incorrect_paths.empty()) {
        BOOST_TEST_MESSAGE("Future include path violations found:");
        for (const auto& violation : incorrect_paths) {
            BOOST_TEST_MESSAGE("  " << violation);
        }
    }
    
    // Property: No incorrect paths should be found
    BOOST_TEST(incorrect_paths.empty(), 
               "Future include path violations found. All files should use #include <raft/future.hpp> for kythira::Future.");
    
    BOOST_TEST_MESSAGE("Future include path consistency property test passed");
}
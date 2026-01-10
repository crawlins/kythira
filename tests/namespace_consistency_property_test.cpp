/**
 * Property Test: Namespace Consistency
 * 
 * **Feature: network-concept-template-fix, Property 2: Namespace consistency**
 * **Validates: Requirements 1.3, 2.5, 3.3, 4.3**
 * 
 * This test validates that all references to network_client or network_server concepts
 * throughout the codebase use the kythira namespace prefix consistently.
 */

#define BOOST_TEST_MODULE namespace_consistency_property_test
#include <boost/test/unit_test.hpp>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

namespace {
    // Get the source directory from the build directory
    auto get_source_directory() -> std::filesystem::path {
        auto current_path = std::filesystem::current_path();
        // If we're in build/tests, go up two levels to get to source root
        if (current_path.filename() == "tests" && current_path.parent_path().filename() == "build") {
            return current_path.parent_path().parent_path();
        }
        // If we're in build, go up one level
        if (current_path.filename() == "build") {
            return current_path.parent_path();
        }
        // Otherwise assume we're already in source root
        return current_path;
    }
    
    const std::vector<std::string> test_directories = {
        "include/raft",
        "examples/raft",
        "tests"
    };
    
    constexpr const char* file_extensions[] = {
        ".hpp",
        ".cpp"
    };
    
    // Pattern to match network concept usages
    const std::regex network_concept_pattern(
        R"(\b(network_client|network_server)\s*<)",
        std::regex_constants::ECMAScript
    );
    
    // Pattern to match correct kythira namespace usage
    const std::regex correct_namespace_pattern(
        R"(\bkythira::(network_client|network_server)\s*<)",
        std::regex_constants::ECMAScript
    );
    
    // Pattern to match incorrect namespace usage (kythira:: or no namespace)
    const std::regex incorrect_namespace_pattern(
        R"(\b(?:kythira::)?(network_client|network_server)\s*<)",
        std::regex_constants::ECMAScript
    );
}

auto collect_source_files() -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> files;
    auto source_root = get_source_directory();
    
    for (const auto& dir : test_directories) {
        auto full_dir_path = source_root / dir;
        if (!std::filesystem::exists(full_dir_path)) {
            continue;
        }
        
        for (const auto& entry : std::filesystem::recursive_directory_iterator(full_dir_path)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            
            const auto& path = entry.path();
            const auto extension = path.extension().string();
            
            for (const auto& valid_ext : file_extensions) {
                if (extension == valid_ext) {
                    files.push_back(path);
                    break;
                }
            }
        }
    }
    
    return files;
}

auto read_file_content(const std::filesystem::path& file_path) -> std::string {
    std::ifstream file(file_path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + file_path.string());
    }
    
    return std::string(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()
    );
}

auto find_network_concept_usages(const std::string& content) -> std::vector<std::string> {
    std::vector<std::string> usages;
    std::sregex_iterator iter(content.begin(), content.end(), network_concept_pattern);
    std::sregex_iterator end;
    
    for (; iter != end; ++iter) {
        const auto& match = *iter;
        // Extract a larger context around the match for better analysis
        auto start_pos = std::max(0, static_cast<int>(match.position()) - 20);
        auto end_pos = std::min(static_cast<int>(content.length()), 
                               static_cast<int>(match.position()) + static_cast<int>(match.length()) + 50);
        
        std::string context = content.substr(start_pos, end_pos - start_pos);
        usages.push_back(context);
    }
    
    return usages;
}

auto has_correct_namespace(const std::string& usage) -> bool {
    return std::regex_search(usage, correct_namespace_pattern);
}

BOOST_AUTO_TEST_CASE(namespace_consistency_property_test, * boost::unit_test::timeout(60)) {
    // **Feature: network-concept-template-fix, Property 2: Namespace consistency**
    // **Validates: Requirements 1.3, 2.5, 3.3, 4.3**
    
    auto source_files = collect_source_files();
    BOOST_REQUIRE(!source_files.empty());
    
    std::vector<std::string> violations;
    int total_usages = 0;
    int correct_usages = 0;
    
    for (const auto& file_path : source_files) {
        try {
            auto content = read_file_content(file_path);
            auto usages = find_network_concept_usages(content);
            
            for (const auto& usage : usages) {
                total_usages++;
                
                if (has_correct_namespace(usage)) {
                    correct_usages++;
                } else {
                    violations.push_back(
                        "File: " + file_path.string() + "\n" +
                        "Usage: " + usage + "\n"
                    );
                }
            }
        } catch (const std::exception& e) {
            BOOST_FAIL("Failed to process file " + file_path.string() + ": " + e.what());
        }
    }
    
    // Report findings
    BOOST_TEST_MESSAGE("Total network concept usages found: " << total_usages);
    BOOST_TEST_MESSAGE("Correct namespace usages: " << correct_usages);
    BOOST_TEST_MESSAGE("Violations: " << violations.size());
    
    if (!violations.empty()) {
        BOOST_TEST_MESSAGE("Namespace consistency violations:");
        for (const auto& violation : violations) {
            BOOST_TEST_MESSAGE(violation);
        }
    }
    
    // Property: For any reference to network_client or network_server concepts,
    // the kythira namespace prefix should be used
    BOOST_CHECK_MESSAGE(
        violations.empty(),
        "All network concept references must use kythira namespace prefix. "
        "Found " + std::to_string(violations.size()) + " violations."
    );
    
    // Additional check: ensure we found some usages to validate the test is working
    BOOST_CHECK_MESSAGE(
        total_usages > 0,
        "Expected to find network concept usages in the codebase for validation"
    );
}
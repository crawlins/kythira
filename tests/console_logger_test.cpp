#include <raft/console_logger.hpp>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
    constexpr const char* test_message = "Test message";
    constexpr const char* test_key = "key";
    constexpr const char* test_value = "value";
    constexpr std::size_t concurrent_threads = 4;
    constexpr std::size_t messages_per_thread = 10;
}

auto test_basic_logging() -> bool {
    std::cout << "Test 1: Basic logging methods\n";
    
    try {
        kythira::console_logger logger;
        
        logger.trace("This is a trace message");
        logger.debug("This is a debug message");
        logger.info("This is an info message");
        logger.warning("This is a warning message");
        logger.error("This is an error message");
        logger.critical("This is a critical message");
        
        std::cout << "  ✓ Basic logging passed\n\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Basic logging failed: " << e.what() << "\n\n";
        return false;
    }
}

auto test_structured_logging() -> bool {
    std::cout << "Test 2: Structured logging with key-value pairs\n";
    
    try {
        kythira::console_logger logger;
        
        logger.log(
            kythira::log_level::info,
            "Leader election started",
            {
                {"term", "42"},
                {"candidate_id", "node_1"},
                {"timeout_ms", "150"}
            }
        );
        
        logger.log(
            kythira::log_level::warning,
            "Network partition detected",
            {
                {"affected_nodes", "3"},
                {"partition_id", "p1"}
            }
        );
        
        logger.log(
            kythira::log_level::error,
            "Persistence failure",
            {
                {"error_code", "ENOSPC"},
                {"path", "/var/raft/log"}
            }
        );
        
        std::cout << "  ✓ Structured logging passed\n\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Structured logging failed: " << e.what() << "\n\n";
        return false;
    }
}

auto test_log_level_filtering() -> bool {
    std::cout << "Test 3: Log level filtering\n";
    
    try {
        kythira::console_logger logger(kythira::log_level::warning);
        
        std::cout << "  (Messages below WARNING should not appear)\n";
        logger.trace("This should not appear");
        logger.debug("This should not appear");
        logger.info("This should not appear");
        logger.warning("This warning should appear");
        logger.error("This error should appear");
        logger.critical("This critical should appear");
        
        // Verify min level getter
        if (logger.get_min_level() != kythira::log_level::warning) {
            std::cerr << "  ✗ Min level getter failed\n\n";
            return false;
        }
        
        // Change min level
        logger.set_min_level(kythira::log_level::error);
        std::cout << "  (Changed min level to ERROR)\n";
        logger.warning("This warning should not appear");
        logger.error("This error should appear");
        
        std::cout << "  ✓ Log level filtering passed\n\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Log level filtering failed: " << e.what() << "\n\n";
        return false;
    }
}

auto test_thread_safety() -> bool {
    std::cout << "Test 4: Thread safety\n";
    
    try {
        kythira::console_logger logger;
        std::vector<std::thread> threads;
        
        // Launch multiple threads that log concurrently
        for (std::size_t i = 0; i < concurrent_threads; ++i) {
            threads.emplace_back([&logger, i]() {
                for (std::size_t j = 0; j < messages_per_thread; ++j) {
                    logger.info(std::string("Thread ") + std::to_string(i) + 
                               " message " + std::to_string(j));
                    
                    logger.log(
                        kythira::log_level::debug,
                        "Structured message",
                        {
                            {"thread_id", std::to_string(i)},
                            {"message_id", std::to_string(j)}
                        }
                    );
                }
            });
        }
        
        // Wait for all threads to complete
        for (auto& thread : threads) {
            thread.join();
        }
        
        std::cout << "  ✓ Thread safety passed\n\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Thread safety failed: " << e.what() << "\n\n";
        return false;
    }
}

auto test_concept_satisfaction() -> bool {
    std::cout << "Test 5: Concept satisfaction\n";
    
    // Compile-time verification
    static_assert(kythira::diagnostic_logger<kythira::console_logger>,
        "console_logger must satisfy diagnostic_logger concept");
    
    std::cout << "  ✓ Concept satisfaction passed\n\n";
    return true;
}

auto main() -> int {
    std::cout << "Testing console_logger implementation\n";
    std::cout << std::string(60, '=') << "\n\n";
    
    int failed_tests = 0;
    
    if (!test_basic_logging()) failed_tests++;
    if (!test_structured_logging()) failed_tests++;
    if (!test_log_level_filtering()) failed_tests++;
    if (!test_thread_safety()) failed_tests++;
    if (!test_concept_satisfaction()) failed_tests++;
    
    std::cout << std::string(60, '=') << "\n";
    
    if (failed_tests > 0) {
        std::cerr << failed_tests << " test(s) failed\n";
        return 1;
    }
    
    std::cout << "All tests passed!\n";
    return 0;
}

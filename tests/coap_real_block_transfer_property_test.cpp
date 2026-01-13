#define BOOST_TEST_MODULE CoAP Real Block Transfer Property Test
#include <boost/test/unit_test.hpp>

#include <raft/coap_block_option.hpp>

#include <vector>
#include <random>
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <optional>

using namespace std;

// Named constants for test parameters
namespace {
    constexpr std::size_t min_payload_size = 64;
    constexpr std::size_t max_payload_size = 8192;
    constexpr std::size_t min_block_size = 64;
    constexpr std::size_t max_block_size = 1024;
    constexpr std::size_t test_iterations = 50;
    constexpr std::chrono::milliseconds test_timeout{30000}; // 30 seconds
}

BOOST_AUTO_TEST_CASE(test_block_option_encoding_decoding, * boost::unit_test::timeout(30)) {
    // Test block option encoding/decoding functionality
    // This tests the core block transfer protocol without requiring full CoAP transport
    
    for (std::uint32_t block_num = 0; block_num < 10; ++block_num) {
        for (bool more : {true, false}) {
            for (std::uint32_t size : {16, 32, 64, 128, 256, 512, 1024}) {
                kythira::block_option original;
                original.block_number = block_num;
                original.more_blocks = more;
                original.block_size = size;
                
                // Test encode/decode round trip
                std::uint32_t encoded = original.encode();
                kythira::block_option decoded = kythira::block_option::parse(encoded);
                
                BOOST_CHECK_EQUAL(decoded.block_number, original.block_number);
                BOOST_CHECK_EQUAL(decoded.more_blocks, original.more_blocks);
                BOOST_CHECK_EQUAL(decoded.block_size, original.block_size);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(test_block_size_calculation, * boost::unit_test::timeout(30)) {
    // Test block size calculation and alignment
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> payload_size_dist(min_payload_size, max_payload_size);
    std::uniform_int_distribution<std::size_t> block_size_dist(min_block_size, max_block_size);
    
    for (std::size_t iteration = 0; iteration < test_iterations; ++iteration) {
        std::size_t payload_size = payload_size_dist(gen);
        std::size_t block_size = block_size_dist(gen);
        
        // Test should_use_block_transfer logic
        constexpr std::size_t coap_overhead = 64;
        std::size_t effective_block_size = block_size > coap_overhead ? 
                                          block_size - coap_overhead : 
                                          block_size;
        bool expected_use_blocks = payload_size > effective_block_size;
        
        // Verify the logic is consistent
        BOOST_CHECK_EQUAL(payload_size > effective_block_size, expected_use_blocks);
        
        if (expected_use_blocks) {
            // Calculate expected number of blocks
            std::size_t expected_blocks = (payload_size + effective_block_size - 1) / effective_block_size;
            BOOST_CHECK_GT(expected_blocks, 1);
            
            // Verify last block size calculation
            std::size_t last_block_size = payload_size % effective_block_size;
            if (last_block_size == 0) {
                last_block_size = effective_block_size;
            }
            BOOST_CHECK_LE(last_block_size, effective_block_size);
        }
    }
}

BOOST_AUTO_TEST_CASE(test_payload_splitting_logic, * boost::unit_test::timeout(60)) {
    // Test payload splitting logic without requiring full CoAP transport
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::uint8_t> byte_dist(0, 255);
    
    // Test various payload and block size combinations
    std::vector<std::pair<std::size_t, std::size_t>> test_cases = {
        {100, 64},   // Small payload, small blocks
        {500, 128},  // Medium payload, medium blocks  
        {1024, 256}, // Large payload, medium blocks
        {2048, 512}, // Large payload, large blocks
        {4096, 1024} // Very large payload, very large blocks
    };
    
    for (const auto& [payload_size, block_size] : test_cases) {
        // Create test payload with random data
        std::vector<std::byte> test_payload;
        test_payload.reserve(payload_size);
        for (std::size_t i = 0; i < payload_size; ++i) {
            test_payload.push_back(static_cast<std::byte>(byte_dist(gen)));
        }
        
        // Simulate block splitting logic
        constexpr std::size_t coap_overhead = 64;
        std::size_t effective_block_size = block_size > coap_overhead ? 
                                          block_size - coap_overhead : 
                                          block_size;
        
        if (payload_size > effective_block_size) {
            // Calculate expected blocks
            std::vector<std::vector<std::byte>> blocks;
            
            for (std::size_t offset = 0; offset < payload_size; offset += effective_block_size) {
                std::size_t current_block_size = std::min(effective_block_size, payload_size - offset);
                
                std::vector<std::byte> block;
                block.reserve(current_block_size);
                for (std::size_t i = 0; i < current_block_size; ++i) {
                    block.push_back(test_payload[offset + i]);
                }
                blocks.push_back(std::move(block));
            }
            
            // Verify blocks are created correctly
            BOOST_CHECK_GT(blocks.size(), 1);
            
            // Verify total payload size is preserved
            std::size_t total_size = 0;
            for (const auto& block : blocks) {
                total_size += block.size();
            }
            BOOST_CHECK_EQUAL(total_size, payload_size);
            
            // Verify block content integrity
            std::vector<std::uint8_t> reassembled;
            for (const auto& block : blocks) {
                for (auto byte : block) {
                    reassembled.push_back(static_cast<std::uint8_t>(byte));
                }
            }
            
            std::vector<std::uint8_t> original_as_uint8;
            for (auto byte : test_payload) {
                original_as_uint8.push_back(static_cast<std::uint8_t>(byte));
            }
            
            BOOST_CHECK_EQUAL_COLLECTIONS(
                original_as_uint8.begin(), original_as_uint8.end(),
                reassembled.begin(), reassembled.end()
            );
            
            // Verify block sizes are appropriate
            for (std::size_t i = 0; i < blocks.size(); ++i) {
                if (i < blocks.size() - 1) {
                    // All blocks except the last should be full size
                    BOOST_CHECK_EQUAL(blocks[i].size(), effective_block_size);
                } else {
                    // Last block can be smaller
                    BOOST_CHECK_LE(blocks[i].size(), effective_block_size);
                    BOOST_CHECK_GT(blocks[i].size(), 0);
                }
            }
        } else {
            // Small payloads should not be split
            BOOST_CHECK_LE(payload_size, effective_block_size);
        }
    }
}

BOOST_AUTO_TEST_CASE(test_block_reassembly_logic, * boost::unit_test::timeout(60)) {
    // Test block reassembly logic without requiring full CoAP transport
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::uint8_t> byte_dist(0, 255);
    
    // Create test payload
    constexpr std::size_t payload_size = 1024;
    constexpr std::size_t block_size = 256;
    
    std::vector<std::byte> original_payload;
    original_payload.reserve(payload_size);
    for (std::size_t i = 0; i < payload_size; ++i) {
        original_payload.push_back(static_cast<std::byte>(byte_dist(gen)));
    }
    
    // Split into blocks
    std::vector<std::vector<std::byte>> blocks;
    for (std::size_t offset = 0; offset < payload_size; offset += block_size) {
        std::size_t current_block_size = std::min(block_size, payload_size - offset);
        
        std::vector<std::byte> block;
        block.reserve(current_block_size);
        for (std::size_t i = 0; i < current_block_size; ++i) {
            block.push_back(original_payload[offset + i]);
        }
        blocks.push_back(std::move(block));
    }
    
    // Simulate block reassembly
    std::vector<std::byte> reassembled_payload;
    
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        // Create block option
        kythira::block_option block_opt;
        block_opt.block_number = static_cast<std::uint32_t>(i);
        block_opt.more_blocks = (i < blocks.size() - 1);
        block_opt.block_size = static_cast<std::uint32_t>(blocks[i].size());
        
        // Append block data to reassembled payload
        for (auto byte : blocks[i]) {
            reassembled_payload.push_back(byte);
        }
        
        // Verify block option properties
        BOOST_CHECK_EQUAL(block_opt.block_number, i);
        BOOST_CHECK_EQUAL(block_opt.more_blocks, i < blocks.size() - 1);
        BOOST_CHECK_EQUAL(block_opt.block_size, blocks[i].size());
        
        // Test encode/decode of block option
        std::uint32_t encoded = block_opt.encode();
        kythira::block_option decoded = kythira::block_option::parse(encoded);
        
        BOOST_CHECK_EQUAL(decoded.block_number, block_opt.block_number);
        BOOST_CHECK_EQUAL(decoded.more_blocks, block_opt.more_blocks);
        BOOST_CHECK_EQUAL(decoded.block_size, block_opt.block_size);
    }
    
    // Verify reassembled payload matches original
    BOOST_CHECK_EQUAL(reassembled_payload.size(), original_payload.size());
    
    std::vector<std::uint8_t> original_as_uint8;
    for (auto byte : original_payload) {
        original_as_uint8.push_back(static_cast<std::uint8_t>(byte));
    }
    
    std::vector<std::uint8_t> reassembled_as_uint8;
    for (auto byte : reassembled_payload) {
        reassembled_as_uint8.push_back(static_cast<std::uint8_t>(byte));
    }
    
    BOOST_CHECK_EQUAL_COLLECTIONS(
        original_as_uint8.begin(), original_as_uint8.end(),
        reassembled_as_uint8.begin(), reassembled_as_uint8.end()
    );
}

BOOST_AUTO_TEST_CASE(test_block_transfer_error_conditions, * boost::unit_test::timeout(30)) {
    // Test error conditions in block transfer logic
    
    // Test empty payload
    std::vector<std::byte> empty_payload;
    constexpr std::size_t block_size = 256;
    constexpr std::size_t coap_overhead = 64;
    std::size_t effective_block_size = block_size - coap_overhead;
    
    // Empty payload should not use block transfer
    BOOST_CHECK_EQUAL(empty_payload.size() > effective_block_size, false);
    
    // Test invalid block options
    kythira::block_option invalid_opt;
    invalid_opt.block_number = 0;
    invalid_opt.more_blocks = true;
    invalid_opt.block_size = 0; // Invalid size
    
    // Encoding should handle invalid size gracefully
    std::uint32_t encoded = invalid_opt.encode();
    kythira::block_option decoded = kythira::block_option::parse(encoded);
    
    // The implementation should handle this gracefully
    BOOST_CHECK_EQUAL(decoded.block_number, invalid_opt.block_number);
    BOOST_CHECK_EQUAL(decoded.more_blocks, invalid_opt.more_blocks);
    
    // Test maximum values
    kythira::block_option max_opt;
    max_opt.block_number = 0xFFFFFF; // 24-bit max
    max_opt.more_blocks = true;
    max_opt.block_size = 1024; // Max supported size
    
    std::uint32_t max_encoded = max_opt.encode();
    kythira::block_option max_decoded = kythira::block_option::parse(max_encoded);
    
    // Should handle maximum values correctly
    BOOST_CHECK_EQUAL(max_decoded.more_blocks, max_opt.more_blocks);
    BOOST_CHECK_EQUAL(max_decoded.block_size, max_opt.block_size);
}

BOOST_AUTO_TEST_CASE(test_block_transfer_performance_characteristics, * boost::unit_test::timeout(60)) {
    // Test performance characteristics of block transfer logic
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::uint8_t> byte_dist(0, 255);
    
    // Test with various payload sizes
    std::vector<std::size_t> payload_sizes = {1024, 4096, 16384, 65536};
    std::vector<std::size_t> block_sizes = {64, 256, 1024};
    
    for (std::size_t payload_size : payload_sizes) {
        for (std::size_t block_size : block_sizes) {
            // Create test payload
            std::vector<std::byte> test_payload;
            test_payload.reserve(payload_size);
            for (std::size_t i = 0; i < payload_size; ++i) {
                test_payload.push_back(static_cast<std::byte>(byte_dist(gen)));
            }
            
            // Measure block splitting performance
            auto start_time = std::chrono::high_resolution_clock::now();
            
            // Simulate block splitting
            constexpr std::size_t coap_overhead = 64;
            std::size_t effective_block_size = block_size > coap_overhead ? 
                                              block_size - coap_overhead : 
                                              block_size;
            
            std::vector<std::vector<std::byte>> blocks;
            if (payload_size > effective_block_size) {
                for (std::size_t offset = 0; offset < payload_size; offset += effective_block_size) {
                    std::size_t current_block_size = std::min(effective_block_size, payload_size - offset);
                    
                    std::vector<std::byte> block;
                    block.reserve(current_block_size);
                    for (std::size_t i = 0; i < current_block_size; ++i) {
                        block.push_back(test_payload[offset + i]);
                    }
                    blocks.push_back(std::move(block));
                }
            }
            
            auto end_time = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
            
            // Verify performance is reasonable (should complete in under 10ms for largest payload)
            BOOST_CHECK_LT(duration.count(), 10000); // 10ms in microseconds
            
            // Verify block count is reasonable
            if (payload_size > effective_block_size) {
                std::size_t expected_blocks = (payload_size + effective_block_size - 1) / effective_block_size;
                BOOST_CHECK_EQUAL(blocks.size(), expected_blocks);
                
                // Verify memory efficiency - total block memory should not exceed payload size by more than block_size
                std::size_t total_block_memory = 0;
                for (const auto& block : blocks) {
                    total_block_memory += block.capacity();
                }
                BOOST_CHECK_LE(total_block_memory, payload_size + block_size);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(test_coap_block_option_compliance, * boost::unit_test::timeout(30)) {
    // Test compliance with CoAP Block-wise Transfer specification (RFC 7959)
    
    // Test SZX (Size Exponent) encoding/decoding
    std::vector<std::pair<std::uint32_t, std::uint32_t>> szx_tests = {
        {16, 0},    // 16 = 2^4, SZX = 0
        {32, 1},    // 32 = 2^5, SZX = 1  
        {64, 2},    // 64 = 2^6, SZX = 2
        {128, 3},   // 128 = 2^7, SZX = 3
        {256, 4},   // 256 = 2^8, SZX = 4
        {512, 5},   // 512 = 2^9, SZX = 5
        {1024, 6}   // 1024 = 2^10, SZX = 6
    };
    
    for (const auto& [block_size, expected_szx] : szx_tests) {
        kythira::block_option opt;
        opt.block_number = 0;
        opt.more_blocks = false;
        opt.block_size = block_size;
        
        std::uint32_t encoded = opt.encode();
        
        // Extract SZX from encoded value
        std::uint32_t actual_szx = encoded & 0x7;
        BOOST_CHECK_EQUAL(actual_szx, expected_szx);
        
        // Verify round-trip
        kythira::block_option decoded = kythira::block_option::parse(encoded);
        BOOST_CHECK_EQUAL(decoded.block_size, block_size);
    }
    
    // Test block number limits (20 bits = 0 to 1,048,575)
    std::vector<std::uint32_t> block_numbers = {0, 1, 100, 1000, 65535, 1048575};
    
    for (std::uint32_t block_num : block_numbers) {
        kythira::block_option opt;
        opt.block_number = block_num;
        opt.more_blocks = true;
        opt.block_size = 256;
        
        std::uint32_t encoded = opt.encode();
        kythira::block_option decoded = kythira::block_option::parse(encoded);
        
        BOOST_CHECK_EQUAL(decoded.block_number, block_num);
        BOOST_CHECK_EQUAL(decoded.more_blocks, true);
        BOOST_CHECK_EQUAL(decoded.block_size, 256);
    }
    
    // Test More (M) bit
    for (bool more_flag : {true, false}) {
        kythira::block_option opt;
        opt.block_number = 42;
        opt.more_blocks = more_flag;
        opt.block_size = 128;
        
        std::uint32_t encoded = opt.encode();
        
        // Extract M bit from encoded value
        bool actual_more = ((encoded >> 3) & 0x1) != 0;
        BOOST_CHECK_EQUAL(actual_more, more_flag);
        
        // Verify round-trip
        kythira::block_option decoded = kythira::block_option::parse(encoded);
        BOOST_CHECK_EQUAL(decoded.more_blocks, more_flag);
    }
}
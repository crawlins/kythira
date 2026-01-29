#define BOOST_TEST_MODULE CoAPSerializationCachingPropertyTest
#include <boost/test/unit_test.hpp>
#include <boost/test/data/test_case.hpp>

#include <raft/coap_transport.hpp>
#include <raft/json_serializer.hpp>
#include <raft/console_logger.hpp>
#include <raft/noop_metrics.hpp>

#include <chrono>
#include <thread>
#include <random>
#include <algorithm>
#include <vector>
#include <memory>
#include <unordered_set>
#include <functional>

namespace {
    constexpr std::size_t test_cache_size = 100;
    constexpr std::size_t test_small_cache_size = 10;
    constexpr std::size_t test_large_cache_size = 1000;
    constexpr std::size_t test_max_data_size = 8192; // 8KB
    constexpr std::size_t test_min_data_size = 64; // 64 bytes
    constexpr std::size_t test_max_operations = 500;
    constexpr std::size_t test_min_operations = 10;
    constexpr const char* test_multicast_address = "224.0.1.201";
    constexpr std::uint16_t test_multicast_port = 5687;
}

using namespace kythira;

// Test types for CoAP transport
struct test_types {
    using future_type = kythira::Future<std::vector<std::byte>>;
    using serializer_type = json_serializer;
    using logger_type = console_logger;
    using metrics_type = noop_metrics;
    using address_type = std::string;
    using port_type = std::uint16_t;
};

// Property test helper functions
namespace property_helpers {
    
    auto generate_random_cache_size() -> std::size_t {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<std::size_t> dis(test_small_cache_size, test_large_cache_size);
        return dis(gen);
    }
    
    auto generate_random_data_size() -> std::size_t {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<std::size_t> dis(test_min_data_size, test_max_data_size);
        return dis(gen);
    }
    
    auto generate_random_operation_count() -> std::size_t {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<std::size_t> dis(test_min_operations, test_max_operations);
        return dis(gen);
    }
    
    auto generate_random_hash() -> std::size_t {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<std::size_t> dis(1, std::numeric_limits<std::size_t>::max());
        return dis(gen);
    }
    
    auto generate_random_data(std::size_t size) -> std::vector<std::byte> {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<int> dis(0, 255);
        
        std::vector<std::byte> data;
        data.reserve(size);
        
        for (std::size_t i = 0; i < size; ++i) {
            data.push_back(static_cast<std::byte>(dis(gen)));
        }
        
        return data;
    }
    
    auto compute_hash(const std::vector<std::byte>& data) -> std::size_t {
        std::hash<std::string> hasher;
        std::string str;
        str.reserve(data.size());
        for (std::byte b : data) {
            str.push_back(static_cast<char>(b));
        }
        return hasher(str);
    }
    
    auto create_test_client_with_caching(std::size_t cache_size) -> std::unique_ptr<coap_client<test_types>> {
        std::unordered_map<std::uint64_t, std::string> endpoints;
        coap_client_config config;
        config.enable_serialization_caching = true;
        config.serialization_cache_size = cache_size;
        config.enable_multicast = true;
        config.multicast_address = test_multicast_address;
        config.multicast_port = test_multicast_port;
        
        return std::make_unique<coap_client<test_types>>(
            std::move(endpoints),
            std::move(config),
            noop_metrics{}
        );
    }
    
    auto create_test_server_with_caching(std::size_t cache_size) -> std::unique_ptr<coap_server<test_types>> {
        coap_server_config config;
        config.enable_serialization_caching = true;
        config.serialization_cache_size = cache_size;
        config.enable_multicast = true;
        config.multicast_address = test_multicast_address;
        config.multicast_port = test_multicast_port;
        
        return std::make_unique<coap_server<test_types>>(
            "0.0.0.0",
            test_multicast_port,
            std::move(config),
            noop_metrics{}
        );
    }
    
    auto create_test_cache_entry(const std::vector<std::byte>& data) -> cache_entry {
        auto hash = compute_hash(data);
        return cache_entry(data, hash);
    }
}

/**
 * Feature: coap-transport, Property 31: Serialization result caching optimization
 * 
 * Property: For any serialization cache, storing and retrieving data should
 * maintain data integrity and provide correct cache hit/miss behavior.
 * 
 * Validates: Requirements 7.1
 */
BOOST_AUTO_TEST_CASE(property_serialization_cache_basic_operations, * boost::unit_test::timeout(60)) {
    using namespace property_helpers;
    
    // Property-based test with multiple iterations
    for (int iteration = 0; iteration < 100; ++iteration) {
        try {
            // Generate random test parameters
            auto cache_size = generate_random_cache_size();
            auto data_size = generate_random_data_size();
            auto hash = generate_random_hash();
            
            // Create test client with caching
            auto client = create_test_client_with_caching(cache_size);
            
            // Generate test data
            auto test_data = generate_random_data(data_size);
            
            // Property: Cache miss should occur for non-existent data
            auto cached_result = client->get_cached_serialization(hash);
            BOOST_CHECK(!cached_result.has_value());
            
            // Property: Caching data should succeed
            client->cache_serialization(hash, test_data);
            
            // Property: Cache hit should occur for stored data
            auto retrieved_result = client->get_cached_serialization(hash);
            BOOST_REQUIRE(retrieved_result.has_value());
            
            // Property: Retrieved data should match original data
            BOOST_CHECK_EQUAL(retrieved_result->size(), test_data.size());
            BOOST_CHECK(std::equal(retrieved_result->begin(), retrieved_result->end(), test_data.begin()));
            
            // Property: Multiple retrievals should return the same data
            auto second_retrieval = client->get_cached_serialization(hash);
            BOOST_REQUIRE(second_retrieval.has_value());
            BOOST_CHECK(std::equal(second_retrieval->begin(), second_retrieval->end(), test_data.begin()));
            
        } catch (const std::exception& e) {
            BOOST_FAIL("Property test iteration " + std::to_string(iteration) + " failed: " + e.what());
        }
    }
}

/**
 * Feature: coap-transport, Property 31: Serialization cache eviction policy
 * 
 * Property: For any serialization cache that exceeds its capacity, the least
 * recently used entries should be evicted to make room for new entries.
 * 
 * Validates: Requirements 7.1
 */
BOOST_AUTO_TEST_CASE(property_serialization_cache_eviction_policy, * boost::unit_test::timeout(120)) {
    using namespace property_helpers;
    
    // Property-based test with multiple iterations
    for (int iteration = 0; iteration < 30; ++iteration) {
        try {
            // Generate test parameters for eviction testing
            auto cache_size = std::max(std::size_t{5}, std::min(generate_random_cache_size(), std::size_t{50}));
            auto data_size = generate_random_data_size();
            
            // Create test client with small cache for eviction testing
            auto client = create_test_client_with_caching(cache_size);
            
            // Fill cache to capacity
            std::vector<std::pair<std::size_t, std::vector<std::byte>>> cached_items;
            for (std::size_t i = 0; i < cache_size; ++i) {
                auto hash = generate_random_hash();
                auto data = generate_random_data(data_size);
                
                client->cache_serialization(hash, data);
                cached_items.emplace_back(hash, data);
            }
            
            // Property: All items should be retrievable when cache is at capacity
            for (const auto& [hash, data] : cached_items) {
                auto retrieved = client->get_cached_serialization(hash);
                BOOST_REQUIRE(retrieved.has_value());
                BOOST_CHECK(std::equal(retrieved->begin(), retrieved->end(), data.begin()));
            }
            
            // Access some items to update their usage (make them more recently used)
            std::size_t recently_used_count = std::min(cache_size / 2, std::size_t{3});
            for (std::size_t i = 0; i < recently_used_count; ++i) {
                auto retrieved = client->get_cached_serialization(cached_items[i].first);
                BOOST_CHECK(retrieved.has_value());
            }
            
            // Add new items that should trigger eviction
            std::size_t new_items_count = std::min(cache_size / 2, std::size_t{3});
            std::vector<std::pair<std::size_t, std::vector<std::byte>>> new_items;
            
            for (std::size_t i = 0; i < new_items_count; ++i) {
                auto hash = generate_random_hash();
                auto data = generate_random_data(data_size);
                
                client->cache_serialization(hash, data);
                new_items.emplace_back(hash, data);
            }
            
            // Property: New items should be retrievable
            for (const auto& [hash, data] : new_items) {
                auto retrieved = client->get_cached_serialization(hash);
                BOOST_REQUIRE(retrieved.has_value());
                BOOST_CHECK(std::equal(retrieved->begin(), retrieved->end(), data.begin()));
            }
            
            // Property: Recently used items should still be in cache
            for (std::size_t i = 0; i < recently_used_count; ++i) {
                auto retrieved = client->get_cached_serialization(cached_items[i].first);
                BOOST_CHECK(retrieved.has_value());
            }
            
            // Property: Some old items should have been evicted
            std::size_t evicted_count = 0;
            for (std::size_t i = recently_used_count; i < cached_items.size(); ++i) {
                auto retrieved = client->get_cached_serialization(cached_items[i].first);
                if (!retrieved.has_value()) {
                    evicted_count++;
                }
            }
            
            // At least some items should have been evicted to make room
            BOOST_CHECK_GT(evicted_count, 0);
            
        } catch (const std::exception& e) {
            BOOST_FAIL("Property test iteration " + std::to_string(iteration) + " failed: " + e.what());
        }
    }
}

/**
 * Feature: coap-transport, Property 31: Serialization cache concurrent access
 * 
 * Property: For any serialization cache accessed concurrently, all operations
 * should be thread-safe and maintain data consistency.
 * 
 * Validates: Requirements 7.1
 */
BOOST_AUTO_TEST_CASE(property_serialization_cache_concurrent_access, * boost::unit_test::timeout(120)) {
    using namespace property_helpers;
    
    // Property-based test with multiple iterations
    for (int iteration = 0; iteration < 20; ++iteration) {
        try {
            // Generate test parameters for concurrent testing
            auto cache_size = generate_random_cache_size();
            auto thread_count = std::min(std::size_t{8}, std::max(std::size_t{2}, generate_random_operation_count() / 100));
            auto operations_per_thread = std::max(std::size_t{10}, generate_random_operation_count() / thread_count);
            
            // Create test client with caching
            auto client = create_test_client_with_caching(cache_size);
            
            // Shared data for concurrent access
            std::vector<std::pair<std::size_t, std::vector<std::byte>>> shared_data;
            for (std::size_t i = 0; i < operations_per_thread; ++i) {
                auto hash = generate_random_hash();
                auto data = generate_random_data(generate_random_data_size());
                shared_data.emplace_back(hash, data);
            }
            
            // Launch concurrent threads
            std::vector<std::thread> threads;
            std::atomic<std::size_t> cache_hits{0};
            std::atomic<std::size_t> cache_misses{0};
            std::atomic<std::size_t> cache_stores{0};
            
            for (std::size_t t = 0; t < thread_count; ++t) {
                threads.emplace_back([&, t]() {
                    for (std::size_t i = 0; i < operations_per_thread; ++i) {
                        auto& [hash, data] = shared_data[i];
                        
                        // Try to retrieve from cache
                        auto cached_result = client->get_cached_serialization(hash);
                        if (cached_result.has_value()) {
                            cache_hits.fetch_add(1);
                            
                            // Verify data integrity
                            if (!std::equal(cached_result->begin(), cached_result->end(), data.begin())) {
                                BOOST_FAIL("Data integrity violation in concurrent access");
                            }
                        } else {
                            cache_misses.fetch_add(1);
                            
                            // Store in cache
                            client->cache_serialization(hash, data);
                            cache_stores.fetch_add(1);
                        }
                        
                        // Small delay to increase contention
                        std::this_thread::sleep_for(std::chrono::microseconds(1));
                    }
                });
            }
            
            // Wait for all threads to complete
            for (auto& thread : threads) {
                thread.join();
            }
            
            // Property: Total operations should be consistent
            auto total_operations = thread_count * operations_per_thread;
            BOOST_CHECK_EQUAL(cache_hits.load() + cache_misses.load(), total_operations);
            
            // Property: All stored items should be retrievable after concurrent access
            for (const auto& [hash, data] : shared_data) {
                auto retrieved = client->get_cached_serialization(hash);
                if (retrieved.has_value()) {
                    BOOST_CHECK(std::equal(retrieved->begin(), retrieved->end(), data.begin()));
                }
            }
            
            // Property: Cache should have some entries after concurrent operations
            // (At least some operations should have resulted in cached data)
            BOOST_CHECK_GT(cache_stores.load(), 0);
            
        } catch (const std::exception& e) {
            BOOST_FAIL("Property test iteration " + std::to_string(iteration) + " failed: " + e.what());
        }
    }
}

/**
 * Feature: coap-transport, Property 31: Serialization cache performance benefits
 * 
 * Property: For any serialization cache with repeated access patterns, cache
 * hits should provide performance benefits over cache misses.
 * 
 * Validates: Requirements 7.1
 */
BOOST_AUTO_TEST_CASE(property_serialization_cache_performance_benefits, * boost::unit_test::timeout(120)) {
    using namespace property_helpers;
    
    // Property-based test with multiple iterations
    for (int iteration = 0; iteration < 10; ++iteration) {
        try {
            // Generate test parameters for performance testing
            auto cache_size = test_large_cache_size; // Large cache for performance test
            auto data_size = generate_random_data_size();
            auto access_count = std::min(generate_random_operation_count(), std::size_t{1000});
            
            // Create test client with caching
            auto client = create_test_client_with_caching(cache_size);
            
            // Generate test data
            auto hash = generate_random_hash();
            auto test_data = generate_random_data(data_size);
            
            // Pre-populate cache
            client->cache_serialization(hash, test_data);
            
            // Measure cache hit performance
            auto hit_start = std::chrono::high_resolution_clock::now();
            std::size_t successful_hits = 0;
            
            for (std::size_t i = 0; i < access_count; ++i) {
                auto cached_result = client->get_cached_serialization(hash);
                if (cached_result.has_value()) {
                    successful_hits++;
                }
            }
            
            auto hit_end = std::chrono::high_resolution_clock::now();
            auto hit_duration = std::chrono::duration_cast<std::chrono::microseconds>(hit_end - hit_start);
            
            // Measure cache miss performance (using different hashes)
            auto miss_start = std::chrono::high_resolution_clock::now();
            std::size_t cache_misses = 0;
            
            for (std::size_t i = 0; i < access_count; ++i) {
                auto different_hash = generate_random_hash();
                auto cached_result = client->get_cached_serialization(different_hash);
                if (!cached_result.has_value()) {
                    cache_misses++;
                }
            }
            
            auto miss_end = std::chrono::high_resolution_clock::now();
            auto miss_duration = std::chrono::duration_cast<std::chrono::microseconds>(miss_end - miss_start);
            
            // Property: All cache accesses should have succeeded/failed as expected
            BOOST_CHECK_EQUAL(successful_hits, access_count);
            BOOST_CHECK_EQUAL(cache_misses, access_count);
            
            // Property: Cache hits should be reasonably fast
            if (successful_hits > 0) {
                auto avg_hit_time = hit_duration.count() / successful_hits;
                BOOST_CHECK_LT(avg_hit_time, 1000); // Less than 1ms per hit on average
            }
            
            // Property: Cache performance should be consistent
            // (Both hits and misses should complete in reasonable time)
            BOOST_CHECK_LT(hit_duration.count(), 10000000); // Less than 10 seconds total
            BOOST_CHECK_LT(miss_duration.count(), 10000000); // Less than 10 seconds total
            
        } catch (const std::exception& e) {
            BOOST_FAIL("Property test iteration " + std::to_string(iteration) + " failed: " + e.what());
        }
    }
}

/**
 * Feature: coap-transport, Property 31: Serialization cache entry lifecycle
 * 
 * Property: For any cache entry, its lifecycle properties (creation time,
 * access count, age) should be accurately maintained and updated.
 * 
 * Validates: Requirements 7.1
 */
BOOST_AUTO_TEST_CASE(property_serialization_cache_entry_lifecycle, * boost::unit_test::timeout(60)) {
    using namespace property_helpers;
    
    // Property-based test with multiple iterations
    for (int iteration = 0; iteration < 50; ++iteration) {
        try {
            // Generate test parameters
            auto data_size = generate_random_data_size();
            auto access_count = std::min(generate_random_operation_count(), std::size_t{100});
            
            // Create test data and cache entry
            auto test_data = generate_random_data(data_size);
            auto entry = create_test_cache_entry(test_data);
            
            // Property: New entry should have correct initial state
            BOOST_CHECK_EQUAL(entry.access_count, 1); // Constructor sets initial access count
            BOOST_CHECK_EQUAL(entry.serialized_data.size(), test_data.size());
            BOOST_CHECK(std::equal(entry.serialized_data.begin(), entry.serialized_data.end(), test_data.begin()));
            
            // Property: Age should be minimal for new entry
            auto initial_age = entry.age();
            BOOST_CHECK_LT(initial_age.count(), 1000); // Less than 1 second
            
            // Property: Time since last access should be minimal
            auto initial_time_since_access = entry.time_since_last_access();
            BOOST_CHECK_LT(initial_time_since_access.count(), 1000); // Less than 1 second
            
            // Simulate multiple accesses
            auto initial_access_count = entry.access_count;
            for (std::size_t i = 0; i < access_count; ++i) {
                entry.touch();
                std::this_thread::sleep_for(std::chrono::microseconds(100)); // Small delay
            }
            
            // Property: Access count should be updated correctly
            BOOST_CHECK_EQUAL(entry.access_count, initial_access_count + access_count);
            
            // Property: Age should have increased
            auto final_age = entry.age();
            BOOST_CHECK_GT(final_age, initial_age);
            
            // Property: Time since last access should be recent
            auto final_time_since_access = entry.time_since_last_access();
            BOOST_CHECK_LT(final_time_since_access.count(), 1000); // Should be recent due to touch()
            
            // Wait a bit and check aging
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            
            auto aged_time_since_access = entry.time_since_last_access();
            BOOST_CHECK_GT(aged_time_since_access, final_time_since_access);
            
        } catch (const std::exception& e) {
            BOOST_FAIL("Property test iteration " + std::to_string(iteration) + " failed: " + e.what());
        }
    }
}

/**
 * Feature: coap-transport, Property 31: Serialization cache hash collision handling
 * 
 * Property: For any serialization cache, hash collisions should be handled
 * correctly by overwriting existing entries with the same hash.
 * 
 * Validates: Requirements 7.1
 */
BOOST_AUTO_TEST_CASE(property_serialization_cache_hash_collision_handling, * boost::unit_test::timeout(60)) {
    using namespace property_helpers;
    
    // Property-based test with multiple iterations
    for (int iteration = 0; iteration < 50; ++iteration) {
        try {
            // Generate test parameters
            auto cache_size = generate_random_cache_size();
            auto hash = generate_random_hash();
            
            // Create test client with caching
            auto client = create_test_client_with_caching(cache_size);
            
            // Create first data set
            auto first_data = generate_random_data(generate_random_data_size());
            client->cache_serialization(hash, first_data);
            
            // Property: First data should be retrievable
            auto first_retrieval = client->get_cached_serialization(hash);
            BOOST_REQUIRE(first_retrieval.has_value());
            BOOST_CHECK(std::equal(first_retrieval->begin(), first_retrieval->end(), first_data.begin()));
            
            // Create second data set with same hash (simulating collision)
            auto second_data = generate_random_data(generate_random_data_size());
            client->cache_serialization(hash, second_data);
            
            // Property: Second data should overwrite first data
            auto second_retrieval = client->get_cached_serialization(hash);
            BOOST_REQUIRE(second_retrieval.has_value());
            BOOST_CHECK(std::equal(second_retrieval->begin(), second_retrieval->end(), second_data.begin()));
            
            // Property: First data should no longer be retrievable
            BOOST_CHECK(!std::equal(second_retrieval->begin(), second_retrieval->end(), first_data.begin()));
            
            // Create third data set with same hash
            auto third_data = generate_random_data(generate_random_data_size());
            client->cache_serialization(hash, third_data);
            
            // Property: Third data should overwrite second data
            auto third_retrieval = client->get_cached_serialization(hash);
            BOOST_REQUIRE(third_retrieval.has_value());
            BOOST_CHECK(std::equal(third_retrieval->begin(), third_retrieval->end(), third_data.begin()));
            
            // Property: Only the most recent data should be retrievable
            BOOST_CHECK(!std::equal(third_retrieval->begin(), third_retrieval->end(), first_data.begin()));
            BOOST_CHECK(!std::equal(third_retrieval->begin(), third_retrieval->end(), second_data.begin()));
            
        } catch (const std::exception& e) {
            BOOST_FAIL("Property test iteration " + std::to_string(iteration) + " failed: " + e.what());
        }
    }
}
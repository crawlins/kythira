#pragma once

#include <vector>
#include <unordered_map>
#include <shared_mutex>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <cstddef>
#include <string>
#include <sstream>

namespace kythira {

// Forward declarations
class memory_pool;
class memory_pool_guard;

// Memory pool metrics structure
struct memory_pool_metrics {
    std::size_t total_size{0};           // Total pool size in bytes
    std::size_t allocated_size{0};       // Currently allocated bytes
    std::size_t free_size{0};            // Available bytes
    std::size_t allocation_count{0};     // Total allocations
    std::size_t deallocation_count{0};   // Total deallocations
    std::size_t peak_usage{0};           // Peak memory usage
    std::size_t fragmentation_ratio{0};  // Fragmentation percentage
    std::chrono::steady_clock::time_point last_reset;
};

// Memory leak information structure
struct memory_leak_info {
    void* address{nullptr};              // Address of leaked memory
    std::size_t size{0};                 // Size of allocation
    std::chrono::steady_clock::time_point allocation_time;
    std::string allocation_context;      // Stack trace or context info
    std::chrono::seconds age;            // Age of allocation
    std::string thread_id;               // Thread that allocated
};

// Memory block structure for fixed-size block allocation
struct memory_block {
    std::byte* data{nullptr};            // Pointer to block data
    std::size_t size{0};                 // Size of the block
    bool is_free{true};                  // Whether block is available
    std::chrono::steady_clock::time_point allocation_time;
    std::string allocation_context;      // Context info for leak detection
    std::string thread_id;               // Thread ID that allocated
    
    memory_block() = default;
    
    memory_block(std::byte* ptr, std::size_t sz)
        : data(ptr)
        , size(sz)
        , is_free(true)
        , allocation_time(std::chrono::steady_clock::now())
    {}
};

// Memory pool class with fixed-size block allocation
class memory_pool {
public:
    // Constructor: Initialize pool with specified size and block size
    memory_pool(std::size_t pool_size, std::size_t block_size, 
                std::chrono::seconds reset_interval = std::chrono::seconds{0},
                bool enable_leak_detection = false,
                std::chrono::seconds leak_threshold = std::chrono::seconds{60})
        : _pool_size(pool_size)
        , _block_size(block_size)
        , _pool_memory(pool_size)
        , _reset_interval(reset_interval)
        , _periodic_reset_enabled(reset_interval.count() > 0)
        , _leak_detection_enabled(enable_leak_detection)
        , _leak_threshold(leak_threshold)
    {
        _metrics.total_size = pool_size;
        _metrics.free_size = pool_size;
        _metrics.last_reset = std::chrono::steady_clock::now();
        
        // Initialize free blocks
        initialize_blocks();
        
        // Start periodic reset thread if enabled
        if (_periodic_reset_enabled) {
            _reset_thread = std::thread([this]() { periodic_reset_worker(); });
        }
    }
    
    // Destructor: Clean up resources
    ~memory_pool() {
        // Stop periodic reset thread
        {
            std::unique_lock<std::shared_mutex> lock(_mutex);
            _shutdown = true;
        }
        _reset_cv.notify_all();
        
        if (_reset_thread.joinable()) {
            _reset_thread.join();
        }
        
        // Clean up allocations
        std::unique_lock<std::shared_mutex> lock(_mutex);
        _free_blocks.clear();
        _allocations.clear();
        _allocation_contexts.clear();
    }
    
    // Allocate memory from the pool
    auto allocate(std::size_t size, const std::string& context = "") -> void* {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        
        // Check if requested size exceeds block size
        if (size > _block_size) {
            return nullptr;
        }
        
        // Find a free block
        if (_free_blocks.empty()) {
            return nullptr; // Pool exhausted
        }
        
        // Get a free block from the front
        void* block_ptr = _free_blocks.front();
        _free_blocks.erase(_free_blocks.begin());
        
        // Record allocation
        _allocations[block_ptr] = size;
        
        // Capture allocation context if leak detection is enabled
        if (_leak_detection_enabled) {
            allocation_info info;
            info.timestamp = std::chrono::steady_clock::now();
            info.context = context.empty() ? capture_allocation_context() : context;
            info.thread_id = get_thread_id();
            _allocation_contexts[block_ptr] = info;
        } else {
            // Just store timestamp for basic tracking
            allocation_info info;
            info.timestamp = std::chrono::steady_clock::now();
            _allocation_contexts[block_ptr] = info;
        }
        
        // Update metrics
        _metrics.allocation_count++;
        _metrics.allocated_size += _block_size;
        _metrics.free_size -= _block_size;
        
        // Track peak usage
        if (_metrics.allocated_size > _metrics.peak_usage) {
            _metrics.peak_usage = _metrics.allocated_size;
        }
        
        return block_ptr;
    }
    
    // Deallocate memory back to the pool
    auto deallocate(void* ptr) -> void {
        if (ptr == nullptr) {
            return;
        }
        
        std::unique_lock<std::shared_mutex> lock(_mutex);
        
        // Check if this is a valid allocation
        auto it = _allocations.find(ptr);
        if (it == _allocations.end()) {
            return; // Not a valid allocation from this pool
        }
        
        // Remove from allocations
        std::size_t size = it->second;
        _allocations.erase(it);
        _allocation_contexts.erase(ptr);
        
        // Return block to free list
        _free_blocks.push_back(ptr);
        
        // Update metrics
        _metrics.deallocation_count++;
        _metrics.allocated_size -= _block_size;
        _metrics.free_size += _block_size;
    }
    
    // Reset the pool (defragment and reclaim memory)
    auto reset() -> void {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        
        // Clear all allocations
        _allocations.clear();
        _allocation_contexts.clear();
        _free_blocks.clear();
        
        // Reinitialize blocks
        initialize_blocks_unlocked();
        
        // Update metrics
        _metrics.allocated_size = 0;
        _metrics.free_size = _pool_size;
        _metrics.last_reset = std::chrono::steady_clock::now();
    }
    
    // Enable or disable periodic reset
    auto set_periodic_reset(bool enabled, std::chrono::seconds interval = std::chrono::seconds{300}) -> void {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        
        if (enabled && !_periodic_reset_enabled) {
            _reset_interval = interval;
            _periodic_reset_enabled = true;
            
            // Start reset thread if not already running
            if (!_reset_thread.joinable()) {
                _reset_thread = std::thread([this]() { periodic_reset_worker(); });
            }
        } else if (!enabled && _periodic_reset_enabled) {
            _periodic_reset_enabled = false;
            _reset_cv.notify_all();
        }
    }
    
    // Get time since last reset
    auto time_since_last_reset() const -> std::chrono::seconds {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::seconds>(now - _metrics.last_reset);
    }
    
    // Create RAII guard for automatic cleanup
    auto allocate_guarded(std::size_t size, const std::string& context = "") -> memory_pool_guard;
    
    // Get current pool metrics
    auto get_metrics() const -> memory_pool_metrics {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        
        // Calculate fragmentation ratio
        memory_pool_metrics metrics = _metrics;
        if (_pool_size > 0) {
            std::size_t used_blocks = _allocations.size();
            std::size_t total_blocks = _pool_size / _block_size;
            if (total_blocks > 0) {
                metrics.fragmentation_ratio = 
                    static_cast<std::size_t>((1.0 - static_cast<double>(used_blocks) / total_blocks) * 100);
            }
        }
        
        return metrics;
    }
    
    // Detect memory leaks (allocations that have been held too long)
    auto detect_leaks() -> std::vector<memory_leak_info> {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        
        std::vector<memory_leak_info> leaks;
        auto now = std::chrono::steady_clock::now();
        
        for (const auto& [ptr, size] : _allocations) {
            auto alloc_info_it = _allocation_contexts.find(ptr);
            if (alloc_info_it != _allocation_contexts.end()) {
                const auto& info = alloc_info_it->second;
                auto age = std::chrono::duration_cast<std::chrono::seconds>(now - info.timestamp);
                
                // Use >= to catch allocations that are exactly at the threshold
                if (age >= _leak_threshold) {
                    memory_leak_info leak;
                    leak.address = ptr;
                    leak.size = size;
                    leak.allocation_time = info.timestamp;
                    leak.age = age;
                    
                    // Include detailed context if leak detection is enabled
                    if (_leak_detection_enabled) {
                        leak.allocation_context = info.context;
                        leak.thread_id = info.thread_id;
                    } else {
                        leak.allocation_context = "Long-lived allocation detected (enable leak detection for details)";
                        leak.thread_id = "unknown";
                    }
                    
                    leaks.push_back(leak);
                }
            }
        }
        
        return leaks;
    }
    
    // Enable or disable leak detection
    auto set_leak_detection(bool enabled, std::chrono::seconds threshold = std::chrono::seconds{60}) -> void {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        _leak_detection_enabled = enabled;
        _leak_threshold = threshold;
    }
    
    // Check if leak detection is enabled
    auto is_leak_detection_enabled() const -> bool {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        return _leak_detection_enabled;
    }
    
    // Get current leak threshold
    auto get_leak_threshold() const -> std::chrono::seconds {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        return _leak_threshold;
    }
    
    // Get pool utilization percentage
    auto get_utilization_percentage() const -> double {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        if (_pool_size == 0) {
            return 0.0;
        }
        return (static_cast<double>(_metrics.allocated_size) / _pool_size) * 100.0;
    }
    
    // Check if pool is exhausted
    auto is_exhausted() const -> bool {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        return _free_blocks.empty();
    }
    
    // Get available space
    auto available_space() const -> std::size_t {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        return _metrics.free_size;
    }
    
    // Get number of free blocks
    auto free_block_count() const -> std::size_t {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        return _free_blocks.size();
    }
    
    // Get number of allocated blocks
    auto allocated_block_count() const -> std::size_t {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        return _allocations.size();
    }

private:
    // Allocation information structure for leak detection
    struct allocation_info {
        std::chrono::steady_clock::time_point timestamp;
        std::string context;
        std::string thread_id;
    };
    
    // Capture allocation context (simplified stack trace)
    auto capture_allocation_context() -> std::string {
        // In a real implementation, this would capture a stack trace
        // For now, we'll return a simple context string
        return "allocation_context_captured";
    }
    
    // Get current thread ID as string
    auto get_thread_id() -> std::string {
        std::ostringstream oss;
        oss << std::this_thread::get_id();
        return oss.str();
    }
    
    // Initialize blocks (called from constructor)
    auto initialize_blocks() -> void {
        std::size_t num_blocks = _pool_size / _block_size;
        _free_blocks.reserve(num_blocks);
        
        for (std::size_t i = 0; i < num_blocks; ++i) {
            void* block_ptr = _pool_memory.data() + (i * _block_size);
            _free_blocks.push_back(block_ptr);
        }
    }
    
    // Initialize blocks without locking (called from reset)
    auto initialize_blocks_unlocked() -> void {
        std::size_t num_blocks = _pool_size / _block_size;
        _free_blocks.reserve(num_blocks);
        
        for (std::size_t i = 0; i < num_blocks; ++i) {
            void* block_ptr = _pool_memory.data() + (i * _block_size);
            _free_blocks.push_back(block_ptr);
        }
    }
    
    // Periodic reset worker thread
    auto periodic_reset_worker() -> void {
        while (true) {
            std::unique_lock<std::shared_mutex> lock(_mutex);
            
            // Wait for reset interval or shutdown
            _reset_cv.wait_for(lock, _reset_interval, [this]() {
                return _shutdown || !_periodic_reset_enabled;
            });
            
            if (_shutdown) {
                break;
            }
            
            if (!_periodic_reset_enabled) {
                continue;
            }
            
            // Perform reset if there are no active allocations
            if (_allocations.empty()) {
                lock.unlock();
                reset();
            }
        }
    }

    // Pool configuration
    std::size_t _pool_size;
    std::size_t _block_size;
    
    // Pool memory storage
    std::vector<std::byte> _pool_memory;
    
    // Block management
    std::vector<void*> _free_blocks;
    std::unordered_map<void*, std::size_t> _allocations;
    std::unordered_map<void*, allocation_info> _allocation_contexts;
    
    // Metrics
    memory_pool_metrics _metrics;
    
    // Periodic reset
    std::chrono::seconds _reset_interval;
    bool _periodic_reset_enabled{false};
    bool _shutdown{false};
    std::thread _reset_thread;
    std::condition_variable_any _reset_cv;
    
    // Leak detection
    bool _leak_detection_enabled{false};
    std::chrono::seconds _leak_threshold{60};
    
    // Thread safety
    mutable std::shared_mutex _mutex;
};

// RAII guard for automatic memory pool cleanup
class memory_pool_guard {
public:
    memory_pool_guard(memory_pool& pool, void* ptr)
        : _pool(pool)
        , _ptr(ptr)
    {}
    
    ~memory_pool_guard() {
        if (_ptr) {
            _pool.deallocate(_ptr);
        }
    }
    
    // Disable copy
    memory_pool_guard(const memory_pool_guard&) = delete;
    memory_pool_guard& operator=(const memory_pool_guard&) = delete;
    
    // Enable move
    memory_pool_guard(memory_pool_guard&& other) noexcept
        : _pool(other._pool)
        , _ptr(other._ptr)
    {
        other._ptr = nullptr;
    }
    
    memory_pool_guard& operator=(memory_pool_guard&& other) noexcept {
        if (this != &other) {
            if (_ptr) {
                _pool.deallocate(_ptr);
            }
            _ptr = other._ptr;
            other._ptr = nullptr;
        }
        return *this;
    }
    
    auto get() const -> void* { return _ptr; }
    auto release() -> void* {
        void* ptr = _ptr;
        _ptr = nullptr;
        return ptr;
    }

private:
    memory_pool& _pool;
    void* _ptr;
};

// Implementation of allocate_guarded
inline auto memory_pool::allocate_guarded(std::size_t size, const std::string& context) -> memory_pool_guard {
    void* ptr = allocate(size, context);
    return memory_pool_guard(*this, ptr);
}

} // namespace kythira

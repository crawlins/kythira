#include <raft/metrics.hpp>
#include <cassert>
#include <chrono>

// Test that noop_metrics satisfies the metrics concept
static_assert(raft::metrics<raft::noop_metrics>, 
    "noop_metrics must satisfy metrics concept");

int main() {
    // Create a noop_metrics instance
    raft::noop_metrics metrics;
    
    // Test all operations - they should all be no-ops
    metrics.set_metric_name("test_metric");
    metrics.add_dimension("node_id", "node_1");
    metrics.add_dimension("cluster", "test_cluster");
    
    // Test recording methods
    metrics.add_one();
    metrics.add_count(100);
    metrics.add_duration(std::chrono::milliseconds{500});
    metrics.add_value(42.5);
    
    // Test emission
    metrics.emit();
    
    // All operations should complete without error
    // Since it's a no-op implementation, there's nothing to verify
    // except that it compiles and runs
    
    return 0;
}

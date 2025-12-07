#include <raft/metrics.hpp>
#include <string_view>
#include <chrono>

// Template function that uses metrics concept
template<raft::metrics M>
auto record_operation(M& metric, std::string_view operation_name, std::chrono::nanoseconds duration) -> void {
    metric.set_metric_name(operation_name);
    metric.add_dimension("operation_type", "test");
    metric.add_duration(duration);
    metric.add_one();
    metric.emit();
}

// Template class that uses metrics concept
template<raft::metrics M>
class operation_tracker {
public:
    explicit operation_tracker(M& metric) : _metric(metric) {}
    
    auto track_operation(std::string_view name, std::int64_t count) -> void {
        _metric.set_metric_name(name);
        _metric.add_count(count);
        _metric.emit();
    }
    
private:
    M& _metric;
};

int main() {
    // Test that noop_metrics works with template functions
    raft::noop_metrics metrics;
    
    record_operation(metrics, "test_operation", std::chrono::milliseconds{100});
    
    // Test that noop_metrics works with template classes
    operation_tracker<raft::noop_metrics> tracker(metrics);
    tracker.track_operation("tracked_operation", 42);
    
    // All operations should complete without error and with zero overhead
    return 0;
}

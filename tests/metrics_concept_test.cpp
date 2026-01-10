#include <raft/metrics.hpp>
#include <string_view>
#include <chrono>
#include <cstdint>

// Test implementation that satisfies the metrics concept
class test_metrics {
public:
    auto set_metric_name(std::string_view name) -> void {
        _name = name;
    }
    
    auto add_dimension(std::string_view dimension_name, std::string_view dimension_value) -> void {
        // Store dimension
    }
    
    auto add_one() -> void {
        _count += 1;
    }
    
    auto add_count(std::int64_t count) -> void {
        _count += count;
    }
    
    auto add_duration(std::chrono::nanoseconds duration) -> void {
        _total_duration += duration;
    }
    
    auto add_value(double value) -> void {
        _value = value;
    }
    
    auto emit() -> void {
        // Emit the metric
    }

private:
    std::string_view _name;
    std::int64_t _count{0};
    std::chrono::nanoseconds _total_duration{0};
    double _value{0.0};
};

// Verify that test_metrics satisfies the metrics concept
static_assert(kythira::metrics<test_metrics>, "test_metrics must satisfy metrics concept");

// Test that a non-conforming type does not satisfy the concept
class non_metrics {
public:
    auto set_metric_name(std::string_view name) -> void {}
    // Missing other required methods
};

static_assert(!kythira::metrics<non_metrics>, "non_metrics must not satisfy metrics concept");

int main() {
    // Instantiate to ensure it compiles
    test_metrics m;
    m.set_metric_name("test_metric");
    m.add_dimension("node_id", "node_1");
    m.add_one();
    m.add_count(5);
    m.add_duration(std::chrono::milliseconds{100});
    m.add_value(42.5);
    m.emit();
    
    return 0;
}

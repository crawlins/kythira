# Comprehensive Raft Performance Benchmarking Framework

**Last Updated**: February 2026
**Requirements**: 13.1, 13.2, 13.3, 13.4, 13.5

## Overview

This document describes the comprehensive performance benchmarking framework for the Raft consensus implementation. The framework provides extensive performance testing across multiple dimensions to ensure the system meets production-grade performance requirements.

## Framework Components

The benchmarking framework consists of 15 comprehensive test cases organized into 5 main categories:

### 1. Throughput Benchmarks (Requirement 13.1)

Measures operations per second under various conditions to validate system capacity.

#### Test Cases:

**1.1 Single-Threaded Throughput**
- **Purpose**: Establish baseline single-threaded performance
- **Metrics**: Operations per second
- **Target**: > 10,000 ops/sec
- **Duration**: 5 seconds sustained load

**1.2 Multi-Threaded Throughput**
- **Purpose**: Measure concurrent processing capability
- **Metrics**: Total ops/sec, per-thread ops/sec
- **Configuration**: 4 concurrent threads
- **Target**: > 20,000 ops/sec total
- **Duration**: 5 seconds sustained load

**1.3 Batched Operations Throughput**
- **Purpose**: Evaluate batch processing efficiency
- **Metrics**: Ops/sec, batches/sec
- **Configuration**: 100 operations per batch
- **Target**: > 50,000 ops/sec
- **Duration**: 5 seconds sustained load

### 2. Latency Benchmarks (Requirement 13.2)

Measures end-to-end latency for various operations to ensure responsive behavior.

#### Test Cases:

**2.1 Command Application Latency**
- **Purpose**: Measure basic operation latency
- **Metrics**: Mean, P50, P95, P99, P99.9 latencies
- **Sample Size**: 10,000 operations
- **Targets**:
  - Mean: < 100μs
  - P99: < 500μs

**2.2 Latency Percentile Distribution**
- **Purpose**: Analyze latency distribution for different operation types
- **Operations**: WRITE and READ operations
- **Metrics**: Full percentile distribution (P50, P95, P99, P99.9)
- **Sample Size**: 10,000 operations per type
- **Target**: P99 < 1000μs for both types

**2.3 Latency Under Load**
- **Purpose**: Validate latency stability under increasing load
- **Load Levels**: 100, 1,000, 10,000, 50,000 operations
- **Metrics**: P50, P95, P99, P99.9 at each load level
- **Target**: P99 < 2000μs even at highest load

### 3. Scalability Testing (Requirement 13.3)

Measures performance characteristics as the system scales along various dimensions.

#### Test Cases:

**3.1 State Size Scalability**
- **Purpose**: Evaluate performance impact of growing state
- **State Sizes**: 1K, 10K, 100K, 500K operations
- **Metrics**: Build time, operation latency at each size
- **Target**: P99 latency < 1000μs regardless of state size

**3.2 Concurrent Client Scalability**
- **Purpose**: Measure system scaling with concurrent clients
- **Client Counts**: 1, 2, 4, 8, 16 concurrent clients
- **Metrics**: Total throughput, per-client throughput
- **Duration**: 3 seconds per configuration
- **Target**: Reasonable scaling (> 50% efficiency)

**3.3 Snapshot Operation Scalability**
- **Purpose**: Validate snapshot performance at various state sizes
- **State Sizes**: 1K, 10K, 50K, 100K operations
- **Metrics**: Snapshot creation P50/P99, restore P50/P99
- **Sample Size**: 50 iterations per size
- **Targets**:
  - Create P99: < 50ms
  - Restore P99: < 50ms

### 4. Resource Usage Profiling (Requirement 13.4)

Measures resource consumption patterns to ensure efficient resource utilization.

#### Test Cases:

**4.1 Memory Footprint Analysis**
- **Purpose**: Measure memory usage at various state sizes
- **State Sizes**: 1K, 10K, 100K, 500K, 1M operations
- **Metrics**: Total memory, bytes per operation, efficiency rating
- **Target**: < 1000 bytes per operation

**4.2 Memory Growth Rate Tracking**
- **Purpose**: Analyze memory growth patterns
- **Checkpoints**: 1K, 5K, 10K, 50K, 100K operations
- **Metrics**: Growth rate between checkpoints
- **Target**: Linear, consistent growth

**4.3 Operation Overhead Measurement**
- **Purpose**: Measure memory overhead for different payload sizes
- **Payload Sizes**: 10, 100, 1000, 10000 bytes
- **Metrics**: Memory delta, overhead ratio
- **Target**: Overhead ratio < 5x payload size

### 5. Performance Regression Detection (Requirement 13.5)

Establishes baseline performance metrics and validates against them to detect regressions.

#### Test Cases:

**5.1 Throughput Baseline Validation**
- **Purpose**: Establish and validate throughput baseline
- **Duration**: 5 seconds
- **Baselines**:
  - Minimum: 10,000 ops/sec
  - Target: 100,000 ops/sec
- **Status**: PASS/FAIL with performance rating

**5.2 Latency Baseline Validation**
- **Purpose**: Establish and validate latency baselines
- **Sample Size**: 10,000 operations
- **Baselines**:
  - P50: ≤ 50μs
  - P99: ≤ 500μs
  - P99.9: ≤ 2000μs
- **Status**: Individual PASS/FAIL for each percentile

**5.3 Memory Baseline Validation**
- **Purpose**: Establish and validate memory usage baseline
- **Test Size**: 100,000 operations
- **Baselines**:
  - Maximum: 1000 bytes/op
  - Target: 100 bytes/op
- **Status**: PASS/FAIL with efficiency rating

**5.4 Performance Stability Verification**
- **Purpose**: Verify performance consistency over time
- **Rounds**: 5 measurement rounds
- **Operations**: 10,000 per round
- **Metrics**: Coefficient of Variation (CV) for throughput and latency
- **Target**: CV < 15% (stable performance)

## Running the Benchmarks

### Quick Start

```bash
# Build the benchmark
cmake --build build --target raft_comprehensive_performance_benchmark

# Run all benchmarks via CTest
ctest --test-dir build -R raft_comprehensive_performance_benchmark --verbose

# Run with output stored for analysis
ctest --test-dir build -R raft_comprehensive_performance_benchmark --verbose 2>&1 | tee benchmark_results.txt
```

### Benchmark Configuration

The framework uses the following configuration constants (defined in the test file):

```cpp
constexpr std::size_t warmup_iterations = 100;
constexpr std::size_t benchmark_iterations = 10000;
constexpr std::size_t throughput_duration_seconds = 5;
constexpr std::size_t scalability_test_sizes[] = {3, 5, 7, 9};
constexpr std::size_t load_levels[] = {100, 1000, 10000, 50000};
```

### Test Properties

- **Timeout**: 600 seconds (10 minutes)
- **Labels**: `performance`, `benchmark`, `raft`
- **Test Framework**: Boost.Test

### Filtering Benchmarks

```bash
# Run only performance benchmarks
ctest --test-dir build -L performance

# Run only Raft benchmarks
ctest --test-dir build -L raft

# Run specific benchmark category (requires test name pattern)
ctest --test-dir build -R "throughput|latency|scalability"
```

## Interpreting Results

### Success Criteria

All benchmarks must pass their respective targets:

| Category | Metric | Target | Status |
|----------|--------|--------|--------|
| Throughput | Single-threaded | > 10K ops/sec | ✓ |
| Throughput | Multi-threaded | > 20K ops/sec | ✓ |
| Throughput | Batched | > 50K ops/sec | ✓ |
| Latency | P99 (basic) | < 500μs | ✓ |
| Latency | P99 (under load) | < 2000μs | ✓ |
| Scalability | State size impact | P99 < 1000μs | ✓ |
| Scalability | Snapshot ops | P99 < 50ms | ✓ |
| Memory | Bytes per operation | < 1000 bytes | ✓ |
| Stability | Coefficient of Variation | < 15% | ✓ |

### Performance Ratings

The framework provides qualitative performance ratings:

**Throughput**:
- **EXCELLENT**: Exceeds target (100K ops/sec)
- **GOOD**: 5x minimum (50K ops/sec)
- **ACCEPTABLE**: Meets minimum (10K ops/sec)

**Memory Efficiency**:
- **EXCELLENT**: < 100 bytes/op
- **GOOD**: < 500 bytes/op
- **FAIR**: < 1000 bytes/op
- **POOR**: ≥ 1000 bytes/op

**Stability**:
- **STABLE**: CV < 15%
- **UNSTABLE**: CV ≥ 15%

### Output Format

Each benchmark produces detailed output including:

1. **Test Header**: Requirement number and test name
2. **Configuration**: Test parameters and settings
3. **Results**: Measured metrics with units
4. **Statistics**: Mean, median, percentiles, min, max, stddev
5. **Status**: PASS/FAIL against targets
6. **Rating**: Qualitative performance assessment

Example output:

```
=== Requirement 13.1: Throughput Benchmark (Single-Threaded) ===
Single-Threaded Throughput:
  Operations: 523,456
  Duration: 5000 ms
  Throughput: 104,691.20 ops/sec
  Status: PASS
  Performance: EXCELLENT (exceeds target)
```

## Performance Analysis

### Statistics Provided

For each benchmark, the framework calculates:

- **Mean**: Average value
- **Median (P50)**: 50th percentile
- **P95**: 95th percentile
- **P99**: 99th percentile
- **P99.9**: 99.9th percentile
- **Min**: Minimum value
- **Max**: Maximum value
- **StdDev**: Standard deviation
- **Sample Count**: Number of measurements

### Coefficient of Variation (CV)

Used for stability analysis:

```
CV = (Standard Deviation / Mean) × 100%
```

Lower CV indicates more stable, consistent performance.

## Integration with CI/CD

### Automated Testing

The benchmark can be integrated into CI/CD pipelines:

```bash
# In CI/CD script
cmake --build build --target raft_comprehensive_performance_benchmark
ctest --test-dir build -R raft_comprehensive_performance_benchmark --output-on-failure

# Check exit code
if [ $? -ne 0 ]; then
    echo "Performance benchmarks failed!"
    exit 1
fi
```

### Performance Tracking

Store benchmark results over time to track performance trends:

```bash
# Store results with timestamp
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
ctest --test-dir build -R raft_comprehensive_performance_benchmark --verbose \
    2>&1 | tee "benchmark_results_${TIMESTAMP}.txt"

# Extract key metrics for tracking
grep "Throughput:" "benchmark_results_${TIMESTAMP}.txt" > metrics.csv
```

## Troubleshooting

### Common Issues

**1. Benchmarks Timeout**
- Increase timeout in CMakeLists.txt
- Reduce benchmark iterations
- Check system load

**2. Inconsistent Results**
- Ensure system is idle during benchmarking
- Disable CPU frequency scaling
- Run multiple times and average

**3. Performance Below Targets**
- Check compiler optimization flags (-O3)
- Verify Release build configuration
- Profile to identify bottlenecks

### Debug Mode

For detailed debugging:

```bash
# Run with Boost.Test verbose logging
./build/tests/raft_comprehensive_performance_benchmark --log_level=all

# Run specific test case
./build/tests/raft_comprehensive_performance_benchmark \
    --run_test=benchmark_throughput_single_threaded
```

## Best Practices

1. **Run on Dedicated Hardware**: Avoid running on shared/loaded systems
2. **Use Release Builds**: Always benchmark with optimizations enabled
3. **Warm Up System**: Run warmup iterations before measurements
4. **Multiple Runs**: Run benchmarks multiple times for consistency
5. **Track Over Time**: Store results to detect performance regressions
6. **Document Changes**: Note any configuration or code changes
7. **Isolate Variables**: Change one thing at a time when investigating issues

## Future Enhancements

Potential improvements to the framework:

1. **Distributed Benchmarks**: Multi-node cluster performance testing
2. **Network Simulation**: Test with simulated network conditions
3. **Failure Scenarios**: Performance under failure conditions
4. **Real Workloads**: Benchmark with production-like workloads
5. **Automated Regression Detection**: Automatic comparison with baselines
6. **Performance Visualization**: Graphs and charts of results
7. **Profiling Integration**: Automatic CPU/memory profiling
8. **Comparative Analysis**: Compare against other Raft implementations

## See Also

- [Raft Performance Benchmarking Guide](raft_performance_benchmarking.md)
- [Raft Configuration Guide](raft_configuration_guide.md)
- [Production Readiness Checklist](RAFT_PRODUCTION_REDINESS.md)
- [Test Execution Standards](../tests/test-execution-standards.md)
- [C++ Coding Standards](../tests/cpp-coding-standards.md)

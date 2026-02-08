#!/bin/bash
# Efficient Test Execution Script
# This script demonstrates the recommended workflow for running and analyzing tests

set -e  # Exit on error

# Configuration
BUILD_DIR="build"
RESULTS_DIR="test_results"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
TEST_OUTPUT="${RESULTS_DIR}/test_results_${TIMESTAMP}.txt"
SUMMARY_FILE="${RESULTS_DIR}/test_summary_${TIMESTAMP}.md"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Create results directory if it doesn't exist
mkdir -p "${RESULTS_DIR}"

echo "=========================================="
echo "Efficient Test Execution"
echo "=========================================="
echo ""

# Step 1: Run tests ONCE and store output
echo -e "${YELLOW}Step 1: Running tests and storing output...${NC}"
echo "Output will be stored in: ${TEST_OUTPUT}"
echo ""

# Add metadata to the output file
{
    echo "=== Test Run Metadata ==="
    echo "Date: $(date)"
    echo "Hostname: $(hostname)"
    if command -v git &> /dev/null && [ -d .git ]; then
        echo "Git Commit: $(git rev-parse HEAD 2>/dev/null || echo 'N/A')"
        echo "Git Branch: $(git branch --show-current 2>/dev/null || echo 'N/A')"
    fi
    echo "========================"
    echo ""
    
    # Run the actual tests
    ctest --test-dir "${BUILD_DIR}" --output-on-failure -j$(nproc)
} 2>&1 | tee "${TEST_OUTPUT}"

TEST_EXIT_CODE=${PIPESTATUS[0]}

echo ""
echo -e "${GREEN}✓ Test execution complete. Output stored in: ${TEST_OUTPUT}${NC}"
echo ""

# Step 2: Analyze stored results
echo -e "${YELLOW}Step 2: Analyzing stored results...${NC}"
echo ""

# Extract statistics from stored output
TOTAL_TESTS=$(grep -c "Test #" "${TEST_OUTPUT}" || echo "0")
PASSED_TESTS=$(grep -c "Passed" "${TEST_OUTPUT}" || echo "0")
FAILED_TESTS=$(grep -c "Failed" "${TEST_OUTPUT}" || echo "0")

# Calculate pass rate
if [ "${TOTAL_TESTS}" -gt 0 ]; then
    PASS_RATE=$(awk "BEGIN {printf \"%.1f\", (${PASSED_TESTS}/${TOTAL_TESTS})*100}")
else
    PASS_RATE="0.0"
fi

# Display summary
echo "Test Statistics:"
echo "  Total Tests:  ${TOTAL_TESTS}"
echo "  Passed:       ${PASSED_TESTS}"
echo "  Failed:       ${FAILED_TESTS}"
echo "  Pass Rate:    ${PASS_RATE}%"
echo ""

# Show failed tests if any
if [ "${FAILED_TESTS}" -gt 0 ]; then
    echo -e "${RED}Failed Tests:${NC}"
    grep "Test.*Failed" "${TEST_OUTPUT}" | head -20
    if [ "${FAILED_TESTS}" -gt 20 ]; then
        echo "  ... and $((FAILED_TESTS - 20)) more"
    fi
    echo ""
fi

# Show slowest tests
echo "Slowest Tests (top 10):"
grep "sec$" "${TEST_OUTPUT}" | sort -k4 -n -r | head -10
echo ""

# Step 3: Generate summary report
echo -e "${YELLOW}Step 3: Generating summary report...${NC}"

cat > "${SUMMARY_FILE}" << EOF
# Test Execution Summary

**Date**: $(date)
**Test Output File**: ${TEST_OUTPUT}
**Exit Code**: ${TEST_EXIT_CODE}

## Results

\`\`\`
Total Tests:  ${TOTAL_TESTS}
Passed:       ${PASSED_TESTS}
Failed:       ${FAILED_TESTS}
Pass Rate:    ${PASS_RATE}%
\`\`\`

## Failed Tests

$(if [ "${FAILED_TESTS}" -gt 0 ]; then
    grep "Test.*Failed" "${TEST_OUTPUT}" | sed 's/^/- /'
else
    echo "No failures ✓"
fi)

## Slowest Tests (Top 10)

\`\`\`
$(grep "sec$" "${TEST_OUTPUT}" | sort -k4 -n -r | head -10)
\`\`\`

## Test Output Summary

\`\`\`
$(tail -30 "${TEST_OUTPUT}")
\`\`\`

## Analysis Commands

To analyze the stored test output further, use:

\`\`\`bash
# View full output
less ${TEST_OUTPUT}

# Search for specific test
grep "test_name" ${TEST_OUTPUT}

# Count specific test types
grep "raft_.*test" ${TEST_OUTPUT} | wc -l

# Extract test timing
grep "sec$" ${TEST_OUTPUT} | sort -k4 -n -r

# Compare with previous run
diff test_results_previous.txt ${TEST_OUTPUT}
\`\`\`

## Re-running Failed Tests

To re-run only the failed tests:

\`\`\`bash
ctest --test-dir ${BUILD_DIR} --rerun-failed --output-on-failure
\`\`\`
EOF

echo -e "${GREEN}✓ Summary report generated: ${SUMMARY_FILE}${NC}"
echo ""

# Step 4: Cleanup old test results (keep last 10)
echo -e "${YELLOW}Step 4: Cleaning up old test results...${NC}"
OLD_RESULTS=$(ls -t "${RESULTS_DIR}"/test_results_*.txt 2>/dev/null | tail -n +11)
if [ -n "${OLD_RESULTS}" ]; then
    echo "${OLD_RESULTS}" | xargs rm -f
    echo "Removed $(echo "${OLD_RESULTS}" | wc -l) old test result files"
else
    echo "No old test results to clean up"
fi
echo ""

# Final summary
echo "=========================================="
echo "Test Execution Complete"
echo "=========================================="
echo ""
echo "Results stored in:"
echo "  - Test output: ${TEST_OUTPUT}"
echo "  - Summary:     ${SUMMARY_FILE}"
echo ""

if [ "${TEST_EXIT_CODE}" -eq 0 ]; then
    echo -e "${GREEN}✓ All tests passed!${NC}"
    exit 0
else
    echo -e "${RED}✗ Some tests failed. See ${TEST_OUTPUT} for details.${NC}"
    echo ""
    echo "To re-run failed tests:"
    echo "  ctest --test-dir ${BUILD_DIR} --rerun-failed --output-on-failure"
    exit 1
fi

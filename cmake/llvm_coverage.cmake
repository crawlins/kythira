# cmake/llvm_coverage.cmake — cmake -P script invoked by the coverage targets.
#
# Required -D arguments:
#   BUILD_DIR     path to the cmake binary directory
#   LLVM_PROFDATA path to llvm-profdata executable
#   LLVM_COV      path to llvm-cov executable
#   PROFDATA      output path for merged.profdata
#   MODE          "report" or "html"
#   HTML_DIR      output directory (MODE=html only)
#   IGNORE_LIST   semicolon-separated filename regex patterns to exclude

cmake_minimum_required(VERSION 3.20)

# ── Collect and merge profraw files ───────────────────────────────────────────
file(GLOB_RECURSE PROFRAW_FILES "${BUILD_DIR}/*.profraw")
if(NOT PROFRAW_FILES)
    message(FATAL_ERROR "[coverage] No .profraw files found in ${BUILD_DIR}")
endif()

execute_process(
    COMMAND "${LLVM_PROFDATA}" merge -sparse ${PROFRAW_FILES} -o "${PROFDATA}"
    RESULT_VARIABLE RC
)
if(NOT RC EQUAL 0)
    message(FATAL_ERROR "[coverage] llvm-profdata merge failed")
endif()

# ── Collect test binaries (files without extensions in tests/) ─────────────────
file(GLOB TEST_CANDIDATES "${BUILD_DIR}/tests/*")
list(FILTER TEST_CANDIDATES EXCLUDE REGEX ".*\\.[^/]+$")
list(SORT TEST_CANDIDATES)
if(NOT TEST_CANDIDATES)
    message(FATAL_ERROR "[coverage] No test binaries found in ${BUILD_DIR}/tests/")
endif()

list(GET TEST_CANDIDATES 0 MAIN_BIN)
list(REMOVE_AT TEST_CANDIDATES 0)
set(EXTRA_BINS)
foreach(b ${TEST_CANDIDATES})
    list(APPEND EXTRA_BINS -object "${b}")
endforeach()

# ── Build ignore flags ─────────────────────────────────────────────────────────
set(IGNORE_FLAGS)
foreach(pat ${IGNORE_LIST})
    list(APPEND IGNORE_FLAGS "--ignore-filename-regex=${pat}")
endforeach()

# ── Run llvm-cov ──────────────────────────────────────────────────────────────
if(MODE STREQUAL "html")
    file(MAKE_DIRECTORY "${HTML_DIR}")
    execute_process(
        COMMAND "${LLVM_COV}" show
            --instr-profile="${PROFDATA}"
            "${MAIN_BIN}" ${EXTRA_BINS}
            --format=html
            --output-dir="${HTML_DIR}"
            ${IGNORE_FLAGS}
        RESULT_VARIABLE RC
    )
    if(NOT RC EQUAL 0)
        message(FATAL_ERROR "[coverage] llvm-cov show failed")
    endif()
    message(STATUS "[coverage] HTML report: ${HTML_DIR}/index.html")
else()
    execute_process(
        COMMAND "${LLVM_COV}" report
            --instr-profile="${PROFDATA}"
            "${MAIN_BIN}" ${EXTRA_BINS}
            ${IGNORE_FLAGS}
        RESULT_VARIABLE RC
    )
    if(NOT RC EQUAL 0)
        message(FATAL_ERROR "[coverage] llvm-cov report failed")
    endif()
endif()

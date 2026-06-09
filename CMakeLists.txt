cmake_minimum_required(VERSION 3.20)
project(network_simulator VERSION 0.1.0 LANGUAGES CXX)

# C++23 standard required
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Code coverage support
option(ENABLE_COVERAGE "Build with gcov coverage instrumentation" OFF)

if(ENABLE_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(WARNING "Coverage instrumentation is only supported with GCC or Clang "
                        "(current: ${CMAKE_CXX_COMPILER_ID})")
    endif()
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
        message(STATUS "ENABLE_COVERAGE: forcing CMAKE_BUILD_TYPE=Debug")
    endif()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-arcs -ftest-coverage")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} --coverage")
    message(STATUS "Coverage instrumentation enabled")
endif()

# Find dependencies
# Note: folly is required for the full implementation
# For now, we'll make it optional to allow initial build
find_package(folly QUIET)
find_package(Boost QUIET COMPONENTS system thread unit_test_framework json context)
find_package(httplib QUIET)
find_package(OpenSSL QUIET)

# Find libcoap using CMake config (vcpkg) or PkgConfig (system)
find_package(libcoap QUIET CONFIG)
if(NOT libcoap_FOUND)
    # Fallback to PkgConfig for system installations
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBCOAP QUIET libcoap-3)
    endif()
else()
    # Set LIBCOAP_FOUND for consistency with pkg-config path
    set(LIBCOAP_FOUND TRUE)
endif()

if(NOT folly_FOUND)
    message(WARNING "folly not found. Full implementation will require folly library.")
endif()

if(NOT Boost_FOUND)
    message(WARNING "Boost not found. Tests will not be built.")
endif()

if(NOT httplib_FOUND)
    message(WARNING "cpp-httplib not found. HTTP transport will not be available.")
endif()

if(NOT OpenSSL_FOUND)
    message(WARNING "OpenSSL not found. HTTPS/TLS support will not be available.")
endif()

if(NOT LIBCOAP_FOUND)
    message(WARNING "libcoap not found. CoAP transport will not be available.")
endif()

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)

# Library target
add_library(network_simulator INTERFACE)
target_include_directories(network_simulator INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
if(folly_FOUND)
    target_link_libraries(network_simulator INTERFACE Folly::folly)
    # When linking Folly, we must also link Boost::context (required by Folly fibers)
    # Use vcpkg static library if available for compatibility
    if(EXISTS "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-linux/lib/libboost_context.a")
        target_link_libraries(network_simulator INTERFACE "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-linux/lib/libboost_context.a")
    elseif(TARGET Boost::context)
        target_link_libraries(network_simulator INTERFACE Boost::context)
    endif()
endif()

if(Boost_FOUND)
    target_link_libraries(network_simulator INTERFACE
        Boost::system
        Boost::thread
    )
    # Create boost_json imported library using vcpkg static library if available
    # This ensures compatibility with vcpkg-built Folly
    if(NOT TARGET boost_json)
        if(EXISTS "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-linux/lib/libboost_json.a")
            add_library(boost_json STATIC IMPORTED)
            set_target_properties(boost_json PROPERTIES
                IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-linux/lib/libboost_json.a"
            )
        elseif(TARGET Boost::json)
            add_library(boost_json ALIAS Boost::json)
        endif()
    endif()
endif()

if(httplib_FOUND)
    target_link_libraries(network_simulator INTERFACE httplib::httplib)
endif()

if(OpenSSL_FOUND)
    target_link_libraries(network_simulator INTERFACE OpenSSL::SSL OpenSSL::Crypto)
endif()

if(LIBCOAP_FOUND)
    target_include_directories(network_simulator INTERFACE ${LIBCOAP_INCLUDE_DIRS})
    target_link_libraries(network_simulator INTERFACE ${LIBCOAP_LIBRARIES})
    target_compile_definitions(network_simulator INTERFACE ${LIBCOAP_CFLAGS_OTHER})
endif()

target_compile_features(network_simulator INTERFACE cxx_std_23)

# Debug executable - commented out due to missing source files
# add_executable(debug_connect_test debug_connect_test.cpp)
# target_link_libraries(debug_connect_test PRIVATE network_simulator)
# if(folly_FOUND)
#     target_link_libraries(debug_connect_test PRIVATE Folly::folly)
# endif()
# target_compile_features(debug_connect_test PRIVATE cxx_std_23)

# Register debug test with CTest
# add_test(NAME debug_connect_test COMMAND debug_connect_test)
# set_tests_properties(debug_connect_test PROPERTIES
#     TIMEOUT 30
#     LABELS "debug;connection"
# )

# Debug accept test - commented out due to missing source files
# add_executable(debug_accept_test debug_accept_test.cpp)
# target_link_libraries(debug_accept_test PRIVATE network_simulator)
# if(folly_FOUND)
#     target_link_libraries(debug_accept_test PRIVATE Folly::folly)
# endif()
# target_compile_features(debug_accept_test PRIVATE cxx_std_23)

# Single test case - commented out due to missing source files
# add_executable(test_single_case test_single_case.cpp)
# target_link_libraries(test_single_case PRIVATE network_simulator Boost::unit_test_framework)
# if(folly_FOUND)
#     target_link_libraries(test_single_case PRIVATE Folly::folly)
# endif()
# target_compile_features(test_single_case PRIVATE cxx_std_23)

# Enable testing
enable_testing()

# Add subdirectories
if(Boost_FOUND)
    add_subdirectory(tests)
endif()
add_subdirectory(examples)

# Installation rules
install(TARGETS network_simulator
    EXPORT network_simulator_targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)

install(DIRECTORY include/
    DESTINATION include
    FILES_MATCHING PATTERN "*.hpp"
)

install(EXPORT network_simulator_targets
    FILE network_simulator_targets.cmake
    NAMESPACE network_simulator::
    DESTINATION lib/cmake/network_simulator
)
# Thread safety test was removed after verification

# Coverage targets — only available when configured with -DENABLE_COVERAGE=ON
if(ENABLE_COVERAGE)
    include(ProcessorCount)
    ProcessorCount(PROCESSOR_COUNT)
    if(PROCESSOR_COUNT EQUAL 0)
        set(PROCESSOR_COUNT 4)
    endif()

    find_program(GCOVR_EXECUTABLE gcovr
        HINTS "$ENV{HOME}/.local/bin" /usr/local/bin /usr/bin
    )

    if(NOT GCOVR_EXECUTABLE)
        message(WARNING "gcovr not found — coverage targets unavailable. "
                        "Install with: pip3 install gcovr")
        foreach(_t coverage coverage-html coverage-reset)
            add_custom_target(${_t}
                COMMAND ${CMAKE_COMMAND} -E echo
                    "ERROR: gcovr not found. Install with: pip3 install gcovr"
                COMMAND ${CMAKE_COMMAND} -E false
            )
        endforeach()
    else()
        set(COVERAGE_REPORT "${CMAKE_BINARY_DIR}/coverage-report")

        # gcovr exclusion patterns (Python regex applied to source paths)
        set(GCOVR_EXCLUDES
            --exclude ".*build-coverage.*"
            --exclude ".*vcpkg_installed.*"
            --exclude ".*/usr/.*"
            --gcov-ignore-parse-errors=negative_hits.warn
        )

        add_custom_target(coverage-reset
            COMMAND ${CMAKE_COMMAND} -E echo "Resetting coverage counters..."
            COMMAND find ${CMAKE_BINARY_DIR} -name "*.gcda" -delete
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Resetting coverage counters"
        )

        add_custom_target(coverage
            COMMAND find ${CMAKE_BINARY_DIR} -name "*.gcda" -delete
            COMMAND ${CMAKE_CTEST_COMMAND} -j${PROCESSOR_COUNT} --output-on-failure
            COMMAND ${GCOVR_EXECUTABLE}
                    --root ${CMAKE_SOURCE_DIR}
                    --object-directory ${CMAKE_BINARY_DIR}
                    ${GCOVR_EXCLUDES}
                    --print-summary
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Running full test suite and collecting coverage"
        )

        add_custom_target(coverage-html
            COMMAND find ${CMAKE_BINARY_DIR} -name "*.gcda" -delete
            COMMAND ${CMAKE_CTEST_COMMAND} -j${PROCESSOR_COUNT} --output-on-failure
            COMMAND ${CMAKE_COMMAND} -E make_directory ${COVERAGE_REPORT}
            COMMAND ${GCOVR_EXECUTABLE}
                    --root ${CMAKE_SOURCE_DIR}
                    --object-directory ${CMAKE_BINARY_DIR}
                    ${GCOVR_EXCLUDES}
                    --print-summary
                    --html-details ${COVERAGE_REPORT}/index.html
            COMMAND ${CMAKE_COMMAND} -E echo
                    "HTML report: ${COVERAGE_REPORT}/index.html"
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Running full test suite and generating HTML coverage report"
        )
    endif()
endif()
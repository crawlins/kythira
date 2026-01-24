cmake_minimum_required(VERSION 3.20)
project(network_simulator VERSION 0.1.0 LANGUAGES CXX)

# C++23 standard required
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find dependencies
# Note: folly is required for the full implementation
# For now, we'll make it optional to allow initial build
find_package(folly QUIET)
find_package(Boost QUIET COMPONENTS system thread unit_test_framework)
find_package(httplib QUIET)
find_package(OpenSSL QUIET)

# Find libcoap using PkgConfig
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBCOAP QUIET libcoap-3)
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
endif()

if(Boost_FOUND)
    target_link_libraries(network_simulator INTERFACE
        Boost::system
        Boost::thread
    )
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
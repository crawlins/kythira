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

if(NOT folly_FOUND)
    message(WARNING "folly not found. Full implementation will require folly library.")
endif()

if(NOT Boost_FOUND)
    message(WARNING "Boost not found. Tests will not be built.")
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
target_compile_features(network_simulator INTERFACE cxx_std_23)

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

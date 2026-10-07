# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Vendored build script for Poco DNSSD (pocoproject/poco-dnssd), which ships
# only Poco's make rules and Visual Studio projects. Copied into the extracted
# source tree by portfile.cmake and driven with vcpkg_cmake_configure /
# vcpkg_cmake_install.
#
# The source lists mirror upstream's Makefile and Avahi/Makefile at the pinned
# commit. Library names (PocoDNSSD, PocoDNSSDAvahi) match what those Makefiles
# produce, which is also what the hand-built archives this port replaces were
# called.
cmake_minimum_required(VERSION 3.20)
project(poco-dnssd VERSION 1.7.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_package(Poco REQUIRED COMPONENTS Foundation Net)
find_package(PkgConfig REQUIRED)
# vcpkg has no avahi port, so the client library comes from the host
# (libavahi-client-dev). IMPORTED_TARGET carries its include and link flags.
pkg_check_modules(AVAHI REQUIRED IMPORTED_TARGET avahi-client)

add_library(PocoDNSSD STATIC
    src/DNSSDBrowser.cpp
    src/DNSSDException.cpp
    src/DNSSDResponder.cpp
    src/DNSSDResponderImpl.cpp
    src/Domain.cpp
    src/Error.cpp
    src/Record.cpp
    src/Service.cpp
)
target_include_directories(PocoDNSSD PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(PocoDNSSD PUBLIC Poco::Net Poco::Foundation)

add_library(PocoDNSSDAvahi STATIC
    Avahi/src/AvahiBrowserImpl.cpp
    Avahi/src/AvahiResponderImpl.cpp
)
target_include_directories(PocoDNSSDAvahi PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Avahi/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
# avahi-client stays PRIVATE: no installed header includes an Avahi header, so
# consumers need only its link flags, which CMake still propagates for a
# static library through the exported $<LINK_ONLY:...> entry.
target_link_libraries(PocoDNSSDAvahi
    PUBLIC PocoDNSSD Poco::Net Poco::Foundation
    PRIVATE PkgConfig::AVAHI
)

set_target_properties(PocoDNSSD PocoDNSSDAvahi PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

install(TARGETS PocoDNSSD PocoDNSSDAvahi
    EXPORT poco-dnssd-targets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
install(DIRECTORY include/Poco Avahi/include/Poco
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(EXPORT poco-dnssd-targets
    FILE poco-dnssd-targets.cmake
    NAMESPACE poco-dnssd::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/poco-dnssd"
)
configure_package_config_file(
    poco-dnssd-config.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/poco-dnssd-config.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/poco-dnssd"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/poco-dnssd-config-version.cmake"
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/poco-dnssd-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/poco-dnssd-config-version.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/poco-dnssd"
)

# Vendored build script for cantcoap (staropram/cantcoap), which ships no build
# system of its own. Copied into the extracted source tree by portfile.cmake and
# driven with vcpkg_cmake_configure / vcpkg_cmake_install. Deliberately minimal:
# cantcoap is a two-translation-unit codec, so this just compiles it into a
# static lib, installs the public headers, and exports a cantcoap::cantcoap
# target so downstream find_package(cantcoap CONFIG) works.
#
# SKELETON: the source/header lists reflect cantcoap's current layout but should
# be reconfirmed against the pinned commit — upstream occasionally renames the
# debug/endian helpers.
cmake_minimum_required(VERSION 3.20)
project(cantcoap VERSION 0.0.0 LANGUAGES C CXX)

include(GNUInstallDirs)

add_library(cantcoap STATIC
    cantcoap.cpp
    nendian.c
)

# cantcoap.cpp uses C++; nendian.c is C. No special standard is required, but
# match kythira's toolchain expectations loosely (headers are C++-safe).
set_target_properties(cantcoap PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_include_directories(cantcoap PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

install(TARGETS cantcoap
    EXPORT cantcoap-targets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)

# cantcoap's public surface. dbg.h is pulled in by cantcoap.h for its
# debug macros, so it must be installed too.
install(FILES
    cantcoap.h
    nendian.h
    dbg.h
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(EXPORT cantcoap-targets
    FILE cantcoap-config.cmake
    NAMESPACE cantcoap::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cantcoap"
)

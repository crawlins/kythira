# Vendored build script for cantcoap (staropram/cantcoap), which ships no build
# system of its own -- only a Makefile that builds its test binaries. Copied
# into the extracted source tree by portfile.cmake and driven with
# vcpkg_cmake_configure / vcpkg_cmake_install.
#
# Deliberately minimal: cantcoap is a *codec*, so the library is one translation
# unit. The file list below was confirmed against the pinned commit rather than
# assumed -- see the note on nethelper.c.
cmake_minimum_required(VERSION 3.20)
project(cantcoap VERSION 0.0.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# cantcoap.cpp is the whole codec. It includes cantcoap.h, dbg.h and sysdep.h
# and needs nothing else.
#
# nethelper.c is deliberately NOT built: it is getaddrinfo/socket boilerplate
# for cantcoap's own example programs, and kythira owns its socket anyway (the
# adapter has to -- that is the defining property of this backend). Building it
# would add a C source and an unused dependency for nothing.
add_library(cantcoap STATIC cantcoap.cpp)

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

# cantcoap.h is the public surface; dbg.h is included *by* it, so a consumer
# that includes cantcoap.h needs both. sysdep.h is used only inside
# cantcoap.cpp and so is not installed.
install(FILES cantcoap.h dbg.h DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

install(EXPORT cantcoap-targets
    FILE cantcoap-targets.cmake
    NAMESPACE cantcoap::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cantcoap"
)

# A real config file that includes the targets file, rather than installing the
# targets file *as* the config. The latter appears to work until someone calls
# find_package(cantcoap CONFIG REQUIRED) twice in one project, at which point
# the second include re-defines the imported target and hard-errors.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/cantcoap-config.cmake"
"include(\"\${CMAKE_CURRENT_LIST_DIR}/cantcoap-targets.cmake\")\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cantcoap-config.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cantcoap")

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/cantcoap-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cantcoap-config-version.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cantcoap")

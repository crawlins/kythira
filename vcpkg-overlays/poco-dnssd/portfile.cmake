# Overlay port for Poco DNSSD (pocoproject/poco-dnssd) and its Avahi backend,
# which kythira's poco_peer_discovery uses for DNS-SD registration/browsing.
#
# DNSSD was split out of the main Poco tree, so vcpkg's poco port has no
# feature for it. Before this port the two static archives were built by hand
# and copied into the source tree's vcpkg_installed/, where any reinstall
# deleted them and nothing recorded how to rebuild them. This port makes that
# build reproducible and lets vcpkg install them next to poco[net].
#
# Upstream ships no CMake build (only Poco's make rules and Visual Studio
# projects), so the port vendors a CMakeLists.txt, as the cantcoap port does.
# Only the Avahi backend is built: kythira is Linux-only, and Bonjour would
# need Apple's mDNSResponder SDK.

# vcpkg_from_git rather than vcpkg_from_github: the pinned commit is the
# integrity check, so there is no archive SHA512 to keep in step, and the
# fetch goes over git, which works in sandboxes that block codeload archives.
vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL https://github.com/pocoproject/poco-dnssd.git
    # Upstream has no tags and has not moved since its initial import
    # (2016-10-05); a38083e is the head of both master and develop.
    REF a38083e94aacd912b3ec38da83319f87f2ca5102
    HEAD_REF master
)

file(COPY
    "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt"
    "${CMAKE_CURRENT_LIST_DIR}/poco-dnssd-config.cmake.in"
    DESTINATION "${SOURCE_PATH}"
)

vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(PACKAGE_NAME poco-dnssd CONFIG_PATH lib/cmake/poco-dnssd)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

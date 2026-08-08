# SKELETON overlay port for `cantcoap` (staropram/cantcoap), a minimal
# BSD-2-licensed CoAP PDU codec in C++. It is a candidate "own-the-stack"
# alternate backend for kythira's coap_transport: cantcoap parses and builds
# CoAP messages and nothing else, so the adapter
# (include/raft/coap_transport_cantcoap_impl.hpp) must supply the UDP socket,
# retransmission/dedup, block-wise reassembly and DTLS itself — reusing the
# existing coap_security / coap_block_option / pending_message scaffolding.
# See doc/coap_library_alternatives.md for the trade-off vs libnyoci/libcoap.
#
# KEY DIFFERENCE FROM the libnyoci port: cantcoap has NO build system at all —
# upstream is a handful of source files (cantcoap.cpp, nendian.c) plus headers
# and a Makefile that only builds its own tests. So this port VENDORS its own
# CMakeLists.txt (CMAKE_LISTS.txt below, copied into the source tree) and drives
# it with the standard vcpkg_cmake_* helpers — the same helpers ion-c uses, but
# against a build script we supply rather than one upstream ships.
#
# STATUS: skeleton. REF/SHA512 are placeholders and MUST be pinned + regenerated
# before this port will fetch — same procedure as vcpkg-overlays/ion-c/README.md.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO staropram/cantcoap
    # cantcoap is unversioned upstream; pin to a specific commit SHA (not a
    # moving branch) for reproducibility. TODO: replace with a real commit.
    REF "0000000000000000000000000000000000000000"
    # TODO(placeholder): regenerate — vcpkg prints the real digest on first fetch.
    SHA512 0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
    HEAD_REF master
)

# Upstream ships no CMakeLists; drop our vendored one in next to the sources so
# vcpkg_cmake_configure has something to drive. Overwrites nothing (there is no
# upstream CMakeLists to clobber).
file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")

vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")

vcpkg_cmake_install()

# The vendored CMakeLists installs a cantcoap-config.cmake exporting the
# cantcoap::cantcoap imported target; normalise its location per convention.
vcpkg_cmake_config_fixup(PACKAGE_NAME cantcoap CONFIG_PATH lib/cmake/cantcoap)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

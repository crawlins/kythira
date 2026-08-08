# SKELETON overlay port for `cantcoap` (staropram/cantcoap), a minimal
# BSD-2-licensed CoAP PDU codec in C++. It is a candidate "own-the-stack"
# alternate backend for kythira's coap_transport: cantcoap parses and builds
# CoAP messages and nothing else, so the adapter
# (include/raft/coap_transport_cantcoap_impl.hpp) must supply the UDP socket,
# retransmission/dedup, block-wise reassembly and DTLS itself — reusing the
# existing coap_security / coap_block_option / pending_message scaffolding.
# See doc/coap_library_alternatives.md for the trade-off vs libnyoci/libcoap.
#
# KEY DIFFERENCE FROM the libnyoci port: cantcoap has NO build system at all --
# upstream is a handful of source files plus a Makefile that only builds its own
# tests. So this port VENDORS its own CMakeLists.txt (copied into the source
# tree) and drives it with the standard vcpkg_cmake_* helpers -- the same
# helpers ion-c uses, but against a build script we supply rather than one
# upstream ships. That is the whole packaging cost here, and it is trivial next
# to libnyoci's autotools port; the cost of *this* backend lands in the adapter
# instead.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO staropram/cantcoap
    # cantcoap is unversioned upstream -- no tags at all -- so this pins the
    # commit rather than a moving branch. 99e9ed5 is master's head
    # (2026-05-09), and unlike libnyoci this project is still maintained, so
    # re-pinning periodically is reasonable.
    REF 99e9ed517d50d36bdaa195f3034af435c23fb210
    SHA512 044cd680161eee72a211149086d3ebc3df5053c521cc174c26f97b8bacd8a27586a20a845e0b0c5e0a305cdb52dec3a3f645e5d77ef6a31dc7569bcf0d0b49bf
    HEAD_REF master
)

# Upstream ships no CMakeLists; drop our vendored one in next to the sources so
# vcpkg_cmake_configure has something to drive. Overwrites nothing (there is no
# upstream CMakeLists to clobber).
file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")

vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")

vcpkg_cmake_install()

# The vendored CMakeLists installs cantcoap-config.cmake (plus the targets and
# version files) exporting the cantcoap::cantcoap imported target; normalise
# their location per vcpkg convention.
vcpkg_cmake_config_fixup(PACKAGE_NAME cantcoap CONFIG_PATH lib/cmake/cantcoap)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

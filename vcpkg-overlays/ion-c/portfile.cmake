# Overlay port for `ion-c` (amazon-ion/ion-c), the C reference implementation of
# Amazon Ion. It backs kythira's ion_rpc_serializer
# (include/raft/ion_serializer.hpp), an alternative Types::serializer_type to
# json_rpc_serializer.
#
# ion-c is a plain CMake C project with no Rust/toolchain requirement beyond
# what this project already needs, so this port follows the same CMake-port
# shape as vcpkg-overlays/stdexec (vcpkg_from_github + vcpkg_cmake_configure /
# vcpkg_cmake_install / vcpkg_cmake_config_fixup), NOT the cargo-build shape of
# vcpkg-overlays/lakers. It is gated behind the opt-in "ion" vcpkg feature (see
# the root vcpkg.json), so a default install never fetches or builds it — the
# same treatment lakers gets behind the "edhoc" feature. See
# .kiro/specs/ion-rpc-serializer/ for the full design and README.md here for the
# pin/hash regeneration procedure.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO amazon-ion/ion-c
    REF v1.1.3
    # SHA512 of https://github.com/amazon-ion/ion-c/archive/refs/tags/v1.1.3.tar.gz.
    # Regenerate on pin bump with:
    #   vcpkg install ion-c --overlay-ports=vcpkg-overlays
    # and copy the "Actual hash" vcpkg prints on first fetch (see README.md).
    SHA512 0
    HEAD_REF master
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        # ion_rpc_serializer only uses the reader/writer; skip ion-c's own test
        # suite so the port builds just the library and its config package.
        -DIONC_BUILD_TESTS=OFF
)

vcpkg_cmake_install()

# ion-c installs its config package as "IonC" (project(IonC)); normalise the
# find_package name to lowercase "ionc" per vcpkg convention so the root
# CMakeLists.txt can find_package(ionc CONFIG). The imported target namespace
# inside the config (IonC::ionc / IonC::decNumber) is preserved.
vcpkg_cmake_config_fixup(PACKAGE_NAME ionc CONFIG_PATH lib/cmake/IonC)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

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
#
# 0001-fix-version-header-without-git-describe.patch: ion-c's own
# cmake/VersionHeader.cmake generates build_version.h's IONC_VERSION_MAJOR/
# MINOR/PATCH macros by regex-parsing `git describe --long --tags --dirty
# --match "v*"`'s output. vcpkg_from_github below extracts a plain source
# tarball with no .git directory, so `git describe` (run from inside the
# extracted tree) fails, the regex never matches, and the three macros
# substitute as empty -- which fails ion_version.c's build with "expected
# expression before ';' token" (`*major = ;`). Confirmed by direct build:
# this is not a hash/fetch problem, it is ion-c's own version-generation
# logic having no non-git fallback. Patches in a fallback to
# CMAKE_PROJECT_VERSION_MAJOR/MINOR/PATCH (already known from this file's
# own `project(IonC VERSION 1.1.3 ...)`) when the git-describe regex
# doesn't match, matching what a real git checkout with the v1.1.3 tag
# would have produced anyway.
#
# 0002-fix-assert-infinite-loop-under-ndebug.patch: ion-c's own internal
# ASSERT(x) macro (ion_internal.h) is
# `while (!(x)) { ion_helper_breakpoint(), assert(x); }`, unconditionally --
# under NDEBUG (this port's Release build, the default), assert() itself
# compiles away entirely, so a failed ASSERT()'s while-condition never
# becomes false and the process spins at 100% CPU forever instead of either
# aborting (a debug build) or being skipped (a release build, matching
# plain assert()'s own NDEBUG semantics). Confirmed directly: malformed/
# truncated Ion input reaching an internal reader-state invariant (e.g.
# ion_reader_step_out() on a container whose declared length was never
# fully consumed because the caller's buffer was truncated mid-container --
# exactly the case ion-rpc-serializer's own
# tests/ion_malformed_message_property_test.cpp exercises) hung indefinitely
# inside this exact macro. Patches ASSERT() to be a true no-op under NDEBUG.
# 0003-fix-gcc-14-incompatible-pointer-types.patch: GCC 14 turned
# -Wincompatible-pointer-types from a warning into an error by default, and
# ion-c 1.1.3 trips it twice, so the port does not compile at all under GCC 14
# (Debian 13 trixie, which ships gcc 14 and has no gcc-13 package). CI uses
# g++-13 and clang++-18 and so never sees either site. First:
# _ion_strdup()'s memcpy source is a conditional whose arms are "\0" (char *)
# and src->value (BYTE *), which have no composite type -- repaired by casting
# the literal arm, copying identical bytes. Second, and the more interesting
# one: ion_binary_read_int_64_and_sign() is the library's only ION_GET call
# site whose variable is not an int, so ion_stream_read_byte()'s `int *p_c`
# receives a `uint64_t *` and writes 4 of the 8 bytes through the wrong type --
# a genuine latent aliasing bug that happens to work only because the variable
# is zero-initialised on a little-endian host. Repaired by reading into an int
# and widening, as every other ION_GET site already does. Behaviour is
# unchanged including at EOF; see the patch header for that argument in full.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO amazon-ion/ion-c
    REF v1.1.3
    # SHA512 of https://github.com/amazon-ion/ion-c/archive/refs/tags/v1.1.3.tar.gz.
    # Regenerate on pin bump with:
    #   vcpkg install ion-c --overlay-ports=vcpkg-overlays
    # and copy the "Actual hash" vcpkg prints on first fetch (see README.md).
    SHA512 efc9658b27b639a1c01e30652fe602a04bcc81b39f59dc2ca5b42a9be873a9a5d819a6d8fc737f0b43007fc1385d647c9fb58c31d8bff41e91720bb49cb572b7
    HEAD_REF master
    PATCHES
        0001-fix-version-header-without-git-describe.patch
        0002-fix-assert-infinite-loop-under-ndebug.patch
        0003-fix-gcc-14-incompatible-pointer-types.patch
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
# find_package *directory* to lowercase "ionc" per vcpkg convention so the
# root CMakeLists.txt can find_package(ionc CONFIG). The imported target
# namespace inside the config (IonC::ionc / IonC::decNumber) is preserved.
vcpkg_cmake_config_fixup(PACKAGE_NAME ionc CONFIG_PATH lib/cmake/IonC)

# vcpkg_cmake_config_fixup()'s PACKAGE_NAME only controls which share/<name>
# directory the config files land in -- it does NOT rename the files
# themselves, which ion-c's own build still names IonCConfig.cmake/
# IonCConfigVersion.cmake (from project(IonC ...)). CMake's find_package(ionc
# CONFIG) looks for an exactly-cased ioncConfig.cmake/ionc-config.cmake, which
# a case-sensitive filesystem (Linux) will not match against IonCConfig.cmake
# even though it is in the right directory -- confirmed directly: without
# this rename, find_package(ionc CONFIG) fails with "Could not find a package
# configuration file... ioncConfig.cmake / ionc-config.cmake" despite
# share/ionc/IonCConfig.cmake existing. IonCConfig.cmake's own include() of
# IonCTargets.cmake uses a same-directory relative path, so that file (and
# IonCTargets-debug.cmake/IonCTargets-release.cmake) does not need renaming.
file(RENAME "${CURRENT_PACKAGES_DIR}/share/ionc/IonCConfig.cmake"
     "${CURRENT_PACKAGES_DIR}/share/ionc/ioncConfig.cmake")
file(RENAME "${CURRENT_PACKAGES_DIR}/share/ionc/IonCConfigVersion.cmake"
     "${CURRENT_PACKAGES_DIR}/share/ionc/ioncConfigVersion.cmake")

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

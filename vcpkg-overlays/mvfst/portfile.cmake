# Overlay port for `mvfst` (facebook/mvfst), Facebook's IETF QUIC
# implementation. kythira does not use mvfst directly -- it is pulled in as a
# dependency of `proxygen`, which backs the opt-in Proxygen HTTP transport.
#
# Unlike this project's other overlay ports, mvfst DOES have an official vcpkg
# registry port, and this overlay is not here to add a missing package. It
# exists solely to carry
# 0001-include-cstdlib-for-std-abort.patch, and is otherwise a verbatim copy of
# the registry port at the version the root vcpkg.json's `builtin-baseline`
# resolves to (2025.05.19.00 -- the same folly/fizz/wangle/proxygen release
# train). Keep it that way: if the baseline moves mvfst, re-copy the registry
# port for the new version and re-apply only the PATCHES line, rather than
# letting this file drift into a fork.
#
# `version-string` in this directory's vcpkg.json must stay pinned to that same
# baseline-resolved version. An overlay port overrides *all* versions of a
# package regardless of the baseline, so a mismatch here would silently build a
# different mvfst than the folly/fizz stack it has to be ABI-compatible with.
#
# 0001-include-cstdlib-for-std-abort.patch: mvfst's vendored tiny-optional copy
# (quic/common/third-party/optional.h) calls std::abort() in four value()
# overloads without including <cstdlib>, relying -- by an explicit comment in
# that file -- on <optional> pulling it in transitively under gcc's libstdc++.
# That holds on libstdc++ 13 and stops holding on libstdc++ 14, where the build
# fails with "'abort' is not a member of 'std'". CI builds this project under
# g++-13 and clang++-18, so CI never hits it; a Debian 13 box (g++ 14) does,
# immediately, in mvfst's first debug compile. The patch is additive and is a
# no-op on the compilers that were already fine, so carrying it costs those
# nothing. See README.md for the full diagnosis.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO facebook/mvfst
    REF "v${VERSION}"
    SHA512 eefc84958d57ba09bff3498899f5b71b3bd4afd54def56115c4ecd6e0506a14bd3912b3c8a8824d42c57b1842b7a493613e92cedc5ad2a9a702bda4e348788f2
    HEAD_REF main
    PATCHES
        0001-include-cstdlib-for-std-abort.patch
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DBUILD_TESTS=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/mvfst)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

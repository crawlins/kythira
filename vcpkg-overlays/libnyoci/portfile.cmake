# SKELETON overlay port for `libnyoci` (darconeous/libnyoci, formerly SMCP),
# a full-stack RFC 7252 CoAP client/server library in C. It is a candidate
# alternate backend for kythira's coap_transport, sitting behind the same
# LIBNYOCI_AVAILABLE gate that LIBCOAP_AVAILABLE uses today (see the root
# CMakeLists.txt COAP_TRANSPORT block around line 211 for the pattern this
# mirrors, and doc/coap_library_alternatives.md for the comparison writeup).
#
# KEY DIFFERENCE FROM ion-c / stdexec ports: libnyoci is an AUTOTOOLS project
# (autogen.sh + configure.ac + automake), NOT a CMake project. So this port
# uses vcpkg_configure_make / vcpkg_install_make (the same shape vcpkg's own
# autotools ports use), NOT vcpkg_cmake_configure. That is the single biggest
# packaging cost of this option relative to cantcoap, whose port is a trivial
# vendored CMakeLists.
#
# STATUS: skeleton. The REF/SHA512 below are placeholders and MUST be pinned
# to a real tag/commit and regenerated before this port will fetch. Follow the
# same procedure as vcpkg-overlays/ion-c/README.md: run
#   vcpkg install libnyoci --overlay-ports=vcpkg-overlays
# and copy the "Actual hash" vcpkg prints on the first (failing) fetch.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO darconeous/libnyoci
    # TODO: pin to a real release tag or commit SHA before use.
    REF "v0.1.2"
    # TODO(placeholder): regenerate — see header comment. vcpkg will reject
    # this all-zero digest and print the real one on first fetch.
    SHA512 0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
    HEAD_REF master
    # libnyoci ships a bundled missing.h / autotools tree; extra patches (e.g.
    # a fallback version header, like ion-c's 0001 patch) may be needed once a
    # real REF is pinned and the from-tarball build is exercised.
    # PATCHES
)

# libnyoci's release tarballs normally include a pre-generated ./configure, but
# GitHub archive tarballs (what vcpkg_from_github fetches) do NOT — they contain
# only configure.ac/Makefile.am. AUTOCONFIG re-runs autoreconf to regenerate
# the build scripts. This needs autoconf/automake/libtool/pkg-config on the
# host; document that host requirement in README.md alongside the pin procedure.
vcpkg_configure_make(
    SOURCE_PATH "${SOURCE_PATH}"
    AUTOCONFIG
    OPTIONS
        # Kythira only consumes the library, not the `nyocictl` CLI or the
        # plugtest/example daemons. Trim them so the port builds just libnyoci.
        --disable-tests
        --without-examples
        --enable-static
        # DTLS in libnyoci is provided through an external ssl plugin; wire the
        # concrete backend here once the transport's security story is decided
        # (kythira already links mbedTLS/OpenSSL via coap_security). Left off in
        # the skeleton so the first build is the plain-CoAP core only.
        # --with-ssl=openssl
)

vcpkg_install_make()

# libnyoci does not ship a CMake config package (it is autotools + pkg-config),
# so there is no vcpkg_cmake_config_fixup step. It DOES install a
# libnyoci.pc pkg-config file; fix up its prefix so downstream
# pkg_check_modules(LIBNYOCI QUIET libnyoci) resolves under the vcpkg tree.
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

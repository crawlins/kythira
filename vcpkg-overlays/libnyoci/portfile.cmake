# Overlay port for `libnyoci` (darconeous/libnyoci, formerly SMCP), a
# full-stack RFC 7252 CoAP client/server library in C. It backs the alternate
# CoAP transport in include/raft/coap_transport_libnyoci_impl.hpp, sitting
# behind the LIBNYOCI_AVAILABLE gate that mirrors LIBCOAP_AVAILABLE (see the
# root CMakeLists.txt COAP_TRANSPORT block for the pattern this follows, and
# doc/coap_library_alternatives.md for the comparison writeup).
#
# KEY DIFFERENCE FROM ion-c / stdexec ports: libnyoci is an AUTOTOOLS project
# (bootstrap.sh + configure.ac + automake), NOT a CMake project. So this port
# uses vcpkg_configure_make / vcpkg_install_make (the same shape vcpkg's own
# autotools ports use), NOT vcpkg_cmake_configure. That is the single biggest
# packaging cost of this option relative to cantcoap, whose port would be a
# trivial vendored CMakeLists.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO darconeous/libnyoci
    # Pinned to master's head rather than the one published tag
    # ("0.07.00rc1", refs/tags/latest-release). That tag is the 2017 *initial*
    # release; the 35 commits since it carry the fixes that make the library
    # usable here -- the fuzzing round (null-deref in nyoci-list, missing
    # parens in an option-length check), the URI corruption fix in
    # nyoci_outbound_set_uri(), and the signature change that gave
    # nyoci_inbound_get_path() its maximum-length argument. The adapter calls
    # the three-argument nyoci_inbound_get_path(), so it does not even compile
    # against the tag. Upstream has been quiet since 2019, so this commit is
    # effectively the current release.
    REF 11a6e1b107cb1577cb0fffca7e3fd261f26da94a
    SHA512 ed94ea704b916682ea4a86c2fd46f725eab7d1efde34ccb2d5c6773dc4c16c03157b0d53e1443383e28152d5a05fa0ef51ffe18454cedeef2febaab676f07f0e
    HEAD_REF master
    PATCHES
        # libnyoci was written against a pre-2018 autoconf-archive, whose
        # AX_CODE_COVERAGE AC_SUBST'd a CODE_COVERAGE_RULES make fragment that
        # its Makefile.am files interpolate as `@CODE_COVERAGE_RULES@`. Modern
        # autoconf-archive (serial >= 25) emits those rules through
        # AX_ADD_AM_MACRO_STATIC/aminclude_static.am instead and no longer
        # substitutes the variable at all, so every generated Makefile keeps a
        # literal `@CODE_COVERAGE_RULES@` line and make dies with "missing
        # separator". Dropping the eleven interpolation sites is the smallest
        # fix; kythira does not build libnyoci with coverage instrumentation.
        # (The AC_SUBST fallback in configure.ac's m4_ifdef only fires when the
        # macro is *absent*, which it is not on a host that has the archive
        # installed -- and the archive must be installed, since configure.ac
        # also calls AX_PTHREAD.)
        0001-drop-CODE_COVERAGE_RULES-substitution.patch
        # libnyoci.pc declares no OpenSSL dependency, so with --enable-tls below
        # every static consumer that discovers the library through pkg-config
        # fails to link against ~36 undefined SSL_*/EVP_*/HMAC_* symbols.
        0002-declare-the-openssl-dependency-in-libnyoci-pc.patch
)

# libnyoci's release tarballs would normally include a pre-generated
# ./configure, but GitHub archive tarballs (what vcpkg_from_github fetches) do
# NOT -- they contain only configure.ac/Makefile.am. AUTOCONFIG re-runs
# autoreconf to regenerate the build scripts. This needs autoconf, automake,
# libtool and autoconf-archive on the host; see README.md.
vcpkg_configure_make(
    SOURCE_PATH "${SOURCE_PATH}"
    AUTOCONFIG
    OPTIONS
        # Kythira consumes the library only, not the `nyocictl` CLI, the
        # plugtest daemon or the example programs. Note these are the flags
        # configure.ac actually defines -- there is no --disable-tests and no
        # --without-examples.
        --disable-examples
        --disable-nyocictl
        --disable-plugtest
        # automake's generated depfile bootstrap fails inside the vcpkg build
        # tree the same way it fails anywhere the compiler is invoked through a
        # wrapper; upstream's own CI passes this flag for the same reason
        # (commit 1330755, "travis: configure with --disable-dependency-tracking").
        --disable-dependency-tracking
        # DTLS. libnyoci's OpenSSL plugin is what lets the backend offer
        # dtls_psk and dtls_pki at all: kythira's own coap_security_provider is
        # expressed entirely in libcoap types and has no seam another backend
        # can plug into (see coap_transport_libnyoci_impl.hpp's plan_security()
        # and doc/coap_library_alternatives.md).
        #
        # Upstream still labels this "experimental" and defaults it off, and the
        # plugin calls OpenSSL 1.x-era APIs (HMAC_CTX_new, ERR_load_BIO_strings,
        # ERR_remove_state) that are deprecated-but-present in OpenSSL 3.x, so
        # it builds with deprecation warnings. It is exercised end to end by
        # tests/coap_libnyoci_dtls_test.cpp.
        --enable-tls
)

vcpkg_install_make()

# libnyoci does not ship a CMake config package (it is autotools + pkg-config),
# so there is no vcpkg_cmake_config_fixup step. It DOES install a libnyoci.pc;
# fix up its prefix so downstream pkg_check_modules(LIBNYOCI QUIET libnyoci)
# resolves under the vcpkg tree.
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
# nyocictl is disabled above, so its man page has no binary to document.
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/share/man")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

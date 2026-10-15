# Overlay port for `libcoap` (obgm/libcoap). This is a verbatim copy of the
# microsoft/vcpkg registry port at 4.3.5b (registry baseline
# e182cb4dd2df2ab02f66a1aabd5f35bbdc9522c7, git-tree 8e35de30) plus one extra
# PATCHES entry, fix-ndebug-i2d-x509-in-assert.patch. See README.md.
#
# libcoap 4.3.5a/b moved the i2d_X509() call that serialises the peer
# certificate for validate_cn_call_back inside an assert(). Release builds
# define NDEBUG, so the call is compiled out and the callback receives an
# uninitialised buffer.
#
# It also turns libcoap's own OSCORE off (-DENABLE_OSCORE=OFF): Kythira does
# OSCORE itself, per Raft group, and libcoap's would swallow every protected
# request before Kythira saw it. Drop this overlay only once upstream ships the
# assert fix AND a release offers an external OSCORE context lookup the backend
# has moved onto; until then keep both changes when re-copying the port.

# dllexport is not supported.
if(VCPKG_TARGET_IS_WINDOWS)
  vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
endif()

vcpkg_from_github(
  OUT_SOURCE_PATH SOURCE_PATH
  REPO obgm/libcoap
  REF "v${VERSION}"
  SHA512 b8fc435412cd1909bc9ba5683cfa138a3ea08a76fecab78739ceedc7bb903d15d50d4362f702f7380fd047e5b6df3c76dfb75dd30bb20670a62205e6bc85021d
  HEAD_REF main
  PATCHES
      obgm-remove-self-configure-file.patch # https://github.com/obgm/libcoap/pull/1736
      remove-hardcoded-tinydtls-path.patch
      fix-ndebug-i2d-x509-in-assert.patch
)

vcpkg_check_features(
  OUT_FEATURE_OPTIONS FEATURE_OPTIONS
  FEATURES
      examples ENABLE_EXAMPLES
      dtls     ENABLE_DTLS
)

vcpkg_cmake_configure(
  SOURCE_PATH "${SOURCE_PATH}"
  OPTIONS
      ${FEATURE_OPTIONS}
      -DENABLE_DOCS=OFF
      -DENABLE_OSCORE=OFF
      -DDTLS_BACKEND=openssl)

vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/libcoap")

if("examples" IN_LIST FEATURES)
  vcpkg_copy_tools(
      TOOL_NAMES coap-client coap-rd coap-server
      AUTO_CLEAN
  )
  # Same condition in licoap/CMakeLists.txt
  if(NOT VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_MINGW)
    vcpkg_copy_tools(
        TOOL_NAMES etsi_iot_01 tiny oscore-interop-server
        AUTO_CLEAN
    )
  endif()
endif()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

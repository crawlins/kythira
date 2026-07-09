# Overlay port for `lakers` (EDHOC, RFC 9528), gating Kythira's
# coap-transport-security EDHOC-to-OSCORE bootstrap (oscore_bootstrap::edhoc).
#
# lakers publishes pre-built lakers-c static libraries on its GitHub releases
# only for embedded-MCU targets (e.g. cryptocell310-thumbv7em-none-eabihf) —
# there is no desktop/server (x64-linux, etc.) binary to download. This port
# therefore builds from source with cargo, using the small FFI crate vendored
# at ffi/ (Kythira-authored; see ffi/src/lib.rs) rather than upstream's
# lakers-c, which only exposes the Initiator role over C — Kythira's
# coap_server also needs the Responder role. A Rust toolchain (cargo/rustc)
# must be available wherever this port is actually installed; it is not a
# default dependency of this project (see the top-level vcpkg.json's
# optional "edhoc" feature) so its absence never affects the default build.

# Rust staticlibs aren't meaningfully split into separate debug/release
# artifacts the way vcpkg's CMake-based ports are; build release only and
# reuse it for both configurations (see lakers-config.cmake.in).
set(VCPKG_BUILD_TYPE "release")
set(VCPKG_POLICY_MISMATCHED_NUMBER_OF_BINARIES enabled)

find_program(KYTHIRA_CARGO_EXECUTABLE cargo)
if(NOT KYTHIRA_CARGO_EXECUTABLE)
    message(FATAL_ERROR
        "lakers (EDHOC) requires a Rust toolchain (cargo/rustc) to build "
        "ffi/, since upstream lakers only publishes pre-built binaries for "
        "embedded-MCU targets, not this triplet. Install Rust (e.g. via "
        "rustup) and re-run 'vcpkg install lakers', or drop the 'edhoc' "
        "feature from your vcpkg.json if EDHOC bootstrap isn't needed.")
endif()

set(KYTHIRA_LAKERS_FFI_SRC "${CMAKE_CURRENT_LIST_DIR}/ffi")
set(KYTHIRA_LAKERS_TARGET_DIR "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-rel/cargo-target")

vcpkg_execute_required_process(
    COMMAND "${KYTHIRA_CARGO_EXECUTABLE}" build --release
            --manifest-path "${KYTHIRA_LAKERS_FFI_SRC}/Cargo.toml"
            --target-dir "${KYTHIRA_LAKERS_TARGET_DIR}"
    WORKING_DIRECTORY "${KYTHIRA_LAKERS_FFI_SRC}"
    LOGNAME "build-lakers-kythira-ffi-${TARGET_TRIPLET}"
)

set(KYTHIRA_LAKERS_LIB "${KYTHIRA_LAKERS_TARGET_DIR}/release/liblakers_kythira_ffi.a")
if(NOT EXISTS "${KYTHIRA_LAKERS_LIB}")
    message(FATAL_ERROR "cargo build did not produce ${KYTHIRA_LAKERS_LIB}")
endif()

file(INSTALL "${KYTHIRA_LAKERS_LIB}" DESTINATION "${CURRENT_PACKAGES_DIR}/lib")
file(INSTALL "${KYTHIRA_LAKERS_FFI_SRC}/include/lakers_kythira.h"
     DESTINATION "${CURRENT_PACKAGES_DIR}/include")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/lakers-config.cmake.in"
     DESTINATION "${CURRENT_PACKAGES_DIR}/share/lakers"
     RENAME "lakers-config.cmake")

vcpkg_install_copyright(FILE_LIST "${CMAKE_CURRENT_LIST_DIR}/LICENSE")

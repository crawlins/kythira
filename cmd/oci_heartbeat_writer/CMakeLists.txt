# The on-instance heartbeat process (Requirement 4.4) — see main.cpp.
#
# Gated by the caller (root CMakeLists.txt) on the same condition as the OCI
# quorum-manager tests: the writer's header includes the quorum manager's for
# its tag constants and parsing helpers, so the two are one feature from the
# build's point of view. network_simulator carries the include path, Folly
# and httplib the same way it does for kythira_add_oci_test targets;
# boost_json is linked explicitly because network_simulator defines that
# target without linking it.
#
# This binary is shipped to a pool instance by the oci CI job (uploaded to
# the kythira-ci-artifacts bucket, fetched by cloud-init via a
# pre-authenticated request URL — scripts/ci-cloud-credentials/oci/
# heartbeat-cloud-init.sh). That works only while the build host's glibc is
# not newer than the pool image's; both are Ubuntu 24.04 by construction
# (ci.yml runners, and the pool's instance configuration since the
# Requirement 4.4 bring-up).

add_executable(oci_heartbeat_writer_node main.cpp)

target_include_directories(oci_heartbeat_writer_node PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
)

# This binary runs on a stock Ubuntu cloud image with NO package installs
# possible (the pool subnet has no internet route). Found the hard way: the
# clang-built CI binary linked libatomic.so.1 dynamically, which Canonical's
# cloud images do not ship — the writer died at load with "cannot open
# shared object file", observable only via serial-console exfiltration. The
# image's loader reported ONLY libatomic missing, so libstdc++/libgcc/glibc
# are all present there and stay dynamic — deliberately: -static-libstdc++
# was tried and collides with folly's ExceptionTracerLib, which interposes
# std::rethrow_exception and multiply-defines it against libstdc++.a.
#
# The dynamic -latomic comes from network_simulator's INTERFACE link (the
# root CMakeLists adds it for Clang), so a link *option* on this target
# cannot beat it — options precede libraries on the link line. Instead the
# static archive is linked FIRST in the library list: every atomic symbol
# binds from the archive, the trailing -latomic contributes nothing, and
# --as-needed (passed explicitly below — clang++ on this runner does NOT
# default to it) drops its DT_NEEDED entry. Verified with ldd on the clang
# build: no libatomic.so.1 row.
execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libatomic.a
                OUTPUT_VARIABLE _KYTHIRA_LIBATOMIC_A OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT EXISTS "${_KYTHIRA_LIBATOMIC_A}")
    set(_KYTHIRA_LIBATOMIC_A "")
    message(WARNING "libatomic.a not found via ${CMAKE_CXX_COMPILER}; "
                    "oci_heartbeat_writer_node may not run on a stock cloud image")
endif()

target_link_options(oci_heartbeat_writer_node PRIVATE "LINKER:--as-needed")
target_link_libraries(oci_heartbeat_writer_node PRIVATE
    ${_KYTHIRA_LIBATOMIC_A}
    network_simulator
    boost_json
    httplib::httplib
    OpenSSL::SSL
    OpenSSL::Crypto
)

target_compile_features(oci_heartbeat_writer_node PRIVATE cxx_std_23)

message(STATUS "oci_heartbeat_writer_node: enabled (Requirement 4.4 on-instance heartbeat)")

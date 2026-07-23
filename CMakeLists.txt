cmake_minimum_required(VERSION 3.20)
project(network_simulator VERSION 0.1.0 LANGUAGES CXX)

# C++23 standard required
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# vcpkg triplet for this host, used everywhere a path under vcpkg_installed/
# needs to be resolved manually (i.e. anywhere find_package()/vcpkg's own
# CMAKE_PREFIX_PATH wiring isn't already doing it for us). Kythira only
# targets 64-bit x86 and 64-bit ARM Linux; every other CMAKE_SYSTEM_PROCESSOR
# value falls back to x64-linux rather than failing configure outright.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(KYTHIRA_VCPKG_TRIPLET "arm64-linux")
else()
    set(KYTHIRA_VCPKG_TRIPLET "x64-linux")
endif()

# ccache: speeds up rebuilds by skipping recompilation of translation units
# whose preprocessed content + compiler flags exactly match a prior compile.
# Purely opt-in acceleration — absent or disabled, the build is identical to
# today. See .kiro/specs/ccache-adoption/ for the measured impact (~59%
# reduction in a from-scratch "nothing changed" rebuild; real numbers from
# a throwaway CI experiment, not a guess).
option(KYTHIRA_ENABLE_CCACHE "Use ccache to speed up rebuilds when available" ON)

if(KYTHIRA_ENABLE_CCACHE)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}" CACHE STRING "" FORCE)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}" CACHE STRING "" FORCE)
        message(STATUS "ccache: enabled (${CCACHE_PROGRAM})")
    else()
        message(STATUS "ccache: not found, building without it")
    endif()
else()
    message(STATUS "ccache: disabled (KYTHIRA_ENABLE_CCACHE=OFF)")
endif()

# Code coverage support
option(ENABLE_COVERAGE "Build with gcov coverage instrumentation" OFF)

if(ENABLE_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR
            "ENABLE_COVERAGE requires Clang for source-based coverage "
            "(current compiler: ${CMAKE_CXX_COMPILER_ID}). "
            "Reconfigure with -DCMAKE_CXX_COMPILER=clang++")
    endif()
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
        message(STATUS "ENABLE_COVERAGE: forcing CMAKE_BUILD_TYPE=Debug")
    endif()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-instr-generate -fcoverage-mapping")
    # -latomic: clang with GCC's libstdc++ doesn't provide __atomic_is_lock_free
    # as a compiler builtin the way GCC does; libatomic supplies it at link time.
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fprofile-instr-generate -latomic")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fprofile-instr-generate")
    message(STATUS "Source-based coverage enabled "
                   "(${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION})")
endif()

# Find dependencies
# Note: folly is required for the full implementation
# For now, we'll make it optional to allow initial build
find_package(folly QUIET)
find_package(Boost QUIET COMPONENTS system thread unit_test_framework json context)
find_package(httplib QUIET)
find_package(OpenSSL QUIET)

# Find libcoap using CMake config (vcpkg) or PkgConfig (system)
find_package(libcoap QUIET CONFIG)
if(NOT libcoap_FOUND)
    # Fallback to PkgConfig for system installations
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBCOAP QUIET libcoap-3)
    endif()
else()
    # Set LIBCOAP_FOUND for consistency with pkg-config path
    set(LIBCOAP_FOUND TRUE)
endif()

# Find libldns using PkgConfig (system installation; pc file is named "ldns")
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBLDNS QUIET ldns)
endif()

# EDHOC support (coap-transport-security spec, Requirement 5) — optional;
# only present when vcpkg installed with the "edhoc" feature enabled (it
# requires a Rust toolchain to build from source; see
# vcpkg-overlays/lakers/README.md). Absent by default, in which case
# LAKERS_AVAILABLE stays undefined and oscore_bootstrap::edhoc fails
# construction with a descriptive error instead of silently degrading.
find_package(lakers CONFIG QUIET)

# stdexec-future-backend spec: a second, sender/receiver-based Future/
# Promise/Executor implementation alongside the default Folly one. A
# default (non-feature-gated) dependency, same as folly — see
# vcpkg-overlays/stdexec/README.md for why it's an overlay port.
find_package(stdexec CONFIG QUIET)

if(NOT folly_FOUND)
    # include/raft/future.hpp (kythira::Future/Promise/Try, the default
    # backend every other backend coexists alongside — see
    # .kiro/specs/stdexec-future-backend/ and
    # .kiro/specs/boost-future-backend/) includes Folly headers
    # unconditionally, with no #ifdef of its own. Nearly everything in this
    # project transitively depends on it: the Raft core, both HTTP/CoAP
    # transports, certificate_authority/ca_service/ca_cluster_node (all
    # gated on folly_FOUND accordingly), and most of the test suite. This
    # is not a partial-implementation warning — it means most targets will
    # not be defined at all, cleanly skipped via each one's own folly_FOUND
    # guard, rather than a hard configure failure.
    message(WARNING
        "folly not found — most targets will be unavailable (Raft core, "
        "HTTP/CoAP transports, certificate_authority/ca_service/"
        "ca_cluster_node, and most of the test suite all require it "
        "transitively via include/raft/future.hpp). Only Folly-independent "
        "targets will be configured. See vcpkg.json / DEPENDENCIES.md."
    )
endif()

if(NOT stdexec_FOUND)
    message(WARNING "stdexec not found. The stdexec-backed Future/Promise implementation will not be available.")
endif()

if(NOT Boost_FOUND)
    message(WARNING "Boost not found. Tests will not be built.")
endif()

if(NOT httplib_FOUND)
    message(WARNING "cpp-httplib not found. HTTP transport will not be available.")
endif()

if(NOT OpenSSL_FOUND)
    message(WARNING "OpenSSL not found. HTTPS/TLS support will not be available.")
endif()

if(NOT LIBCOAP_FOUND)
    message(WARNING "libcoap not found. CoAP transport will not be available.")
endif()

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)

# Library target
add_library(network_simulator INTERFACE)
target_include_directories(network_simulator INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
if(folly_FOUND)
    target_link_libraries(network_simulator INTERFACE Folly::folly)
    # When linking Folly, we must also link Boost::context (required by Folly fibers)
    # Use vcpkg static library if available for compatibility
    if(EXISTS "${CMAKE_SOURCE_DIR}/vcpkg_installed/${KYTHIRA_VCPKG_TRIPLET}/lib/libboost_context.a")
        target_link_libraries(network_simulator INTERFACE "${CMAKE_SOURCE_DIR}/vcpkg_installed/${KYTHIRA_VCPKG_TRIPLET}/lib/libboost_context.a")
    elseif(TARGET Boost::context)
        target_link_libraries(network_simulator INTERFACE Boost::context)
    endif()
endif()

if(Boost_FOUND)
    target_link_libraries(network_simulator INTERFACE
        Boost::system
        Boost::thread
    )
    # Create boost_json imported library using vcpkg static library if available
    # This ensures compatibility with vcpkg-built Folly
    if(NOT TARGET boost_json)
        if(EXISTS "${CMAKE_SOURCE_DIR}/vcpkg_installed/${KYTHIRA_VCPKG_TRIPLET}/lib/libboost_json.a")
            add_library(boost_json STATIC IMPORTED)
            set_target_properties(boost_json PROPERTIES
                IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/vcpkg_installed/${KYTHIRA_VCPKG_TRIPLET}/lib/libboost_json.a"
            )
        elseif(TARGET Boost::json)
            add_library(boost_json ALIAS Boost::json)
        endif()
    endif()
endif()

if(httplib_FOUND)
    target_link_libraries(network_simulator INTERFACE httplib::httplib)
endif()

if(OpenSSL_FOUND)
    target_link_libraries(network_simulator INTERFACE OpenSSL::SSL OpenSSL::Crypto)
endif()

if(lakers_FOUND)
    target_link_libraries(network_simulator INTERFACE lakers::lakers)
    target_compile_definitions(network_simulator INTERFACE LAKERS_AVAILABLE)
endif()

if(stdexec_FOUND)
    target_link_libraries(network_simulator INTERFACE STDEXEC::stdexec)
    target_compile_definitions(network_simulator INTERFACE KYTHIRA_HAS_STDEXEC)
    # exec/single_thread_context.hpp (and likely other exec/ scheduler
    # headers) reference TBB symbols unconditionally at link time —
    # confirmed during the Phase 0 spike (see spike-notes.md) — regardless
    # of whether stdexec itself was built with STDEXEC_ENABLE_TBB. The
    # "tbb" vcpkg feature (requested in vcpkg.json) provisions a hermetic
    # TBB build for exactly this.
    find_package(TBB CONFIG QUIET)
    if(TBB_FOUND)
        target_link_libraries(network_simulator INTERFACE TBB::tbb)
    else()
        message(WARNING "TBB not found even though stdexec was — code using exec:: scheduler types that need TBB may fail to link. Re-run vcpkg install to pick up the \"tbb\" feature.")
    endif()
    # Confirmed via an isolated repro during Phase 3 (see
    # .kiro/specs/stdexec-future-backend/spike-notes.md): GCC 13 at -O2/-O3
    # miscompiles exec::any_sender's small-buffer-optimized move
    # constructor — moving one any_sender holding a small payload (e.g.
    # stdexec::just(int), the common case) into another any_sender of the
    # same type corrupts the heap, crashing on a later, unrelated
    # allocation. -O1 is unaffected, as is clang++-18 at every level;
    # -fno-strict-aliasing at -O2/-O3 with GCC also resolves it, so that's
    # applied here rather than capping GCC builds at -O1 project-wide.
    # GCC-only: clang++-18 needs no workaround and -fno-strict-aliasing is
    # a spelling GCC understands but is unnecessary noise on other
    # compilers' command lines.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(network_simulator INTERFACE -fno-strict-aliasing)
    endif()
endif()

# Requirement 11: which backend kythira::future_default<T> (the non-generic
# call sites that just want "the project's chosen future implementation"
# without spelling out a backend) resolves to. Templated core code that is
# already generic over a future type never references future_default and
# is unaffected by this option either way (Requirement 11.2/11.5).
set(KYTHIRA_DEFAULT_FUTURE_BACKEND "folly" CACHE STRING
    "Backend kythira::future_default<T> resolves to: folly (default) or stdexec")
set_property(CACHE KYTHIRA_DEFAULT_FUTURE_BACKEND PROPERTY STRINGS folly stdexec)
if(NOT KYTHIRA_DEFAULT_FUTURE_BACKEND STREQUAL "folly" AND NOT KYTHIRA_DEFAULT_FUTURE_BACKEND STREQUAL "stdexec")
    message(FATAL_ERROR "KYTHIRA_DEFAULT_FUTURE_BACKEND must be \"folly\" or \"stdexec\", got \"${KYTHIRA_DEFAULT_FUTURE_BACKEND}\"")
endif()
if(KYTHIRA_DEFAULT_FUTURE_BACKEND STREQUAL "stdexec")
    if(NOT stdexec_FOUND)
        message(FATAL_ERROR "KYTHIRA_DEFAULT_FUTURE_BACKEND=stdexec requires the stdexec dependency, which was not found")
    endif()
    target_compile_definitions(network_simulator INTERFACE KYTHIRA_FUTURE_BACKEND_STDEXEC)
endif()

# Property 5: Optional Dependency Isolation (Task 1.1, Requirements 4.2,
# 4.3) — configures a throwaway build directory with stdexec hidden from
# find_package() and confirms the rest of the project still configures
# and builds. `cmake --build build --target verify-optional-dependency-isolation`.
add_custom_target(verify-optional-dependency-isolation
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/verify-optional-dependency-isolation.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Verifying the project builds correctly with stdexec hidden from find_package()"
    VERBATIM
)

# cpp-httplib is a single header; its own vcpkg port treats "openssl" as a
# consumer-opt-in feature rather than a default one (no extra vcpkg feature is
# needed on our side — we already depend on openssl directly). Without this
# definition, httplib::SSLServer/SSLClient simply don't exist, which is what
# left cpp_httplib_server's SSL support unimplemented (see certificate-authority
# spec Requirement 14) and would otherwise block ca_service --serve's own
# optional --tls-cert/--tls-key mode (Requirement 11.5).
if(httplib_FOUND AND OpenSSL_FOUND)
    target_compile_definitions(network_simulator INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)
endif()

if(LIBCOAP_FOUND)
    target_include_directories(network_simulator INTERFACE ${LIBCOAP_INCLUDE_DIRS})
    target_link_libraries(network_simulator INTERFACE ${LIBCOAP_LIBRARIES})
    target_compile_definitions(network_simulator INTERFACE ${LIBCOAP_CFLAGS_OTHER})
endif()

if(LIBLDNS_FOUND)
    target_compile_definitions(network_simulator INTERFACE KYTHIRA_HAS_LDNS)
    target_include_directories(network_simulator INTERFACE ${LIBLDNS_INCLUDE_DIRS})
    target_link_libraries(network_simulator INTERFACE ${LIBLDNS_LIBRARIES})
    message(STATUS "libldns found: ${LIBLDNS_VERSION}")
else()
    message(STATUS "libldns not found — rfc1035/rfc2136 peer discovery disabled")
endif()

# ── Poco DNSSD (poco_peer_discovery) ──────────────────────────────────────────
# DNSSD is not a vcpkg feature; the static libs are built manually and placed
# in the vcpkg_installed tree alongside the standard poco[net] artifacts.
# This project only manually builds/provides these archives for x64-linux —
# on any other resolved triplet (e.g. arm64-linux) the EXISTS check below
# simply fails and POCO_DNSSD_FOUND stays FALSE, exactly like an x64-linux
# host that hasn't built the archives either. See doc/TODO.md / README.md's
# ARM support section for this known limitation.
set(POCO_DNSSD_FOUND FALSE)
set(_poco_dnssd_lib     "${CMAKE_SOURCE_DIR}/vcpkg_installed/${KYTHIRA_VCPKG_TRIPLET}/lib/libPocoDNSSD.a")
set(_poco_dnssd_avahi   "${CMAKE_SOURCE_DIR}/vcpkg_installed/${KYTHIRA_VCPKG_TRIPLET}/lib/libPocoDNSSDAvahi.a")
set(_poco_dnssd_inc     "${CMAKE_SOURCE_DIR}/vcpkg_installed/${KYTHIRA_VCPKG_TRIPLET}/include")
if(EXISTS "${_poco_dnssd_lib}" AND EXISTS "${_poco_dnssd_avahi}")
    # avahi-client / avahi-common are runtime-installed but their dev headers and
    # unversioned .so symlinks are absent without the -dev package.  We look first
    # in the extracted deb tree (symlinks or static archives created without sudo).
    # CMAKE_LIBRARY_ARCHITECTURE is CMake's own Debian/Ubuntu multiarch-tuple
    # detection (e.g. "x86_64-linux-gnu", "aarch64-linux-gnu"); fall back to a
    # triplet-derived guess only in the unlikely case it's unset.
    if(CMAKE_LIBRARY_ARCHITECTURE)
        set(_avahi_multiarch_tuple "${CMAKE_LIBRARY_ARCHITECTURE}")
    elseif(KYTHIRA_VCPKG_TRIPLET STREQUAL "arm64-linux")
        set(_avahi_multiarch_tuple "aarch64-linux-gnu")
    else()
        set(_avahi_multiarch_tuple "x86_64-linux-gnu")
    endif()
    find_library(AVAHI_CLIENT_LIB
        NAMES avahi-client
        PATHS /tmp/avahi-dev/usr/lib/${_avahi_multiarch_tuple}
              /usr/lib/${_avahi_multiarch_tuple}
              /usr/lib
        NO_DEFAULT_PATH
    )
    find_library(AVAHI_COMMON_LIB
        NAMES avahi-common
        PATHS /tmp/avahi-dev/usr/lib/${_avahi_multiarch_tuple}
              /usr/lib/${_avahi_multiarch_tuple}
              /usr/lib
        NO_DEFAULT_PATH
    )
    if(AVAHI_CLIENT_LIB AND AVAHI_COMMON_LIB)
        set(POCO_DNSSD_FOUND TRUE)
        set(POCO_DNSSD_LIBRARIES
            "${_poco_dnssd_avahi}"
            "${_poco_dnssd_lib}"
            "${CMAKE_SOURCE_DIR}/vcpkg_installed/${KYTHIRA_VCPKG_TRIPLET}/lib/libPocoNet.a"
            "${CMAKE_SOURCE_DIR}/vcpkg_installed/${KYTHIRA_VCPKG_TRIPLET}/lib/libPocoFoundation.a"
            "${AVAHI_CLIENT_LIB}"
            "${AVAHI_COMMON_LIB}"
        )
        # Don't expose the whole manually-populated vcpkg_installed/.../include
        # root as an include path: it also contains a full, independently-
        # extracted copy of every other vcpkg package (boost included) that can
        # genuinely diverge from the build tree's own vcpkg_installed/ copy —
        # observed concretely as an unpatched, pre-fix boost/mpl (and
        # boost/config) whose parenthesized computed-include macros
        # (`#define BOOST_SLIST_HEADER (<ext/slist>)` etc.) don't survive
        # BOOST_PP_STRINGIZE on Clang, producing a literal malformed path like
        # `boost/mpl/aux_/preprocessed/(gcc/or.hpp)`. Any target that also
        # #includes <boost/test/unit_test.hpp> (i.e. any Boost.Test-based unit
        # test — this Poco DNSSD one is the only current example) would pick up
        # that broken copy instead of the correct, already-on-the-search-path
        # build-tree one. A shim directory containing only a symlink to the
        # manually-built Poco/DNSSD subtree gets consumers exactly the headers
        # they need without ever exposing this dir's boost/ (or anything else)
        # to the compiler.
        set(_poco_dnssd_shim "${CMAKE_BINARY_DIR}/poco_dnssd_include_shim")
        file(MAKE_DIRECTORY "${_poco_dnssd_shim}/Poco")
        if(NOT EXISTS "${_poco_dnssd_shim}/Poco/DNSSD")
            file(CREATE_LINK "${_poco_dnssd_inc}/Poco/DNSSD" "${_poco_dnssd_shim}/Poco/DNSSD"
                 SYMBOLIC)
        endif()
        set(POCO_DNSSD_INCLUDE_DIRS "${_poco_dnssd_shim}")
        message(STATUS "Poco DNSSD found in vcpkg tree (manually built)")
    else()
        message(STATUS "Poco DNSSD: avahi libs not found — poco_peer_discovery disabled")
    endif()
else()
    # Fall back to CMake config (system install) or pkg-config.
    find_package(Poco QUIET COMPONENTS DNSSD Foundation)
    if(Poco_FOUND AND TARGET Poco::DNSSD)
        set(POCO_DNSSD_FOUND TRUE)
        set(POCO_DNSSD_LIBRARIES Poco::DNSSD Poco::Foundation)
        set(POCO_DNSSD_INCLUDE_DIRS "")
        message(STATUS "Poco DNSSD found via CMake config")
    else()
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(POCO_DNSSD QUIET PocoDNSSD)
        endif()
        if(POCO_DNSSD_FOUND)
            message(STATUS "Poco DNSSD found via pkg-config: ${POCO_DNSSD_VERSION}")
        endif()
    endif()
endif()

if(POCO_DNSSD_FOUND)
    target_compile_definitions(network_simulator INTERFACE KYTHIRA_HAS_POCO_DNSSD)
    # vcpkg's toolchain already adds the build tree's own vcpkg_installed/.../include
    # to every target's search path, which covers ordinary vcpkg-managed Poco
    # headers (Foundation, Net, ...) automatically. It does NOT cover
    # Poco/DNSSD specifically — those headers are manually built and placed
    # only in the source tree's vcpkg_installed/ (see above), never
    # replicated into the build tree's copy vcpkg itself manages. Anything
    # that #includes <Poco/DNSSD/...> directly (poco_discovery_node,
    # poco_peer_discovery_unit_test) must add ${POCO_DNSSD_INCLUDE_DIRS}
    # itself; not added here because network_simulator's own headers don't
    # need it, and propagating it INTERFACE would leak the source tree's
    # vcpkg_installed/.../include root (with its own independently-extracted,
    # potentially-diverged copy of every other vcpkg package) to every
    # consumer — see POCO_DNSSD_INCLUDE_DIRS's own shim-directory comment
    # above for the concrete bug that caused.
    target_link_libraries(network_simulator INTERFACE ${POCO_DNSSD_LIBRARIES})
else()
    message(STATUS "Poco DNSSD not found — poco_peer_discovery disabled")
endif()

# ── AWS SDK (aws_ec2_quorum_manager, aws_asg_quorum_manager) ──────────────────
find_package(AWSSDK QUIET COMPONENTS core ec2 autoscaling iam s3 sts)
if(AWSSDK_FOUND)
    message(STATUS "AWS SDK found: ${AWSSDK_VERSION}")
    target_compile_definitions(network_simulator INTERFACE KYTHIRA_HAS_AWS_SDK)
    target_link_libraries(network_simulator INTERFACE ${AWSSDK_LINK_LIBRARIES})
else()
    message(STATUS "AWS SDK not found — aws_ec2_quorum_manager and aws_asg_quorum_manager disabled")
endif()

# ── ACM Private CA (aws_acm_pca_provider) ─────────────────────────────────────
# Independent of KYTHIRA_HAS_AWS_SDK: environments with the core AWS SDK but
# without the acm-pca component still build everything except aws_acm_pca_provider.
# AWSSDKConfig.cmake short-circuits (skips its own body, including defining any
# new component's imported target) once AWSSDK_FOUND is already TRUE from the
# earlier find_package(AWSSDK COMPONENTS core ec2 ...) call above — unset it so
# this second call actually re-runs and picks up the acm-pca component. Checking
# for the component's own imported target afterward (rather than trusting
# AWSSDK_FOUND) additionally guards against the component genuinely being
# absent, matching the project's find_package(AWSSDK...) result caching model.
unset(AWSSDK_FOUND)
find_package(AWSSDK QUIET COMPONENTS acm-pca)
if(TARGET aws-cpp-sdk-acm-pca)
    message(STATUS "AWS ACM Private CA SDK component found")
    target_compile_definitions(network_simulator INTERFACE KYTHIRA_HAS_AWS_ACM_PCA)
    target_link_libraries(network_simulator INTERFACE aws-cpp-sdk-acm-pca)
else()
    message(STATUS "AWS ACM Private CA SDK component not found — aws_acm_pca_provider disabled")
endif()

# ── certificate_authority (CA testing/provisioning framework) ────────────────
# folly_FOUND, not just TARGET OpenSSL::SSL: certificate_provider.hpp's
# kythira::Future usage (via include/raft/future.hpp) pulls in Folly
# unconditionally — that #include has no #ifdef guard of its own, unlike the
# stdexec/boost backends, which only exist behind their own *_FOUND checks.
# Without this, a folly-not-found configure still attempted this target and
# failed deep inside src/certificate_authority.cpp's #include chain with a
# `GLOG_EXPORT` "unknown type name" error from glog/flags.h — a real,
# confirmed failure (reproduced via `-DCMAKE_DISABLE_FIND_PACKAGE_folly=ON`,
# the same isolation technique
# scripts/verify-optional-dependency-isolation.sh already uses for stdexec)
# that gives no hint the actual problem is a missing Folly dependency.
if(TARGET OpenSSL::SSL AND folly_FOUND)
    add_library(certificate_authority STATIC src/certificate_authority.cpp)
    target_include_directories(certificate_authority PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    # network_simulator (PUBLIC): certificate_provider.hpp's kythira::Future usage
    # pulls in Folly, which needs the same include-order/link setup
    # network_simulator's INTERFACE target already carries — certificate_authority
    # consumers get it transitively rather than duplicating the Folly/glog wiring.
    target_link_libraries(certificate_authority PUBLIC OpenSSL::SSL OpenSSL::Crypto network_simulator)
    target_compile_definitions(certificate_authority PUBLIC KYTHIRA_HAS_OPENSSL)
    target_compile_features(certificate_authority PUBLIC cxx_std_23)
    message(STATUS "certificate_authority target: enabled")
elseif(NOT TARGET OpenSSL::SSL)
    message(STATUS "OpenSSL not found — certificate_authority target disabled")
else()
    message(STATUS "folly not found — certificate_authority target disabled (requires Folly via raft/future.hpp)")
endif()

# ── libssh2 (real-EC2 integration tests, in-memory SSH key material) ─────────
# Prefer the CMake config package (vcpkg, conan) over pkg-config so that we get
# a proper imported target rather than a bare -lssh2 that the linker can't resolve.
find_package(libssh2 QUIET)
if(libssh2_FOUND)
    set(LIBSSH2_FOUND TRUE)
    set(LIBSSH2_LIBRARIES libssh2::libssh2)
    set(LIBSSH2_INCLUDE_DIRS "")
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBSSH2 QUIET libssh2)
    endif()
endif()
if(LIBSSH2_FOUND)
    message(STATUS "libssh2 found — real-EC2 SSH tests enabled")
else()
    message(STATUS "libssh2 not found — real-EC2 SSH tests disabled")
endif()

target_compile_features(network_simulator INTERFACE cxx_std_23)

# Debug executable - commented out due to missing source files
# add_executable(debug_connect_test debug_connect_test.cpp)
# target_link_libraries(debug_connect_test PRIVATE network_simulator)
# if(folly_FOUND)
#     target_link_libraries(debug_connect_test PRIVATE Folly::folly)
# endif()
# target_compile_features(debug_connect_test PRIVATE cxx_std_23)

# Register debug test with CTest
# add_test(NAME debug_connect_test COMMAND debug_connect_test)
# set_tests_properties(debug_connect_test PROPERTIES
#     TIMEOUT 30
#     LABELS "debug;connection"
# )

# Debug accept test - commented out due to missing source files
# add_executable(debug_accept_test debug_accept_test.cpp)
# target_link_libraries(debug_accept_test PRIVATE network_simulator)
# if(folly_FOUND)
#     target_link_libraries(debug_accept_test PRIVATE Folly::folly)
# endif()
# target_compile_features(debug_accept_test PRIVATE cxx_std_23)

# Single test case - commented out due to missing source files
# add_executable(test_single_case test_single_case.cpp)
# target_link_libraries(test_single_case PRIVATE network_simulator Boost::unit_test_framework)
# if(folly_FOUND)
#     target_link_libraries(test_single_case PRIVATE Folly::folly)
# endif()
# target_compile_features(test_single_case PRIVATE cxx_std_23)

# Find libfiu for fault injection / chaos tests
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FIU QUIET libfiu)
endif()

if(FIU_FOUND)
    set(CHAOS_TESTS_ENABLED TRUE)
    message(STATUS "libfiu found (${FIU_VERSION}) — chaos tests and fault injection enabled")
else()
    set(CHAOS_TESTS_ENABLED FALSE)
    message(STATUS "libfiu not found — chaos tests disabled (sudo apt install libfiu-dev)")
endif()

# Enable testing
enable_testing()

# Add subdirectories
# cmd/ca_service and cmd/ca_cluster_node are added before tests/
# (tests/CMakeLists.txt gates its dependent targets on `if(TARGET ca_service)`
# / `if(TARGET ca_cluster_node)`, which only sees targets already defined
# earlier in configure-time processing order).
if(NOT ENABLE_COVERAGE)
    if(TARGET certificate_authority)
        add_subdirectory(cmd/ca_service)
        if(folly_FOUND AND Boost_FOUND AND httplib_FOUND)
            add_subdirectory(cmd/ca_cluster_node)
        endif()
    endif()
endif()
# folly_FOUND, not just Boost_FOUND: confirmed by direct build attempt
# (-DCMAKE_DISABLE_FIND_PACKAGE_folly=ON) that the overwhelming majority of
# this project's 391+ registered tests transitively require Folly (every
# kythira::Future/Promise/Try use is Folly-backed regardless of which
# backend a given test is actually about — even stdexec-only test files
# still transitively reach include/raft/future.hpp through shared project
# headers) and fail the same GLOG_EXPORT compile error the
# certificate_authority/examples guards above document. tests/CMakeLists.txt
# already does its own finer-grained per-target gating in specific places
# (e.g. the discovery-node docker image targets, each on their own FOUND
# variable) - this guard only prevents attempting the whole subdirectory
# when Folly, tests/CMakeLists.txt's own single largest transitive
# dependency, is absent.
if(Boost_FOUND AND folly_FOUND)
    add_subdirectory(tests)
elseif(Boost_FOUND)
    message(STATUS "folly not found — tests/ disabled (391+ registered tests overwhelmingly require it transitively)")
endif()
if(NOT ENABLE_COVERAGE)
    if(folly_FOUND AND Boost_FOUND AND httplib_FOUND)
        add_subdirectory(cmd/chaos_node)
        add_subdirectory(cmd/poco_discovery_node)
    endif()
    if(LIBLDNS_FOUND AND folly_FOUND AND httplib_FOUND)
        add_subdirectory(cmd/dns_discovery_node)
        add_subdirectory(cmd/dns_sd_discovery_node)
    endif()
endif()
# folly_FOUND: confirmed by direct build attempt (-DCMAKE_DISABLE_FIND_
# PACKAGE_folly=ON) that every example target transitively requires Folly
# (via network_simulator's own headers, which reach include/raft/future.hpp
# through several paths) and fails the same confusing GLOG_EXPORT compile
# error the certificate_authority guard above documents — not a partial
# gap affecting only some examples.
if(folly_FOUND)
    add_subdirectory(examples)
else()
    message(STATUS "folly not found — examples/ targets disabled (all transitively require Folly)")
endif()

# Installation rules
install(TARGETS network_simulator
    EXPORT network_simulator_targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)

install(DIRECTORY include/
    DESTINATION include
    FILES_MATCHING PATTERN "*.hpp"
)

install(EXPORT network_simulator_targets
    FILE network_simulator_targets.cmake
    NAMESPACE network_simulator::
    DESTINATION lib/cmake/network_simulator
)
# Thread safety test was removed after verification

# Coverage targets — only available when configured with -DENABLE_COVERAGE=ON
if(ENABLE_COVERAGE)
    include(ProcessorCount)
    ProcessorCount(PROCESSOR_COUNT)
    if(PROCESSOR_COUNT EQUAL 0)
        set(PROCESSOR_COUNT 4)
    endif()

    find_program(LLVM_PROFDATA NAMES llvm-profdata-18 llvm-profdata
        HINTS /usr/bin /usr/local/bin
    )
    find_program(LLVM_COV NAMES llvm-cov-18 llvm-cov
        HINTS /usr/bin /usr/local/bin
    )

    if(NOT LLVM_PROFDATA OR NOT LLVM_COV)
        message(WARNING "llvm-profdata/llvm-cov not found — coverage targets unavailable. "
                        "Install with: apt-get install llvm")
        foreach(_t coverage coverage-html coverage-reset)
            add_custom_target(${_t}
                COMMAND ${CMAKE_COMMAND} -E echo
                    "ERROR: llvm-profdata/llvm-cov not found"
                COMMAND ${CMAKE_COMMAND} -E false
            )
        endforeach()
    else()
        set(COVERAGE_REPORT   "${CMAKE_BINARY_DIR}/coverage-report")
        set(COVERAGE_PROFDATA "${CMAKE_BINARY_DIR}/merged.profdata")
        set(LLVM_COVERAGE_SCRIPT "${CMAKE_SOURCE_DIR}/cmake/llvm_coverage.cmake")

        # Semicolon-separated ignore list passed to cmake/llvm_coverage.cmake.
        set(_IGNORE_LIST
            ".*/build-coverage/.*"
            ".*/vcpkg_installed/.*"
            ".*/usr/.*"
            ".*/cmd/.*"
            ".*/tests/docker_chaos/.*"
            ".*/examples/.*"
        )
        string(JOIN ";" _IGNORE_JOINED ${_IGNORE_LIST})

        add_custom_target(coverage-reset
            COMMAND ${CMAKE_COMMAND} -E echo "Resetting coverage counters..."
            COMMAND find "${CMAKE_BINARY_DIR}" -name "*.profraw" -delete
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
            COMMENT "Resetting coverage counters"
        )

        add_custom_target(coverage
            COMMAND find "${CMAKE_BINARY_DIR}" -name "*.profraw" -delete
            COMMAND ${CMAKE_COMMAND} -E env
                    "LLVM_PROFILE_FILE=${CMAKE_BINARY_DIR}/%p-%m.profraw"
                    ${CMAKE_CTEST_COMMAND} -j${PROCESSOR_COUNT} --output-on-failure
            COMMAND ${CMAKE_COMMAND}
                    -DBUILD_DIR=${CMAKE_BINARY_DIR}
                    -DLLVM_PROFDATA=${LLVM_PROFDATA}
                    -DLLVM_COV=${LLVM_COV}
                    -DPROFDATA=${COVERAGE_PROFDATA}
                    -DMODE=report
                    "-DIGNORE_LIST=${_IGNORE_JOINED}"
                    -P "${LLVM_COVERAGE_SCRIPT}"
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
            COMMENT "Source-based coverage report"
        )

        add_custom_target(coverage-html
            COMMAND find "${CMAKE_BINARY_DIR}" -name "*.profraw" -delete
            COMMAND ${CMAKE_COMMAND} -E env
                    "LLVM_PROFILE_FILE=${CMAKE_BINARY_DIR}/%p-%m.profraw"
                    ${CMAKE_CTEST_COMMAND} -j${PROCESSOR_COUNT} --output-on-failure
            COMMAND ${CMAKE_COMMAND}
                    -DBUILD_DIR=${CMAKE_BINARY_DIR}
                    -DLLVM_PROFDATA=${LLVM_PROFDATA}
                    -DLLVM_COV=${LLVM_COV}
                    -DPROFDATA=${COVERAGE_PROFDATA}
                    -DMODE=html
                    -DHTML_DIR=${COVERAGE_REPORT}
                    "-DIGNORE_LIST=${_IGNORE_JOINED}"
                    -P "${LLVM_COVERAGE_SCRIPT}"
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
            COMMENT "Source-based HTML coverage report"
        )
    endif()
endif()

# ── clang-format targets ──────────────────────────────────────────────────────
find_program(CLANG_FORMAT_EXECUTABLE clang-format
    HINTS "$ENV{HOME}/.local/bin" /usr/local/bin /usr/bin
)

file(GLOB_RECURSE FORMAT_SOURCES
    "${CMAKE_SOURCE_DIR}/src/*.cpp"      "${CMAKE_SOURCE_DIR}/src/*.hpp"
    "${CMAKE_SOURCE_DIR}/include/*.cpp"  "${CMAKE_SOURCE_DIR}/include/*.hpp"
    "${CMAKE_SOURCE_DIR}/tests/*.cpp"    "${CMAKE_SOURCE_DIR}/tests/*.hpp"
    "${CMAKE_SOURCE_DIR}/examples/*.cpp" "${CMAKE_SOURCE_DIR}/examples/*.hpp"
)
list(LENGTH FORMAT_SOURCES FORMAT_SOURCES_COUNT)

if(NOT CLANG_FORMAT_EXECUTABLE)
    message(WARNING "clang-format not found — 'format' and 'format-check' targets unavailable. "
                    "Install with: pip3 install clang-format  or  apt install clang-format")
    foreach(_t format format-check)
        add_custom_target(${_t}
            COMMAND ${CMAKE_COMMAND} -E echo "ERROR: clang-format not found."
            COMMAND ${CMAKE_COMMAND} -E false
        )
    endforeach()
else()
    add_custom_target(format
        COMMAND ${CLANG_FORMAT_EXECUTABLE} -i ${FORMAT_SOURCES}
        COMMAND ${CMAKE_COMMAND} -E echo
            "format: reformatted ${FORMAT_SOURCES_COUNT} files"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Reformatting C++ sources with clang-format"
    )

    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT_EXECUTABLE} --dry-run --Werror ${FORMAT_SOURCES}
        COMMAND ${CMAKE_COMMAND} -E echo
            "format-check: all ${FORMAT_SOURCES_COUNT} files OK"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Checking C++ formatting with clang-format"
    )
endif()

# ── clang-tidy targets ────────────────────────────────────────────────────────
find_program(CLANG_TIDY_EXECUTABLE clang-tidy
    HINTS "$ENV{HOME}/.local/bin" /usr/local/bin /usr/bin
)
find_program(RUN_CLANG_TIDY_EXECUTABLE
    NAMES run-clang-tidy run-clang-tidy-18 run-clang-tidy.py run-clang-tidy-18.py
    HINTS /usr/bin /usr/lib/llvm-18/bin "$ENV{HOME}/.local/bin"
)

# .cpp files only — headers are checked transitively through their TUs
file(GLOB_RECURSE TIDY_SOURCES
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/tests/*.cpp"
    "${CMAKE_SOURCE_DIR}/examples/*.cpp"
)
list(LENGTH TIDY_SOURCES TIDY_SOURCES_COUNT)

if(NOT CLANG_TIDY_EXECUTABLE)
    message(WARNING "clang-tidy not found — 'static-analysis' and 'static-analysis-fix' targets "
                    "unavailable.  Install with: apt install clang-tidy")
    foreach(_t static-analysis static-analysis-fix)
        add_custom_target(${_t}
            COMMAND ${CMAKE_COMMAND} -E echo "ERROR: clang-tidy not found."
            COMMAND ${CMAKE_COMMAND} -E false
        )
    endforeach()
else()
    # Verify compile_commands.json exists before defining targets that need it
    set(_COMPDB "${CMAKE_BINARY_DIR}/compile_commands.json")

    if(RUN_CLANG_TIDY_EXECUTABLE)
        # Parallel path via run-clang-tidy
        if(CMAKE_BUILD_PARALLEL_LEVEL)
            set(_TIDY_JOBS ${CMAKE_BUILD_PARALLEL_LEVEL})
        else()
            cmake_host_system_information(RESULT _TIDY_JOBS QUERY NUMBER_OF_LOGICAL_CORES)
        endif()

        add_custom_target(static-analysis
            COMMAND ${CMAKE_COMMAND} -E cmake_echo_color --cyan
                "static-analysis: running clang-tidy on ${TIDY_SOURCES_COUNT} files, parallel"
            COMMAND ${CMAKE_COMMAND} -DFILE=${_COMPDB}
                    -P ${CMAKE_SOURCE_DIR}/cmake/check_compdb.cmake
            COMMAND ${RUN_CLANG_TIDY_EXECUTABLE}
                    -p ${CMAKE_BINARY_DIR}
                    -j ${_TIDY_JOBS}
                    ${TIDY_SOURCES}
            COMMAND ${CMAKE_COMMAND} -E echo
                "static-analysis: all ${TIDY_SOURCES_COUNT} files clean"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Running clang-tidy static analysis"
        )

        add_custom_target(static-analysis-fix
            COMMAND ${CMAKE_COMMAND} -E cmake_echo_color --cyan
                "static-analysis-fix: applying clang-tidy fixes on ${TIDY_SOURCES_COUNT} files"
            COMMAND ${CMAKE_COMMAND} -DFILE=${_COMPDB}
                    -P ${CMAKE_SOURCE_DIR}/cmake/check_compdb.cmake
            COMMAND ${RUN_CLANG_TIDY_EXECUTABLE}
                    -p ${CMAKE_BINARY_DIR}
                    -fix
                    ${TIDY_SOURCES}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Applying clang-tidy auto-fixes"
        )
    else()
        # Sequential fallback — run-clang-tidy not found
        add_custom_target(static-analysis
            COMMAND ${CMAKE_COMMAND} -E cmake_echo_color --cyan
                "static-analysis: running clang-tidy on ${TIDY_SOURCES_COUNT} files, sequential"
            COMMAND ${CMAKE_COMMAND} -DFILE=${_COMPDB}
                    -P ${CMAKE_SOURCE_DIR}/cmake/check_compdb.cmake
            COMMAND ${CLANG_TIDY_EXECUTABLE} -p ${CMAKE_BINARY_DIR} ${TIDY_SOURCES}
            COMMAND ${CMAKE_COMMAND} -E echo
                "static-analysis: all ${TIDY_SOURCES_COUNT} files clean"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Running clang-tidy static analysis, sequential"
        )

        add_custom_target(static-analysis-fix
            COMMAND ${CMAKE_COMMAND} -E cmake_echo_color --cyan
                "static-analysis-fix: applying clang-tidy fixes on ${TIDY_SOURCES_COUNT} files"
            COMMAND ${CMAKE_COMMAND} -DFILE=${_COMPDB}
                    -P ${CMAKE_SOURCE_DIR}/cmake/check_compdb.cmake
            COMMAND ${CLANG_TIDY_EXECUTABLE} -p ${CMAKE_BINARY_DIR} --fix --fix-errors
                    ${TIDY_SOURCES}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Applying clang-tidy auto-fixes, sequential"
        )
    endif()
endif()

# ── Doxygen documentation target ─────────────────────────────────────────────
find_package(Doxygen QUIET)
if(DOXYGEN_FOUND)
    message(STATUS "Doxygen ${DOXYGEN_VERSION} found — 'docs' target available")
    add_custom_target(docs
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Generating API documentation with Doxygen"
        VERBATIM
    )
else()
    message(STATUS "Doxygen not found — 'docs' target unavailable. "
                   "Install with: apt install doxygen")
    add_custom_target(docs
        COMMAND ${CMAKE_COMMAND} -E echo "ERROR: doxygen not found."
        COMMAND ${CMAKE_COMMAND} -E false
    )
endif()
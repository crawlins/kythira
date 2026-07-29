cmake_minimum_required(VERSION 3.20)
project(network_simulator VERSION 0.1.0 LANGUAGES CXX)

# Kconfig integration: resolves a .config into KCONFIG_<NAME> CMake variables
# (used below to gate optional find_package() calls) and a generated C++
# header. Must run before any find_package() call in this file. See
# cmake/Kconfig.cmake and .kiro/specs/kconfig-integration/ for the design.
include(cmake/Kconfig.cmake)

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

# This project configures vcpkg's manifest-mode output via CMAKE_PREFIX_PATH
# rather than vcpkg's own scripts/buildsystems/vcpkg.cmake toolchain file
# (every dependency up through Beast resolved fine that way). The
# proxygen-http-transport spec's first-time-needed transitive dependency
# (unofficial-sodium, pulled in by fizz/mvfst) is the first vcpkg port in
# this project's tree whose generated *-config.cmake references
# _VCPKG_INSTALLED_DIR/VCPKG_TARGET_TRIPLET directly (most ports instead
# derive their own install prefix relative to the config file's own
# location, which needs neither variable) -- those two variables are only
# ever defined by vcpkg's toolchain file, so they're empty here, collapsing
# unofficial-sodium's INTERFACE_INCLUDE_DIRECTORIES to a literal "//include"
# and failing CMake's imported-target path-existence check at generate time.
# Defining both here (only if a caller hasn't already, e.g. via the real
# toolchain file) is a minimal, general fix for this whole class of vcpkg
# port -- not specific to proxygen or unofficial-sodium -- without adopting
# the toolchain file project-wide.
if(NOT DEFINED _VCPKG_INSTALLED_DIR)
    set(_VCPKG_INSTALLED_DIR "${CMAKE_SOURCE_DIR}/vcpkg_installed" CACHE PATH
        "vcpkg manifest-mode install root, for port *-config.cmake scripts that need it directly")
endif()
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "${KYTHIRA_VCPKG_TRIPLET}" CACHE STRING
        "vcpkg triplet, for port *-config.cmake scripts that need it directly")
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

# Code coverage support. CONFIG_COVERAGE (cmake/Kconfig.cmake) pre-seeds this
# cache entry when Kconfig is engaged; set(... CACHE ...) without FORCE only
# takes effect if the cache entry doesn't already exist, so an explicit
# -DENABLE_COVERAGE=ON on the command line (legacy, no Kconfig involved)
# always wins over KCONFIG_COVERAGE, and the option() call below is always a
# no-op once either has run.
if(DEFINED KCONFIG_COVERAGE)
    set(ENABLE_COVERAGE ${KCONFIG_COVERAGE} CACHE BOOL "Build with gcov coverage instrumentation")
endif()
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
# Boost is a hard-required core dependency, outside Kconfig's control
# entirely -- it is not modeled as a CONFIG symbol and always runs
# unconditionally. Folly, by contrast, is CONFIG_FOLLY-gated like any other
# optional dependency in the "Futures" menu (Kconfig): every other backend
# it coexists alongside (stdexec, boost) is gated the same way, and
# CONFIG_FOLLY/CONFIG_STDEXEC_BACKEND/CONFIG_BOOST_FUTURE_BACKEND together
# enforce that at least one backend stays selected (see the Kconfig
# "comment" beneath them, and the hard configure-time check below). Note
# that deselecting CONFIG_FOLLY only stops CMake from probing for it --
# include/raft/future.hpp is still included unconditionally by
# include/raft/future_default.hpp regardless of KYTHIRA_DEFAULT_FUTURE_
# BACKEND, so most of the codebase (certificate_authority, both
# transports, most of the test suite) still requires Folly transitively
# today even with a non-folly default backend selected; see doc/TODO.md's
# "Known Follow-ups" for the header-level decoupling this would still need.
kythira_find_optional(FOLLY folly)
find_package(Boost QUIET COMPONENTS system thread unit_test_framework json context beast)
kythira_find_optional(HTTP_TRANSPORT httplib)
kythira_find_optional(OPENSSL OpenSSL)

# Find libcoap using CMake config (vcpkg) or PkgConfig (system)
kythira_kconfig_gate(COAP_TRANSPORT)
if(_KYTHIRA_GATE_COAP_TRANSPORT)
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
endif()
kythira_kconfig_require(COAP_TRANSPORT LIBCOAP_FOUND "libcoap")

# Find libldns using PkgConfig (system installation; pc file is named "ldns")
kythira_kconfig_gate(DNS_DISCOVERY)
if(_KYTHIRA_GATE_DNS_DISCOVERY)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBLDNS QUIET ldns)
    endif()
endif()
kythira_kconfig_require(DNS_DISCOVERY LIBLDNS_FOUND "libldns")

# EDHOC support (coap-transport-security spec, Requirement 5) — optional;
# only present when vcpkg installed with the "edhoc" feature enabled (it
# requires a Rust toolchain to build from source; see
# vcpkg-overlays/lakers/README.md). Absent by default, in which case
# LAKERS_AVAILABLE stays undefined and oscore_bootstrap::edhoc fails
# construction with a descriptive error instead of silently degrading.
kythira_find_optional(EDHOC lakers CONFIG)

# stdexec-future-backend spec: a second, sender/receiver-based Future/
# Promise/Executor implementation alongside the default Folly one. A
# default (non-feature-gated) dependency, same as folly — see
# vcpkg-overlays/stdexec/README.md for why it's an overlay port.
find_package(stdexec CONFIG QUIET)

# boost-beast-http-transport spec: a second network_client/network_server
# implementation alongside the existing cpp-httplib-backed one
# (include/raft/http_transport.hpp). boost-beast is a default (unconditional,
# not Kconfig-gated) vcpkg dependency, same as stdexec above — it is
# header-only and low-cost, so there is no reason to make vcpkg itself skip
# installing it the way EDHOC's Rust-toolchain-requiring lakers dependency
# is skipped via a real vcpkg "feature" (COMPONENTS beast is always
# requested as part of the single unified Boost find_package() call,
# above, the same way COMPONENTS json already is). Requirement 17.2 calls
# for gating this feature's own targets/tests through
# kythira_kconfig_gate/kythira_kconfig_require -- the same machinery every
# other optional dependency in this project uses -- rather than a bespoke
# option()/CACHE BOOL check (unlike KYTHIRA_BUILD_BOOST_FUTURE_BACKEND
# below, which predates this convention).
kythira_kconfig_gate(BOOST_BEAST_TRANSPORT)
set(KYTHIRA_BUILD_BOOST_BEAST_TRANSPORT FALSE)
if(_KYTHIRA_GATE_BOOST_BEAST_TRANSPORT AND TARGET Boost::beast)
    set(KYTHIRA_BUILD_BOOST_BEAST_TRANSPORT TRUE)
    message(STATUS "boost-beast HTTP transport enabled (.kiro/specs/boost-beast-http-transport/)")
else()
    message(STATUS "boost-beast HTTP transport disabled (Boost::beast not found, or CONFIG_BOOST_BEAST_TRANSPORT=n)")
endif()
kythira_kconfig_require(BOOST_BEAST_TRANSPORT KYTHIRA_BUILD_BOOST_BEAST_TRANSPORT "Boost::beast")

# proxygen-http-transport spec: a third network_client/network_server
# implementation, backed by Meta's Proxygen (itself built directly on
# Folly's EventBase/AsyncSocket). Unlike boost-beast above, proxygen is a
# genuinely optional vcpkg dependency (Requirement 20.1) -- it unconditionally
# depends on folly, which is itself CONFIG_FOLLY-gated in this project
# (doc/TODO.md's Folly-decoupling entries), so enabling this feature
# requires both `TARGET proxygen::proxygen` *and* `TARGET Folly::folly` to
# be present, not either alone (Requirement 20.4). See
# .kiro/specs/proxygen-http-transport/ for the full design.
kythira_find_optional(PROXYGEN_TRANSPORT proxygen CONFIG)

kythira_kconfig_gate(PROXYGEN_TRANSPORT)
set(KYTHIRA_BUILD_PROXYGEN_TRANSPORT FALSE)
if(_KYTHIRA_GATE_PROXYGEN_TRANSPORT AND TARGET proxygen::proxygen AND TARGET Folly::folly)
    set(KYTHIRA_BUILD_PROXYGEN_TRANSPORT TRUE)
    message(STATUS "Proxygen HTTP transport enabled (.kiro/specs/proxygen-http-transport/)")
else()
    message(STATUS "Proxygen HTTP transport disabled "
                    "(proxygen and/or Folly not found, or CONFIG_PROXYGEN_TRANSPORT=n)")
endif()
kythira_kconfig_require(PROXYGEN_TRANSPORT KYTHIRA_BUILD_PROXYGEN_TRANSPORT "proxygen::proxygen")

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
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>
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

# CONFIG_DEFAULT_FUTURE_BACKEND_{FOLLY,STDEXEC,BOOST} (Kconfig `choice`) pre-
# seeds KYTHIRA_DEFAULT_FUTURE_BACKEND's cache entry below the same way
# CONFIG_COVERAGE pre-seeds ENABLE_COVERAGE — before it's used (the boost
# branch below reads it), so it must run ahead of both this block and the
# set(... CACHE STRING ...) declaration further down. An explicit
# -DKYTHIRA_DEFAULT_FUTURE_BACKEND=... on the command line always wins,
# since that already populates the cache before this file starts executing.
if(DEFINED KCONFIG_DEFAULT_FUTURE_BACKEND_STDEXEC AND KCONFIG_DEFAULT_FUTURE_BACKEND_STDEXEC)
    set(KYTHIRA_DEFAULT_FUTURE_BACKEND "stdexec" CACHE STRING
        "Backend kythira::future_default<T> resolves to: folly (default), stdexec, or boost")
elseif(DEFINED KCONFIG_DEFAULT_FUTURE_BACKEND_BOOST AND KCONFIG_DEFAULT_FUTURE_BACKEND_BOOST)
    set(KYTHIRA_DEFAULT_FUTURE_BACKEND "boost" CACHE STRING
        "Backend kythira::future_default<T> resolves to: folly (default), stdexec, or boost")
elseif(DEFINED KCONFIG_DEFAULT_FUTURE_BACKEND_FOLLY)
    set(KYTHIRA_DEFAULT_FUTURE_BACKEND "folly" CACHE STRING
        "Backend kythira::future_default<T> resolves to: folly (default), stdexec, or boost")
endif()

# boost-future-backend spec: a third Future/Promise/Executor
# implementation, backed by boost::thread's extended future API (then()/
# when_all/when_any). boost-thread and boost-asio are already required
# (non-optional) dependencies of this project for unrelated reasons - this
# option only controls whether THIS backend's own targets/headers are
# built, not whether Boost itself is available. CONFIG_BOOST_FUTURE_BACKEND
# pre-seeds the cache entry the same way CONFIG_COVERAGE pre-seeds
# ENABLE_COVERAGE, above.
if(DEFINED KCONFIG_BOOST_FUTURE_BACKEND)
    set(KYTHIRA_BUILD_BOOST_FUTURE_BACKEND ${KCONFIG_BOOST_FUTURE_BACKEND} CACHE BOOL
        "Build the boost::thread-backed Future/Promise/Executor backend")
endif()
option(KYTHIRA_BUILD_BOOST_FUTURE_BACKEND "Build the boost::thread-backed Future/Promise/Executor backend" OFF)

# BOOST_THREAD_VERSION is a project-wide ABI setting, not a per-file flag:
# Boost.Thread's future/promise templates compile differently depending on
# this macro's value at each translation unit's compile time, and mixing
# TUs compiled with different values that pass the same boost::future/
# boost::promise object across a TU boundary is undefined behavior. Applied
# once here as an INTERFACE compile definition on network_simulator - the
# same mechanism KYTHIRA_HAS_STDEXEC already uses - so every consumer gets
# the identical value automatically. BOOST_THREAD_PROVIDES_EXECUTORS is
# defined explicitly rather than relied upon as an auto-definition: the
# Phase 0 spike (.kiro/specs/boost-future-backend/spike-notes.md) found it
# is only auto-defined at BOOST_THREAD_VERSION>=5, not >=4 as originally
# assumed from source inspection alone.
if(KYTHIRA_BUILD_BOOST_FUTURE_BACKEND OR KYTHIRA_DEFAULT_FUTURE_BACKEND STREQUAL "boost")
    if(NOT Boost_FOUND)
        message(FATAL_ERROR "KYTHIRA_BUILD_BOOST_FUTURE_BACKEND requires Boost, which was not found")
    endif()
    target_compile_definitions(network_simulator INTERFACE
        BOOST_THREAD_VERSION=4
        BOOST_THREAD_PROVIDES_EXECUTORS
        KYTHIRA_HAS_BOOST_FUTURE
    )
    target_link_libraries(network_simulator INTERFACE Boost::thread)
    message(STATUS "boost future backend enabled (BOOST_THREAD_VERSION=4)")
endif()

# Requirement 11: which backend kythira::future_default<T> (the non-generic
# call sites that just want "the project's chosen future implementation"
# without spelling out a backend) resolves to. Templated core code that is
# already generic over a future type never references future_default and
# is unaffected by this option either way (Requirement 11.2/11.5).
set(KYTHIRA_DEFAULT_FUTURE_BACKEND "folly" CACHE STRING
    "Backend kythira::future_default<T> resolves to: folly (default), stdexec, or boost")
set_property(CACHE KYTHIRA_DEFAULT_FUTURE_BACKEND PROPERTY STRINGS folly stdexec boost)
if(NOT KYTHIRA_DEFAULT_FUTURE_BACKEND STREQUAL "folly"
   AND NOT KYTHIRA_DEFAULT_FUTURE_BACKEND STREQUAL "stdexec"
   AND NOT KYTHIRA_DEFAULT_FUTURE_BACKEND STREQUAL "boost")
    message(FATAL_ERROR "KYTHIRA_DEFAULT_FUTURE_BACKEND must be \"folly\", \"stdexec\", or \"boost\", got \"${KYTHIRA_DEFAULT_FUTURE_BACKEND}\"")
endif()
if(KYTHIRA_DEFAULT_FUTURE_BACKEND STREQUAL "stdexec")
    if(NOT stdexec_FOUND)
        message(FATAL_ERROR "KYTHIRA_DEFAULT_FUTURE_BACKEND=stdexec requires the stdexec dependency, which was not found")
    endif()
    target_compile_definitions(network_simulator INTERFACE KYTHIRA_FUTURE_BACKEND_STDEXEC)
elseif(KYTHIRA_DEFAULT_FUTURE_BACKEND STREQUAL "boost")
    target_compile_definitions(network_simulator INTERFACE KYTHIRA_FUTURE_BACKEND_BOOST)
endif()

# Whether the actually-selected future backend's dependency is satisfied --
# not "is folly found", which is the wrong question once a non-folly
# backend is selected. stdexec/boost already fail configure above (via
# FATAL_ERROR) if their own dependency is missing while selected, so by
# the time this is evaluated, the only remaining gap is folly selected
# without being found. Gates below use this instead of folly_FOUND
# directly so certificate_authority/tests/examples/the discovery-node
# executables aren't skipped just because CONFIG_FOLLY=n, as long as
# whichever backend *is* selected actually works (see doc/TODO.md's
# Folly-decoupling gap 2 note).
if(folly_FOUND OR NOT KYTHIRA_DEFAULT_FUTURE_BACKEND STREQUAL "folly")
    set(KYTHIRA_FUTURE_BACKEND_AVAILABLE TRUE)
else()
    set(KYTHIRA_FUTURE_BACKEND_AVAILABLE FALSE)
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
kythira_kconfig_gate(HTTP_TRANSPORT_TLS)
if(httplib_FOUND AND OpenSSL_FOUND AND _KYTHIRA_GATE_HTTP_TRANSPORT_TLS)
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
kythira_kconfig_gate(POCO_DISCOVERY)
set(_poco_dnssd_lib     "${CMAKE_SOURCE_DIR}/vcpkg_installed/${KYTHIRA_VCPKG_TRIPLET}/lib/libPocoDNSSD.a")
set(_poco_dnssd_avahi   "${CMAKE_SOURCE_DIR}/vcpkg_installed/${KYTHIRA_VCPKG_TRIPLET}/lib/libPocoDNSSDAvahi.a")
set(_poco_dnssd_inc     "${CMAKE_SOURCE_DIR}/vcpkg_installed/${KYTHIRA_VCPKG_TRIPLET}/include")
if(_KYTHIRA_GATE_POCO_DISCOVERY)
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
endif() # _KYTHIRA_GATE_POCO_DISCOVERY
kythira_kconfig_require(POCO_DISCOVERY POCO_DNSSD_FOUND "Poco DNSSD")

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
kythira_find_optional(AWS_SDK AWSSDK COMPONENTS core ec2 autoscaling iam s3 sts)
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
kythira_kconfig_gate(AWS_ACM_PCA)
if(_KYTHIRA_GATE_AWS_ACM_PCA)
    unset(AWSSDK_FOUND)
    find_package(AWSSDK QUIET COMPONENTS acm-pca)
endif()
if(TARGET aws-cpp-sdk-acm-pca)
    set(_KYTHIRA_AWS_ACM_PCA_FOUND TRUE)
else()
    set(_KYTHIRA_AWS_ACM_PCA_FOUND FALSE)
endif()
kythira_kconfig_require(AWS_ACM_PCA _KYTHIRA_AWS_ACM_PCA_FOUND "AWS ACM Private CA SDK component")
if(TARGET aws-cpp-sdk-acm-pca)
    message(STATUS "AWS ACM Private CA SDK component found")
    target_compile_definitions(network_simulator INTERFACE KYTHIRA_HAS_AWS_ACM_PCA)
    target_link_libraries(network_simulator INTERFACE aws-cpp-sdk-acm-pca)
else()
    message(STATUS "AWS ACM Private CA SDK component not found — aws_acm_pca_provider disabled")
endif()

# ── certificate_authority (CA testing/provisioning framework) ────────────────
# KYTHIRA_FUTURE_BACKEND_AVAILABLE, not just TARGET OpenSSL::SSL:
# certificate_provider.hpp's kythira::future_default usage still needs
# *some* future backend's dependency satisfied, same as everything else
# that transitively includes future_default.hpp. This used to be
# folly_FOUND specifically, back when future_default.hpp unconditionally
# pulled in Folly regardless of KYTHIRA_DEFAULT_FUTURE_BACKEND (see
# doc/TODO.md's Folly-decoupling entries) — that's fixed now, so this
# only needs *a* backend, not folly by name.
if(TARGET OpenSSL::SSL AND KYTHIRA_FUTURE_BACKEND_AVAILABLE)
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
kythira_kconfig_gate(LIBSSH2_TESTS)
if(_KYTHIRA_GATE_LIBSSH2_TESTS)
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
endif()
kythira_kconfig_require(LIBSSH2_TESTS LIBSSH2_FOUND "libssh2")
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
kythira_kconfig_gate(CHAOS_TESTS)
if(_KYTHIRA_GATE_CHAOS_TESTS)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FIU QUIET libfiu)
    endif()
endif()
kythira_kconfig_require(CHAOS_TESTS FIU_FOUND "libfiu")

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
        if(KYTHIRA_FUTURE_BACKEND_AVAILABLE AND Boost_FOUND AND httplib_FOUND)
            add_subdirectory(cmd/ca_cluster_node)
        endif()
    endif()
endif()
# KYTHIRA_FUTURE_BACKEND_AVAILABLE, not folly_FOUND: with a non-folly
# backend selected and its own dependency present, the overwhelming
# majority of this project's registered tests build and pass fine --
# every kythira::future_default use now resolves to whichever backend
# was actually selected (see doc/TODO.md's Folly-decoupling entries).
# A real, still-substantial minority remain genuinely Folly-only by
# design (HTTP/CoAP transport tests -- http_transport.hpp/
# coap_transport.hpp hardcode folly::Future<T> themselves, unlike the
# RPC transports -- Folly concept-wrapper tests, and the cross-backend
# benchmark comparisons); those are individually gated on
# `if(TARGET Folly::folly)` within tests/CMakeLists.txt itself rather
# than blocking the whole subdirectory the way this guard used to.
if(Boost_FOUND AND KYTHIRA_FUTURE_BACKEND_AVAILABLE)
    add_subdirectory(tests)
elseif(Boost_FOUND)
    message(STATUS "folly not found and no alternate future backend selected — tests/ disabled")
endif()
if(NOT ENABLE_COVERAGE)
    if(KYTHIRA_FUTURE_BACKEND_AVAILABLE AND Boost_FOUND AND httplib_FOUND)
        add_subdirectory(cmd/chaos_node)
        add_subdirectory(cmd/poco_discovery_node)
    endif()
    if(LIBLDNS_FOUND AND KYTHIRA_FUTURE_BACKEND_AVAILABLE AND httplib_FOUND)
        add_subdirectory(cmd/dns_discovery_node)
        add_subdirectory(cmd/dns_sd_discovery_node)
    endif()
endif()
# folly_FOUND, not KYTHIRA_FUTURE_BACKEND_AVAILABLE: unlike tests/ and
# certificate_authority, every example program calls folly::init()/
# constructs folly::Init directly in its own main() to demonstrate real
# Folly usage -- that's the point of an "example". Deliberately left
# Folly-only rather than extended the gap-1-style backend-conditional
# treatment used for tests/ (see doc/TODO.md's Folly-decoupling gap 2
# note); examples/ isn't part of the test suite CONFIG_FOLLY=n needs to
# keep working.
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

# ── Kconfig targets ───────────────────────────────────────────────────────────
# menuconfig/guiconfig/savedefconfig/kconfig-check follow the same
# stub-target-when-tool-missing pattern as format/format-check and
# static-analysis/static-analysis-fix below: `cmake --build build --target
# help` always lists them, but invoking one without python3/kconfiglib
# available prints an actionable install hint and fails only that target.
if(_KYTHIRA_KCONFIG_OK)
    add_custom_target(menuconfig
        COMMAND ${CMAKE_COMMAND} -E env "KCONFIG_CONFIG=${CMAKE_BINARY_DIR}/.config"
                ${Python3_EXECUTABLE} -m menuconfig "${CMAKE_SOURCE_DIR}/Kconfig"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Interactive Kconfig menu -- writes ${CMAKE_BINARY_DIR}/.config"
        USES_TERMINAL
        VERBATIM
    )
    add_custom_target(guiconfig
        COMMAND ${CMAKE_COMMAND} -E env "KCONFIG_CONFIG=${CMAKE_BINARY_DIR}/.config"
                ${Python3_EXECUTABLE} -m guiconfig "${CMAKE_SOURCE_DIR}/Kconfig"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Tkinter Kconfig menu -- writes ${CMAKE_BINARY_DIR}/.config"
        USES_TERMINAL
        VERBATIM
    )
    add_custom_target(savedefconfig
        COMMAND ${CMAKE_COMMAND} -E env "KCONFIG_CONFIG=${CMAKE_BINARY_DIR}/.config"
                ${Python3_EXECUTABLE} -m savedefconfig
                --kconfig "${CMAKE_SOURCE_DIR}/Kconfig"
                --out "${CMAKE_BINARY_DIR}/defconfig"
        COMMAND ${CMAKE_COMMAND} -E echo
            "savedefconfig: wrote ${CMAKE_BINARY_DIR}/defconfig -- cp it into configs/<name>_defconfig to check it in"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Writing minimal defconfig from ${CMAKE_BINARY_DIR}/.config"
        VERBATIM
    )
    add_custom_target(kconfig-check
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/scripts/kconfig/check_defconfigs.py"
                "${CMAKE_SOURCE_DIR}/Kconfig" "${CMAKE_SOURCE_DIR}/configs"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Validating every configs/*_defconfig against Kconfig"
        VERBATIM
    )
else()
    foreach(_t menuconfig guiconfig savedefconfig kconfig-check)
        add_custom_target(${_t}
            COMMAND ${CMAKE_COMMAND} -E echo
                "ERROR: python3/kconfiglib not found. Install with: pip install -r scripts/kconfig/requirements.txt"
            COMMAND ${CMAKE_COMMAND} -E false
        )
    endforeach()
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
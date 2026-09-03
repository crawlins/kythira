# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

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

# clang++/aarch64 + GCC's libstdc++ <ext/opt_random.h> incompatibility:
# folly/Random.h (pulled in transitively by proxygen_http_transport.hpp via
# proxygen -> wangle -> folly, none of which any pre-existing target in this
# project included before .kiro/specs/proxygen-http-transport/) includes
# <ext/random>, which on aarch64 conditionally includes libstdc++'s own
# <ext/opt_random.h> under `#if __ARM_NEON`. That header's NEON fast path
# uses GCC's `__builtin_shuffle`, which clang does not implement at all, so
# clang cannot compile it under that branch. Confirmed directly from a real
# CI failure (Build & Test (clang++-18, arm64)): g++-13/aarch64 (GCC
# compiling GCC's own header) and clang++-18/x64 (__ARM_NEON never defined,
# not an ARM target) both build the identical header chain cleanly --
# clang+aarch64 is the one combination where both conditions needed to
# trigger this hold simultaneously.
#
# A first fix (globally passing -U__ARM_NEON for this compiler+arch) made
# the build succeed but introduced a real, measured ABI mismatch: it also
# flips every other `#ifdef __ARM_NEON` branch in folly/proxygen's own
# headers for the *whole* translation unit, which changes the computed
# layout of unrelated types -- e.g. proxygen::HTTPMessage came out as 608
# bytes under that workaround vs. 576 bytes under both GCC and a plain
# clang build. Since the vcpkg-built proxygen/folly static libraries are
# always GCC-compiled (vcpkg does not key its build on the consuming
# project's compiler), that 32-byte mismatch meant clang-compiled code
# stack-allocating a proxygen::HTTPMessage disagreed with GCC-compiled
# proxygen internals about its own member offsets -- the actual root cause
# of a proxygen HTTPHeaders segfault that only reproduced on clang+aarch64,
# root-caused via a sizeof(HTTPMessage) probe compiled with both compilers
# on real ARM64 hardware.
#
# Fix: force-include arm64_clang_ext_random_compat.hpp first in every TU
# instead. It pre-processes <ext/random> with __ARM_NEON locally undefined
# only for that one system header; the header's own include guard then
# makes every later #include <ext/random> (transitively, via
# folly/Random.h) a no-op, so __ARM_NEON stays defined -- matching GCC --
# for every other header in the translation unit. Confirmed on real ARM64
# hardware to restore sizeof(HTTPMessage) to 576 while still compiling
# cleanly under clang.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-include "${CMAKE_SOURCE_DIR}/include/raft/arm64_clang_ext_random_compat.hpp")
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

# Sanitizer support. This project's own AddressSanitizer runs during the
# Boost.Beast and Proxygen transport specs' development
# (`.kiro/specs/boost-beast-http-transport/`/`.kiro/specs/proxygen-http-transport/`
# design.md/tasks.md, `doc/CHANGELOG.md`'s July 28, 2026 entry) were all
# ad hoc (`g++-13 -std=c++23 -fsanitize=address`, run and discarded, never
# a registered build configuration) — both specs' tasks.md flagged a
# registered, repeatable sanitizer build as their own last open follow-up.
# This is the first sanitizer-support CMake option this project has,
# following `ENABLE_COVERAGE`'s own established shape immediately above
# (plain cache-variable-driven flags appended to
# `CMAKE_CXX_FLAGS`/`CMAKE_EXE_LINKER_FLAGS`) rather than introducing
# CMakePresets.json, which this project doesn't otherwise use anywhere.
set(KYTHIRA_SANITIZER "" CACHE STRING
    "Sanitizer to build with: empty (none, default), 'thread', or 'address'")
set_property(CACHE KYTHIRA_SANITIZER PROPERTY STRINGS "" thread address)

if(NOT KYTHIRA_SANITIZER STREQUAL "" AND ENABLE_COVERAGE)
    message(FATAL_ERROR "KYTHIRA_SANITIZER and ENABLE_COVERAGE are mutually exclusive "
                        "(both instrument every translation unit, and LLVM source-based "
                        "coverage's own instrumentation is not sanitizer-safe)")
endif()

if(KYTHIRA_SANITIZER STREQUAL "thread")
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
        message(STATUS "KYTHIRA_SANITIZER=thread: forcing CMAKE_BUILD_TYPE=Debug")
    endif()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=thread")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=thread")
    message(STATUS "ThreadSanitizer enabled (KYTHIRA_SANITIZER=thread)")
elseif(KYTHIRA_SANITIZER STREQUAL "address")
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
        message(STATUS "KYTHIRA_SANITIZER=address: forcing CMAKE_BUILD_TYPE=Debug")
    endif()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
    message(STATUS "AddressSanitizer enabled (KYTHIRA_SANITIZER=address)")
elseif(NOT KYTHIRA_SANITIZER STREQUAL "")
    message(FATAL_ERROR "KYTHIRA_SANITIZER must be empty, 'thread', or 'address', "
                        "got \"${KYTHIRA_SANITIZER}\"")
endif()

# vcpkg installs two copies of every static library -- a debug one built
# without NDEBUG and a release one built with it -- and CMake chooses between
# them per *imported configuration*. `RelWithDebInfo` and `MinSizeRel` have no
# imported configuration of their own, so with no mapping CMake falls back to
# whichever configuration an imported target happens to list first: DEBUG,
# because `<pkg>-targets-debug.cmake` sorts ahead of `-release.cmake` in the
# glob its `-config.cmake` uses. Our own translation units, meanwhile, get
# `-DNDEBUG` under both of those build types, from CMake's own
# `CMAKE_CXX_FLAGS_<CONFIG>` defaults.
#
# For folly and proxygen that is not a cosmetic difference. NDEBUG changes
# their class layouts: `sizeof(proxygen::HTTPSessionBase)` is 1624 with it and
# 1632 without, measured with this project's own include paths and defines. A
# RelWithDebInfo build therefore reads every field of a session the library
# itself constructed at an eight-byte-shifted offset, silently -- an ODR
# violation the linker cannot see, since both halves declare the same class.
#
# It is not hypothetical. `-DKYTHIRA_SANITIZER=address
# -DCMAKE_BUILD_TYPE=RelWithDebInfo` produced a build in which *every* Proxygen
# client transaction was refused: a freshly connected upstream session read back
# `getMaxConcurrentOutgoingStreams() == 0` and `getNumStreams() == 2839225156`
# where the same code in a Release build read 1 and 0. That cost three
# investigation sessions of `.kiro/specs/multi-raft-performance/` task 5, which
# attributed the resulting refusal storm to the transport.
#
# Same class of defect as the `__ARM_NEON` layout mismatch documented at the top
# of this file, different trigger. The trailing "" lets an imported target that
# publishes only a configuration-less location still resolve.
set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release "")
set(CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL Release "")

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
#
# Two symbols, not one. `COAP_TRANSPORT` decides whether the transport is built
# at all -- `KYTHIRA_BUILD_COAP_TRANSPORT` below carries that answer to every
# CoAP target, including the 45 test binaries in tests/CMakeLists.txt, which
# until August 10, 2026 were gated on `TARGET Folly::folly` and so were never
# connected to this symbol: setting it n produced a byte-identical build.
# `COAP_BACKEND_LIBCOAP` decides whether the libcoap wire implementation is one
# of the backends built into it, making libcoap symmetrical with
# COAP_TRANSPORT_LIBNYOCI and COAP_TRANSPORT_CANTCOAP below.
#
# The combination "transport on, every backend off" is deliberate and
# supported: it builds the backend-free surface. `LIBCOAP_FOUND` stays purely a
# statement about the library being present, which is what the
# `LIBCOAP_AVAILABLE` wiring further down keys on.
set(KYTHIRA_BUILD_COAP_TRANSPORT FALSE)
kythira_kconfig_gate(COAP_TRANSPORT)
if(_KYTHIRA_GATE_COAP_TRANSPORT)
    set(KYTHIRA_BUILD_COAP_TRANSPORT TRUE)
endif()

kythira_kconfig_gate(COAP_BACKEND_LIBCOAP)
if(_KYTHIRA_GATE_COAP_TRANSPORT AND _KYTHIRA_GATE_COAP_BACKEND_LIBCOAP)
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
        # vcpkg's CONFIG-mode package exports a proper imported target
        # (libcoap::coap-3), not the pkg-config-style LIBCOAP_LIBRARIES/
        # LIBCOAP_INCLUDE_DIRS/LIBCOAP_CFLAGS_OTHER variables the
        # pkg_check_modules() fallback above populates. Setting
        # LIBCOAP_LIBRARIES to the target name lets the single shared
        # target_link_libraries() call below work for both paths --
        # linking an imported target also pulls in its own
        # INTERFACE_INCLUDE_DIRECTORIES for free, so
        # LIBCOAP_INCLUDE_DIRS/LIBCOAP_CFLAGS_OTHER don't need setting
        # here. Without this, every CONFIG-mode (i.e. every vcpkg) build
        # silently linked nothing at all despite libcoap_FOUND being
        # true, and every #ifdef LIBCOAP_AVAILABLE code path in
        # coap_transport_impl.hpp compiled out to its stub branch
        # instead -- confirmed directly: no coap_*_test.cpp in this
        # project ever actually exercised the real libcoap-backed
        # implementation before this fix.
        set(LIBCOAP_LIBRARIES libcoap::coap-3)
    endif()
endif()
# The requirement belongs to the backend symbol, not the feature: under the
# split above, "transport on, libcoap off" is a legal configuration, so
# requiring libcoap whenever COAP_TRANSPORT=y would contradict it. Keying on
# COAP_BACKEND_LIBCOAP preserves what this line was actually for -- strict mode
# still fails fast if libcoap stops being installable in an image that asked
# for it.
kythira_kconfig_require(COAP_BACKEND_LIBCOAP LIBCOAP_FOUND "libcoap")

# coap-transport-libnyoci spec: libnyoci backs a *second* implementation of the
# same network_client/network_server concepts the libcoap-backed
# coap_client/coap_server satisfy (include/raft/coap_transport_libnyoci_impl.hpp).
# Structurally identical to the LIBCOAP_FOUND -> LIBCOAP_AVAILABLE wiring above,
# with three deliberate differences:
#
#  * Discovery is pkg-config only. libnyoci is autotools and ships no CMake
#    config package at all, so there is no find_package(... CONFIG) first
#    attempt -- only the pkg_check_modules path libcoap uses as its *fallback*.
#  * It defaults to OFF (CONFIG_COAP_TRANSPORT_LIBNYOCI=n, and the vcpkg
#    "coap-libnyoci" feature is opt-in), so a normal build neither fetches nor
#    links it -- Requirement 2.1.
#  * It does not depend on COAP_TRANSPORT. The two backends are additive and
#    coexist at link time (Requirement 2.4), verified by linking a program that
#    calls into both archives. The two export exactly one name in common --
#    coap_insert_option -- and that is survivable only because these are static
#    archives: the linker pulls an object file solely to satisfy an undefined
#    symbol, and no translation unit references both libraries (it cannot; their
#    headers are mutually exclusive, see coap_transport_config.hpp). If either
#    is ever built shared, or a third name starts overlapping, revisit this.
kythira_kconfig_gate(COAP_TRANSPORT_LIBNYOCI)
if(_KYTHIRA_GATE_COAP_TRANSPORT_LIBNYOCI)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBNYOCI QUIET libnyoci)
    endif()
endif()
kythira_kconfig_require(COAP_TRANSPORT_LIBNYOCI LIBNYOCI_FOUND "libnyoci")

# coap-transport-cantcoap spec: a *third* implementation of the same
# network_client/network_server concepts, this one over cantcoap
# (include/raft/coap_transport_cantcoap_impl.hpp). Unlike libcoap and libnyoci,
# cantcoap is a message codec only -- no socket, no reliability, no block-wise,
# no DTLS -- so the adapter supplies all of that itself.
#
# Discovery is find_package(... CONFIG) against the config package the overlay
# port's vendored CMakeLists exports, not pkg-config: cantcoap ships no build
# system at all, so the port supplies both. Defaults OFF and does not depend on
# COAP_TRANSPORT; all three backends are additive.
kythira_kconfig_gate(COAP_TRANSPORT_CANTCOAP)
if(_KYTHIRA_GATE_COAP_TRANSPORT_CANTCOAP)
    find_package(cantcoap QUIET CONFIG)
endif()
kythira_kconfig_require(COAP_TRANSPORT_CANTCOAP cantcoap_FOUND "cantcoap")

# grpc-transport spec: gRPC + Protocol Buffers back a third full-concept-family
# transport (.kiro/specs/grpc-transport/). Optional and gracefully degrading,
# exactly like libcoap above: probed only when CONFIG_GRPC_TRANSPORT is wanted,
# skipped-with-a-warning when gRPC/Protobuf are absent, and a hard configure
# failure only under KYTHIRA_KCONFIG_STRICT. gRPC pulls Protobuf in
# transitively via the vcpkg `grpc` port, but we find_package(Protobuf)
# explicitly too so the protobuf::protoc / gRPC::grpc_cpp_plugin codegen
# targets are guaranteed to be imported.
#
# The `gRPC_FOUND AND Protobuf_FOUND` test lives *inside* the gate, and the
# variable is initialised to FALSE before it. Written that way on purpose: this
# block shares `Protobuf_FOUND` with the PROTOBUF_SERIALIZER block below, and
# with the test outside the gate the result depended on which block CMake had
# already executed. It happened to be correct only because this one runs first,
# so neither variable is populated yet when the test is reached. Moving these
# two blocks, or adding a third protobuf consumer above this line, would have
# silently made `CONFIG_GRPC_TRANSPORT=n` stop disabling the transport — the
# exact defect that `CONFIG_PROTOBUF_SERIALIZER` shipped with. Correctness here
# should not be a property of statement order.
set(GRPC_TRANSPORT_FOUND FALSE)
kythira_kconfig_gate(GRPC_TRANSPORT)
if(_KYTHIRA_GATE_GRPC_TRANSPORT)
    find_package(gRPC CONFIG QUIET)
    find_package(Protobuf CONFIG QUIET)
    if(gRPC_FOUND AND Protobuf_FOUND)
        set(GRPC_TRANSPORT_FOUND TRUE)
    endif()
endif()
kythira_kconfig_require(GRPC_TRANSPORT GRPC_TRANSPORT_FOUND "grpc")

# oci-cloud-provider spec: OCI Instance Pool quorum management and Certificates
# Management certificate issuance, both speaking the OCI REST API directly.
#
# No find_package and no kythira_kconfig_require, unlike every gate above. That
# is not an omission: there is no OCI SDK to probe. The providers hand-roll OCI
# Request Signing v1 over OpenSSL and send it with httplib
# (include/raft/oci_signing.hpp, include/raft/oci_http_client.hpp), and all
# three of OpenSSL, httplib and boost::json are unconditional dependencies. So
# these gates answer "is OCI support wanted?" rather than "was a dependency
# found?", and there is no configuration in which wanting it can fail — which
# is why adding a `require` here would assert something that cannot be false.
kythira_kconfig_gate(OCI_QUORUM_MANAGER)
kythira_kconfig_gate(OCI_CERTIFICATES_PROVIDER)
kythira_kconfig_gate(OCI_OBJECT_PERSISTENCE)
if(_KYTHIRA_GATE_OCI_QUORUM_MANAGER OR _KYTHIRA_GATE_OCI_CERTIFICATES_PROVIDER
        OR _KYTHIRA_GATE_OCI_OBJECT_PERSISTENCE)
    # The signing/HTTP layer is shared, so it is built when *any* of the three
    # components is wanted rather than being duplicated under each.
    #
    # OCI_OBJECT_PERSISTENCE belongs in this condition and not merely beside it:
    # oci_object_storage_client is built on oci_http_client and oci_signing, so a
    # configuration that wanted object persistence with both other components off
    # would otherwise ask for a client whose transport was never built.
    set(KYTHIRA_OCI_SHARED TRUE)
    message(STATUS "OCI support: enabled (quorum_manager=${_KYTHIRA_GATE_OCI_QUORUM_MANAGER}, "
                   "certificates_provider=${_KYTHIRA_GATE_OCI_CERTIFICATES_PROVIDER}, "
                   "object_persistence=${_KYTHIRA_GATE_OCI_OBJECT_PERSISTENCE})")
else()
    set(KYTHIRA_OCI_SHARED FALSE)
    message(STATUS "OCI support: disabled by Kconfig")
endif()
if(NOT _KYTHIRA_GATE_OCI_OBJECT_PERSISTENCE)
    message(STATUS "oci_object_storage_client disabled (CONFIG_OCI_OBJECT_PERSISTENCE=n)")
endif()

# alibaba-cloud-services spec: ESS quorum management and OSS-backed Raft
# persistence, both speaking the Alibaba REST APIs directly.
#
# No find_package and no kythira_kconfig_require, for exactly the reason the
# OCI block above documents: there is no Alibaba SDK to probe. The components
# hand-roll ACS3-HMAC-SHA256 (control plane) and OSS V4 (data plane) signing
# over OpenSSL and send with httplib, and OpenSSL/httplib/boost::json are all
# unconditional dependencies. These gates answer "is Alibaba support wanted?".
kythira_kconfig_gate(ALIBABA_QUORUM_MANAGER)
kythira_kconfig_gate(ALIBABA_OSS_PERSISTENCE)
if(_KYTHIRA_GATE_ALIBABA_QUORUM_MANAGER OR _KYTHIRA_GATE_ALIBABA_OSS_PERSISTENCE)
    # The signing/HTTP layer is shared, so it is built when *either*
    # component is wanted rather than being duplicated under each.
    set(KYTHIRA_ALIBABA_SHARED TRUE)
    message(STATUS "Alibaba support: enabled (quorum_manager=${_KYTHIRA_GATE_ALIBABA_QUORUM_MANAGER}, "
                   "oss_persistence=${_KYTHIRA_GATE_ALIBABA_OSS_PERSISTENCE})")
else()
    set(KYTHIRA_ALIBABA_SHARED FALSE)
    message(STATUS "Alibaba support: disabled by Kconfig")
endif()

# ion-rpc-serializer spec: Amazon Ion (via the ion-c C library) backs a second
# rpc_serializer implementation (ion_rpc_serializer, include/raft/ion_serializer.hpp)
# alongside json_rpc_serializer. Optional and gracefully degrading, exactly like
# libcoap/grpc above: probed only when CONFIG_ION_SERIALIZER is wanted, skipped
# with a warning when ion-c is absent, and a hard configure failure only under
# KYTHIRA_KCONFIG_STRICT. ion-c is provided by the opt-in vcpkg "ion" feature
# (vcpkg-overlays/ion-c overlay port) and is not part of a default install, so
# the common case here is "not found -> Ion serializer unavailable, rest of the
# project unaffected". See .kiro/specs/ion-rpc-serializer/ for the full design.
kythira_kconfig_gate(ION_SERIALIZER)
if(_KYTHIRA_GATE_ION_SERIALIZER)
    find_package(ionc CONFIG QUIET)
endif()
kythira_kconfig_require(ION_SERIALIZER ionc_FOUND "ion-c")

if(ionc_FOUND)
    # ion_serializer.hpp is header-only (like json_serializer.hpp); this
    # INTERFACE target exists purely to hand ion-c linkage + include dirs +
    # the KYTHIRA_ION_SERIALIZER_AVAILABLE define to consuming targets, so a
    # consumer only needs target_link_libraries(... raft_ion_serializer).
    add_library(raft_ion_serializer INTERFACE)
    # ion-c's config package namespaces its imported target as IonC::ionc
    # (project(IonC)); accept the plausible variants so a future upstream
    # rename doesn't silently drop the link.
    if(TARGET IonC::ionc)
        target_link_libraries(raft_ion_serializer INTERFACE IonC::ionc)
    elseif(TARGET ionc::ionc)
        target_link_libraries(raft_ion_serializer INTERFACE ionc::ionc)
    elseif(TARGET ionc)
        target_link_libraries(raft_ion_serializer INTERFACE ionc)
    endif()
    target_include_directories(raft_ion_serializer INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    # ion-c's own ion_decimal.h #errors ("DECNUMDIGITS must be defined to be
    # >= DECQUAD_Pmax") unless every translation unit that includes it also
    # defines DECNUMDIGITS -- decNumber/decQuad values must be laid out
    # identically between ion-c's own compiled library and any consumer's
    # includes of its headers. ion-c's own CMakeLists.txt sets this via
    # add_definitions(-DDECNUMDIGITS=${IONC_DECIMAL_NUM_DIGITS}) with a
    # default of "34", which is directory-scoped and not exported on
    # IonC::ionc's usage requirements, so consumers never inherit it
    # transitively -- confirmed directly (a fresh consumer build failed with
    # exactly that #error until this was added). vcpkg-overlays/ion-c's
    # portfile.cmake does not override IONC_DECIMAL_NUM_DIGITS, so 34 (the
    # value ion-c itself was actually built with here) is correct; bump this
    # only in lockstep with that portfile if it ever passes a different
    # -DIONC_DECIMAL_NUM_DIGITS=... value.
    target_compile_definitions(raft_ion_serializer INTERFACE
        KYTHIRA_ION_SERIALIZER_AVAILABLE
        DECNUMDIGITS=34)
    target_compile_features(raft_ion_serializer INTERFACE cxx_std_23)
    message(STATUS "raft_ion_serializer target: enabled (.kiro/specs/ion-rpc-serializer/)")
else()
    # STATUS, not WARNING: ion-c ships behind the opt-in "ion" vcpkg feature and
    # is absent in a default install, so "not found" is the expected common case
    # here (unlike the default-present libcoap/grpc), and a per-configure warning
    # would just be noise. KYTHIRA_KCONFIG_STRICT still hard-fails via the
    # kythira_kconfig_require() call above when ION_SERIALIZER is explicitly on.
    message(STATUS "ion-c not found — Ion RPC serializer disabled (opt-in vcpkg feature: ion).")
endif()

# Find libldns using PkgConfig (system installation; pc file is named "ldns")
kythira_kconfig_gate(DNS_DISCOVERY)
if(_KYTHIRA_GATE_DNS_DISCOVERY)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBLDNS QUIET ldns)
    endif()
endif()
kythira_kconfig_require(DNS_DISCOVERY LIBLDNS_FOUND "libldns")

# protobuf-rpc-serializer spec: a Protocol Buffers implementation of the
# rpc_serializer concept (include/raft/protobuf_serializer.hpp), alongside the
# existing json_rpc_serializer/cbor_rpc_serializer. Protobuf is used purely as
# an encoding library for the existing transport-agnostic serializer slot --
# there is NO gRPC dependency here, and this is deliberately independent of the
# (separately specified) grpc-transport: distinct .proto packages
# (kythira.raft.serializer.v1 vs. kythira.raft.v1) let both coexist in one
# build. Gracefully optional, mirroring COAP_TRANSPORT: try the vcpkg CONFIG
# package first, then fall back to CMake's module-mode FindProtobuf for system
# installs. Both provide the protobuf::libprotobuf and protobuf::protoc targets
# this feature needs. See .kiro/specs/protobuf-rpc-serializer/ for the design.
#
# `PROTOBUF_SERIALIZER_FOUND` rather than bare `Protobuf_FOUND` at the use
# sites, and computed *inside* the gate. `Protobuf_FOUND` is not this feature's
# to read: the GRPC_TRANSPORT block above already calls
# find_package(Protobuf CONFIG QUIET) for its own codegen targets, so the
# variable is set whenever gRPC is enabled — which is the default. Gating the
# probe while testing `Protobuf_FOUND` downstream therefore did nothing at all:
# `CONFIG_PROTOBUF_SERIALIZER=n` still produced a build with the serializer,
# its target and its four test binaries, silently and with no warning. What
# actually controlled this feature was CONFIG_GRPC_TRANSPORT.
#
# The gRPC block's own `if(gRPC_FOUND AND Protobuf_FOUND)` sits outside its
# gate and is correct only because it runs first, so nothing has populated
# either variable yet. That is a property of statement order, not of the code,
# and it is why this one is written the robust way instead of matching it.
set(PROTOBUF_SERIALIZER_FOUND FALSE)
kythira_kconfig_gate(PROTOBUF_SERIALIZER)
if(_KYTHIRA_GATE_PROTOBUF_SERIALIZER)
    find_package(Protobuf CONFIG QUIET)
    if(NOT Protobuf_FOUND)
        find_package(Protobuf QUIET)
    endif()
    if(Protobuf_FOUND)
        set(PROTOBUF_SERIALIZER_FOUND TRUE)
    endif()
endif()
kythira_kconfig_require(PROTOBUF_SERIALIZER PROTOBUF_SERIALIZER_FOUND "protobuf")

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

# redis-compatible-kv spec: a RESP gateway over multi_raft for sccache's
# redis backend. No new third-party dependency (Requirement 16.2): Boost.Asio
# is part of the unconditional Boost dependency and OpenSSL backs both the
# PBKDF2 secret records and the TLS listener. The gate exists so that a build
# can leave the gateway's headers, daemon and tests out entirely, which
# Requirement 16.4 checks by diffing the target list with the symbol ON and OFF.
kythira_kconfig_gate(REDIS_GATEWAY)
set(KYTHIRA_BUILD_REDIS_GATEWAY FALSE)
if(_KYTHIRA_GATE_REDIS_GATEWAY AND OpenSSL_FOUND AND Boost_FOUND)
    set(KYTHIRA_BUILD_REDIS_GATEWAY TRUE)
    message(STATUS "redis-compatible KV gateway enabled (.kiro/specs/redis-compatible-kv/)")
else()
    message(STATUS "redis-compatible KV gateway disabled (OpenSSL/Boost not found, or CONFIG_REDIS_GATEWAY=n)")
endif()
kythira_kconfig_require(REDIS_GATEWAY KYTHIRA_BUILD_REDIS_GATEWAY "OpenSSL")

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

# Three outcomes, three messages. The single old warning ("libcoap not found.
# CoAP transport will not be available.") was wrong in both new directions: it
# fired for a deliberate backend-free build, and it claimed the transport was
# unavailable in configurations that built all 45 of its targets.
if(NOT KYTHIRA_BUILD_COAP_TRANSPORT)
    message(STATUS "CoAP transport disabled (CONFIG_COAP_TRANSPORT=n) — no CoAP targets.")
elseif(LIBCOAP_FOUND)
    message(STATUS "CoAP transport enabled, libcoap backend available.")
elseif(NOT _KYTHIRA_GATE_COAP_BACKEND_LIBCOAP)
    message(STATUS "CoAP transport enabled without the libcoap backend "
                   "(CONFIG_COAP_BACKEND_LIBCOAP=n) — backend-free surface only.")
else()
    message(WARNING "libcoap not found. CoAP transport will build its "
                    "backend-free surface only.")
endif()

# ── protobuf_rpc_serializer: generated-code target ───────────────────────────
# Generate raft_messages.pb.{h,cc} from proto/raft_messages.proto via protoc at
# build time (the generated code is NOT checked in -- Requirement 2.6), then
# compile it into the raft_protobuf_serializer library. Tests/consumers that use
# include/raft/protobuf_serializer.hpp link this target (which links only the
# Protobuf runtime -- no gRPC). Only defined when Protobuf was found; otherwise
# the target is skipped and the rest of the project configures unaffected
# (Requirement 9.1), the same graceful-degradation pattern LIBCOAP_FOUND uses.
if(PROTOBUF_SERIALIZER_FOUND)
    set(RAFT_MESSAGES_PROTO "${CMAKE_CURRENT_SOURCE_DIR}/proto/raft_messages.proto")
    set(PROTOBUF_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/raft_messages")
    file(MAKE_DIRECTORY "${PROTOBUF_GENERATED_DIR}")

    add_custom_command(
        OUTPUT "${PROTOBUF_GENERATED_DIR}/raft_messages.pb.cc"
               "${PROTOBUF_GENERATED_DIR}/raft_messages.pb.h"
        COMMAND protobuf::protoc
        ARGS --cpp_out "${PROTOBUF_GENERATED_DIR}"
             -I "${CMAKE_CURRENT_SOURCE_DIR}/proto" "${RAFT_MESSAGES_PROTO}"
        DEPENDS "${RAFT_MESSAGES_PROTO}" protobuf::protoc
        COMMENT "Generating raft_messages.pb.{h,cc} from proto/raft_messages.proto"
        VERBATIM
    )

    add_library(raft_protobuf_serializer
        "${PROTOBUF_GENERATED_DIR}/raft_messages.pb.cc"
    )
    target_include_directories(raft_protobuf_serializer PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        "${PROTOBUF_GENERATED_DIR}"
    )
    target_link_libraries(raft_protobuf_serializer PUBLIC protobuf::libprotobuf)
    target_compile_features(raft_protobuf_serializer PUBLIC cxx_std_23)
    message(STATUS "protobuf_rpc_serializer enabled (.kiro/specs/protobuf-rpc-serializer/)")
elseif(NOT _KYTHIRA_GATE_PROTOBUF_SERIALIZER)
    # Deliberate, so STATUS rather than WARNING — and worth distinguishing from
    # "not found", because until this gate was honoured the two were
    # indistinguishable from the log: turning the symbol off produced the
    # *enabled* message.
    message(STATUS
        "protobuf_rpc_serializer disabled (CONFIG_PROTOBUF_SERIALIZER=n).")
else()
    message(WARNING "Protobuf not found. protobuf_rpc_serializer will not be available.")
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

    # cpp-httplib's own vcpkg port unconditionally enables
    # CPPHTTPLIB_USE_NON_BLOCKING_GETADDRINFO (its INTERFACE_COMPILE_DEFINITIONS
    # bakes in `$<$<BOOL:ON>:CPPHTTPLIB_USE_NON_BLOCKING_GETADDRINFO>` with no
    # feature to turn it off), which makes cpp-httplib call glibc's
    # getaddrinfo_a() -- an asynchronous API that spins up its own internal
    # worker thread pool (nss/getaddrinfo.c's generate_addrinfo, invoked from
    # resolv/gai_misc.c's handle_requests on a thread glibc creates itself,
    # outside any pthread_create() call this codebase makes). Under TSan,
    # tests/beast_cross_transport_equivalence_test.cpp (the one test that
    # exercises cpp-httplib and Beast side by side, and so the one with by far
    # the most live threads at once) reliably segfaults inside TSan's own
    # runtime allocator (__tsan::user_alloc_internal) while servicing a
    # malloc() call from exactly that glibc-internal resolver thread -- a
    # TSan/glibc-internal-thread interaction bug, not a defect in this
    # codebase or in cpp-httplib's own request-handling logic (every other
    # symbol on the crashing stack is glibc/TSan runtime, nothing of ours).
    # Stripping the define here forces cpp-httplib back onto plain, blocking
    # getaddrinfo() -- no extra thread, so nothing for TSan to lose track of --
    # for the TSan build specifically; every other configuration (including
    # ASan, which already ran clean) keeps cpp-httplib's own default.
    if(KYTHIRA_SANITIZER STREQUAL "thread")
        get_target_property(_kythira_httplib_defs httplib::httplib INTERFACE_COMPILE_DEFINITIONS)
        if(_kythira_httplib_defs)
            list(FILTER _kythira_httplib_defs EXCLUDE REGEX "CPPHTTPLIB_USE_NON_BLOCKING_GETADDRINFO")
            set_target_properties(httplib::httplib PROPERTIES
                INTERFACE_COMPILE_DEFINITIONS "${_kythira_httplib_defs}")
        endif()
        unset(_kythira_httplib_defs)
    endif()
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
    # Backs LIBCOAP_AVAILABLE project-wide -- COAP_TRANSPORT's own Kconfig
    # help text already says "Backs LIBCOAP_AVAILABLE", but nothing
    # actually defined it here; only three individually-flagged test
    # targets (tests/CMakeLists.txt) turned it on for themselves. Every
    # other coap_*_test.cpp, and every production call site through
    # network_simulator, compiled coap_transport_impl.hpp's #ifdef
    # LIBCOAP_AVAILABLE branches out entirely and ran the stub
    # implementation instead, regardless of whether libcoap was actually
    # available.
    target_compile_definitions(network_simulator INTERFACE LIBCOAP_AVAILABLE)
endif()

if(LIBNYOCI_FOUND)
    target_include_directories(network_simulator INTERFACE ${LIBNYOCI_INCLUDE_DIRS})
    target_link_libraries(network_simulator INTERFACE ${LIBNYOCI_LIBRARIES})
    target_link_directories(network_simulator INTERFACE ${LIBNYOCI_LIBRARY_DIRS})
    target_compile_definitions(network_simulator INTERFACE ${LIBNYOCI_CFLAGS_OTHER})
    # Backs LIBNYOCI_AVAILABLE project-wide, exactly as the LIBCOAP_AVAILABLE
    # line above does -- without it coap_transport_libnyoci_impl.hpp compiles
    # to its stub and every coap_libnyoci_* test silently skips, which is the
    # failure mode that hid the libcoap backend from its own test suite for as
    # long as it did.
    target_compile_definitions(network_simulator INTERFACE LIBNYOCI_AVAILABLE)
    message(STATUS "libnyoci found: ${LIBNYOCI_VERSION} — alternate CoAP backend enabled "
                   "(.kiro/specs/coap-transport-libnyoci/)")
else()
    message(STATUS "libnyoci not found — alternate CoAP backend disabled "
                   "(opt in with --x-feature=coap-libnyoci)")
endif()

if(cantcoap_FOUND)
    target_link_libraries(network_simulator INTERFACE cantcoap::cantcoap)
    # Backs CANTCOAP_AVAILABLE project-wide, as the two blocks above do for
    # their libraries. Without it coap_transport_cantcoap_impl.hpp compiles to
    # its stub and every cantcoap test silently skips.
    target_compile_definitions(network_simulator INTERFACE CANTCOAP_AVAILABLE)
    message(STATUS "cantcoap found — codec-only CoAP backend enabled "
                   "(.kiro/specs/coap-transport-cantcoap/)")
else()
    message(STATUS "cantcoap not found — codec-only CoAP backend disabled "
                   "(opt in with --x-feature=coap-cantcoap)")
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
# The `if(TARGET ...)` test is inside the gate deliberately. An imported target
# is global and outlives the find_package() that created it, so testing for one
# outside the gate asks "does this target exist anywhere in this configure",
# not "did this feature's own probe succeed" -- and any other AWSSDK
# find_package that happens to define it would then defeat CONFIG_AWS_ACM_PCA=n
# silently. That is precisely how CONFIG_PROTOBUF_SERIALIZER came to be inert.
# The `unset(AWSSDK_FOUND)` below is a hint that this file has already been
# bitten once by AWSSDK's variables leaking between component probes.
set(_KYTHIRA_AWS_ACM_PCA_FOUND FALSE)
kythira_kconfig_gate(AWS_ACM_PCA)
if(_KYTHIRA_GATE_AWS_ACM_PCA)
    unset(AWSSDK_FOUND)
    find_package(AWSSDK QUIET COMPONENTS acm-pca)
    if(TARGET aws-cpp-sdk-acm-pca)
        set(_KYTHIRA_AWS_ACM_PCA_FOUND TRUE)
    endif()
endif()
kythira_kconfig_require(AWS_ACM_PCA _KYTHIRA_AWS_ACM_PCA_FOUND "AWS ACM Private CA SDK component")
if(TARGET aws-cpp-sdk-acm-pca)
    message(STATUS "AWS ACM Private CA SDK component found")
    target_compile_definitions(network_simulator INTERFACE KYTHIRA_HAS_AWS_ACM_PCA)
    target_link_libraries(network_simulator INTERFACE aws-cpp-sdk-acm-pca)
else()
    message(STATUS "AWS ACM Private CA SDK component not found — aws_acm_pca_provider disabled")
endif()

# ── AWS S3 object persistence (aws_s3_client) ─────────────────────────────────
# No find_package and no kythira_kconfig_require: the `s3` component this needs
# is already in the unconditional AWSSDK component list the AWS_SDK block above
# resolves, so this gate answers "is S3 object persistence wanted?" rather than
# "was a dependency found?" — the shape the OCI and Alibaba blocks below use.
kythira_kconfig_gate(AWS_S3_PERSISTENCE)
if(_KYTHIRA_GATE_AWS_S3_PERSISTENCE)
    message(STATUS "AWS S3 object persistence: enabled (aws_s3_client)")
else()
    message(STATUS "aws_s3_client disabled (CONFIG_AWS_S3_PERSISTENCE=n)")
endif()

# ── GCP Compute Engine SDK (gcp_compute_quorum_manager, gcp_mig_quorum_manager) ─
# google-cloud-cpp ships one CMake package per API surface; the two quorum
# managers need the compute_instances, compute_instance_group_managers, and
# compute_zone_operations components, all provided by google_cloud_cpp_compute.
# KYTHIRA_HAS_GCP_SDK is set only when all three imported targets are present,
# mirroring how KYTHIRA_HAS_AWS_SDK gates aws_ec2/aws_asg_quorum_manager.
#
# compute_machine_types is requested alongside them but deliberately excluded
# from that three-target condition: it is used only by the real-GCE test's
# launch-ladder discovery, never by the production managers, so a build lacking
# it should still get working GCP quorum managers rather than none.
set(_KYTHIRA_GCP_SDK_FOUND FALSE)
kythira_kconfig_gate(GCP_SDK)
if(_KYTHIRA_GATE_GCP_SDK)
    find_package(google_cloud_cpp_compute QUIET COMPONENTS
        compute_instances compute_instance_group_managers compute_zone_operations
        compute_machine_types)
    # Inside the gate: imported targets are global, so this test outside it
    # would ask whether the target exists anywhere rather than whether this
    # feature's probe found it. See the AWS_ACM_PCA block above.
    if(TARGET google-cloud-cpp::compute_instances
            AND TARGET google-cloud-cpp::compute_instance_group_managers
            AND TARGET google-cloud-cpp::compute_zone_operations)
        set(_KYTHIRA_GCP_SDK_FOUND TRUE)
    endif()
endif()
kythira_kconfig_require(GCP_SDK _KYTHIRA_GCP_SDK_FOUND "google-cloud-cpp compute components")
if(_KYTHIRA_GCP_SDK_FOUND)
    message(STATUS "google-cloud-cpp compute components found — GCP quorum managers enabled")
    target_compile_definitions(network_simulator INTERFACE KYTHIRA_HAS_GCP_SDK)
    target_link_libraries(network_simulator INTERFACE
        google-cloud-cpp::compute_instances
        google-cloud-cpp::compute_instance_group_managers
        google-cloud-cpp::compute_zone_operations)
    # Test-only (see the note above). KYTHIRA_HAS_GCP_MACHINE_TYPES lets
    # gcp_quorum_manager_real_gce_test compile its machine-type discovery out
    # and fall back to the configured machine type, so the absence of this one
    # component degrades the launch ladder instead of breaking the build.
    if(TARGET google-cloud-cpp::compute_machine_types)
        target_compile_definitions(network_simulator INTERFACE KYTHIRA_HAS_GCP_MACHINE_TYPES)
        target_link_libraries(network_simulator INTERFACE
            google-cloud-cpp::compute_machine_types)
    else()
        message(STATUS "google-cloud-cpp compute_machine_types not found — "
            "real-GCE launch ladder will fall back to the configured machine type")
    endif()
else()
    message(STATUS "google-cloud-cpp compute components not found — "
        "gcp_compute_quorum_manager and gcp_mig_quorum_manager disabled")
endif()

# ── GCP Certificate Authority Service (gcp_privateca_certificate_provider) ─────
# Independent of KYTHIRA_HAS_GCP_SDK (mirrors KYTHIRA_HAS_AWS_ACM_PCA's
# independence from KYTHIRA_HAS_AWS_SDK): an environment with the compute
# components but without privateca still builds everything except the
# certificate provider, and vice versa.
set(_KYTHIRA_GCP_PRIVATECA_FOUND FALSE)
kythira_kconfig_gate(GCP_PRIVATECA)
if(_KYTHIRA_GATE_GCP_PRIVATECA)
    find_package(google_cloud_cpp_privateca QUIET)
    # Inside the gate, for the same reason as the two blocks above.
    if(TARGET google-cloud-cpp::privateca)
        set(_KYTHIRA_GCP_PRIVATECA_FOUND TRUE)
    endif()
endif()
kythira_kconfig_require(GCP_PRIVATECA _KYTHIRA_GCP_PRIVATECA_FOUND "google-cloud-cpp privateca component")
if(_KYTHIRA_GCP_PRIVATECA_FOUND)
    message(STATUS "google-cloud-cpp privateca component found — gcp_privateca_certificate_provider enabled")
    target_compile_definitions(network_simulator INTERFACE KYTHIRA_HAS_GCP_PRIVATECA)
    target_link_libraries(network_simulator INTERFACE google-cloud-cpp::privateca)
else()
    message(STATUS "google-cloud-cpp privateca component not found — gcp_privateca_certificate_provider disabled")
endif()

# ── GCP Cloud Storage (gcp_gcs_client) ────────────────────────────────────────
# Independent of both blocks above, and for a stronger reason than privateca's:
# the storage component shares no code with the Compute API surface, so a node
# that persists Raft state in GCS need not carry the quorum managers at all.
# Note that `storage` is in google-cloud-cpp's *own* default feature set, so on a
# tree built with --x-feature=gcp this find_package already succeeds today; the
# explicit vcpkg.json entry makes the dependency intentional rather than
# inherited (see that file's `gcp` feature description).
set(_KYTHIRA_GCP_STORAGE_FOUND FALSE)
kythira_kconfig_gate(GCP_STORAGE)
if(_KYTHIRA_GATE_GCP_STORAGE)
    find_package(google_cloud_cpp_storage QUIET)
    # Inside the gate, for the same reason as the three blocks above: imported
    # targets are global, so testing outside it would ask whether the target
    # exists anywhere rather than whether this feature's probe found it.
    if(TARGET google-cloud-cpp::storage)
        set(_KYTHIRA_GCP_STORAGE_FOUND TRUE)
    endif()
endif()
kythira_kconfig_require(GCP_STORAGE _KYTHIRA_GCP_STORAGE_FOUND "google-cloud-cpp storage component")
if(_KYTHIRA_GCP_STORAGE_FOUND)
    message(STATUS "google-cloud-cpp storage component found — gcp_gcs_client enabled")
    target_compile_definitions(network_simulator INTERFACE KYTHIRA_HAS_GCP_STORAGE)
    target_link_libraries(network_simulator INTERFACE google-cloud-cpp::storage)
else()
    message(STATUS "google-cloud-cpp storage component not found — gcp_gcs_client disabled "
        "(configure with --x-feature=gcp to enable the GCS persistence backend)")
endif()

# Separate from the find_package above on purpose: that one answers "was the
# storage component found?", this one answers "do we want the GCS persistence
# backend built?". Kconfig's `depends on GCP_STORAGE` keeps the second from
# being selectable without the first.
kythira_kconfig_gate(GCP_STORAGE_PERSISTENCE)
if(_KYTHIRA_GCP_STORAGE_FOUND AND _KYTHIRA_GATE_GCP_STORAGE_PERSISTENCE)
    message(STATUS "GCP Cloud Storage object persistence: enabled (gcp_gcs_client)")
elseif(NOT _KYTHIRA_GATE_GCP_STORAGE_PERSISTENCE)
    message(STATUS "gcp_gcs_client disabled (CONFIG_GCP_STORAGE_PERSISTENCE=n)")
endif()

# ── Azure SDK (azure_vm_quorum_manager, azure_vmss_quorum_manager) ───────────
kythira_find_optional(AZURE_SDK azure-core-cpp CONFIG)
kythira_find_optional(AZURE_IDENTITY azure-identity-cpp CONFIG)
if(azure-core-cpp_FOUND AND azure-identity-cpp_FOUND)
    message(STATUS "Azure SDK found: azure-core-cpp ${azure-core-cpp_VERSION}, "
                    "azure-identity-cpp ${azure-identity-cpp_VERSION}")
    target_compile_definitions(network_simulator INTERFACE KYTHIRA_HAS_AZURE_SDK)
    target_link_libraries(network_simulator INTERFACE Azure::azure-core Azure::azure-identity)
    # BearerTokenAuthenticationPolicy (azure-core-cpp's policy.hpp) holds a
    # `std::atomic<bool>` member; Clang linked against GCC's libstdc++ doesn't
    # provide `__atomic_is_lock_free` as a compiler builtin the way GCC does
    # (the same root cause CMakeLists.txt's ENABLE_COVERAGE block already
    # works around further down) — libatomic supplies it at link time. Not
    # needed for the AWS SDK, which doesn't hit this code path.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_link_libraries(network_simulator INTERFACE atomic)
    endif()
    # azure_vm_quorum_manager.hpp/azure_vmss_quorum_manager.hpp build ARM REST
    # request/response bodies with boost::json directly (there is no generated
    # ARM Compute/Network client in the Azure SDK for C++ — see those headers'
    # own file comments). boost_json is defined above (~line 322) but not an
    # INTERFACE dependency of network_simulator by default, so any consumer
    # of these two headers needs it linked explicitly.
    if(TARGET boost_json)
        target_link_libraries(network_simulator INTERFACE boost_json)
    endif()
else()
    message(STATUS "Azure SDK not found — azure_vm_quorum_manager and azure_vmss_quorum_manager disabled")
endif()

# ── Azure Key Vault Keys (azure_key_vault_ca_provider) ───────────────────────
# Independent of KYTHIRA_HAS_AZURE_SDK, mirroring AWS_ACM_PCA's independence
# from AWS_SDK above: an environment with the quorum-manager SDK components
# but not Key Vault Keys (or vice versa) still builds everything except the
# component it lacks.
kythira_find_optional(AZURE_KEY_VAULT azure-security-keyvault-keys-cpp CONFIG)
if(azure-security-keyvault-keys-cpp_FOUND)
    message(STATUS "Azure Key Vault Keys SDK found: ${azure-security-keyvault-keys-cpp_VERSION}")
    target_compile_definitions(network_simulator INTERFACE KYTHIRA_HAS_AZURE_KEY_VAULT)
    target_link_libraries(network_simulator INTERFACE Azure::azure-security-keyvault-keys)
else()
    message(STATUS "Azure Key Vault Keys SDK not found — azure_key_vault_ca_provider disabled")
endif()

# ── Azure Blob object persistence (azure_blob_client) ─────────────────────────
# No find_package and no kythira_kconfig_require, and this one is worth being
# explicit about: task 0.6 decided to take **no** azure-storage-blobs-cpp
# dependency, so the client speaks the Blob REST API over httplib directly. What
# it uses azure-core/azure-identity for is the credential chain its bearer token
# comes from — which the AZURE_SDK block above already found. So this gate
# answers "is Blob object persistence wanted?", not "was a storage SDK found?".
kythira_kconfig_gate(AZURE_BLOB_PERSISTENCE)
if(_KYTHIRA_GATE_AZURE_BLOB_PERSISTENCE)
    message(STATUS "Azure Blob object persistence: enabled (azure_blob_client)")
else()
    message(STATUS "azure_blob_client disabled (CONFIG_AZURE_BLOB_PERSISTENCE=n)")
endif()

# When ON, azure_quorum_manager_real_test/azure_key_vault_ca_provider_real_test
# are registered with CTest (tests/CMakeLists.txt) and compiled with their real
# test bodies active; when OFF (the default), both binaries still compile —
# each falls back to a trivial compile-only stub case — but are never
# add_test()'d, so a default `ctest` run neither builds real-Azure-dependent
# assumptions into its test count nor needs real Azure credentials at all.
# Deliberately a stronger gate than the AWS real-EC2 tests' own
# KYTHIRA_AWS_REAL_EC2_TESTS compile definition (those always register and
# skip at runtime via SKIP_RETURN_CODE 77) — this spec's tasks.md calls for
# not registering at all when unset.
option(KYTHIRA_AZURE_REAL_TESTS "Register real-Azure integration tests with CTest" OFF)

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

# ── raft_grpc_transport (gRPC + Protocol Buffers transport) ──────────────────
# .kiro/specs/grpc-transport/. Unlike the header-only HTTP/CoAP transports
# (folded into network_simulator's INTERFACE target), the gRPC transport needs
# a compiled library: protoc + grpc_cpp_plugin generate raft.pb.{h,cc} /
# raft.grpc.pb.{h,cc} into the build tree (never checked in, Requirement 2.7),
# and those .cc files plus src/grpc_transport_impl.cpp make up the target.
# Gated on GRPC_TRANSPORT_FOUND (probed above) AND Folly: like the HTTP/CoAP
# transports, grpc_transport.hpp hardcodes kythira::Future/Promise (Folly's
# default backend) rather than the backend-selected future_default, so the
# target is Folly-oriented and requires TARGET Folly::folly, not merely *some*
# future backend. Absent gRPC → skipped with a warning, rest of the project
# unaffected (Requirement 18.1).
if(GRPC_TRANSPORT_FOUND AND TARGET Folly::folly)
    set(RAFT_PROTO "${CMAKE_CURRENT_SOURCE_DIR}/proto/raft.proto")
    set(GRPC_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/raft")
    file(MAKE_DIRECTORY "${GRPC_GENERATED_DIR}")

    add_custom_command(
        OUTPUT "${GRPC_GENERATED_DIR}/raft.pb.cc" "${GRPC_GENERATED_DIR}/raft.pb.h"
               "${GRPC_GENERATED_DIR}/raft.grpc.pb.cc" "${GRPC_GENERATED_DIR}/raft.grpc.pb.h"
        COMMAND protobuf::protoc
        # No quotes around the plugin path: under VERBATIM each ARGS token is
        # passed literally, so quotes *inside* the --plugin= token would reach
        # protoc as part of the path ("…/grpc_cpp_plugin": program not found).
        # VERBATIM already handles any escaping the path itself needs.
        ARGS --grpc_out "${GRPC_GENERATED_DIR}" --cpp_out "${GRPC_GENERATED_DIR}"
             --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
             -I "${CMAKE_CURRENT_SOURCE_DIR}/proto" "${RAFT_PROTO}"
        DEPENDS "${RAFT_PROTO}" protobuf::protoc gRPC::grpc_cpp_plugin
        COMMENT "Generating gRPC/Protobuf sources from proto/raft.proto"
        VERBATIM
    )

    add_library(raft_grpc_transport STATIC
        src/grpc_transport_impl.cpp
        "${GRPC_GENERATED_DIR}/raft.pb.cc"
        "${GRPC_GENERATED_DIR}/raft.grpc.pb.cc"
    )
    target_include_directories(raft_grpc_transport PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        "${GRPC_GENERATED_DIR}"
    )
    # network_simulator (PUBLIC): grpc_transport_impl.hpp's kythira::Future/
    # Promise usage pulls in Folly, which needs the same include-order/link
    # setup network_simulator's INTERFACE target already carries — mirrors how
    # certificate_authority links network_simulator rather than re-wiring Folly.
    target_link_libraries(raft_grpc_transport PUBLIC
        gRPC::grpc++ protobuf::libprotobuf OpenSSL::SSL OpenSSL::Crypto network_simulator
    )
    target_compile_features(raft_grpc_transport PUBLIC cxx_std_23)
    message(STATUS "raft_grpc_transport target: enabled (.kiro/specs/grpc-transport/)")
elseif(NOT GRPC_TRANSPORT_FOUND)
    message(WARNING "gRPC/Protobuf not found. gRPC transport will not be available.")
else()
    message(STATUS "Folly not found — raft_grpc_transport target disabled (transport hardcodes kythira::Future via raft/future.hpp)")
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

# ── The heavy-translation-unit job pool ──────────────────────────────────────
#
# A Ninja job pool that lets at most ONE memory-heavy translation unit compile
# at a time, across every directory. Declared here rather than in
# `tests/CMakeLists.txt`, where it started, because `cmd/multi_raft_node` is
# now a member and `tests/` is added conditionally -- a target naming a pool
# that was never declared makes Ninja fail at build time with "unknown pool".
#
# MEASURED, not guessed, with this project's CI compiler (g++-13, Release,
# `-DKYTHIRA_DEFAULT_FUTURE_BACKEND=stdexec`), one translation unit at a time:
#
#     POOLED
#       cmd/multi_raft_node/run_beast_cbor.cpp             5,967 MiB   280 s
#       cmd/multi_raft_node/run_beast_json.cpp             5,965 MiB   305 s
#       three_way_http_transport_equivalence_test          5,649 MiB          *
#       cmd/multi_raft_node/run_httplib_cbor.cpp           5,407 MiB   234 s
#       cmd/multi_raft_node/run_httplib_json.cpp           5,407 MiB   204 s
#       tests/http_implementation_interop_test.cpp         4,422 MiB   174 s
#       tests/beast_cross_transport_equivalence_test.cpp   4,420 MiB   176 s
#       tests/proxygen_negotiation_integration_test.cpp    3,280 MiB    84 s
#       tests/proxygen_transport_test.cpp                  2,458 MiB    53 s
#       cmd/multi_raft_node/main.cpp, after the split      1,509 MiB    25 s
#       cmd/multi_raft_node/config.cpp                       208 MiB     3 s
#     NOT POOLED
#       tests/beast_negotiation_retry_test.cpp             4,373 MiB   180 s
#       tests/beast_server_test.cpp                        3,843 MiB   141 s
#       tests/beast_client_test.cpp                        3,519 MiB   153 s
#       tests/beast_integration_test.cpp                   3,514 MiB   155 s
#       tests/beast_ssl_test.cpp                           3,501 MiB   131 s
#       examples/raft/proxygen_transport_example.cpp       2,287 MiB    52 s
#       cmd/multi_raft_bench/main.cpp                      1,611 MiB    45 s
#     AND, THE REASON THIS POOL MOVED HERE AND GREW
#       cmd/multi_raft_node/main.cpp, before the split    10,974 MiB   535 s
#
#     * carried from this pool's original comment, not re-measured here. Every
#       other figure was taken this way in this configuration.
#
# A GitHub runner has 16 GiB, and the `Full suite` job builds with `-j3`. Three
# of the 5-plus-GiB class at once does not fit; one of them beside two of the
# unpooled beast tests comes to about 14 GiB and does. That is the whole
# design: this pool bounds the worst single allocation, `-j3` bounds the
# aggregate, and both are needed. See `.github/workflows/ci.yml`'s Build step
# for the wall-clock measurements that rejected `-j$(nproc)`.
#
# beast_cross_transport_equivalence_test joined the pool on this evidence, and
# only this evidence: at 4,420 MiB it is within 2 MiB of
# http_implementation_interop_test, which was already pooled, and the two are
# gated by the SAME `if()` and built by the same helper. They compile Beast and
# cpp-httplib into one translation unit apiece; there was never a reason for
# one to be bounded and the other not, and nothing but a measurement would have
# shown that.
#
# The rest of the beast family is deliberately left out. Pooling it too would
# serialise most of the expensive tail, and the numbers say it does not have
# to be: the worst remaining unpooled pair (4,373 + 3,843) beside the worst
# pooled TU is inside the envelope that measured green at 1 h 51 m before
# cmd/multi_raft_node existed. If this leg fails again, that family is where to
# look next, and these are its figures.
#
# The members are expensive for a legible reason: each instantiates a whole
# transport stack -- or, in the tests' case, Beast, Proxygen and cpp-httplib
# together -- and stdexec's sender types multiply what that costs.
#
# `JOB_POOL_COMPILE` is a Ninja feature and is ignored by other generators,
# which is harmless: CI configures every affected job with `-G Ninja`, and a
# Makefiles build keeps today's behaviour.
set_property(GLOBAL APPEND PROPERTY JOB_POOLS heavy_tu=1)

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
        # The Tier C/E measurement host and its driver
        # (.kiro/specs/multi-raft-host-binary/). Neither is in any install
        # target: they are measurement tools, not supported servers, and the
        # surest way to keep one from being deployed is for `cmake --install`
        # never to place it anywhere. Both are excluded from coverage builds by
        # the enclosing guard, like every other `cmd/` binary.
        add_subdirectory(cmd/multi_raft_node)
        add_subdirectory(cmd/multi_raft_bench)
    endif()
    if(KYTHIRA_BUILD_REDIS_GATEWAY AND KYTHIRA_FUTURE_BACKEND_AVAILABLE AND folly_FOUND)
        # The Redis-compatible gateway daemon (.kiro/specs/redis-compatible-kv/
        # task 10). Same posture as multi_raft_node: not in any install target.
        add_subdirectory(cmd/redis_gateway_node)
    endif()
    if(LIBLDNS_FOUND AND KYTHIRA_FUTURE_BACKEND_AVAILABLE AND httplib_FOUND)
        add_subdirectory(cmd/dns_discovery_node)
        add_subdirectory(cmd/dns_sd_discovery_node)
    endif()
    # The Requirement 4.4 on-instance heartbeat process. Same gate as the
    # OCI quorum-manager tests, because the writer header includes the
    # manager's (tag constants, parsing helpers) — one feature, one gate.
    if(_KYTHIRA_GATE_OCI_QUORUM_MANAGER AND TARGET OpenSSL::SSL AND TARGET httplib::httplib
       AND KYTHIRA_FUTURE_BACKEND_AVAILABLE)
        add_subdirectory(cmd/oci_heartbeat_writer)
    endif()
    # The backup/restore CLI (Requirement 10.6). Gated on no cloud provider at
    # all: Requirement 16.4 says it builds with *whichever* providers are enabled
    # and names the absent ones, so a zero-provider build must still produce a
    # working binary rather than no binary — "command not found" is the wrong
    # thing to hand an operator in a recovery window.
    #
    # It is gated on a future backend, and only because the header chain reaches
    # one: object_store_backup.hpp → object_store_persistence.hpp →
    # persistence.hpp → future.hpp. The tool itself uses no futures.
    if(Boost_FOUND AND KYTHIRA_FUTURE_BACKEND_AVAILABLE)
        add_subdirectory(cmd/raft_object_backup)
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
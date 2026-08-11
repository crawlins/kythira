# Kconfig integration -- see .kiro/specs/kconfig-integration/ for the full
# design. Included at the very top of the root CMakeLists.txt, before any
# find_package() call: resolves a Kconfig .config (from -DKYTHIRA_KCONFIG=,
# or build/.config if present, or the Kconfig-declared defaults if neither
# exists) into two generated files consumed by the rest of CMakeLists.txt --
# build/generated/autoconf.cmake (KCONFIG_<NAME> CMake variables) and
# build/generated/kythira/autoconf.hpp (the existing KYTHIRA_HAS_*/etc. C++
# macros, unchanged names, so no #ifdef call site needs to change).
#
# Kconfig does not replace find_package() -- CMake still does the actual
# probing of what's installed. What Kconfig adds is a single tree of symbols
# with declared `depends on` relationships, a named/checked-in way to select
# a feature set, and an opt-in strict mode (KYTHIRA_KCONFIG_STRICT=ON) that
# turns "wanted but not found" from a silent skip into a hard configure
# failure.
#
# kconfiglib is tooling, not a build requirement: a machine with neither
# Python 3 nor kconfiglib builds exactly as it does today -- autoconf.cmake
# ends up empty, and every kythira_find_optional()/kythira_kconfig_require()
# call below behaves as an unconditional QUIET probe (every KCONFIG_<NAME>
# is simply undefined, and "NOT DEFINED KCONFIG_<NAME>" reads as "want it").

set(KYTHIRA_KCONFIG "" CACHE FILEPATH
    "Path to a Kconfig .config/defconfig. Empty = build/.config if present, else Kconfig defaults.")
set(KYTHIRA_KCONFIG_STRICT OFF CACHE BOOL
    "If ON, a CONFIG symbol set to y makes its find_package() REQUIRED instead of QUIET-and-degrade.")

find_package(Python3 QUIET COMPONENTS Interpreter)
set(_KYTHIRA_KCONFIG_OK FALSE)
if(Python3_FOUND)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} -c "import kconfiglib"
        RESULT_VARIABLE _KYTHIRA_KCONFIGLIB_MISSING
        OUTPUT_QUIET ERROR_QUIET)
    if(_KYTHIRA_KCONFIGLIB_MISSING EQUAL 0)
        set(_KYTHIRA_KCONFIG_OK TRUE)
    endif()
endif()

set(_KYTHIRA_KCONFIG_CFG "${KYTHIRA_KCONFIG}")
if(NOT _KYTHIRA_KCONFIG_CFG AND EXISTS "${CMAKE_BINARY_DIR}/.config")
    set(_KYTHIRA_KCONFIG_CFG "${CMAKE_BINARY_DIR}/.config")
endif()

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/kythira")
set(KYTHIRA_KCONFIG_CMAKE_OUT "${CMAKE_BINARY_DIR}/generated/autoconf.cmake")
set(KYTHIRA_KCONFIG_HEADER_OUT "${CMAKE_BINARY_DIR}/generated/kythira/autoconf.hpp")

if(_KYTHIRA_KCONFIG_OK)
    set(_KYTHIRA_GENCONFIG_ARGS
        "${CMAKE_SOURCE_DIR}/Kconfig")
    if(_KYTHIRA_KCONFIG_CFG)
        list(APPEND _KYTHIRA_GENCONFIG_ARGS "${_KYTHIRA_KCONFIG_CFG}")
    endif()
    execute_process(
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/scripts/kconfig/genconfig.py"
                ${_KYTHIRA_GENCONFIG_ARGS}
                --cmake-out "${KYTHIRA_KCONFIG_CMAKE_OUT}"
                --header-out "${KYTHIRA_KCONFIG_HEADER_OUT}"
        RESULT_VARIABLE _KYTHIRA_GENCONFIG_FAILED)
    if(_KYTHIRA_GENCONFIG_FAILED)
        message(FATAL_ERROR "Kconfig: genconfig.py failed -- see output above")
    endif()
    if(_KYTHIRA_KCONFIG_CFG)
        message(STATUS "Kconfig: configured from ${_KYTHIRA_KCONFIG_CFG}")
    else()
        message(STATUS "Kconfig: no .config supplied -- using Kconfig-declared defaults")
    endif()
else()
    # An explicitly requested config file that cannot take effect must be a
    # hard error, not a fall-through: silently resolving a *different* feature
    # set than the one named on the command line is exactly the CI/local
    # divergence this integration exists to prevent.
    if(KYTHIRA_KCONFIG)
        message(FATAL_ERROR "Kconfig: -DKYTHIRA_KCONFIG=${KYTHIRA_KCONFIG} was "
                            "given, but python3/kconfiglib is unavailable, so "
                            "the file cannot be applied and configure would "
                            "silently fall back to autodetect mode. "
                            "Install with: pip install -r scripts/kconfig/requirements.txt")
    endif()
    message(STATUS "Kconfig: python3/kconfiglib not found -- "
                    "menuconfig/defconfig unavailable, all optional "
                    "find_package() calls fall back to unconditional QUIET "
                    "probing (equivalent to every optional symbol "
                    "defaulting to y, autodetect mode). "
                    "Install with: pip install -r scripts/kconfig/requirements.txt")
    file(WRITE "${KYTHIRA_KCONFIG_CMAKE_OUT}" "# python3/kconfiglib not found -- no KCONFIG_* variables defined\n")
    file(WRITE "${KYTHIRA_KCONFIG_HEADER_OUT}" "#pragma once\n")
endif()

include("${KYTHIRA_KCONFIG_CMAKE_OUT}")

# Structural validity check, not a "dependency not found" case, so it applies
# regardless of KYTHIRA_KCONFIG_STRICT and only when Kconfig actually
# resolved a full symbol set (KCONFIG_FOLLY etc. are only ever all three
# simultaneously defined when genconfig.py ran successfully -- the
# kconfiglib-absent branch above leaves autoconf.cmake empty, so this never
# fires on the zero-config/no-kconfiglib path). CONFIG_FOLLY defaults to y,
# so this only trips if a user explicitly deselects every backend.
if(DEFINED KCONFIG_FOLLY AND DEFINED KCONFIG_STDEXEC_BACKEND AND DEFINED KCONFIG_BOOST_FUTURE_BACKEND)
    if(NOT KCONFIG_FOLLY AND NOT KCONFIG_STDEXEC_BACKEND AND NOT KCONFIG_BOOST_FUTURE_BACKEND)
        message(FATAL_ERROR
            "Kconfig: at least one Future backend must be selected "
            "(CONFIG_FOLLY, CONFIG_STDEXEC_BACKEND, CONFIG_BOOST_FUTURE_BACKEND "
            "are all n)")
    endif()
endif()

# kythira_find_optional(<SYMBOL> <find_package-style call>)
# Wraps one optional dependency behind a single, plain find_package() call:
# skipped entirely if KCONFIG_<SYMBOL> is explicitly OFF (Requirement 4.3);
# otherwise QUIET-and-degrade unless KYTHIRA_KCONFIG_STRICT is ON, in which
# case not-found is a hard configure error via find_package()'s own REQUIRED
# handling (Requirement 4.1, 4.2). Not usable for dependencies resolved via
# pkg_check_modules() or hand-written multi-step detection (libcoap,
# libldns, libfiu, Poco DNSSD, AWS_ACM_PCA's two-step AWSSDK re-probe) --
# those keep their existing detection logic wrapped in the same
# "if(NOT DEFINED KCONFIG_<SYMBOL> OR KCONFIG_<SYMBOL>)" gate plus an
# explicit kythira_kconfig_require() call afterward; see the root
# CMakeLists.txt call sites for both patterns.
macro(kythira_find_optional SYMBOL)
    if(NOT DEFINED KCONFIG_${SYMBOL} OR KCONFIG_${SYMBOL})
        if(KYTHIRA_KCONFIG_STRICT AND DEFINED KCONFIG_${SYMBOL})
            find_package(${ARGN} REQUIRED)
        else()
            find_package(${ARGN} QUIET)
        endif()
    endif()
endmacro()

# kythira_kconfig_require(<SYMBOL> <FOUND_VAR> <package label>)
# Companion to the hand-written-detection pattern above: call after a
# manually-gated find_package()/pkg_check_modules() block to enforce strict
# mode the same way kythira_find_optional()'s inline REQUIRED does, for
# dependencies whose detection can't be expressed as one find_package() call.
macro(kythira_kconfig_require SYMBOL FOUND_VAR LABEL)
    if(KYTHIRA_KCONFIG_STRICT AND DEFINED KCONFIG_${SYMBOL} AND KCONFIG_${SYMBOL} AND NOT ${FOUND_VAR})
        message(FATAL_ERROR "Kconfig: CONFIG_${SYMBOL}=y (strict mode) but ${LABEL} was not found")
    endif()
endmacro()

# kythira_kconfig_gate(<SYMBOL>) -> TRUE/FALSE in _KYTHIRA_GATE_<SYMBOL>
# Sets whether a hand-written detection block should run at all: "wanted"
# means either no Kconfig involvement (KCONFIG_<SYMBOL> undefined, today's
# behavior) or CONFIG_<SYMBOL>=y. Same visibility rule kythira_find_optional
# applies, extracted so multi-step detection blocks can guard themselves
# without duplicating the "NOT DEFINED ... OR ..." condition inline.
macro(kythira_kconfig_gate SYMBOL)
    if(NOT DEFINED KCONFIG_${SYMBOL} OR KCONFIG_${SYMBOL})
        set(_KYTHIRA_GATE_${SYMBOL} TRUE)
    else()
        set(_KYTHIRA_GATE_${SYMBOL} FALSE)
    endif()
endmacro()

# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Copies the shared libraries that the ledger chaos image's binaries load
# from the build host's own trees (vcpkg_installed, the build directory)
# into DEST, so the image carries them. System libraries (libc, libstdc++,
# OpenSSL from the distribution, ...) are left to the image's base layer.
#
# Why: the Ion job's vcpkg tree builds ion-c as a shared library, so
# ledger_node and ledger_ctl need libionc.so and libdecNumber.so at run time
# even though RocksDB and the rest come in statically. A local build that
# happened to link ion-c statically hid this until CI built the image.
#
# Usage:
#   cmake -DDEST=<dir> -DROOTS=<dir1|dir2> -DEXECUTABLES=<exe1|exe2>
#         -P stage_runtime_libs.cmake
#
# The lists are '|'-separated: a ';' does not survive add_custom_target's
# argument handling reliably.

if(NOT DEST OR NOT EXECUTABLES OR NOT ROOTS)
    message(FATAL_ERROR "stage_runtime_libs.cmake needs DEST, ROOTS and EXECUTABLES")
endif()

string(REPLACE "|" ";" ROOTS "${ROOTS}")
string(REPLACE "|" ";" EXECUTABLES "${EXECUTABLES}")

file(REMOVE_RECURSE "${DEST}")
file(MAKE_DIRECTORY "${DEST}")

set(_include_regexes "")
foreach(_root IN LISTS ROOTS)
    file(REAL_PATH "${_root}" _real_root)
    string(REGEX REPLACE "([][+.*()^$?|\\\\])" "\\\\\\1" _escaped "${_real_root}")
    list(APPEND _include_regexes "^${_escaped}/")
endforeach()

file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES ${EXECUTABLES}
    RESOLVED_DEPENDENCIES_VAR _resolved
    UNRESOLVED_DEPENDENCIES_VAR _unresolved
    POST_INCLUDE_REGEXES ${_include_regexes}
    POST_EXCLUDE_REGEXES ".*"
)

if(_unresolved)
    message(FATAL_ERROR "unresolved shared libraries: ${_unresolved}")
endif()

foreach(_lib IN LISTS _resolved)
    # Copy under the name the binary asks for (the SONAME symlink usually
    # points at a longer versioned file); follow symlinks to get the bytes.
    get_filename_component(_name "${_lib}" NAME)
    file(REAL_PATH "${_lib}" _target)
    file(COPY_FILE "${_target}" "${DEST}/${_name}")
    message(STATUS "staged ${_name}")
endforeach()

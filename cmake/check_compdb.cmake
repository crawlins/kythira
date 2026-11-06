# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Fail the static-analysis targets early, with an actionable message, when the
# build tree has no compilation database. Invoked as
#   cmake -DFILE=<build>/compile_commands.json -P cmake/check_compdb.cmake
# Without it run-clang-tidy prints a Python traceback, and plain clang-tidy
# silently falls back to compiling every file with no flags at all.
if(NOT DEFINED FILE)
    message(FATAL_ERROR "check_compdb.cmake: pass -DFILE=<path to compile_commands.json>")
endif()
if(NOT EXISTS "${FILE}")
    message(FATAL_ERROR
        "static-analysis: ${FILE} does not exist. Configure with "
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON (the root CMakeLists.txt sets it) "
        "using a Ninja or Makefile generator.")
endif()

# With -DSOURCES=<build>/tidy-sources.txt (one absolute path per line), also
# fail when a file the target is about to analyse has no compile command.
# run-clang-tidy only runs the database entries its file arguments match, so
# such a file is skipped silently and the run still reports "all clean". A
# file that only builds under an optional dependency belongs in
# configs/tidy-variant-sources.txt, under the variant that builds it.
if(DEFINED SOURCES)
    file(READ "${FILE}" _compdb)
    file(STRINGS "${SOURCES}" _sources)
    set(_missing "")
    foreach(_src IN LISTS _sources)
        if(_src STREQUAL "")
            continue()
        endif()
        # Ninja and Makefile generators both write `"file": "<absolute path>"`;
        # the quoted path appears nowhere else in an entry (the command line
        # holds it unquoted, inside a longer string).
        string(FIND "${_compdb}" "\"${_src}\"" _at)
        if(_at EQUAL -1)
            list(APPEND _missing "${_src}")
        endif()
    endforeach()
    if(_missing)
        list(LENGTH _missing _n)
        string(REPLACE ";" "\n  " _list "${_missing}")
        message(FATAL_ERROR
            "static-analysis: ${_n} file(s) have no entry in ${FILE}, so "
            "clang-tidy would skip them silently:\n  ${_list}\n"
            "A file that only builds under an optional dependency goes in "
            "configs/tidy-variant-sources.txt under the variant that builds "
            "it; a file this configuration should build is missing from its "
            "target.")
    endif()
endif()

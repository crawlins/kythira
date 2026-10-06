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

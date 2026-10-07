# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Per-test Boost.Test reports, so CI can see which test *cases* ran.
#
# ctest's verdict is per binary. A Boost case whose precondition() is false
# is reported "skipped", the binary still exits 0, and ctest records a pass:
# coap_oscore_over_dtls_test, for one, would go on passing with all five of
# its cases skipped if the build stopped linking OSCORE and DTLS together.
# scripts/check-test-run.sh --case-reports reads the files written here and
# fails on any skipped case or suite it was not told to expect.
#
# Off unless KYTHIRA_TEST_REPORT_DIR is set, because the report then goes to
# the file instead of the console, and a local run should look like it always
# has. Non-Boost tests ignore the two variables and write nothing.
set(KYTHIRA_TEST_REPORT_DIR "" CACHE PATH
    "If set (absolute path), every test writes a detailed Boost.Test report to <dir>/<test>.txt")

if(KYTHIRA_TEST_REPORT_DIR)
    get_filename_component(_KYTHIRA_TEST_REPORT_DIR_ABS "${KYTHIRA_TEST_REPORT_DIR}"
        ABSOLUTE BASE_DIR "${CMAKE_BINARY_DIR}")
    file(MAKE_DIRECTORY "${_KYTHIRA_TEST_REPORT_DIR_ABS}")
endif()

# kythira_attach_test_reports()
# Call at the end of every CMakeLists.txt that calls add_test(): the TESTS
# directory property only lists the current directory's tests, and
# set_property(TEST) only reaches tests of the current directory. APPEND keeps
# whatever ENVIRONMENT a test already set. The file name rule must match the
# one in scripts/check-test-run.sh.
function(kythira_attach_test_reports)
    if(NOT KYTHIRA_TEST_REPORT_DIR)
        return()
    endif()
    get_property(_tests DIRECTORY PROPERTY TESTS)
    foreach(_test IN LISTS _tests)
        string(REGEX REPLACE "[^A-Za-z0-9_.-]" "_" _file "${_test}")
        set_property(TEST "${_test}" APPEND PROPERTY ENVIRONMENT
            "BOOST_TEST_REPORT_LEVEL=detailed"
            "BOOST_TEST_REPORT_SINK=${_KYTHIRA_TEST_REPORT_DIR_ABS}/${_file}.txt")
    endforeach()
endfunction()

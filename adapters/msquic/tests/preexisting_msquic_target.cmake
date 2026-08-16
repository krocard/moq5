# FindMsQuic's third branch: the caller already defined msquic::msquic.
#
# That dependency lives only in the parent's own scope, so the nested-configure
# registration regression cannot reproduce it and is deliberately NOT registered
# in this mode. This control proves the omission is a clean skip rather than a
# failing test, and that nothing else in the MsQuic suite went missing with it.
#
# Fail-closed throughout: a failed configure, an unreadable cache, or an
# unparseable structured listing is an error, never an empty set that compares
# equal to an expectation.

cmake_minimum_required(VERSION 3.20)

# PARENT_OPENSSL may legitimately be empty (no openssl on this host), so it is
# checked for definedness only, separately from the must-be-nonempty inputs.
if(NOT DEFINED PARENT_OPENSSL)
    message(FATAL_ERROR "preexisting_msquic_target: PARENT_OPENSSL not provided")
endif()

foreach(_v SOURCE_DIR BUILD_DIR CMAKE_CMD CTEST_CMD MSQUIC_INCLUDE_DIR
           MSQUIC_LIBRARY SELF_TEST_NAME)
    if(NOT DEFINED ${_v} OR "${${_v}}" STREQUAL "")
        message(FATAL_ERROR "preexisting_msquic_target: ${_v} not provided")
    endif()
endforeach()

file(REMOVE_RECURSE "${BUILD_DIR}")
if(EXISTS "${BUILD_DIR}")
    message(FATAL_ERROR
        "preexisting_msquic_target: could not remove ${BUILD_DIR}")
endif()
file(MAKE_DIRECTORY "${BUILD_DIR}")

# Define a real imported msquic::msquic BEFORE the project's find_package(MsQuic)
# runs, which is exactly the situation FindMsQuic's first branch exists for.
set(inject "${BUILD_DIR}/preexisting-init.cmake")
file(WRITE "${inject}"
"add_library(msquic::msquic UNKNOWN IMPORTED)\n"
"set_target_properties(msquic::msquic PROPERTIES\n"
"    IMPORTED_LOCATION \"${MSQUIC_LIBRARY}\"\n"
"    INTERFACE_INCLUDE_DIRECTORIES \"${MSQUIC_INCLUDE_DIR}\")\n")

execute_process(
    COMMAND "${CMAKE_CMD}" -S "${SOURCE_DIR}" -B "${BUILD_DIR}"
            -DMOQ_BUILD_ADAPTER_MSQUIC=ON
            -DMOQ_BUILD_MSQUIC_MANAGED=ON
            "-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=${inject}"
            # Unlike the configure-ORDER regression, this fixture is testing
            # the preexisting-target branch, not first-pass find_program()
            # ordering -- so pinning the child's openssl disposition to the
            # parent's is correct here, and is what makes the expectations
            # below deterministic in both capability states.
            "-DMOQ_OPENSSL=${PARENT_OPENSSL}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR
        "preexisting_msquic_target: configure with a preexisting "
        "msquic::msquic target FAILED (rc=${rc}). This branch is supported and "
        "must configure cleanly.\n--- stdout ---\n${out}\n--- stderr ---\n${err}")
endif()

# The recorded mode must be the preexisting branch. It is a normal result
# variable, so it is read from the configure log the branch prints rather than
# from the cache -- see the status line emitted by adapters/msquic.
if(NOT out MATCHES "MsQuic discovery mode: preexisting")
    message(FATAL_ERROR
        "preexisting_msquic_target: the configure did not report the "
        "preexisting discovery branch.\n--- stdout ---\n${out}")
endif()

# The openssl pin must have actually taken, or the expectations below are
# checking the wrong capability state. A pre-seeded cache entry suppresses
# find_program's search, but that is a property of the CMake in use rather
# than something this fixture controls, so it is verified instead of assumed.
if(NOT EXISTS "${BUILD_DIR}/CMakeCache.txt")
    message(FATAL_ERROR
        "preexisting_msquic_target: the child configure left no cache at "
        "${BUILD_DIR}/CMakeCache.txt")
endif()
file(STRINGS "${BUILD_DIR}/CMakeCache.txt" ssl_lines
     REGEX "^MOQ_OPENSSL:[^=]*=")
# Emptiness is tested by LENGTH, not by if(<var>): the matched line ends in
# the NOTFOUND sentinel whenever openssl is absent, which if() reads as false
# and would report a present entry as missing.
list(LENGTH ssl_lines ssl_count)
if(ssl_count EQUAL 0)
    message(FATAL_ERROR
        "preexisting_msquic_target: the child cache has no MOQ_OPENSSL entry")
endif()
list(GET ssl_lines 0 ssl_line)
string(REGEX REPLACE "^MOQ_OPENSSL:[^=]*=" "" child_openssl "${ssl_line}")
if(NOT child_openssl STREQUAL PARENT_OPENSSL)
    message(FATAL_ERROR
        "preexisting_msquic_target: the child's openssl was NOT pinned to the "
        "parent's -- child='${child_openssl}' parent='${PARENT_OPENSSL}'. The "
        "certificate expectations below assume the two agree.")
endif()

execute_process(
    COMMAND "${CTEST_CMD}" --test-dir "${BUILD_DIR}" --show-only=json-v1
    RESULT_VARIABLE rc OUTPUT_VARIABLE listing ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR
        "preexisting_msquic_target: ctest --show-only=json-v1 failed "
        "(rc=${rc})\n--- stdout ---\n${listing}\n--- stderr ---\n${err}")
endif()
string(JSON tests_type ERROR_VARIABLE jerr TYPE "${listing}" tests)
if(jerr OR NOT tests_type STREQUAL "ARRAY")
    message(FATAL_ERROR
        "preexisting_msquic_target: listing has no `tests` ARRAY "
        "(error='${jerr}')\n--- ctest output ---\n${listing}")
endif()
string(JSON n_tests ERROR_VARIABLE jerr LENGTH "${listing}" tests)
if(jerr OR n_tests EQUAL 0)
    message(FATAL_ERROR
        "preexisting_msquic_target: no tests registered at all; an empty set "
        "would make every absence check below vacuous (error='${jerr}')")
endif()
set(names "")
math(EXPR last "${n_tests} - 1")
foreach(i RANGE ${last})
    string(JSON nm ERROR_VARIABLE jerr GET "${listing}" tests ${i} name)
    if(jerr OR nm STREQUAL "")
        message(FATAL_ERROR
            "preexisting_msquic_target: tests[${i}] has no usable name "
            "(error='${jerr}')")
    endif()
    list(APPEND names "${nm}")
endforeach()

# 1. the inapplicable nested-configure test is ABSENT, not failing
if("${SELF_TEST_NAME}" IN_LIST names)
    message(FATAL_ERROR
        "preexisting_msquic_target: ${SELF_TEST_NAME} is registered in "
        "preexisting mode, where its child cannot be given the parent's "
        "target. It must be omitted, not registered to fail.")
endif()

# 2. the tests that need no certificate are still there, in BOTH capability
#    states -- omitting the nested-configure test must not take the rest of the
#    MsQuic suite with it
foreach(want msquic_public_compile msquic_unit msquic_settings)
    if(NOT "${want}" IN_LIST names)
        message(FATAL_ERROR
            "preexisting_msquic_target: ${want} is missing. Omitting the "
            "nested-configure test must not take the rest of the MsQuic suite "
            "with it.\n  registered: ${names}")
    endif()
endforeach()

# 3. the certificate-dependent tests follow the parent's openssl disposition,
#    which the child was pinned to: all present when openssl exists, all absent
#    when it does not. Requiring them unconditionally would make this control
#    fail on an openssl-less host for a reason that has nothing to do with the
#    preexisting-target branch.
set(cert_tests msquic_gen_certs msquic_recv_loopback msquic_loopback)
if(PARENT_OPENSSL)
    foreach(want IN LISTS cert_tests)
        if(NOT "${want}" IN_LIST names)
            message(FATAL_ERROR
                "preexisting_msquic_target: openssl is available "
                "('${PARENT_OPENSSL}') so ${want} must be registered.\n"
                "  registered: ${names}")
        endif()
    endforeach()
else()
    foreach(unwanted IN LISTS cert_tests)
        if("${unwanted}" IN_LIST names)
            message(FATAL_ERROR
                "preexisting_msquic_target: openssl is unavailable, so "
                "${unwanted} must NOT be registered.\n  registered: ${names}")
        endif()
    endforeach()
endif()

file(REMOVE_RECURSE "${BUILD_DIR}")
message(STATUS
    "preexisting_msquic_target: ${n_tests} tests, nested-configure regression "
    "correctly omitted")

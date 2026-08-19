# Stage a fresh install into a private directory and check its exact contents.
# Out-of-tree by construction: the prefix is created here and removed first, so
# the check never inspects a prefix another run left behind.
set(stage "${BUILDDIR}/relay-install-surface-stage")
file(REMOVE_RECURSE "${stage}")
file(MAKE_DIRECTORY "${stage}")

execute_process(
    COMMAND ${CMAKE_COMMAND} --install "${BUILDDIR}" --prefix "${stage}"
    RESULT_VARIABLE rc OUTPUT_QUIET)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "cmake --install failed: ${rc}")
endif()

execute_process(
    COMMAND "${BASH}" "${GUARD}" "${stage}" "${SRCROOT}" "${DEPLIB}"
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "install-surface check failed")
endif()

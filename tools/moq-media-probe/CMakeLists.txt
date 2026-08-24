# moq_media_probe: a black-box JSONL probe over LibMoQ's MSF/CMSF catalog parser.
# Owned entirely by LibMoQ; links only moq::msf (which brings moq::core). The
# vendored JSON parser (media/msf/vendor/json.h) is reused for request parsing --
# no second parser is introduced.

set(_PROBE_VENDOR ${CMAKE_CURRENT_SOURCE_DIR}/../../media/msf/vendor)
set(_PROBE_FIXTURES ${CMAKE_CURRENT_SOURCE_DIR}/../../media/msf/tests/fixtures)

add_executable(moq_media_probe main.c probe.c)
target_link_libraries(moq_media_probe PRIVATE moq::msf)
target_include_directories(moq_media_probe PRIVATE ${_PROBE_VENDOR})
target_compile_features(moq_media_probe PRIVATE c_std_11)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(moq_media_probe PRIVATE
        -Wall -Wextra -Wpedantic $<$<BOOL:${MOQ_WARNINGS_AS_ERRORS}>:-Werror>)
endif()

if(MOQ_BUILD_TESTS)
    add_executable(test_moq_media_probe tests/test_probe.c probe.c)
    target_link_libraries(test_moq_media_probe PRIVATE moq::msf)
    target_include_directories(test_moq_media_probe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${_PROBE_VENDOR})
    target_compile_definitions(test_moq_media_probe PRIVATE
        MOQ_MSF_FIXTURE_DIR="${_PROBE_FIXTURES}")
    target_compile_features(test_moq_media_probe PRIVATE c_std_11)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(test_moq_media_probe PRIVATE
            -Wall -Wextra -Wpedantic $<$<BOOL:${MOQ_WARNINGS_AS_ERRORS}>:-Werror>)
    endif()
    add_test(NAME moq_media_probe COMMAND test_moq_media_probe)

    # End-to-end CLI check: drive the real executable's stdin/stdout with JSONL.
    add_test(NAME moq_media_probe_cli
             COMMAND ${CMAKE_COMMAND}
                     -DPROBE=$<TARGET_FILE:moq_media_probe>
                     -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_smoke.cmake)
endif()

# End-to-end CLI check for moq_media_probe: drive the real executable's
# stdin/stdout with JSONL and assert one-response-per-line, stdout-only output,
# deterministic capabilities, and that a malformed line does not poison a later
# valid line. Invoked by CTest via `cmake -DPROBE=... -DWORKDIR=... -P`.

if(NOT PROBE OR NOT WORKDIR)
    message(FATAL_ERROR "cli_smoke.cmake requires -DPROBE and -DWORKDIR")
endif()

# Bracket argument [=[...]=] writes the JSONL verbatim (no escape processing),
# so the inline catalog's \" survive to the file. Lines: capabilities, a
# malformed line, then a valid catalog.parse.
set(_in "${WORKDIR}/cli_in.jsonl")
file(WRITE "${_in}" [=[
{"protocol":"moq-media-probe/1","id":"caps","operation":"capabilities"}
garbage not json
{"protocol":"moq-media-probe/1","id":"p","operation":"catalog.parse","profile":"msf-01","input":{"utf8":"{\"version\":\"1\",\"tracks\":[{\"name\":\"v\",\"packaging\":\"loc\",\"isLive\":true}]}"}}
]=])

execute_process(
    COMMAND "${PROBE}"
    INPUT_FILE "${_in}"
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
    RESULT_VARIABLE _rc)

if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "probe exited with ${_rc}\nstderr: ${_err}")
endif()
# Machine output goes only to stdout; nothing on stderr on the normal path.
if(NOT _err STREQUAL "")
    message(FATAL_ERROR "probe wrote to stderr (must be stdout-only):\n${_err}")
endif()

# Exactly three response lines (one per request line).
string(REGEX MATCHALL "\n" _nls "${_out}")
list(LENGTH _nls _lines)
if(NOT _lines EQUAL 3)
    message(FATAL_ERROR "expected 3 response lines, got ${_lines}:\n${_out}")
endif()

# Every line is valid JSON carrying the protocol, and each is a single line.
string(REGEX REPLACE "\n$" "" _trim "${_out}")
string(REPLACE "\n" ";" _list "${_trim}")
foreach(_line IN LISTS _list)
    string(JSON _proto ERROR_VARIABLE _e GET "${_line}" "protocol")
    if(_e OR NOT _proto STREQUAL "moq-media-probe/1")
        message(FATAL_ERROR "line is not a valid probe response: ${_line}\n${_e}")
    endif()
endforeach()

# The malformed middle line yields an error; the valid line still succeeds.
if(NOT _out MATCHES "\"category\":\"malformed-json\"")
    message(FATAL_ERROR "expected a malformed-json error line:\n${_out}")
endif()
if(NOT _out MATCHES "\"id\":\"p\"" OR NOT _out MATCHES "\"version\":\"1\"")
    message(FATAL_ERROR "expected the valid catalog.parse response:\n${_out}")
endif()

# Capabilities are deterministic byte-for-byte across runs.
set(_caps "${WORKDIR}/cli_caps.jsonl")
file(WRITE "${_caps}" [=[
{"protocol":"moq-media-probe/1","id":"c","operation":"capabilities"}
]=])
execute_process(COMMAND "${PROBE}" INPUT_FILE "${_caps}" OUTPUT_VARIABLE _c1 RESULT_VARIABLE _r1)
execute_process(COMMAND "${PROBE}" INPUT_FILE "${_caps}" OUTPUT_VARIABLE _c2 RESULT_VARIABLE _r2)
if(NOT _r1 EQUAL 0 OR NOT _r2 EQUAL 0)
    message(FATAL_ERROR "capabilities run failed")
endif()
if(NOT _c1 STREQUAL _c2)
    message(FATAL_ERROR "capabilities output is not deterministic:\n<<<${_c1}>>>\n<<<${_c2}>>>")
endif()

message(STATUS "moq_media_probe CLI smoke passed (${_lines} lines, deterministic capabilities)")

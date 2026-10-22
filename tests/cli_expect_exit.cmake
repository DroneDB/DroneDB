# Runs a command and fails unless it exits with exactly EXPECTED. Optional checks:
#   EXPECTED_STDOUT=<text>   the standard output contains the text
#   EXPECTED_STDERR=<text>   the standard error contains the text
#   EXPECTED_JSON_STDOUT=ON  the standard output is one JSON document and nothing else (a log
#                            record on stdout breaks the scripts that parse `ddb info -f json`)
#
#   cmake -DEXPECTED=<code> [-DEXPECTED_STDOUT=<text>] [-DEXPECTED_STDERR=<text>]
#         [-DEXPECTED_JSON_STDOUT=ON] -P cli_expect_exit.cmake -- <command> [args...]
#
# ctest's WILL_FAIL accepts any failure, including a crash or a missing DLL; this
# script tells "exited with the expected code" apart from everything else.

if(NOT DEFINED EXPECTED)
    message(FATAL_ERROR "cli_expect_exit: EXPECTED is not set")
endif()

# The command follows the first "--" in the script arguments
set(_command "")
set(_collect FALSE)
math(EXPR _last "${CMAKE_ARGC} - 1")
foreach(_i RANGE 0 ${_last})
    if(_collect)
        list(APPEND _command "${CMAKE_ARGV${_i}}")
    elseif("${CMAKE_ARGV${_i}}" STREQUAL "--")
        set(_collect TRUE)
    endif()
endforeach()

if(NOT _command)
    message(FATAL_ERROR "cli_expect_exit: no command after --")
endif()

# ENCODING (Windows only): the CLI writes UTF-8 (src/include/mio.h, "Path encoding")
execute_process(
    COMMAND ${_command}
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr
    ENCODING UTF8
)

message("stdout:\n${_stdout}")
message("stderr:\n${_stderr}")

# _result is a number for a normal exit, a description (e.g. "Segmentation fault") otherwise
if(NOT "${_result}" STREQUAL "${EXPECTED}")
    message(FATAL_ERROR "cli_expect_exit: expected exit code ${EXPECTED}, got '${_result}': ${_command}")
endif()

if(DEFINED EXPECTED_STDOUT)
    string(FIND "${_stdout}" "${EXPECTED_STDOUT}" _found)
    if(_found EQUAL -1)
        message(FATAL_ERROR "cli_expect_exit: the output does not contain '${EXPECTED_STDOUT}': ${_command}")
    endif()
endif()

if(DEFINED EXPECTED_STDERR)
    string(FIND "${_stderr}" "${EXPECTED_STDERR}" _found)
    if(_found EQUAL -1)
        message(FATAL_ERROR "cli_expect_exit: the error output does not contain '${EXPECTED_STDERR}': ${_command}")
    endif()
endif()

if(EXPECTED_JSON_STDOUT)
    # string(JSON) stops after the first value and accepts a bare number, so a log line that
    # starts with a date ("2026-10-22 ...") would parse as 2026: the text must also be one
    # object or array from its first to its last non-blank character
    string(STRIP "${_stdout}" _json)
    string(JSON _type ERROR_VARIABLE _jsonError TYPE "${_json}")
    if(NOT _jsonError AND NOT _type MATCHES "^(OBJECT|ARRAY)$")
        set(_jsonError "the document is a ${_type}, not an object or an array")
    endif()
    if(NOT _jsonError AND NOT _json MATCHES "^[[{].*[]}]$")
        set(_jsonError "text before or after the document")
    endif()
    if(_jsonError)
        message(FATAL_ERROR "cli_expect_exit: the output is not one JSON document (${_jsonError}): ${_command}")
    endif()
endif()

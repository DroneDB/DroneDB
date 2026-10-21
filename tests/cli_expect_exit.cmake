# Runs a command and fails unless it exits with exactly EXPECTED (and, with EXPECTED_STDOUT,
# unless its standard output contains that text).
#
#   cmake -DEXPECTED=<code> [-DEXPECTED_STDOUT=<text>] -P cli_expect_exit.cmake -- <command> [args...]
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

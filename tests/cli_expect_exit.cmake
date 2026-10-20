# Runs a command and fails unless it exits with exactly EXPECTED.
#
#   cmake -DEXPECTED=<code> -P cli_expect_exit.cmake -- <command> [args...]
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

execute_process(
    COMMAND ${_command}
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr
)

message("stdout:\n${_stdout}")
message("stderr:\n${_stderr}")

# _result is a number for a normal exit, a description (e.g. "Segmentation fault") otherwise
if(NOT "${_result}" STREQUAL "${EXPECTED}")
    message(FATAL_ERROR "cli_expect_exit: expected exit code ${EXPECTED}, got '${_result}': ${_command}")
endif()

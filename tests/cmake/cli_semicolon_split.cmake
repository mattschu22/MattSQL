execute_process(
  COMMAND "${MATTSQL_CLI}"
  INPUT_FILE "${SQL_FILE}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error
)

if(result EQUAL 0)
  message(FATAL_ERROR "expected CLI to fail on trailing malformed SQL")
endif()

if(NOT output MATCHES "(^|[\r\n])a[\r\n]+1([\r\n]|$)")
  message(FATAL_ERROR "CLI did not execute the complete statement before the trailing error. stdout='${output}' stderr='${error}'")
endif()

if(NOT error MATCHES "error\\[ParseError\\]")
  message(FATAL_ERROR "expected ParseError for trailing malformed SQL. stderr='${error}'")
endif()

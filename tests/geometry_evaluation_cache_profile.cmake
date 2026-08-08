if(NOT DEFINED CACHE_FIXTURE)
  message(FATAL_ERROR "CACHE_FIXTURE is required")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env TEKLA_DB1_OCCT_CACHE_PROFILE=1 "${CACHE_FIXTURE}"
  RESULT_VARIABLE fixture_result
  OUTPUT_VARIABLE fixture_stdout
  ERROR_VARIABLE fixture_stderr)
if(NOT fixture_result EQUAL 0)
  message(FATAL_ERROR
    "cache fixture failed (${fixture_result})\n${fixture_stdout}\n${fixture_stderr}")
endif()

foreach(expected IN ITEMS
    "\"outcome\":\"hit\""
    "\"outcome\":\"miss\""
    "\"outcome\":\"bypass\""
    "\"outcome\":\"insert\""
    "\"outcome\":\"not_retained\"")
  string(FIND "${fixture_stderr}" "${expected}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "cache profile omitted ${expected}")
  endif()
endforeach()

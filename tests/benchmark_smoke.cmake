if(NOT EXISTS "${BENCHMARK_EXECUTABLE}")
    message(FATAL_ERROR "benchmark executable not found: ${BENCHMARK_EXECUTABLE}")
endif()

execute_process(
    COMMAND
        "${BENCHMARK_EXECUTABLE}"
        --keys 1000
        --prefix-groups 16
        --profile fast
        --phases insert,find,get,prefix,erase,compact,prefix_compact,save,load,find_loaded,get_loaded
        --read-repeats 2
    RESULT_VARIABLE fast_result
    OUTPUT_VARIABLE fast_output
    ERROR_VARIABLE fast_error
)
if(NOT fast_result EQUAL 0)
    message(FATAL_ERROR "fast benchmark smoke run failed (${fast_result}): ${fast_error}")
endif()
if(NOT fast_output MATCHES "serialized_bytes=[1-9][0-9]*")
    message(FATAL_ERROR "fast benchmark did not serialize a non-empty snapshot")
endif()
if(NOT fast_output MATCHES "find_id_loaded[^\n]*ops=1600")
    message(FATAL_ERROR "fast benchmark did not complete loaded point lookups")
endif()
if(NOT fast_output MATCHES "sink=[0-9]+")
    message(FATAL_ERROR "fast benchmark did not produce its completion marker")
endif()

execute_process(
    COMMAND
        "${BENCHMARK_EXECUTABLE}"
        --keys 1000
        --prefix-groups 16
        --profile compact
        --phases insert,compact,save,load,steady_loaded
        --read-repeats 2
        --loaded-find-ratio 3
        --loaded-get-ratio 1
    RESULT_VARIABLE compact_result
    OUTPUT_VARIABLE compact_output
    ERROR_VARIABLE compact_error
)
if(NOT compact_result EQUAL 0)
    message(FATAL_ERROR "compact benchmark smoke run failed (${compact_result}): ${compact_error}")
endif()
if(NOT compact_output MATCHES "selected_profile=compact")
    message(FATAL_ERROR "compact benchmark did not select the requested profile")
endif()
if(NOT compact_output MATCHES "steady_loaded[^\n]*ops=8000")
    message(FATAL_ERROR "compact benchmark did not complete steady-state reads")
endif()
if(NOT compact_output MATCHES "sink=[0-9]+")
    message(FATAL_ERROR "compact benchmark did not produce its completion marker")
endif()

cmake_minimum_required(VERSION 3.20)

project(string_bimap_subproject_consumer LANGUAGES CXX)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../.." string_bimap)

foreach(
    development_option
    IN ITEMS
        STRING_BIMAP_BUILD_EXAMPLES
        STRING_BIMAP_BUILD_TOOLS
        STRING_BIMAP_BUILD_TESTS
        STRING_BIMAP_BUILD_BENCHMARKS
        STRING_BIMAP_BUILD_FUZZERS
)
    if(${development_option})
        message(FATAL_ERROR "vendored string_bimap unexpectedly enabled ${development_option}")
    endif()
endforeach()

foreach(
    development_target
    IN ITEMS
        string_bimap_basic_example
        string_bimap_core_test
        string_bimap_persistence_test
        string_bimap_query_test
        string_bimap_bench
        string_bimap_pthash_tool
)
    if(TARGET ${development_target})
        message(FATAL_ERROR "vendored string_bimap unexpectedly enabled ${development_target}")
    endif()
endforeach()

add_executable(string_bimap_subproject_consumer ../package_consumer/main.cpp)
target_link_libraries(string_bimap_subproject_consumer PRIVATE string_bimap::string_bimap)
target_compile_features(string_bimap_subproject_consumer PRIVATE cxx_std_17)

enable_testing()
add_test(NAME string_bimap_subproject_consumer COMMAND string_bimap_subproject_consumer)

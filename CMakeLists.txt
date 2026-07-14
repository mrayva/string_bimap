cmake_minimum_required(VERSION 3.20)

project(string_bimap LANGUAGES CXX VERSION 0.1.0)

set(_STRING_BIMAP_DEVELOPMENT_DEFAULT OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(_STRING_BIMAP_DEVELOPMENT_DEFAULT ON)
endif()

option(STRING_BIMAP_BUILD_EXAMPLES "Build examples" ${_STRING_BIMAP_DEVELOPMENT_DEFAULT})
option(STRING_BIMAP_BUILD_TOOLS "Build command-line tools" ${_STRING_BIMAP_DEVELOPMENT_DEFAULT})
option(STRING_BIMAP_BUILD_TESTS "Build test executables" ${_STRING_BIMAP_DEVELOPMENT_DEFAULT})
option(STRING_BIMAP_BUILD_BENCHMARKS "Build benchmark executables" ${_STRING_BIMAP_DEVELOPMENT_DEFAULT})
option(STRING_BIMAP_BUILD_FUZZERS "Build libFuzzer targets (Clang only)" OFF)
option(STRING_BIMAP_USE_XCDAT "Enable xcdat-backed static index when xcdat headers are available" ON)
option(STRING_BIMAP_USE_MARISA "Enable marisa-trie-backed static index when marisa headers and library are available" ON)
option(STRING_BIMAP_USE_HAT_TRIE "Enable hat-trie-backed mutable index when hat-trie headers are available" ON)
option(STRING_BIMAP_USE_ARRAY_HASH "Enable array-hash-backed mutable index when array-hash headers are available" ON)
option(STRING_BIMAP_USE_PTHASH "Enable the optional pthash-backed static runtime bimap when a local pthash checkout is available" ON)

set(STRING_BIMAP_ARRAY_HASH_ROOT "" CACHE PATH "Optional array-hash source root")
set(STRING_BIMAP_PTHASH_ROOT "" CACHE PATH "Optional pthash source root")
unset(_STRING_BIMAP_DEVELOPMENT_DEFAULT)

add_library(string_bimap INTERFACE)
add_library(string_bimap::string_bimap ALIAS string_bimap)

target_compile_features(string_bimap INTERFACE cxx_std_17)
target_include_directories(
    string_bimap
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

if(STRING_BIMAP_USE_XCDAT)
    find_path(STRING_BIMAP_XCDAT_INCLUDE_DIR xcdat.hpp)
    if(STRING_BIMAP_XCDAT_INCLUDE_DIR)
        target_include_directories(
            string_bimap
            SYSTEM INTERFACE
                $<BUILD_INTERFACE:${STRING_BIMAP_XCDAT_INCLUDE_DIR}>
        )
        target_compile_definitions(string_bimap INTERFACE $<BUILD_INTERFACE:STRING_BIMAP_HAS_XCDAT=1>)
    endif()
endif()

if(STRING_BIMAP_USE_MARISA)
    find_path(STRING_BIMAP_MARISA_INCLUDE_DIR marisa.h)
    find_library(STRING_BIMAP_MARISA_LIBRARY NAMES marisa libmarisa)
    if(STRING_BIMAP_MARISA_INCLUDE_DIR AND STRING_BIMAP_MARISA_LIBRARY)
        target_include_directories(
            string_bimap
            SYSTEM INTERFACE
                $<BUILD_INTERFACE:${STRING_BIMAP_MARISA_INCLUDE_DIR}>
        )
        target_link_libraries(
            string_bimap
            INTERFACE
                $<BUILD_INTERFACE:${STRING_BIMAP_MARISA_LIBRARY}>
        )
        target_compile_definitions(string_bimap INTERFACE $<BUILD_INTERFACE:STRING_BIMAP_HAS_MARISA=1>)
    endif()
endif()

if(STRING_BIMAP_USE_HAT_TRIE)
    find_path(STRING_BIMAP_HAT_TRIE_INCLUDE_DIR tsl/htrie_map.h)
    if(STRING_BIMAP_HAT_TRIE_INCLUDE_DIR)
        target_include_directories(
            string_bimap
            SYSTEM INTERFACE
                $<BUILD_INTERFACE:${STRING_BIMAP_HAT_TRIE_INCLUDE_DIR}>
        )
        target_compile_definitions(string_bimap INTERFACE $<BUILD_INTERFACE:STRING_BIMAP_HAS_HAT_TRIE=1>)
    endif()
endif()

if(STRING_BIMAP_USE_ARRAY_HASH)
    set(STRING_BIMAP_ARRAY_HASH_INCLUDE_DIR "")
    if(STRING_BIMAP_ARRAY_HASH_ROOT)
        if(EXISTS "${STRING_BIMAP_ARRAY_HASH_ROOT}/include/tsl/array_map.h")
            set(STRING_BIMAP_ARRAY_HASH_INCLUDE_DIR "${STRING_BIMAP_ARRAY_HASH_ROOT}/include")
        elseif(EXISTS "${STRING_BIMAP_ARRAY_HASH_ROOT}/tsl/array_map.h")
            set(STRING_BIMAP_ARRAY_HASH_INCLUDE_DIR "${STRING_BIMAP_ARRAY_HASH_ROOT}")
        endif()
    endif()
    if(NOT STRING_BIMAP_ARRAY_HASH_INCLUDE_DIR)
        find_path(STRING_BIMAP_ARRAY_HASH_INCLUDE_DIR tsl/array_map.h)
    endif()
    if(STRING_BIMAP_ARRAY_HASH_INCLUDE_DIR)
        target_include_directories(
            string_bimap
            SYSTEM INTERFACE
                $<BUILD_INTERFACE:${STRING_BIMAP_ARRAY_HASH_INCLUDE_DIR}>
        )
        target_compile_definitions(string_bimap INTERFACE $<BUILD_INTERFACE:STRING_BIMAP_HAS_ARRAY_HASH=1>)
    endif()
endif()

if(STRING_BIMAP_USE_PTHASH)
    if(STRING_BIMAP_PTHASH_ROOT AND EXISTS "${STRING_BIMAP_PTHASH_ROOT}/CMakeLists.txt")
        if(EXISTS "${STRING_BIMAP_PTHASH_ROOT}/external/bits/CMakeLists.txt")
            add_subdirectory("${STRING_BIMAP_PTHASH_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/pthash" EXCLUDE_FROM_ALL)
            foreach(_string_bimap_pthash_target IN ITEMS PTHASH BITS ESSENTIALS)
                get_target_property(
                    _string_bimap_pthash_include_dirs
                    ${_string_bimap_pthash_target}
                    INTERFACE_INCLUDE_DIRECTORIES
                )
                if(_string_bimap_pthash_include_dirs)
                    set_property(
                        TARGET ${_string_bimap_pthash_target}
                        PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
                                 "${_string_bimap_pthash_include_dirs}"
                    )
                endif()
            endforeach()

            # PTHash's bits target exports project-local diagnostic flags to consumers.
            foreach(_string_bimap_pthash_target IN ITEMS PTHASH BITS)
                get_target_property(
                    _string_bimap_pthash_compile_options
                    ${_string_bimap_pthash_target}
                    INTERFACE_COMPILE_OPTIONS
                )
                if(_string_bimap_pthash_compile_options)
                    list(
                        REMOVE_ITEM _string_bimap_pthash_compile_options
                        -ggdb
                        -Wall
                        -Wextra
                        -Wno-missing-braces
                        -Wno-unknown-attributes
                        -Wno-unused-function
                    )
                    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND NOT APPLE)
                        # PTHash v2 forces libc++ headers without matching linker flags.
                        list(REMOVE_ITEM _string_bimap_pthash_compile_options -stdlib=libc++)
                    endif()
                    set_property(
                        TARGET ${_string_bimap_pthash_target}
                        PROPERTY INTERFACE_COMPILE_OPTIONS "${_string_bimap_pthash_compile_options}"
                    )
                endif()
            endforeach()
            target_link_libraries(string_bimap INTERFACE $<BUILD_INTERFACE:PTHASH>)
            target_compile_definitions(string_bimap INTERFACE $<BUILD_INTERFACE:STRING_BIMAP_HAS_PTHASH=1>)
            if(EXISTS "${STRING_BIMAP_PTHASH_ROOT}/include/pthash.hpp")
                target_include_directories(
                    string_bimap
                    SYSTEM INTERFACE
                        $<BUILD_INTERFACE:${STRING_BIMAP_PTHASH_ROOT}/include>
                )
                target_compile_definitions(string_bimap INTERFACE $<BUILD_INTERFACE:STRING_BIMAP_PTHASH_V2=1>)
            endif()
        else()
            message(STATUS "STRING_BIMAP_PTHASH_ROOT is set, but required pthash submodules are missing; skipping pthash integration")
        endif()
    endif()
endif()

include(GNUInstallDirs)
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(TARGETS string_bimap EXPORT string_bimapTargets)
install(
    EXPORT string_bimapTargets
    FILE string_bimapTargets.cmake
    NAMESPACE string_bimap::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/string_bimap"
)

include(CMakePackageConfigHelpers)
set(STRING_BIMAP_CONFIG_WITH_XCDAT FALSE)
set(STRING_BIMAP_CONFIG_WITH_MARISA FALSE)
set(STRING_BIMAP_CONFIG_WITH_HAT_TRIE FALSE)
set(STRING_BIMAP_CONFIG_WITH_ARRAY_HASH FALSE)
if(STRING_BIMAP_XCDAT_INCLUDE_DIR)
    set(STRING_BIMAP_CONFIG_WITH_XCDAT TRUE)
endif()
if(STRING_BIMAP_MARISA_INCLUDE_DIR AND STRING_BIMAP_MARISA_LIBRARY)
    set(STRING_BIMAP_CONFIG_WITH_MARISA TRUE)
endif()
if(STRING_BIMAP_HAT_TRIE_INCLUDE_DIR)
    set(STRING_BIMAP_CONFIG_WITH_HAT_TRIE TRUE)
endif()
if(STRING_BIMAP_ARRAY_HASH_INCLUDE_DIR)
    set(STRING_BIMAP_CONFIG_WITH_ARRAY_HASH TRUE)
endif()
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/string_bimapConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/string_bimapConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/string_bimap"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/string_bimapConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
)
install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/string_bimapConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/string_bimapConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/string_bimap"
)

if(STRING_BIMAP_BUILD_EXAMPLES)
    add_executable(string_bimap_basic_example examples/basic.cpp)
    target_link_libraries(string_bimap_basic_example PRIVATE string_bimap::string_bimap)

    if(TARGET PTHASH)
        add_executable(string_bimap_pthash_example examples/pthash_bimap.cpp)
        target_link_libraries(string_bimap_pthash_example PRIVATE string_bimap::string_bimap)
    endif()
endif()

if(TARGET PTHASH AND (STRING_BIMAP_BUILD_TOOLS OR STRING_BIMAP_BUILD_TESTS))
    add_executable(string_bimap_pthash_tool tools/pthash_bimap_tool.cpp)
    target_link_libraries(string_bimap_pthash_tool PRIVATE string_bimap::string_bimap)
endif()

if(STRING_BIMAP_BUILD_TESTS)
    enable_testing()
    add_executable(string_bimap_core_test tests/core_test.cpp)
    target_link_libraries(string_bimap_core_test PRIVATE string_bimap::string_bimap)
    add_test(NAME string_bimap_core_test COMMAND string_bimap_core_test)

    add_executable(string_bimap_persistence_test tests/persistence_test.cpp)
    target_link_libraries(string_bimap_persistence_test PRIVATE string_bimap::string_bimap)
    add_test(NAME string_bimap_persistence_test COMMAND string_bimap_persistence_test)

    add_executable(string_bimap_query_test tests/query_test.cpp)
    target_link_libraries(string_bimap_query_test PRIVATE string_bimap::string_bimap)
    add_test(NAME string_bimap_query_test COMMAND string_bimap_query_test)

    if(TARGET PTHASH)
        add_executable(string_bimap_pthash_bimap_test tests/pthash_bimap_test.cpp)
        target_link_libraries(string_bimap_pthash_bimap_test PRIVATE string_bimap::string_bimap)
        add_test(NAME string_bimap_pthash_bimap_test COMMAND string_bimap_pthash_bimap_test)

        add_executable(string_bimap_pthash_tool_test tests/pthash_tool_test.cpp)
        target_link_libraries(string_bimap_pthash_tool_test PRIVATE string_bimap::string_bimap)
        add_dependencies(string_bimap_pthash_tool_test string_bimap_pthash_tool)
        add_test(
            NAME string_bimap_pthash_tool_test
            COMMAND string_bimap_pthash_tool_test $<TARGET_FILE:string_bimap_pthash_tool>
        )
    endif()
endif()

if(STRING_BIMAP_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "STRING_BIMAP_BUILD_FUZZERS requires Clang")
    endif()

    add_executable(string_bimap_load_fuzzer tests/fuzz_load.cpp)
    target_link_libraries(string_bimap_load_fuzzer PRIVATE string_bimap::string_bimap)
    target_compile_options(
        string_bimap_load_fuzzer
        PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer
    )
    target_link_options(
        string_bimap_load_fuzzer
        PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer
    )
endif()

if(STRING_BIMAP_BUILD_BENCHMARKS)
    add_executable(string_bimap_bench bench/string_bimap_bench.cpp)
    target_link_libraries(string_bimap_bench PRIVATE string_bimap::string_bimap)

    if(STRING_BIMAP_BUILD_TESTS)
        add_test(
            NAME string_bimap_benchmark_smoke_test
            COMMAND
                ${CMAKE_COMMAND}
                -DBENCHMARK_EXECUTABLE=$<TARGET_FILE:string_bimap_bench>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_smoke.cmake
        )
    endif()

    if(TARGET PTHASH)
        add_executable(string_bimap_pthash_bench bench/pthash_bimap_bench.cpp)
        target_link_libraries(string_bimap_pthash_bench PRIVATE string_bimap::string_bimap)
    endif()
endif()

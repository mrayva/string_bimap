cmake_minimum_required(VERSION 3.20)

project(string_bimap LANGUAGES CXX VERSION 0.1.0)

option(STRING_BIMAP_BUILD_EXAMPLES "Build examples" ON)
option(STRING_BIMAP_BUILD_TESTS "Build test executables" ON)
option(STRING_BIMAP_BUILD_BENCHMARKS "Build benchmark executables" ON)
option(STRING_BIMAP_USE_XCDAT "Enable xcdat-backed static index when xcdat headers are available" ON)
option(STRING_BIMAP_USE_MARISA "Enable marisa-trie-backed static index when marisa headers and library are available" ON)
option(STRING_BIMAP_USE_KEYVI "Enable keyvi-backed static index when keyvi headers and dependencies are available" ON)
option(STRING_BIMAP_USE_FSST "Enable FSST-compressed static payload backend when a local FSST checkout is available" ON)
option(STRING_BIMAP_USE_HAT_TRIE "Enable hat-trie-backed mutable index when hat-trie headers are available" ON)

set(STRING_BIMAP_KEYVI_ROOT "" CACHE PATH "Optional keyvi source or install root")
set(STRING_BIMAP_FSST_ROOT "" CACHE PATH "Optional FSST source root")

add_library(string_bimap INTERFACE)
add_library(string_bimap::string_bimap ALIAS string_bimap)

target_compile_features(string_bimap INTERFACE cxx_std_17)
target_include_directories(
    string_bimap
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

if(STRING_BIMAP_USE_FSST)
    set(STRING_BIMAP_FSST_HEADER "")
    set(STRING_BIMAP_FSST_SOURCE "")
    if(STRING_BIMAP_FSST_ROOT)
        if(EXISTS "${STRING_BIMAP_FSST_ROOT}/fsst.h" AND EXISTS "${STRING_BIMAP_FSST_ROOT}/libfsst.cpp")
            set(STRING_BIMAP_FSST_HEADER "${STRING_BIMAP_FSST_ROOT}")
            set(STRING_BIMAP_FSST_SOURCE "${STRING_BIMAP_FSST_ROOT}/libfsst.cpp")
        endif()
    endif()
    if(STRING_BIMAP_FSST_HEADER AND STRING_BIMAP_FSST_SOURCE)
        add_library(string_bimap_fsst STATIC "${STRING_BIMAP_FSST_SOURCE}")
        set(STRING_BIMAP_FSST_EXTRA_INCLUDE_DIRS "")
        if(EXISTS "${STRING_BIMAP_FSST_ROOT}/../../src/include")
            list(APPEND STRING_BIMAP_FSST_EXTRA_INCLUDE_DIRS "${STRING_BIMAP_FSST_ROOT}/../../src/include")
        endif()
        target_include_directories(
            string_bimap_fsst
            PUBLIC
                $<BUILD_INTERFACE:${STRING_BIMAP_FSST_HEADER}>
        )
        foreach(dir IN LISTS STRING_BIMAP_FSST_EXTRA_INCLUDE_DIRS)
            if(dir AND EXISTS "${dir}")
                target_include_directories(
                    string_bimap_fsst
                    PUBLIC
                        $<BUILD_INTERFACE:${dir}>
                )
            endif()
        endforeach()
        target_compile_features(string_bimap_fsst PUBLIC cxx_std_17)
        target_compile_definitions(string_bimap_fsst PRIVATE DUCKDB_DEBUG_NO_SAFETY=1)
        target_link_libraries(string_bimap INTERFACE $<BUILD_INTERFACE:string_bimap_fsst>)
        target_compile_definitions(string_bimap INTERFACE STRING_BIMAP_HAS_FSST=1)
    endif()
endif()

if(STRING_BIMAP_USE_XCDAT)
    find_path(STRING_BIMAP_XCDAT_INCLUDE_DIR xcdat.hpp)
    if(STRING_BIMAP_XCDAT_INCLUDE_DIR)
        target_include_directories(
            string_bimap
            INTERFACE
                $<BUILD_INTERFACE:${STRING_BIMAP_XCDAT_INCLUDE_DIR}>
        )
        target_compile_definitions(string_bimap INTERFACE STRING_BIMAP_HAS_XCDAT=1)
    endif()
endif()

if(STRING_BIMAP_USE_MARISA)
    find_path(STRING_BIMAP_MARISA_INCLUDE_DIR marisa.h)
    find_library(STRING_BIMAP_MARISA_LIBRARY NAMES marisa libmarisa)
    if(STRING_BIMAP_MARISA_INCLUDE_DIR AND STRING_BIMAP_MARISA_LIBRARY)
        target_include_directories(
            string_bimap
            INTERFACE
                $<BUILD_INTERFACE:${STRING_BIMAP_MARISA_INCLUDE_DIR}>
        )
        target_link_libraries(
            string_bimap
            INTERFACE
                $<BUILD_INTERFACE:${STRING_BIMAP_MARISA_LIBRARY}>
        )
        target_compile_definitions(string_bimap INTERFACE STRING_BIMAP_HAS_MARISA=1)
    endif()
endif()

if(STRING_BIMAP_USE_KEYVI)
    set(STRING_BIMAP_KEYVI_INCLUDE_DIR "")
    if(STRING_BIMAP_KEYVI_ROOT)
        if(EXISTS "${STRING_BIMAP_KEYVI_ROOT}/include/keyvi/dictionary/dictionary.h")
            set(STRING_BIMAP_KEYVI_INCLUDE_DIR "${STRING_BIMAP_KEYVI_ROOT}/include")
            set(STRING_BIMAP_KEYVI_EXTRA_INCLUDE_DIRS
                "${STRING_BIMAP_KEYVI_ROOT}/3rdparty/rapidjson/include"
                "${STRING_BIMAP_KEYVI_ROOT}/3rdparty/msgpack-c/include"
                "${STRING_BIMAP_KEYVI_ROOT}/3rdparty/utfcpp/source"
            )
        elseif(EXISTS "${STRING_BIMAP_KEYVI_ROOT}/keyvi/include/keyvi/dictionary/dictionary.h")
            set(STRING_BIMAP_KEYVI_INCLUDE_DIR "${STRING_BIMAP_KEYVI_ROOT}/keyvi/include")
            set(STRING_BIMAP_KEYVI_EXTRA_INCLUDE_DIRS
                "${STRING_BIMAP_KEYVI_ROOT}/keyvi/3rdparty/rapidjson/include"
                "${STRING_BIMAP_KEYVI_ROOT}/keyvi/3rdparty/msgpack-c/include"
                "${STRING_BIMAP_KEYVI_ROOT}/keyvi/3rdparty/utfcpp/source"
            )
        endif()
    endif()

    if(NOT STRING_BIMAP_KEYVI_INCLUDE_DIR)
        find_path(STRING_BIMAP_KEYVI_INCLUDE_DIR keyvi/dictionary/dictionary.h)
        find_path(STRING_BIMAP_RAPIDJSON_INCLUDE_DIR rapidjson/document.h)
        find_path(STRING_BIMAP_MSGPACK_INCLUDE_DIR msgpack.hpp)
        find_path(STRING_BIMAP_UTF8CPP_INCLUDE_DIR utf8.h)
        set(STRING_BIMAP_KEYVI_EXTRA_INCLUDE_DIRS
            "${STRING_BIMAP_RAPIDJSON_INCLUDE_DIR}"
            "${STRING_BIMAP_MSGPACK_INCLUDE_DIR}"
            "${STRING_BIMAP_UTF8CPP_INCLUDE_DIR}"
        )
    endif()

    find_library(STRING_BIMAP_SNAPPY_LIBRARY NAMES snappy libsnappy)
    find_library(
        STRING_BIMAP_BOOST_FILESYSTEM_LIBRARY
        NAMES
            boost_filesystem
            libboost_filesystem.so.1.88.0
        PATH_SUFFIXES
            lib
            lib64
            x86_64-linux-gnu
    )
    find_package(ZLIB QUIET)
    find_library(STRING_BIMAP_ZSTD_LIBRARY NAMES zstd libzstd)

    if(STRING_BIMAP_KEYVI_INCLUDE_DIR
       AND STRING_BIMAP_SNAPPY_LIBRARY
       AND STRING_BIMAP_BOOST_FILESYSTEM_LIBRARY
       AND STRING_BIMAP_ZSTD_LIBRARY
       AND ZLIB_FOUND)
        target_include_directories(
            string_bimap
            INTERFACE
                $<BUILD_INTERFACE:${STRING_BIMAP_KEYVI_INCLUDE_DIR}>
        )
        foreach(dir IN LISTS STRING_BIMAP_KEYVI_EXTRA_INCLUDE_DIRS)
            if(dir AND EXISTS "${dir}")
                target_include_directories(
                    string_bimap
                    INTERFACE
                        $<BUILD_INTERFACE:${dir}>
                )
            endif()
        endforeach()
        target_link_libraries(
            string_bimap
            INTERFACE
                $<BUILD_INTERFACE:${STRING_BIMAP_SNAPPY_LIBRARY}>
                $<BUILD_INTERFACE:${STRING_BIMAP_BOOST_FILESYSTEM_LIBRARY}>
                $<BUILD_INTERFACE:${STRING_BIMAP_ZSTD_LIBRARY}>
                ZLIB::ZLIB
        )
        target_compile_definitions(
            string_bimap
            INTERFACE
                STRING_BIMAP_HAS_KEYVI=1
                RAPIDJSON_HAS_STDSTRING=1
        )
    endif()
endif()

if(STRING_BIMAP_USE_HAT_TRIE)
    find_path(STRING_BIMAP_HAT_TRIE_INCLUDE_DIR tsl/htrie_map.h)
    if(STRING_BIMAP_HAT_TRIE_INCLUDE_DIR)
        target_include_directories(
            string_bimap
            INTERFACE
                $<BUILD_INTERFACE:${STRING_BIMAP_HAT_TRIE_INCLUDE_DIR}>
        )
        target_compile_definitions(string_bimap INTERFACE STRING_BIMAP_HAS_HAT_TRIE=1)
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
endif()

if(STRING_BIMAP_BUILD_TESTS)
    enable_testing()
    add_executable(string_bimap_smoke_test tests/smoke.cpp)
    target_link_libraries(string_bimap_smoke_test PRIVATE string_bimap::string_bimap)
    add_test(NAME string_bimap_smoke_test COMMAND string_bimap_smoke_test)
endif()

if(STRING_BIMAP_BUILD_BENCHMARKS)
    add_executable(string_bimap_bench bench/string_bimap_bench.cpp)
    target_link_libraries(string_bimap_bench PRIVATE string_bimap::string_bimap)
endif()

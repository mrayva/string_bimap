cmake_minimum_required(VERSION 3.20)

project(string_bimap LANGUAGES CXX VERSION 0.1.0)

option(STRING_BIMAP_BUILD_EXAMPLES "Build examples" ON)
option(STRING_BIMAP_BUILD_TESTS "Build test executables" ON)
option(STRING_BIMAP_USE_XCDAT "Enable xcdat-backed static index when xcdat headers are available" ON)
option(STRING_BIMAP_USE_HAT_TRIE "Enable hat-trie-backed mutable index when hat-trie headers are available" ON)

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
            INTERFACE
                $<BUILD_INTERFACE:${STRING_BIMAP_XCDAT_INCLUDE_DIR}>
        )
        target_compile_definitions(string_bimap INTERFACE STRING_BIMAP_HAS_XCDAT=1)
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

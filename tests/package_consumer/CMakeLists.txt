cmake_minimum_required(VERSION 3.20)

project(string_bimap_package_consumer LANGUAGES CXX)

find_package(string_bimap CONFIG REQUIRED)

add_executable(string_bimap_package_consumer main.cpp)
target_link_libraries(string_bimap_package_consumer PRIVATE string_bimap::string_bimap)
target_compile_features(string_bimap_package_consumer PRIVATE cxx_std_17)

if(STRING_BIMAP_EXPECT_OPTIONAL_BACKENDS)
    target_compile_definitions(
        string_bimap_package_consumer
        PRIVATE STRING_BIMAP_EXPECT_OPTIONAL_BACKENDS=1
    )
endif()

enable_testing()
add_test(NAME string_bimap_package_consumer COMMAND string_bimap_package_consumer)

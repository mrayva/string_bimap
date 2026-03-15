set(source_include_dir "${CURRENT_PORT_DIR}/include")

if(NOT EXISTS "${source_include_dir}/xcdat.hpp")
    message(FATAL_ERROR "xcdat overlay port is missing vendored headers")
endif()

file(INSTALL "${source_include_dir}/" DESTINATION "${CURRENT_PACKAGES_DIR}/include")
file(INSTALL "${CURRENT_PORT_DIR}/copyright" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)

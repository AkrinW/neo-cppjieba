cmake_minimum_required(VERSION 3.20)
project(c_api_consumer LANGUAGES C)

if(C_API_VARIANT STREQUAL "static")
    # Static consumers need a C++ linker/runtime even when all their source files are C.
    enable_language(CXX)
endif()
# The package test passes the version from the main project's project() call.
if(NOT CPPJIEBA_TEST_PACKAGE_VERSION)
    message(FATAL_ERROR "Set CPPJIEBA_TEST_PACKAGE_VERSION to the requested neo_cppjieba_c version")
endif()
find_package(neo_cppjieba_c ${CPPJIEBA_TEST_PACKAGE_VERSION} CONFIG REQUIRED COMPONENTS ${C_API_VARIANT})
add_executable(c_api_consumer ../unittest_neo/c_api_smoke.c)
set_target_properties(c_api_consumer PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_compile_options(c_api_consumer PRIVATE "$<$<C_COMPILER_ID:MSVC>:/utf-8>")
target_include_directories(c_api_consumer PRIVATE "${TEST_PATHS_DIR}")
target_link_libraries(c_api_consumer PRIVATE neo_cppjieba::c_${C_API_VARIANT})
if(WIN32 AND C_API_VARIANT STREQUAL "shared")
    add_custom_command(TARGET c_api_consumer POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:neo_cppjieba::c_shared>" "$<TARGET_FILE_DIR:c_api_consumer>" VERBATIM)
endif()
enable_testing()
add_test(NAME installed_c_api_consumer COMMAND c_api_consumer)

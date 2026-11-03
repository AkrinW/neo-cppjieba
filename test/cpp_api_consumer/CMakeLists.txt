cmake_minimum_required(VERSION 3.20)
project(cpp_api_consumer LANGUAGES CXX)

# The package test passes the version from the main project's project() call.
if(NOT CPPJIEBA_TEST_PACKAGE_VERSION)
    message(FATAL_ERROR "Set CPPJIEBA_TEST_PACKAGE_VERSION to the requested neo_cppjieba version")
endif()
if(CPPJIEBA_TEST_OPTIONAL)
    find_package(neo_cppjieba ${CPPJIEBA_TEST_PACKAGE_VERSION} CONFIG REQUIRED
        OPTIONAL_COMPONENTS ${CPPJIEBA_TEST_COMPONENT})
elseif(CPPJIEBA_TEST_COMPONENT)
    find_package(neo_cppjieba ${CPPJIEBA_TEST_PACKAGE_VERSION} CONFIG REQUIRED COMPONENTS ${CPPJIEBA_TEST_COMPONENT})
else()
    find_package(neo_cppjieba ${CPPJIEBA_TEST_PACKAGE_VERSION} CONFIG REQUIRED)
endif()

add_executable(cpp_api_consumer ../../examples/basic_cut.cpp)
target_link_libraries(cpp_api_consumer PRIVATE neo_cppjieba::neo_cppjieba)
if(MSVC)
    target_sources(cpp_api_consumer PRIVATE ../../cmake/utf8.manifest)
endif()
enable_testing()
add_test(NAME installed_cpp_api_consumer COMMAND cpp_api_consumer "${neo_cppjieba_DICT_DIR}" "我爱北京天安门")
set_tests_properties(installed_cpp_api_consumer PROPERTIES
    PASS_REGULAR_EXPRESSION "MIX: \\[我\\] \\[爱\\] \\[北京\\] \\[天安门\\]")

if(CPPJIEBA_TEST_COMPONENT STREQUAL "icu" AND NOT CPPJIEBA_TEST_OPTIONAL)
    add_executable(icu_consumer icu_smoke.cpp)
    target_link_libraries(icu_consumer PRIVATE neo_cppjieba::icu)
    add_test(NAME installed_icu_consumer COMMAND icu_consumer)
elseif(TARGET neo_cppjieba::icu)
    message(FATAL_ERROR "The core-only consumer unexpectedly loaded the ICU target")
endif()

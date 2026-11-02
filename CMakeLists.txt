cmake_minimum_required(VERSION 3.20)
project(neo_cppjieba VERSION 1.2.0 LANGUAGES CXX)

if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 23)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/CppJiebaOptions.cmake)
include(GNUInstallDirs)

add_library(neo_cppjieba INTERFACE)
add_library(neo_cppjieba::neo_cppjieba ALIAS neo_cppjieba)
target_compile_features(neo_cppjieba INTERFACE cxx_std_23)
target_compile_options(neo_cppjieba INTERFACE "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
target_include_directories(neo_cppjieba INTERFACE
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

if(CPPJIEBA_ENABLE_ICU)
    find_package(ICU REQUIRED COMPONENTS uc data)
    message(STATUS "ICU conversion: version ${ICU_VERSION}, headers ${ICU_INCLUDE_DIRS}")
    add_library(neo_cppjieba_icu INTERFACE)
    add_library(neo_cppjieba::icu ALIAS neo_cppjieba_icu)
    set_target_properties(neo_cppjieba_icu PROPERTIES EXPORT_NAME icu)
    target_link_libraries(neo_cppjieba_icu INTERFACE neo_cppjieba::neo_cppjieba ICU::uc ICU::data)
endif()

include(cmake/CppJiebaInstall.cmake)

if(CPPJIEBA_BUILD_LEGACY_TESTS OR CPPJIEBA_BUILD_BENCHMARKS)
    add_subdirectory(deps)
endif()

if(BUILD_TESTING OR CPPJIEBA_BUILD_BENCHMARKS OR CPPJIEBA_BUILD_FUZZERS)
    include(cmake/CppJiebaTestData.cmake)
endif()

if(CPPJIEBA_BUILD_C_API)
    if(BUILD_TESTING)
        enable_language(C)
    endif()
    add_subdirectory(api)
endif()

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(test)
endif()
if(CPPJIEBA_BUILD_BENCHMARKS)
    add_subdirectory(benchmark)
endif()
if(CPPJIEBA_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
if(BUILD_TESTING OR CPPJIEBA_BUILD_FUZZERS)
    add_subdirectory(test/fuzz)
endif()

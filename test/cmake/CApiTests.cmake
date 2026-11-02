# Included in unittest_neo's directory scope so relative test sources and script paths stay local to it.

# Exercise the same ownership and error contract through each enabled library variant.
function(cppjieba_add_c_api_tests name library)
    add_executable(test_${name}.run ../gtest_main.cpp c_api_test.cpp c_api_ranges_test.cpp c_api_sources_test.cpp
        c_api_edit_test.cpp c_api_keyword_test.cpp)
    target_link_libraries(test_${name}.run PRIVATE
        ${library} neo_cppjieba::neo_cppjieba cppjieba_test_support)
    add_test(NAME neo_${name}_tests COMMAND test_${name}.run)

    # c_api_abi_v1.c only holds compile-time checks that freeze the v1 types, layouts, values and function types.
    add_executable(test_${name}_smoke.run c_api_smoke.c c_api_abi_v1.c)
    set_target_properties(test_${name}_smoke.run PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(test_${name}_smoke.run PRIVATE /utf-8)
    endif()
    target_link_libraries(test_${name}_smoke.run PRIVATE ${library} cppjieba_test_data)
    add_test(NAME neo_${name}_c_host COMMAND test_${name}_smoke.run)
endfunction()

if(CPPJIEBA_C_API_BUILD_SHARED)
    cppjieba_add_c_api_tests(c_api neo_cppjieba::c_shared)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(test_c_api_smoke.run PRIVATE NEO_CPPJIEBA_C_TEST_PRIVATE_RUNTIME)
        target_link_options(test_c_api_smoke.run PRIVATE "LINKER:--export-dynamic")
        find_program(NEO_C_API_NM NAMES llvm-nm nm REQUIRED)
        find_program(NEO_C_API_READELF NAMES llvm-readelf readelf REQUIRED)
    elseif(APPLE)
        find_program(NEO_C_API_NM NAMES llvm-nm nm REQUIRED)
        find_program(NEO_C_API_OTOOL NAMES llvm-otool otool REQUIRED)
    elseif(MSVC)
        get_filename_component(c_api_msvc_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        find_program(NEO_C_API_DUMPBIN NAMES dumpbin HINTS "${c_api_msvc_dir}" REQUIRED)
    endif()
    add_test(NAME neo_c_api_symbols COMMAND ${CMAKE_COMMAND}
        "-DLIBRARY=$<TARGET_FILE:neo_cppjieba_c>"
        "-DPLATFORM=${CMAKE_SYSTEM_NAME}"
        "-DNM=${NEO_C_API_NM}" "-DREADELF=${NEO_C_API_READELF}"
        "-DOTOOL=${NEO_C_API_OTOOL}" "-DDUMPBIN=${NEO_C_API_DUMPBIN}"
        -P "${CMAKE_CURRENT_LIST_DIR}/CheckCApiSymbols.cmake")
endif()
if(CPPJIEBA_C_API_BUILD_STATIC)
    cppjieba_add_c_api_tests(c_api_static neo_cppjieba::c_static)
endif()

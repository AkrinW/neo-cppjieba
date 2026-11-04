# GCC stops inlining once a translation unit spends its inline-unit-growth budget. The probe spends it up front at
# -O2, whose early-inliner limits are the strictest, so every helper still inlined there is inlined in any build.
# NDEBUG drops the debug-only assert_check predicates, which UBSan instrumentation grows past the early-inliner limits.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_NM)
    add_executable(neo_inline_probe "${CMAKE_CURRENT_LIST_DIR}/inline_probe.cpp")
    target_compile_definitions(neo_inline_probe PRIVATE NDEBUG)
    target_compile_options(neo_inline_probe PRIVATE -O2 --param=inline-unit-growth=0)
    target_link_libraries(neo_inline_probe PRIVATE neo_cppjieba::neo_cppjieba)
    add_test(NAME neo_inline_hot_paths COMMAND "${CMAKE_COMMAND}" "-DNM=${CMAKE_NM}"
        "-DBINARY=$<TARGET_FILE:neo_inline_probe>" -P "${CMAKE_CURRENT_LIST_DIR}/CheckInlining.cmake")
endif()

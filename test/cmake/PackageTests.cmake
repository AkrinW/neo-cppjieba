# Forward the active toolchain to each installed-package consumer, including multi-value settings.
function(cppjieba_add_package_test name script)
    set(arguments
        "-DBUILD_DIR=${PROJECT_BINARY_DIR}" "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DINSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}" "-DINSTALL_DATADIR=${CMAKE_INSTALL_DATADIR}"
        "-DCONFIG=$<CONFIG>" "-DCTEST=${CMAKE_CTEST_COMMAND}"
        "-DBUILD_SHARED=${CPPJIEBA_C_API_BUILD_SHARED}" "-DBUILD_STATIC=${CPPJIEBA_C_API_BUILD_STATIC}"
        "-DENABLE_ICU=${CPPJIEBA_ENABLE_ICU}" "-DICU_ROOT=${ICU_ROOT}"
        "-DPACKAGE_REQUEST_VERSION=${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
    foreach(setting IN ITEMS GENERATOR GENERATOR_PLATFORM GENERATOR_TOOLSET
            C_COMPILER CXX_COMPILER C_FLAGS CXX_FLAGS EXE_LINKER_FLAGS TOOLCHAIN_FILE PREFIX_PATH
            MSVC_RUNTIME_LIBRARY OSX_ARCHITECTURES OSX_SYSROOT OSX_DEPLOYMENT_TARGET)
        string(REPLACE ";" "\\;" value "${CMAKE_${setting}}")
        list(APPEND arguments "-D${setting}=${value}")
    endforeach()
    add_test(NAME ${name} COMMAND "${CMAKE_COMMAND}" ${arguments} -P "${script}")
endfunction()

cppjieba_add_package_test(neo_cpp_api_install "${CMAKE_CURRENT_LIST_DIR}/CheckCppApiInstall.cmake")
if(CPPJIEBA_BUILD_C_API)
    cppjieba_add_package_test(neo_c_api_install "${CMAKE_CURRENT_LIST_DIR}/CheckCApiInstall.cmake")
endif()

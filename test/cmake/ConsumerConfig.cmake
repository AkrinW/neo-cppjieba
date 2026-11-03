# Consumers request the project's major.minor version, as the documented find_package calls do.
set(configure_args -G "${GENERATOR}" "-DCMAKE_BUILD_TYPE=${CONFIG}"
    "-DCPPJIEBA_TEST_PACKAGE_VERSION=${PACKAGE_REQUEST_VERSION}")
if(NOT GENERATOR_PLATFORM STREQUAL "")
    list(APPEND configure_args -A "${GENERATOR_PLATFORM}")
endif()
if(NOT GENERATOR_TOOLSET STREQUAL "")
    list(APPEND configure_args -T "${GENERATOR_TOOLSET}")
endif()

foreach(setting IN ITEMS C_COMPILER CXX_COMPILER C_FLAGS CXX_FLAGS TOOLCHAIN_FILE PREFIX_PATH
        MSVC_RUNTIME_LIBRARY OSX_ARCHITECTURES OSX_SYSROOT OSX_DEPLOYMENT_TARGET)
    if(setting MATCHES "^(C|CXX)_FLAGS$" OR NOT "${${setting}}" STREQUAL "")
        string(REPLACE ";" "\\;" value "${${setting}}")
        list(APPEND configure_args "-DCMAKE_${setting}=${value}")
    endif()
endforeach()

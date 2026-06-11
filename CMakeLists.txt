cmake_minimum_required(VERSION 3.24)
project(MHD_Finder)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
if(MSVC)
    add_compile_options("$<$<NOT:$<CONFIG:Debug>>:/O2>")
    add_compile_options(/arch:AVX2 /W3)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options("$<$<NOT:$<CONFIG:Debug>>:-O3>")
    add_compile_options(-march=native -Wall)
endif()

# Zlib is a dependency of libzip, and Windows doesn't have it by default
include(FetchContent)
set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG v1.3.1
)
FetchContent_MakeAvailable(zlib)

if(TARGET zlibstatic)
    set(ZLIB_FOUND TRUE CACHE BOOL "" FORCE)
    set(ZLIB_LIBRARY zlibstatic CACHE FILEPATH "" FORCE)
    set(ZLIB_INCLUDE_DIR "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}" CACHE PATH "" FORCE)
    if(NOT TARGET ZLIB::ZLIB)
        add_library(ZLIB::ZLIB ALIAS zlibstatic)
    endif()
endif()

# TLS support: OpenSSL if available, otherwise MbedTLS
find_package(OpenSSL QUIET)

if(OpenSSL_FOUND OR OPENSSL_FOUND)
    message(STATUS "Using system OpenSSL")
    add_compile_definitions(CPPHTTPLIB_OPENSSL_SUPPORT)
    set(TLS_LIBRARIES OpenSSL::SSL OpenSSL::Crypto)
    set(TLS_INCLUDE_DIRS "")
else()
    message(STATUS "OpenSSL not found, using MbedTLS")
    add_compile_definitions(CPPHTTPLIB_MBEDTLS_SUPPORT)

    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ENABLE_DOCS OFF CACHE BOOL "" FORCE)
    set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
    set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(mbedtls
        GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
        GIT_TAG v3.6.3
    )
    FetchContent_MakeAvailable(mbedtls)
    set(TLS_LIBRARIES MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
    set(TLS_INCLUDE_DIRS ${mbedtls_SOURCE_DIR}/include)
endif()

# Libzip: try system first, then fetch if not found
find_package(libzip QUIET)

if(NOT libzip_FOUND)
    message(STATUS "System libzip not found, fetching")
    set(LIBZIP_DO_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(libzip
        GIT_REPOSITORY https://github.com/nih-at/libzip.git
        GIT_TAG v1.10.1
    )
    FetchContent_MakeAvailable(libzip)
    set(LIBZIP_TARGET zip)
else()
    message(STATUS "Using system libzip")
    set(LIBZIP_TARGET libzip::zip)
endif()


# Tell HTTPLib about TLS libraries
if(TARGET httplib)
    if(TLS_INCLUDE_DIRS)
        target_include_directories(httplib INTERFACE ${TLS_INCLUDE_DIRS})
    endif()
    target_link_libraries(httplib INTERFACE ${TLS_LIBRARIES})
endif()

# Main lib to link into executable
add_library(mhd_core STATIC
    src/network.cpp
    src/gtfs_loader.cpp
    src/query.cpp
    src/query_search.cpp
    src/mhd_core.cpp
)
target_include_directories(mhd_core PUBLIC
    include
    ${TLS_INCLUDE_DIRS}
)
target_link_libraries(mhd_core PUBLIC
    ${LIBZIP_TARGET}
    ${TLS_LIBRARIES}
)
if(WIN32)
    target_link_libraries(mhd_core PUBLIC ws2_32 crypt32)
endif()

# Add main project executable
add_executable(mhd_finder src/main.cpp)
target_link_libraries(mhd_finder PRIVATE mhd_core)

add_executable(memory_stress test/memory_stress.cpp)
target_link_libraries(memory_stress PRIVATE mhd_core)

add_executable(csv_test test/csv_test.cpp)
target_link_libraries(csv_test PRIVATE mhd_core)

add_executable(maso src/maso.cpp)
target_link_libraries(maso PRIVATE mhd_core)

add_executable(maso_para src/maso_para.cpp)
find_package(OpenMP REQUIRED)
if(OpenMP_CXX_FOUND)
    target_link_libraries(maso_para PRIVATE OpenMP::OpenMP_CXX mhd_core)
else()
    message(WARNING "OpenMP not found, maso_para will be built without parallel support")
    target_link_libraries(maso_para PRIVATE mhd_core)
endif()


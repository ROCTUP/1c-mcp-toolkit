cmake_minimum_required(VERSION 3.16)
project(MCPHttpTransport LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT linkage — no dependency on vcruntime140.dll
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# x86 and x64 are both supported

# Download cpp-httplib (header-only) if not present
set(HTTPLIB_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/httplib.h")
if(NOT EXISTS "${HTTPLIB_HEADER}")
    set(HTTPLIB_VERSION "v0.18.3")
    message(STATUS "Downloading cpp-httplib ${HTTPLIB_VERSION}...")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/yhirose/cpp-httplib/${HTTPLIB_VERSION}/httplib.h"
        "${HTTPLIB_HEADER}"
        STATUS DOWNLOAD_STATUS
    )
    list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
    if(NOT STATUS_CODE EQUAL 0)
        list(GET DOWNLOAD_STATUS 1 ERROR_MSG)
        file(REMOVE "${HTTPLIB_HEADER}")
        message(FATAL_ERROR "Failed to download cpp-httplib: ${ERROR_MSG}")
    endif()
    message(STATUS "cpp-httplib downloaded to ${HTTPLIB_HEADER}")
endif()

# Sources
set(SOURCES
    src/dllmain.cpp
    src/component.cpp
    src/http_transport.cpp
    src/pending_requests.cpp
    src/sse_stream.cpp
)

set(HEADERS
    src/component.h
    src/http_transport.h
    src/pending_requests.h
    src/sse_stream.h
)

# DLL target
add_library(${PROJECT_NAME} SHARED ${SOURCES} ${HEADERS})

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Windows-specific settings
if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _WINDOWS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
    # Link ws2_32 for cpp-httplib (sockets)
    target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32)

    # Export symbols for 1C Native API via .def file
    set_target_properties(${PROJECT_NAME} PROPERTIES
        PREFIX ""
        OUTPUT_NAME "MCPHttpTransport"
        LINK_FLAGS "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/MCPHttpTransport.def"
    )
endif()

# cpp-httplib: do NOT define CPPHTTPLIB_OPENSSL_SUPPORT — we don't use TLS
# CPPHTTPLIB_NO_EXCEPTIONS is defined in http_transport.cpp before the include

# Compiler warnings
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W3 /utf-8)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
endif()

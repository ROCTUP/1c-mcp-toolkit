cmake_minimum_required(VERSION 3.16)
project(MCPHttpTransport LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

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

# cpp-httplib 0.18.3 ignores failure when flushing response headers. Keep the
# downloaded dependency untouched; every target uses this build-local copy.
file(READ "${HTTPLIB_HEADER}" HTTPLIB_SOURCE)
set(HEADER_FLUSH_OLD "    // Flush buffer\n    auto &data = bstrm.get_buffer();\n    detail::write_data(strm, data.data(), data.size());")
set(HEADER_FLUSH_FIXED "    // Flush buffer\n    auto &data = bstrm.get_buffer();\n    if (!detail::write_data(strm, data.data(), data.size())) { return false; }")
string(REPLACE "\r\n" "\n" HTTPLIB_SOURCE "${HTTPLIB_SOURCE}")
# Accept exactly one known context, including a previously fixed dependency.
string(REPLACE "${HEADER_FLUSH_FIXED}" "${HEADER_FLUSH_OLD}" HTTPLIB_SOURCE "${HTTPLIB_SOURCE}")
string(FIND "${HTTPLIB_SOURCE}" "${HEADER_FLUSH_OLD}" FLUSH_POSITION)
if(FLUSH_POSITION GREATER_EQUAL 0)
    string(REPLACE "${HEADER_FLUSH_OLD}" "" FLUSH_WITHOUT "${HTTPLIB_SOURCE}")
    string(LENGTH "${HTTPLIB_SOURCE}" FLUSH_FULL_LENGTH)
    string(LENGTH "${FLUSH_WITHOUT}" FLUSH_WITHOUT_LENGTH)
    string(LENGTH "${HEADER_FLUSH_OLD}" FLUSH_EXPECTED_LENGTH)
    math(EXPR FLUSH_REMOVED_LENGTH "${FLUSH_FULL_LENGTH}-${FLUSH_WITHOUT_LENGTH}")
    if(NOT FLUSH_REMOVED_LENGTH EQUAL FLUSH_EXPECTED_LENGTH)
        message(FATAL_ERROR "Ambiguous cpp-httplib response header flush context")
    endif()
    string(REPLACE "${HEADER_FLUSH_OLD}" "${HEADER_FLUSH_FIXED}" HTTPLIB_SOURCE "${HTTPLIB_SOURCE}")
else()
    message(FATAL_ERROR "Unsupported cpp-httplib response header flush context")
endif()
set(HTTPLIB_PATCHED_DIR "${CMAKE_CURRENT_BINARY_DIR}/patched-httplib")
file(MAKE_DIRECTORY "${HTTPLIB_PATCHED_DIR}")
file(WRITE "${HTTPLIB_PATCHED_DIR}/httplib.h" "${HTTPLIB_SOURCE}")

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
    src/event_queue.h
)

# DLL target
add_library(${PROJECT_NAME} SHARED ${SOURCES} ${HEADERS})

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    ${HTTPLIB_PATCHED_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)

# Windows-specific settings
if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _WINDOWS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
    # Link ws2_32 for cpp-httplib (sockets); iphlpapi for GetAdaptersAddresses (GetLocalAddresses)
    target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 iphlpapi)

    # Export symbols for 1C Native API via .def file
    set_target_properties(${PROJECT_NAME} PROPERTIES
        PREFIX ""
        OUTPUT_NAME "MCPHttpTransport"
        LINK_FLAGS "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/MCPHttpTransport.def"
    )
else()
    set_target_properties(${PROJECT_NAME} PROPERTIES
        PREFIX ""
        OUTPUT_NAME "MCPHttpTransport"
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

option(MCPHTTPTRANSPORT_SMOKE_TEST "Build smoke test" OFF)
if(MCPHTTPTRANSPORT_SMOKE_TEST)
    enable_testing()
    add_executable(mcp_smoke_test
        tests/smoke_test.cpp
        src/component.cpp
        src/http_transport.cpp
        src/pending_requests.cpp
        src/sse_stream.cpp
    )
    target_include_directories(mcp_smoke_test PRIVATE
        ${HTTPLIB_PATCHED_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(mcp_smoke_test PRIVATE MCPHTTPTRANSPORT_SMOKE_TEST)
    if(MSVC)
        target_compile_options(mcp_smoke_test PRIVATE /W3 /utf-8 /EHsc)
    else()
        target_compile_options(mcp_smoke_test PRIVATE -Wall -Wextra -fexceptions)
    endif()
    target_link_libraries(mcp_smoke_test PRIVATE Threads::Threads)
    if(WIN32)
        # http_transport.cpp (compiled here) calls GetAdaptersAddresses/sockets on Windows
        target_link_libraries(mcp_smoke_test PRIVATE ws2_32 iphlpapi)
    endif()
    add_executable(mcp_queue_test
        tests/queue_test.cpp
        src/component.cpp
        src/http_transport.cpp
        src/pending_requests.cpp
        src/sse_stream.cpp
    )
    target_include_directories(mcp_queue_test PRIVATE ${HTTPLIB_PATCHED_DIR} include src)
    target_compile_definitions(mcp_queue_test PRIVATE MCPHTTPTRANSPORT_SMOKE_TEST)
    target_link_libraries(mcp_queue_test PRIVATE Threads::Threads)
    if(MSVC)
        target_compile_options(mcp_queue_test PRIVATE /W3 /utf-8 /EHsc)
    else()
        target_compile_options(mcp_queue_test PRIVATE -Wall -Wextra -fexceptions)
    endif()
    if(WIN32)
        target_link_libraries(mcp_queue_test PRIVATE ws2_32 iphlpapi)
    endif()
    foreach(test_target mcp_smoke_test mcp_queue_test)
        if(WIN32)
            target_compile_definitions(${test_target} PRIVATE _WINDOWS WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
        endif()
        add_test(NAME ${test_target} COMMAND ${test_target})
        set_tests_properties(${test_target} PROPERTIES TIMEOUT 60)
    endforeach()
endif()

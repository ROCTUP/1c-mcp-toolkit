cmake_minimum_required(VERSION 3.16)
project(SyntaxHelpReader LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(FetchContent)

FetchContent_Declare(
    miniz
    URL https://github.com/richgel999/miniz/releases/download/3.1.1/miniz-3.1.1.zip
    URL_HASH SHA256=cb28402bb2af93bdc331b60d16807e89727d1712a2d0a7ba0cac79a3e406fe40
)
FetchContent_MakeAvailable(miniz)

add_library(SyntaxHelpReader SHARED
    src/dllmain.cpp
    src/component.cpp
    src/search_executor.cpp
    src/hbk.cpp
    src/html_to_md.cpp
    ${miniz_SOURCE_DIR}/miniz.c
)

target_include_directories(SyntaxHelpReader PRIVATE
    include
    src
    ${miniz_SOURCE_DIR}
)

set_target_properties(SyntaxHelpReader PROPERTIES PREFIX "")
target_link_libraries(SyntaxHelpReader PRIVATE Threads::Threads)

if(MSVC)
    set_target_properties(SyntaxHelpReader PROPERTIES
        LINK_FLAGS "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/SyntaxHelpReader.def")
    target_compile_definitions(SyntaxHelpReader PRIVATE
        _WINDOWS WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(SyntaxHelpReader PRIVATE /W3 /utf-8 /EHsc)
else()
    target_compile_options(SyntaxHelpReader PRIVATE -Wall -Wextra -fexceptions)
endif()

# ---- Tests ----
option(SYNTAX_HELP_BUILD_TESTS "Build SyntaxHelpReader native tests" OFF)
if(SYNTAX_HELP_BUILD_TESTS)
    enable_testing()
    add_executable(test_help_executor tests/test_executor.cpp src/component.cpp src/search_executor.cpp
        src/hbk.cpp src/html_to_md.cpp ${miniz_SOURCE_DIR}/miniz.c)
    target_include_directories(test_help_executor PRIVATE src include ${miniz_SOURCE_DIR})
    target_link_libraries(test_help_executor PRIVATE Threads::Threads)
    if(MSVC)
        target_compile_options(test_help_executor PRIVATE /utf-8 /EHsc)
        target_compile_definitions(test_help_executor PRIVATE _WINDOWS NOMINMAX _CRT_SECURE_NO_WARNINGS)
    endif()
    add_test(NAME test_help_executor COMMAND test_help_executor)
    add_executable(test_syntax_help
        tests/test_main.cpp
        src/hbk.cpp
        src/html_to_md.cpp
        ${miniz_SOURCE_DIR}/miniz.c
    )
    target_include_directories(test_syntax_help PRIVATE
        src
        include
        ${miniz_SOURCE_DIR}
    )
    if(MSVC)
        target_compile_options(test_syntax_help PRIVATE /W3 /utf-8 /EHsc)
        target_compile_definitions(test_syntax_help PRIVATE
            _WINDOWS WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    endif()
    if(DEFINED SYNTAX_HELP_HBK_DIR AND NOT SYNTAX_HELP_HBK_DIR STREQUAL "")
        add_test(NAME test_syntax_help COMMAND test_syntax_help "${SYNTAX_HELP_HBK_DIR}")
    else()
        message(STATUS "SYNTAX_HELP_HBK_DIR not set; ctest not registered. Run test_syntax_help.exe <hbk_dir> directly.")
    endif()
endif()

cmake_minimum_required(VERSION 3.25)
project(MarkdownShelf LANGUAGES C CXX)

if(NOT WIN32
   OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC"
   OR NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64"
   OR NOT MSVC_TOOLSET_VERSION EQUAL 143)
    message(FATAL_ERROR "MarkdownShelf requires Windows x64 and MSVC v143.")
endif()

include(CTest)
include(FetchContent)

option(MARKDOWNSHELF_BUILD_BINDINGS "Build the CPython extension" ON)
option(MARKDOWNSHELF_STRICT "Enable MSVC warnings as errors and static analysis" OFF)
set(MARKDOWNSHELF_DEPS_DIR "" CACHE PATH "Directory containing locked C++ source archives")

if(CMAKE_EXPORT_COMPILE_COMMANDS AND DEFINED ENV{VCToolsInstallDir} AND NOT "$ENV{INCLUDE}" STREQUAL "")
    # 让在开发者终端之外启动的编辑器也能找到工具链头文件。
    file(TO_CMAKE_PATH "$ENV{INCLUDE}" msvc_system_include_dirs)
    set(MARKDOWNSHELF_MSVC_SYSTEM_INCLUDE_DIRS "${msvc_system_include_dirs}"
        CACHE INTERNAL "MSVC and Windows SDK include directories from the developer shell")
endif()

# 按 name 注册依赖。配置缓存时从目录读取 archive，其余情况下使用 url。
# FetchContent 在提取源码前核对 sha256，配置了缓存且文件缺失时终止配置。
function(markdownshelf_fetch name archive url sha256)
    if(MARKDOWNSHELF_DEPS_DIR)
        set(url "${MARKDOWNSHELF_DEPS_DIR}/${archive}")
        if(NOT EXISTS "${url}")
            message(FATAL_ERROR "Missing locked dependency archive: ${url}")
        endif()
    endif()
    FetchContent_Declare(${name} URL "${url}" URL_HASH "SHA256=${sha256}")
    FetchContent_MakeAvailable(${name})
endfunction()

# 为一方 target 配置 UTF-8、警告级别和工具链头文件，严格模式另启用静态分析和警告即错误。
function(markdownshelf_compile_options target)
    target_compile_options(${target} PRIVATE /W4 /utf-8)
    if(CMAKE_EXPORT_COMPILE_COMMANDS AND MARKDOWNSHELF_MSVC_SYSTEM_INCLUDE_DIRS)
        target_include_directories(${target} SYSTEM PRIVATE ${MARKDOWNSHELF_MSVC_SYSTEM_INCLUDE_DIRS})
    endif()
    if(MARKDOWNSHELF_STRICT)
        target_compile_options(${target} PRIVATE /WX /analyze /analyze:external-)
    endif()
endfunction()

markdownshelf_fetch(sqlite_amalgamation sqlite-amalgamation-3540100.zip
    https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip
    0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9)
FetchContent_GetProperties(sqlite_amalgamation SOURCE_DIR sqlite_source_dir)
add_library(markdownshelf_sqlite OBJECT "${sqlite_source_dir}/sqlite3.c")
target_compile_definitions(markdownshelf_sqlite PRIVATE SQLITE_ENABLE_FTS5)
target_include_directories(markdownshelf_sqlite SYSTEM INTERFACE "${sqlite_source_dir}")

# 嵌入初始迁移，使独立核心和暂存扩展使用同一份 SQL 源文件。
set(initial_sql_path "${CMAKE_CURRENT_SOURCE_DIR}/src/core/migrations/0001_initial.sql")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${initial_sql_path}")
file(READ "${initial_sql_path}" markdownshelf_initial_sql)
configure_file(src/core/migrations/initial_sql.h.in generated/initial_sql.h @ONLY)

add_library(markdownshelf_core STATIC
    src/core/abi_probe.cpp
    src/core/db/sqlite.cpp
    src/core/db/fts5.cpp
    src/core/domain/library.cpp
    src/core/domain/library_manifest.cpp
    src/core/platform/library_files_win.cpp
    src/core/migrations/migrations.cpp)
target_include_directories(markdownshelf_core PUBLIC src/core/include PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_features(markdownshelf_core PUBLIC cxx_std_17)
target_link_libraries(markdownshelf_core PRIVATE markdownshelf_sqlite Ole32)
markdownshelf_compile_options(markdownshelf_core)

if(MARKDOWNSHELF_BUILD_BINDINGS)
    set(Python_FIND_IMPLEMENTATIONS CPython)
    find_package(Python 3.12...<3.13 REQUIRED COMPONENTS Interpreter Development.Module)
    set(PYBIND11_FINDPYTHON ON)
    markdownshelf_fetch(pybind11 pybind11-3.0.1.tar.gz
        https://codeload.github.com/pybind/pybind11/tar.gz/refs/tags/v3.0.1
        741633da746b7c738bb71f1854f957b9da660bcd2dce68d71949037f0969d0ca)

    pybind11_add_module(_markdownshelf_core NO_EXTRAS src/bindings/module.cpp src/bindings/library.cpp)
    target_link_libraries(_markdownshelf_core PRIVATE markdownshelf_core)
    markdownshelf_compile_options(_markdownshelf_core)
    install(TARGETS _markdownshelf_core LIBRARY DESTINATION . RUNTIME DESTINATION .)
endif()

if(BUILD_TESTING)
    set(BUILD_GMOCK OFF CACHE BOOL "The core tests use GoogleTest without GoogleMock")
    set(INSTALL_GTEST OFF CACHE BOOL "Install GoogleTest with MarkdownShelf")
    set(gtest_force_shared_crt ON CACHE BOOL "Match the MSVC runtime used by MarkdownShelf")
    markdownshelf_fetch(googletest googletest-1.16.0.tar.gz
        https://codeload.github.com/google/googletest/tar.gz/refs/tags/v1.16.0
        78c676fc63881529bf97bf9d45948d905a66833fbfa5318ea2cd7478cb98f399)

    add_executable(markdownshelf_core_tests
        tests/cpp/test_abi_probe.cpp
        tests/cpp/test_sqlite.cpp
        tests/cpp/test_library.cpp)
    target_include_directories(markdownshelf_core_tests SYSTEM PRIVATE "${sqlite_source_dir}")
    target_link_libraries(markdownshelf_core_tests PRIVATE markdownshelf_core GTest::gtest_main)
    markdownshelf_compile_options(markdownshelf_core_tests)
    include(GoogleTest)
    gtest_discover_tests(markdownshelf_core_tests)
endif()

cmake_minimum_required(VERSION 3.22)
project(appbackup LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---- zstd（可选）----
# 查找顺序：已指定 APP_ZSTD_SRC 时 add_subdirectory；否则找系统 zstd；
# 都拿不到就不定义 APP_HAS_ZSTD，工具退化为 gzip/none（运行时会提示）。
option(APP_WITH_ZSTD "Build with zstd support" ON)
set(APP_ZSTD_SRC "" CACHE PATH "Path to zstd source tree (contains build/cmake)")

if(APP_WITH_ZSTD)
    if(APP_ZSTD_SRC)
        enable_language(C)
        set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
        set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
        set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
        set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
        add_subdirectory("${APP_ZSTD_SRC}/build/cmake" "${CMAKE_BINARY_DIR}/zstd" EXCLUDE_FROM_ALL)
        add_library(appbackup_zstd INTERFACE)
        target_link_libraries(appbackup_zstd INTERFACE libzstd_static)
        target_include_directories(appbackup_zstd INTERFACE "${APP_ZSTD_SRC}/lib")
        set(APP_ZSTD_OK ON)
    else()
        find_path(ZSTD_INCLUDE_DIR zstd.h)
        find_library(ZSTD_LIBRARY NAMES zstd libzstd.a)
        if(ZSTD_INCLUDE_DIR AND ZSTD_LIBRARY)
            add_library(appbackup_zstd INTERFACE)
            target_link_libraries(appbackup_zstd INTERFACE ${ZSTD_LIBRARY})
            target_include_directories(appbackup_zstd INTERFACE ${ZSTD_INCLUDE_DIR})
            set(APP_ZSTD_OK ON)
        else()
            message(STATUS "zstd not found — building without zstd support")
            set(APP_ZSTD_OK OFF)
        endif()
    endif()
else()
    set(APP_ZSTD_OK OFF)
endif()

set(APP_SOURCES
    src/main.cpp
    src/common.cpp
    src/proc.cpp
    src/sink.cpp
    src/fsops.cpp
    src/md5.cpp
    src/sha256.cpp
    src/fingerprint.cpp
    src/manifest.cpp
    src/sysarchive.cpp
    src/selinux.cpp
    src/uidmap.cpp
    src/meta.cpp
    src/dbformat.cpp
    src/backup.cpp
    src/restore.cpp
)

add_executable(appbackup ${APP_SOURCES})

target_compile_options(appbackup PRIVATE -Wall -Wextra -O2)
target_include_directories(appbackup PRIVATE src)

find_library(ZLIB_LIB z)
if(NOT ZLIB_LIB)
    message(FATAL_ERROR "zlib not found")
endif()
target_link_libraries(appbackup PRIVATE ${ZLIB_LIB} dl)

# Android 上额外链接 liblog（可选，拿不到也不影响）
if(ANDROID)
    find_library(LOG_LIB log)
    if(LOG_LIB)
        target_link_libraries(appbackup PRIVATE ${LOG_LIB})
    endif()
endif()

install(TARGETS appbackup RUNTIME DESTINATION bin)

# 主机侧自测（Android 交叉编译时跳过）
option(APP_BUILD_TESTS "Build host roundtrip tests" ON)
if(APP_BUILD_TESTS AND NOT ANDROID)
    add_executable(appbackup-test
        tests/roundtrip.cpp
        src/common.cpp
        src/proc.cpp
        src/sink.cpp
        src/fsops.cpp
        src/md5.cpp
        src/sha256.cpp
        src/fingerprint.cpp
        src/manifest.cpp
        src/sysarchive.cpp
        src/selinux.cpp
        src/uidmap.cpp
        src/meta.cpp
        src/dbformat.cpp
    )
    target_compile_options(appbackup-test PRIVATE -Wall -Wextra -O2)
    target_include_directories(appbackup-test PRIVATE src)
    target_link_libraries(appbackup-test PRIVATE ${ZLIB_LIB} dl)
endif()

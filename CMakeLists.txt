cmake_minimum_required(VERSION 3.22)
project(ozj_appdata LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---- zstd（可选）----
# 查找顺序：已指定 OZJ_ZSTD_SRC 时 add_subdirectory；否则找系统 zstd；
# 都拿不到就不定义 OZJ_HAS_ZSTD，工具退化为 gzip/none（运行时会提示）。
option(OZJ_WITH_ZSTD "Build with zstd support" ON)
set(OZJ_ZSTD_SRC "" CACHE PATH "Path to zstd source tree (contains build/cmake)")

if(OZJ_WITH_ZSTD)
    if(OZJ_ZSTD_SRC)
        enable_language(C)
        set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
        set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
        set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
        set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
        add_subdirectory("${OZJ_ZSTD_SRC}/build/cmake" "${CMAKE_BINARY_DIR}/zstd" EXCLUDE_FROM_ALL)
        add_library(ozj_zstd INTERFACE)
        target_link_libraries(ozj_zstd INTERFACE libzstd_static)
        target_include_directories(ozj_zstd INTERFACE "${OZJ_ZSTD_SRC}/lib")
        set(OZJ_ZSTD_OK ON)
    else()
        find_path(ZSTD_INCLUDE_DIR zstd.h)
        find_library(ZSTD_LIBRARY NAMES zstd libzstd.a)
        if(ZSTD_INCLUDE_DIR AND ZSTD_LIBRARY)
            add_library(ozj_zstd INTERFACE)
            target_link_libraries(ozj_zstd INTERFACE ${ZSTD_LIBRARY})
            target_include_directories(ozj_zstd INTERFACE ${ZSTD_INCLUDE_DIR})
            set(OZJ_ZSTD_OK ON)
        else()
            message(STATUS "zstd not found — building without zstd support")
            set(OZJ_ZSTD_OK OFF)
        endif()
    endif()
else()
    set(OZJ_ZSTD_OK OFF)
endif()

set(OZJ_SOURCES
    src/main.cpp
    src/common.cpp
    src/proc.cpp
    src/sink.cpp
    src/fsops.cpp
    src/md5.cpp
    src/tar_writer.cpp
    src/tar_reader.cpp
    src/selinux.cpp
    src/uidmap.cpp
    src/meta.cpp
    src/dbformat.cpp
    src/backup.cpp
    src/restore.cpp
)

add_executable(ozj-appdata ${OZJ_SOURCES})

target_compile_options(ozj-appdata PRIVATE -Wall -Wextra -O2)
target_include_directories(ozj-appdata PRIVATE src)

find_library(ZLIB_LIB z)
if(NOT ZLIB_LIB)
    message(FATAL_ERROR "zlib not found")
endif()
target_link_libraries(ozj-appdata PRIVATE ${ZLIB_LIB} dl)
if(OZJ_ZSTD_OK)
    target_link_libraries(ozj-appdata PRIVATE ozj_zstd)
    target_compile_definitions(ozj-appdata PRIVATE OZJ_HAS_ZSTD=1)
endif()

# Android 上额外链接 liblog（可选，拿不到也不影响）
if(ANDROID)
    find_library(LOG_LIB log)
    if(LOG_LIB)
        target_link_libraries(ozj-appdata PRIVATE ${LOG_LIB})
    endif()
endif()

install(TARGETS ozj-appdata RUNTIME DESTINATION bin)

# 主机侧自测（Android 交叉编译时跳过）
option(OZJ_BUILD_TESTS "Build host roundtrip tests" ON)
if(OZJ_BUILD_TESTS AND NOT ANDROID)
    add_executable(ozj-appdata-test
        tests/roundtrip.cpp
        src/common.cpp
        src/proc.cpp
        src/sink.cpp
        src/fsops.cpp
        src/md5.cpp
        src/tar_writer.cpp
        src/tar_reader.cpp
        src/selinux.cpp
        src/uidmap.cpp
        src/meta.cpp
        src/dbformat.cpp
    )
    target_compile_options(ozj-appdata-test PRIVATE -Wall -Wextra -O2)
    target_include_directories(ozj-appdata-test PRIVATE src)
    target_link_libraries(ozj-appdata-test PRIVATE ${ZLIB_LIB} dl)
    if(OZJ_ZSTD_OK)
        target_link_libraries(ozj-appdata-test PRIVATE ozj_zstd)
        target_compile_definitions(ozj-appdata-test PRIVATE OZJ_HAS_ZSTD=1)
    endif()
endif()

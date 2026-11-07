cmake_minimum_required(VERSION 3.22)
project(ozj_appdata LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(ozj-appdata
    src/main.cpp
    src/common.cpp
    src/proc.cpp
    src/sink.cpp
    src/fsops.cpp
    src/tar_writer.cpp
    src/tar_reader.cpp
    src/selinux.cpp
    src/uidmap.cpp
    src/meta.cpp
    src/backup.cpp
    src/restore.cpp
)

target_compile_options(ozj-appdata PRIVATE -Wall -Wextra -O2)
target_include_directories(ozj-appdata PRIVATE src)

find_library(ZLIB_LIB z)
if(NOT ZLIB_LIB)
    message(FATAL_ERROR "zlib not found")
endif()
target_link_libraries(ozj-appdata PRIVATE ${ZLIB_LIB} dl)

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
        src/tar_writer.cpp
        src/tar_reader.cpp
        src/selinux.cpp
        src/uidmap.cpp
        src/meta.cpp
    )
    target_compile_options(ozj-appdata-test PRIVATE -Wall -Wextra -O2)
    target_include_directories(ozj-appdata-test PRIVATE src)
    target_link_libraries(ozj-appdata-test PRIVATE ${ZLIB_LIB} dl)
endif()

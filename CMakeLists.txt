cmake_minimum_required(VERSION 3.22)
project(appbackup LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 正式产物只有 Android 版：可执行文件内嵌 arm64 静态 tar + zstd（见下方 APP_EMBED_*）。
# 非 Android 构建仅用于开发期编译检查，运行时因 ABI 不符会回退系统 tar。
option(APP_HOST_CHECK "非 Android 主机上构建（仅编译检查）" OFF)
if(NOT ANDROID AND NOT APP_HOST_CHECK)
    message(FATAL_ERROR
        "本项目面向 Android（内嵌 arm64 静态 tar/zstd）。\n"
        "如需在主机上做编译检查，请加 -DAPP_HOST_CHECK=ON。")
endif()

# ---- 内嵌 tar / zstd ----------------------------------------------------------
# 上游 DataBackup 把静态 GNU tar + zstd 放在 assets/bin.zip，运行时解压到私有 bin 目录
# 并 PATH 前置（BaseUtil.EnvInitializer）。归档字节由 tar 实现决定，所以必须用同一份。
# 这里做等价的事：把同样的二进制嵌进可执行文件，运行时释放到磁盘，免去额外部署。
set(APP_EMBED_ABI "arm64-v8a" CACHE STRING "prebuilt/<abi>/ 下的 tar/zstd 目标 ABI")
set(APP_EMBED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt/${APP_EMBED_ABI}")
if(NOT EXISTS "${APP_EMBED_DIR}/tar" OR NOT EXISTS "${APP_EMBED_DIR}/zstd")
    message(FATAL_ERROR "缺少内嵌二进制：${APP_EMBED_DIR}/{tar,zstd}（从上游 assets/bin.zip 提取）")
endif()

set(APP_EMBED_OBJ_DIR "${CMAKE_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${APP_EMBED_OBJ_DIR}")
set(APP_EMBED_OBJS "")
foreach(tool tar zstd)
    set(obj "${APP_EMBED_OBJ_DIR}/${tool}.o")
    add_custom_command(
        OUTPUT "${obj}"
        COMMAND ${CMAKE_LINKER} -r -b binary -o "${obj}" "${tool}"
        WORKING_DIRECTORY "${APP_EMBED_DIR}"
        DEPENDS "${APP_EMBED_DIR}/${tool}"
        COMMENT "Embedding ${tool} (${APP_EMBED_ABI})"
        VERBATIM)
    list(APPEND APP_EMBED_OBJS "${obj}")
endforeach()
add_custom_target(appbackup_embed DEPENDS ${APP_EMBED_OBJS})

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
    src/toolchain.cpp
    src/sysarchive.cpp
    src/selinux.cpp
    src/uidmap.cpp
    src/meta.cpp
    src/dbformat.cpp
    src/backup.cpp
    src/restore.cpp
)

add_executable(appbackup ${APP_SOURCES} ${APP_EMBED_OBJS})
add_dependencies(appbackup appbackup_embed)

target_compile_options(appbackup PRIVATE -Wall -Wextra -O2)
target_include_directories(appbackup PRIVATE src)
# ld -r -b binary 生成的 .o 没有 .note.GNU-stack，显式声明不可执行栈
target_link_options(appbackup PRIVATE -Wl,-z,noexecstack)

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

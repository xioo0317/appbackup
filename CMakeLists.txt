cmake_minimum_required(VERSION 3.22)
project(appbackup LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 正式产物只有 Android/arm64-v8a：可执行文件内嵌同 ABI 的静态 tar + zstd（见下方 APP_EMBED_*）。
# 非 Android 构建仅用于开发期编译检查，运行时因 ABI 不符会回退系统 tar。
option(APP_HOST_CHECK "非 Android 主机上构建（仅编译检查）" OFF)
if(NOT ANDROID AND NOT APP_HOST_CHECK)
    message(FATAL_ERROR
        "本项目面向 Android/arm64-v8a（内嵌同 ABI 静态 tar/zstd）。\n"
        "如需在主机上做编译检查，请加 -DAPP_HOST_CHECK=ON。")
endif()
if(ANDROID AND NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR
        "只发布 arm64-v8a（prebuilt/ 仅提供该 ABI 的 tar/zstd）。当前 ANDROID_ABI=${ANDROID_ABI}。\n"
        "如需其它 ABI，请先从上游 source/app/src/<abi>/assets/bin.zip 提取 tar、zstd 放进 prebuilt/<abi>/。")
endif()

# ---- 内嵌 tar / zstd ----------------------------------------------------------
# 上游 DataBackup 把静态 GNU tar + zstd 放在 assets/bin.zip，运行时解压到私有 bin 目录
# 并 PATH 前置（BaseUtil.EnvInitializer）。归档字节由 tar 实现决定，所以必须用同一份。
# 这里做等价的事：把同样的二进制嵌进可执行文件，运行时释放到磁盘，免去额外部署。
#
# 嵌入方式用汇编指令 `.incbin`，不用 `ld -r -b binary`：NDK 的 ld 是 ld.lld，
# 不支持 bfd 的 -b/--format 选项（报 target emulation unknown），而 .incbin 由
# clang 内置汇编器直接支持，也不依赖 binutils。
set(APP_EMBED_ABI "arm64-v8a" CACHE STRING "prebuilt/<abi>/ 下的 tar/zstd 目标 ABI")
set(APP_EMBED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt/${APP_EMBED_ABI}")
if(NOT EXISTS "${APP_EMBED_DIR}/tar" OR NOT EXISTS "${APP_EMBED_DIR}/zstd")
    message(FATAL_ERROR
        "缺少内嵌二进制：${APP_EMBED_DIR}/{tar,zstd}\n"
        "请从上游 assets/bin.zip（source/app/src/<abi>/assets/）提取 tar 与 zstd 放入该目录。")
endif()

enable_language(ASM)
set(APP_EMBED_OBJ_DIR "${CMAKE_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${APP_EMBED_OBJ_DIR}")
set(APP_EMBED_SRCS "")
foreach(tool tar zstd)
    set(blob "${APP_EMBED_DIR}/${tool}")
    set(asm "${APP_EMBED_OBJ_DIR}/embed_${tool}.S")
    # %progbits：32 位 ARM 汇编器只认 '%' 形式，其余架构也接受
    file(WRITE "${asm}"
        "    .section .rodata.appbackup_${tool},\"a\",%progbits\n"
        "    .p2align 4\n"
        "    .globl appbackup_${tool}_begin\n"
        "    .hidden appbackup_${tool}_begin\n"
        "appbackup_${tool}_begin:\n"
        "    .incbin \"${blob}\"\n"
        "    .globl appbackup_${tool}_end\n"
        "    .hidden appbackup_${tool}_end\n"
        "appbackup_${tool}_end:\n"
        "    .section .note.GNU-stack,\"\",%progbits\n")
    set_property(SOURCE "${asm}" APPEND PROPERTY OBJECT_DEPENDS "${blob}")
    list(APPEND APP_EMBED_SRCS "${asm}")
endforeach()

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

add_executable(appbackup ${APP_SOURCES} ${APP_EMBED_SRCS})

target_compile_options(appbackup PRIVATE -Wall -Wextra -O2)
target_include_directories(appbackup PRIVATE src)
# 显式声明不可执行栈（.note.GNU-stack 已由嵌入用的 .S 提供，这里再加一层保险）
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

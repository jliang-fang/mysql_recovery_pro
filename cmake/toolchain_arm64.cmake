/**
 * @file scripts/build_arm64_cross.sh
 * @brief ARM64交叉编译脚本
 * 
 * 支持在x86_64主机上交叉编译ARM64目标
 */

cmake_minimum_required(VERSION 3.20)
project(mysql_recovery_arm64 VERSION 1.0.0 LANGUAGES CXX)

# ==================== 交叉编译配置 ====================

# 设置ARM64目标三元组
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# 设置编译器路径（根据实际安装路径修改）
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# 设置工具链文件
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ==================== 编译选项 ====================

# ARM64特定优化
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv8-a+crc+crypto")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=armv8-a+crc+crypto")

# 启用NEON支持
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mfpu=neon")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mfpu=neon")

# 优化级别
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")

# ==================== CPU特性检测 ====================

# 检查ARM64特定特性
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-march=armv8-a+crc+crypto" COMPILER_SUPPORTS_ARMV8_CRC)
if(COMPILER_SUPPORTS_ARMV8_CRC)
    target_compile_definitions(dbrecovery_innodb PRIVATE
        ENV_CPU_ARM64=1
        ENV_CPU_HAS_CRC32=1
    )
endif()

check_cxx_compiler_flag("-march=armv8-a+simd+crypto" COMPILER_SUPPORTS_ARMV8_SIMD)
if(COMPILER_SUPPORTS_ARMV8_SIMD)
    target_compile_definitions(dbrecovery_innodb PRIVATE
        ENV_CPU_HAS_NEON=1
    )
endif()

# ==================== 内存对齐优化 ====================

# ARM64 L1 cache line size is 64 bytes
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DARM64_CACHE_LINE_SIZE=64")

# ==================== 禁用不兼容的x86_64特性 ====================

# 禁用SSE/AVX指令集
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -msse=none -mavx=none")

# ==================== 构建类型 ====================

# 默认Release构建
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ==================== 依赖项 ====================

# 集成 nlohmann/json 库
find_package(nlohmann_json REQUIRED)

# 查找MySQL头文件
find_path(MYSQL_INCLUDE_DIR mysql_version.h 
    PATHS /usr/include/mysql /usr/local/include/mysql 
    NO_DEFAULT_PATH)
if(MYSQL_INCLUDE_DIR)
    message(STATUS "Found MySQL headers at: ${MYSQL_INCLUDE_DIR}")
else()
    message(WARNING "MySQL headers not found, some features may be disabled")
endif()

# 查找zlib
find_package(ZLIB REQUIRED)
message(STATUS "Found zlib at: ${ZLIB_LIBRARIES}")

# 查找liburing（ARM64可能需要特殊版本）
find_library(LIBURING_LIBRARY NAMES uring liburing)
find_path(LIBURING_INCLUDE_DIR NAMES liburing.h 
    PATHS /usr/include /usr/local/include)
if(LIBURING_LIBRARY AND LIBURING_INCLUDE_DIR)
    message(STATUS "Found liburing at: ${LIBURING_LIBRARY}")
    set(HAVE_IO_URING ON)
else()
    message(STATUS "liburing not found, io_uring support disabled")
    set(HAVE_IO_URING OFF)
endif()

# ==================== 源文件 ====================

set(SOURCES
    src/engine/row_decoder.cpp
    src/engine/field_decoder.cpp
    src/innodb/binary_reader.cpp
    src/innodb/page_parser.cpp
    src/innodb/page_reader.cpp
    src/innodb/recovery_engine.cpp
    src/innodb/blob_parser.cpp
    src/innodb/btree.cpp
    src/innodb/sdi_manager.cpp
    src/innodb/sdi_cache.cpp
    src/innodb/redo_log.cpp
    src/innodb/redo_log_replayer.cpp
    src/innodb/thread_pool.cpp
    src/recovery/blob_reconstructor.cpp
    src/recovery/engine.cpp
    src/utils/cli_parser.cpp
    src/utils/file_mapper.cpp
    src/export/ibd_exporter.cpp
    src/page/page_v56.cpp
    src/page/page_v80.cpp
    src/page/page_factory.cpp
    src/page/page_v55.cpp
    src/page/page_v84.cpp
    src/innodb/page_parser_v3.cpp
    src/innodb/page_parser_v82.cpp
    src/disk/io_uring_scanner.cpp
    # ARM64特定优化
    src/platform/arm64/cpu_optimizations_arm64.cpp
    # 平台特定实现
    src/platform/linux/file_system_linux.cpp
    src/platform/linux/thread_linux.cpp
    src/platform/linux/memory_linux.cpp
)

add_library(dbrecovery_innodb STATIC ${SOURCES})

target_include_directories(dbrecovery_innodb PUBLIC
    include
    $<$<BOOL:${MYSQL_INCLUDE_DIR}>:${MYSQL_INCLUDE_DIR}>
    $<$<BOOL:${LIBURING_INCLUDE_DIR}>:${LIBURING_INCLUDE_DIR}>
)

# ==================== ARM64特定编译选项 ====================

target_compile_options(dbrecovery_innodb PRIVATE
    -march=armv8-a+crc+crypto
    -mfpu=neon
    -mtune=cortex-a53
)

# 启用ARM64特定特性
target_compile_definitions(dbrecovery_innodb PRIVATE
    ENV_CPU_ARM64=1
    ENV_CPU_HAS_NEON=1
    ENV_CPU_HAS_CRC32=1
    ENV_PLATFORM_LINUX=1
)

# ==================== 链接选项 ====================

target_link_libraries(dbrecovery_innodb PUBLIC nlohmann_json::nlohmann_json)
target_link_libraries(dbrecovery_innodb PUBLIC ZLIB::ZLIB)

if(HAVE_IO_URING)
    target_link_libraries(dbrecovery_innodb PUBLIC ${LIBURING_LIBRARY})
    target_compile_definitions(dbrecovery_innodb PUBLIC HAVE_IO_URING=1)
endif()

# 链接ARM64特定库
target_link_libraries(dbrecovery_innodb PRIVATE pthread rt)

# ==================== 构建CLI可执行文件 ====================

add_executable(mysql_recover_cli src/main_cli.cpp)
target_link_libraries(mysql_recover_cli dbrecovery_innodb)
target_include_directories(mysql_recover_cli PRIVATE ${CMAKE_SOURCE_DIR}/include)

# ==================== 测试配置 ====================

enable_testing()

# ARM64测试配置
# 注意：需要在ARM64设备或使用QEMU用户模式仿真运行测试

# ==================== 安装配置 ====================

install(TARGETS dbrecovery_innodb mysql_recover_cli
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/ DESTINATION include)

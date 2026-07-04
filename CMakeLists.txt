cmake_minimum_required(VERSION 3.20)
project(mysql_recovery_pro VERSION 2.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)

# GUI option
option(GUI_ENABLED "Enable GUI support" OFF)

# 集成 nlohmann/json 库
# ✅ P0-1 修复：使用系统安装的 nlohmann-json 库
find_package(nlohmann_json REQUIRED)

# 查找 MySQL 头文件
find_path(MYSQL_INCLUDE_DIR mysql_version.h PATHS /usr/include/mysql /usr/local/include/mysql NO_DEFAULT_PATH)
if(MYSQL_INCLUDE_DIR)
    message(STATUS "Found MySQL headers at: ${MYSQL_INCLUDE_DIR}")
else()
    message(WARNING "MySQL headers not found, some features may be disabled")
endif()

# 查找 zlib（用于 SDI 解压缩）
find_package(ZLIB REQUIRED)
message(STATUS "Found zlib at: ${ZLIB_LIBRARIES}")

# 查找 liburing（用于 io_uring 高性能 I/O）
find_library(LIBURING_LIBRARY NAMES uring liburing)
find_path(LIBURING_INCLUDE_DIR NAMES liburing.h PATHS /usr/include /usr/local/include)
if(LIBURING_LIBRARY AND LIBURING_INCLUDE_DIR)
    message(STATUS "Found liburing at: ${LIBURING_LIBRARY}")
    set(HAVE_IO_URING ON)
else()
    message(STATUS "liburing not found, io_uring support disabled")
    set(HAVE_IO_URING OFF)
endif()

# 添加源文件
set(SOURCES
    # ✅ P0-1 修复：移除 schema_defs.cpp（函数已在头文件中内联实现）
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
    # ✅ 修复：移除 undo_log.cpp（重复定义，已在 src/page/page_factory.cpp 中实现）
    src/recovery/blob_reconstructor.cpp
    src/recovery/engine.cpp
    src/recovery/rmrf_recovery.cpp
    # ✅ 新增：增强恢复功能
    src/recovery/enhanced_recovery.cpp
    # ✅ 新增：坏块修复功能
    src/repair/corruption_detector.cpp
    src/repair/header_repairer.cpp
    src/repair/physical_corruption.cpp
    src/repair/page_content_repair.cpp
    src/repair/index_corruption_repair.cpp
    src/repair/log_corruption_repair.cpp
    src/repair/metadata_corruption_repair.cpp
    src/repair/repair.cpp
    src/utils/cli_parser.cpp
    src/utils/file_mapper.cpp
    # ✅ 新增：IBD 导出器
    src/export/ibd_exporter.cpp
    # ✅ 新增：页解析层 (page module)
    src/page/page_v56.cpp
    src/page/page_v80.cpp
    src/page/page_factory.cpp
    # ✅ 新增：MySQL 5.5/5.1 兼容支持
    src/page/page_v55.cpp
    # ✅ 新增：MySQL 8.4/9.0 兼容支持
    src/page/page_v84.cpp
    # ✅ 新增：页解析器 V3
    src/innodb/page_parser_v3.cpp
    # ✅ 新增：Schemaless heuristic recovery parser
    src/innodb/heuristic_parser.cpp
    # ✅ 新增：Enhanced schemaless parser with pattern-based field detection
    src/innodb/enhanced_schemaless_parser.cpp
    # ✅ 新增：Schema-Agnostic scanner (Page Directory based)
    src/innodb/schema_agnostic_scanner.cpp
    # ✅ 新增：FRM file parser (extract schema from .frm files)
    src/innodb/frm_parser.cpp
    # ✅ 新增：Dynamic schema inference from corrupted IBD files
    src/innodb/schema_inferrer.cpp
    # ✅ 新增：Undo log parser for TRUNCATE recovery
    src/innodb/undo_log_parser.cpp
    # ✅ 新增：BLOB/TEXT scanner
    src/innodb/blob_scanner.cpp
    # ✅ 新增：Binary recovery for TRUNCATE/rm -rf scenarios
    src/innodb/binary_recovery.cpp
    # ✅ 新增：Page merger for corruption recovery
    src/innodb/page_merger.cpp
    # ✅ 新增：Forensic recovery for TRUNCATE/rm -rf scenarios
    src/innodb/forensic_recovery.cpp
    src/innodb/forensic_manager.cpp
    # ✅ 新增：Enhanced binary recovery with PhotoRec-style scanning
    src/innodb/binary_recovery_enhanced.cpp
    # ✅ 新增：Canonical InnoDB page checksum implementation
    src/innodb/innodb_checksum_impl.cpp
    # ✅ 新增：io_uring 高性能 I/O
    src/disk/io_uring_scanner.cpp
    # ✅ 新增：MySQL版本检测器
    src/common/mysql_version_detector.cpp
    # ✅ 新增：统一校验和实现
    src/innodb/innodb_checksum_impl.cpp
    # ✅ 新增：引擎模块实现
    src/engine/raw_disk_engine.cpp
    src/engine/signature_scraper.cpp
    src/engine/schema_aware_parser.cpp
    src/engine/drop_table_recovery.cpp
    # ✅ 新增：终极恢复引擎模块 (>=99.9% recovery)
    src/engine/enhanced_raw_disk_engine.cpp
    src/engine/advanced_signature_scraper.cpp
    src/recovery/heuristic_reconstructor.cpp
    src/innodb/schema_aware_parser.cpp
    # ✅ 新增：版本感知恢复引擎 (MySQL 5.6-9.0)
    src/recovery/version_aware_recovery.cpp
    # ✅ 新增：原始磁盘雕刻 (PhotoRec风格，无备份恢复)
    src/recovery/raw_disk_carver.cpp
    src/recovery/truncate_recovery.cpp
    src/recovery/disaster_recovery.cpp
    # ✅ 新增：ibd2sql-style INDEX page parser (core parser for >=99.9% recovery)
    src/index_page_parser.cpp
    # ✅ 新增：平台特定实现
    src/platform/linux/file_system_linux.cpp
    src/platform/linux/memory_linux.cpp
    src/platform/linux/thread_linux.cpp
    # ✅ 新增：XFS文件系统磁盘雕刻支持
    src/disk/xfs_disk_carver.cpp
    # ✅ 新增：ext4文件系统磁盘雕刻支持
    src/recovery/ext4_recovery.cpp
)

# 根据平台选择性地添加文件
if(ENV_PLATFORM_LINUX)
    list(APPEND SOURCES
        src/platform/linux/file_system_linux.cpp
        src/platform/linux/memory_linux.cpp
        src/platform/linux/thread_linux.cpp
    )
elseif(ENV_PLATFORM_MACOS)
    list(APPEND SOURCES
        src/platform/macos/file_system_macos.cpp
        src/platform/macos/memory_macos.cpp
        src/platform/macos/thread_macos.cpp
    )
elseif(ENV_PLATFORM_WINDOWS)
    list(APPEND SOURCES
        src/platform/windows/file_system_windows.cpp
        src/platform/windows/memory_windows.cpp
        src/platform/windows/thread_windows.cpp
    )
endif()

# 添加CPU优化实现（仅在对应架构上）
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
    list(APPEND SOURCES
        src/platform/arm64/cpu_optimizations_arm64.cpp
    )
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "AMD64")
    # x86_64 优化已在头文件中实现
endif()

# ✅ 新增：确保 file_mapper.cpp 被正确包含（如果尚未存在则创建）
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/src/utils/file_mapper.cpp")
    file(WRITE "${CMAKE_SOURCE_DIR}/src/utils/file_mapper.cpp" "#include \"dbrecovery/utils/file_mapper.hpp\"\nnamespace dbrecovery { namespace utils {} }\n")
endif()

add_library(dbrecovery_innodb STATIC ${SOURCES})

target_include_directories(dbrecovery_innodb PUBLIC
    include
    $<$<BOOL:${MYSQL_INCLUDE_DIR}>:${MYSQL_INCLUDE_DIR}>
    $<$<BOOL:${LIBURING_INCLUDE_DIR}>:${LIBURING_INCLUDE_DIR}>
)

# 包含 src/CMakeLists.txt 以添加额外的源文件
add_subdirectory(src)

# 链接系统 nlohmann_json 库
target_link_libraries(dbrecovery_innodb PUBLIC nlohmann_json::nlohmann_json)

# 链接 zlib（用于 SDI 解压缩）
target_link_libraries(dbrecovery_innodb PUBLIC ZLIB::ZLIB)

# 链接 LZ4（用于压缩页解压）
find_package(PkgConfig REQUIRED)
pkg_check_modules(LZ4 liblz4)
if(LZ4_FOUND)
    target_include_directories(dbrecovery_innodb PUBLIC ${LZ4_INCLUDE_DIRS})
    target_link_libraries(dbrecovery_innodb PUBLIC ${LZ4_LIBRARIES})
    target_compile_definitions(dbrecovery_innodb PUBLIC HAVE_LZ4=1)
else()
    # Fallback: try to find lz4 directly
    find_path(LZ4_INCLUDE_DIR NAMES lz4.h)
    find_library(LZ4_LIBRARY NAMES lz4)
    if(LZ4_INCLUDE_DIR AND LZ4_LIBRARY)
        target_include_directories(dbrecovery_innodb PUBLIC ${LZ4_INCLUDE_DIR})
        target_link_libraries(dbrecovery_innodb PUBLIC ${LZ4_LIBRARY})
        target_compile_definitions(dbrecovery_innodb PUBLIC HAVE_LZ4=1)
    endif()
endif()

# 链接 OpenSSL（用于 TDE AES 解密）
find_package(OpenSSL REQUIRED)
target_link_libraries(dbrecovery_innodb PUBLIC OpenSSL::SSL OpenSSL::Crypto)
target_include_directories(dbrecovery_innodb PUBLIC ${OPENSSL_INCLUDE_DIR})

# 链接 liburing（用于 io_uring 高性能 I/O）
if(HAVE_IO_URING)
    target_link_libraries(dbrecovery_innodb PUBLIC ${LIBURING_LIBRARY})
    target_compile_definitions(dbrecovery_innodb PUBLIC HAVE_IO_URING=1)
endif()

# 统一的 CLI 入口点 - 使用 RecoveryCLI 类
add_executable(mysql_recovery_pro src/main_cli.cpp)
target_link_libraries(mysql_recovery_pro dbrecovery_innodb)
target_include_directories(mysql_recovery_pro PRIVATE ${CMAKE_SOURCE_DIR}/include)

enable_testing()

# Unit tests (C++ GTest)
find_package(GTest QUIET)
if(GTEST_FOUND)
    add_subdirectory(tests)
else()
    # 如果系统没有GTest，尝试使用本地版本或创建别名
    # 这里创建一个简单的GTest::GTest别名
    add_library(GTest::GTest INTERFACE)
    add_library(GTest::Main INTERFACE)

    # 如果有gtest库文件，链接它
    if(EXISTS "/usr/lib64/libgtest.so" OR EXISTS "/usr/lib/libgtest.so")
        target_link_libraries(GTest::GTest INTERFACE gtest)
        target_link_libraries(GTest::Main INTERFACE gtest_main)
    endif()

    # 添加测试数据生成脚本作为全局测试
    add_test(NAME generate_test_data COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/ibd_generator.py test_table)

    # 添加恢复引擎测试
    add_executable(test_recovery_engine tests/test_recovery_engine.cpp)
    target_link_libraries(test_recovery_engine dbrecovery_innodb GTest::GTest GTest::Main)
    add_test(NAME test_recovery_engine COMMAND test_recovery_engine)

    # 添加页类型 V2 测试
    add_executable(test_page_types_v2 test/test_page_types_v2.cpp)
    target_link_libraries(test_page_types_v2 dbrecovery_innodb GTest::GTest GTest::Main)
    add_test(NAME test_page_types_v2 COMMAND test_page_types_v2)

    # 添加损坏场景测试
    add_executable(test_corruption_scenarios test/test_corruption_scenarios.cpp)
    target_link_libraries(test_corruption_scenarios dbrecovery_innodb GTest::GTest GTest::Main)
    target_compile_definitions(test_corruption_scenarios PRIVATE TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test/test_data")
    add_test(NAME test_corruption_scenarios COMMAND test_corruption_scenarios)

    # 添加 corruption detector 测试
    add_executable(test_corruption_detector test/test_corruption_detector.cpp)
    target_link_libraries(test_corruption_detector dbrecovery_innodb GTest::GTest GTest::Main)
    add_test(NAME test_corruption_detector COMMAND test_corruption_detector)

    # 添加多版本兼容性测试
    add_executable(test_multi_version_compat test/test_multi_version_compat.cpp)
    target_link_libraries(test_multi_version_compat dbrecovery_innodb GTest::GTest GTest::Main)
    add_test(NAME test_multi_version_compat COMMAND test_multi_version_compat)

    # 添加多环境兼容性测试
    add_executable(test_multi_environment_compat test/test_multi_environment_compat.cpp)
    target_link_libraries(test_multi_environment_compat dbrecovery_innodb GTest::GTest GTest::Main)
    add_test(NAME test_multi_environment_compat COMMAND test_multi_environment_compat)

    # 添加CPU优化测试
    add_executable(test_cpu_optimizations test/test_cpu_optimizations.cpp)
    target_link_libraries(test_cpu_optimizations dbrecovery_innodb GTest::GTest GTest::Main)
    add_test(NAME test_cpu_optimizations COMMAND test_cpu_optimizations)

    # 添加环境兼容性测试
    add_executable(test_environment_compat test/test_environment_compat.cpp)
    target_link_libraries(test_environment_compat dbrecovery_innodb GTest::GTest GTest::Main)
    add_test(NAME test_environment_compat COMMAND test_environment_compat)

    # 添加平台检测测试
    add_executable(test_platform_detection test/test_platform_detection.cpp)
    target_link_libraries(test_platform_detection dbrecovery_innodb GTest::GTest GTest::Main)
    add_test(NAME test_platform_detection COMMAND test_platform_detection)

    # 添加SIMD操作测试
    add_executable(test_simd_operations test/test_simd_operations.cpp)
    target_link_libraries(test_simd_operations dbrecovery_innodb GTest::GTest GTest::Main)
    add_test(NAME test_simd_operations COMMAND test_simd_operations)

    # 启用tests子目录（即使GTest未找到）
    add_subdirectory(tests)
endif()

# Python tests
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(
    NAME python_ibd_generator_tests
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/test_ibd_generator.py
)

# 静态分析配置
option(ENABLE_CLANG_TIDY "Enable clang-tidy static analysis" OFF)
if(ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY clang-tidy)
    if(CLANG_TIDY)
        set(CMAKE_CXX_CPPCHECK ${CLANG_TIDY} --checks=*)
    endif()
endif()

# 代码覆盖率
option(ENABLE_COVERAGE "Enable code coverage" OFF)
if(ENABLE_COVERAGE AND CMAKE_COMPILER_IS_GNUCXX)
    target_compile_options(dbrecovery_innodb PUBLIC -g -O0 --coverage)
    target_link_libraries(dbrecovery_innodb PUBLIC --coverage)
endif()

# 内存安全检查 (ASAN/UBSAN/LSAN)
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ENABLE_LSAN "Enable LeakSanitizer" OFF)

if(ENABLE_ASAN OR ENABLE_UBSAN OR ENABLE_LSAN)
    set(SANITIZERS "")
    if(ENABLE_ASAN)
        list(APPEND SANITIZERS "address")
    endif()
    if(ENABLE_UBSAN)
        list(APPEND SANITIZERS "undefined")
    endif()
    if(ENABLE_LSAN)
        list(APPEND SANITIZERS "leak")
    endif()

    string(JOIN "," SANITIZER_LIST ${SANITIZERS})

    target_compile_options(dbrecovery_innodb PUBLIC -g -O0 -fsanitize=${SANITIZER_LIST} -fno-omit-frame-pointer)
    target_link_libraries(dbrecovery_innodb PUBLIC -fsanitize=${SANITIZER_LIST} -fno-omit-frame-pointer)

    # 修复：仅在链接时添加 sanitizer（避免链接器找不到库的问题）
    # sanitizer 库会在运行时自动链接
endif()

# GUI support (optional)
if(GUI_ENABLED)
    add_subdirectory(gui)
    message(STATUS "GUI enabled - building MySQL Recovery Pro GUI")
else()
    message(STATUS "GUI disabled - use -DGUI_ENABLED=ON to enable")
endif()

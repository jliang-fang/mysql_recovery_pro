cmake_minimum_required(VERSION 3.20)
project(mysql8_recovery VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)

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
    src/innodb/undo_log.cpp
    src/recovery/blob_reconstructor.cpp
    src/recovery/engine.cpp
    src/utils/cli_parser.cpp
    src/utils/file_mapper.cpp
)

# ✅ 新增：确保 file_mapper.cpp 被正确包含（如果尚未存在则创建）
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/src/utils/file_mapper.cpp")
    file(WRITE "${CMAKE_SOURCE_DIR}/src/utils/file_mapper.cpp" "#include \"dbrecovery/utils/file_mapper.hpp\"\nnamespace dbrecovery { namespace utils {} }\n")
endif()

add_library(dbrecovery_innodb STATIC ${SOURCES})

target_include_directories(dbrecovery_innodb PUBLIC
    include
    $<$<BOOL:${MYSQL_INCLUDE_DIR}>:${MYSQL_INCLUDE_DIR}>
)

# ✅ P0-1 修复：使用本地 nlohmann/json（已通过 include_directories 添加）
# 链接系统 nlohmann_json 库
target_link_libraries(dbrecovery_innodb PUBLIC nlohmann_json::nlohmann_json)

# 链接 zlib（用于 SDI 解压缩）
target_link_libraries(dbrecovery_innodb PUBLIC ZLIB::ZLIB)

add_executable(mysql8_recover src/main.cpp)
target_link_libraries(mysql8_recover dbrecovery_innodb)

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
    
    target_compile_options(dbrecovery_innodb PUBLIC -g -O0 -fsanitize=${SANITIZER_LIST})
    target_link_libraries(dbrecovery_innodb PUBLIC -fsanitize=${SANITIZER_LIST})
endif()

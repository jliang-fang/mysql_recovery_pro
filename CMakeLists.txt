cmake_minimum_required(VERSION 3.20)
project(mysql8_recovery VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)

# 集成 nlohmann/json 库
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3 # 稳定版本，支持 C++17
)
FetchContent_MakeAvailable(nlohmann_json)

# 查找 MySQL 头文件
find_path(MYSQL_INCLUDE_DIR mysql_version.h PATHS /usr/include/mysql /usr/local/include/mysql NO_DEFAULT_PATH)
if(MYSQL_INCLUDE_DIR)
    message(STATUS "Found MySQL headers at: ${MYSQL_INCLUDE_DIR}")
else()
    message(WARNING "MySQL headers not found, some features may be disabled")
endif()

# 添加源文件
set(SOURCES
    src/catalog/schema_defs.cpp
    src/engine/row_decoder.cpp
    src/engine/field_decoder.cpp
    src/innodb/binary_reader.cpp
    src/innodb/page_parser.cpp
    src/innodb/page_reader.cpp
    src/innodb/recovery_engine.cpp
    src/innodb/blob_parser.cpp
    src/innodb/v2/page_parser.cpp
    src/innodb/v2/record_parser.cpp
    src/innodb/btree.cpp
    src/recovery/blob_reconstructor.cpp
    src/utils/cli_parser.cpp
    src/utils/file_mapper.cpp
)

add_library(dbrecovery_innodb STATIC ${SOURCES})

target_include_directories(dbrecovery_innodb PUBLIC
    include
    $<$<BOOL:${MYSQL_INCLUDE_DIR}>:${MYSQL_INCLUDE_DIR}>
)

# 链接必要的库
target_link_libraries(dbrecovery_innodb PUBLIC nlohmann_json::nlohmann_json)

add_executable(mysql8_recover src/main.cpp)
target_link_libraries(mysql8_recover dbrecovery_innodb)

enable_testing()

# Unit tests
enable_testing()

find_package(GTest QUIET)
if(GTEST_FOUND)
    add_subdirectory(tests)
    
    # 添加测试数据生成脚本作为全局测试
    add_test(NAME generate_test_data COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/ibd_generator.py test_table)
else()
    message(WARNING "GoogleTest not found — tests disabled")
endif()

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

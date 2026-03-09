cmake_minimum_required(VERSION 3.20)
project(mysql8_recovery VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)

find_path(MYSQL_INCLUDE_DIR mysql_version.h PATHS /usr/include/mysql /usr/local/include/mysql NO_DEFAULT_PATH)
if(MYSQL_INCLUDE_DIR)
    message(STATUS "Found MySQL headers at: ${MYSQL_INCLUDE_DIR}")
endif()

add_library(dbrecovery_innodb STATIC
    src/innodb/binary_reader.cpp
    src/engine/row_decoder.cpp
    src/engine/field_decoder.cpp
    src/recovery/blob_reconstructor.cpp
    src/innodb/recovery_engine.cpp
    src/utils/cli_parser.cpp
    src/innodb/page_reader.cpp
)

# 检查缺失的源文件并添加警告
file(GLOB_RECURSE missing_sources 
    LIST_DIRECTORIES false
    "${CMAKE_SOURCE_DIR}/src/innodb/binary_reader.cpp"
    "${CMAKE_SOURCE_DIR}/src/engine/field_decoder.cpp"
    "${CMAKE_SOURCE_DIR}/src/recovery/blob_reconstructor.cpp"
    "${CMAKE_SOURCE_DIR}/src/innodb/recovery_engine.cpp"
)

foreach(source ${missing_sources})
    if(NOT EXISTS ${source})
        message(WARNING "Missing source file: ${source}")
    endif()
endforeach()

target_include_directories(dbrecovery_innodb PUBLIC
    include
    $<$<BOOL:${MYSQL_INCLUDE_DIR}>:${MYSQL_INCLUDE_DIR}>
)

add_executable(mysql8_recover src/main.cpp)
target_link_libraries(mysql8_recover dbrecovery_innodb)

enable_testing()

# Unit tests
find_package(GTest QUIET)
if(GTEST_FOUND)
    add_executable(test_page_reader test/test_page_reader.cpp)
    target_link_libraries(test_page_reader dbrecovery_innodb GTest::GTest GTest::Main)
    gtest_discover_tests(test_page_reader)
else()
    message(WARNING "GoogleTest not found — tests disabled")
endif()

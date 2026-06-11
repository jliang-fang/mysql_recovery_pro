message(STATUS "=== Loading src/innodb/CMakeLists.txt ===")

# add_subdirectory(v2) # Temporarily disabled for stability

# dbrecovery_core: Pure header-only library for page types v2
# page_types_v2.hpp is header-only, no .cpp file needed
add_library(dbrecovery_core INTERFACE)
target_include_directories(dbrecovery_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

# dbrecovery_innodb_sources: Actual source files
add_library(dbrecovery_innodb_sources STATIC
    page_parser.cpp
    binary_reader.cpp
    sdi_manager.cpp
    blob_pointer.cpp
    page_defs.cpp
    # ✅ 新增：记录解析器 (Redundant/Compact/Dynamic)
    record_parser.cpp
    # ✅ 新增：Instant DDL 解析器
    # ✅ 注意：instant_ddl_parser.cpp 已移除，所有实现都在 instant_ddl_parser.hpp 中
    # ✅ 新增：SDI 解析器
    # ✅ 注意：sdi_parser.cpp 已移除，所有实现都在 sdi_parser_v2.hpp 中
    # ✅ 新增：B+树解析器
    btree.cpp
    # ✅ 新增：页扫描器
    page_scanner.cpp
    # ✅ 新增：页链重连器
    page_chain_rebuilder.cpp
    # ✅ 新增：B+树节点解析器
    btree_node_parser.cpp
    # ✅ 新增：记录链重连器
    record_chain_rebuilder.cpp
    # ✅ 新增：BLOB 链重连器
    blob_chain_rebuilder.cpp
    # ✅ 新增：页解析 V2 (多版本支持)
    page_parser_v2.cpp
    # ✅ 新增：页数据导出器
    page_exporter.cpp
    # ✅ 新增：页可视化器
    page_visualizer.cpp
    # ✅ 新增：Undo Log 解析器
    # ✅ 修复：移除 undo_log.cpp（重复定义，已在 undo_log_parser.cpp 中实现）
    undo_log_parser.cpp
    # ✅ 新增：Redo Log 解析器
    redo_log.cpp
    redo_log_replayer.cpp
    redo_log_forward.cpp
    redo_log_enhanced.cpp
    # ✅ 新增：TDE 解密器
    tde_decryptor.cpp
    # ✅ 新增：性能基准测试
    performance_benchmark.cpp
    # ✅ 新增：数据验证器
    data_validator.cpp
    # ✅ 新增：取证恢复模块 (用于 TRUNCATE/rm-rf 场景)
    forensic_recovery.cpp
    forensic_manager.cpp
)
target_include_directories(dbrecovery_innodb_sources PUBLIC ${CMAKE_SOURCE_DIR}/include)

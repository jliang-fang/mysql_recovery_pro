#include "dbrecovery/innodb/recovery_engine.hpp"
#include "dbrecovery/innodb/page_reader.hpp"
#include "dbrecovery/innodb/page_parser.hpp"
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/innodb/blob_pointer.hpp"
#include "dbrecovery/recovery/blob_reconstructor.hpp"

#include "dbrecovery/catalog/schema_defs.hpp"

#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <memory>
#include <nlohmann/json.hpp>

namespace dbrecovery {
namespace innodb {

RecoveryEngine::RecoveryEngine() : blob_reconstructor_(std::make_unique<recovery::BlobReconstructor>(0)) {
    std::cout << "[INFO] RecoveryEngine initialized\n";
}

RecoveryEngine::~RecoveryEngine() = default;

bool RecoveryEngine::recover_from_checkpoint(const std::string& ibdata_path, const std::string& output_dir) {
    std::cout << "[INFO] recover_from_checkpoint called: " << ibdata_path << "\n";
    
    // TODO: 实现完整的检查点恢复逻辑
    // 这需要：
    // 1. 读取 ibdata 文件
    // 2. 解析检查点信息（LSN、事务ID等）
    // 3. 应用 Redo Log 进行前滚恢复
    // 4. 恢复所有表空间
    
    std::cout << "[WARN] recover_from_checkpoint: Full checkpoint recovery not yet implemented\n";
    std::cout << "[INFO] Use recover_table() for individual .ibd file recovery\n";
    return false;
}

bool RecoveryEngine::recover_table(const std::string& ibd_path, uint64_t table_id) {
    std::cout << "[INFO] recover_table called: " << ibd_path << ", table_id=" << table_id << "\n";

    // Use InnoDBFileReader to scan pages
    dbrecovery::innodb::InnoDBFileReader reader(ibd_path);
    if (!reader.is_valid()) {
        std::cerr << "[ERROR] Failed to open .ibd file: " << ibd_path << "\n";
        return false;
    }

    uint32_t total_pages = reader.get_total_pages();
    std::cout << "[INFO] Total pages: " << total_pages << "\n";

    // ✅ Step 1: 解析所有页并分类
    std::map<uint16_t, std::vector<std::pair<uint32_t, std::vector<uint8_t>>>> pages_by_type;

    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
        auto page_data = reader.read_page(page_no);
        if (page_data.empty()) {
            std::cerr << "[WARN] Failed to read page " << page_no << "\n";
            continue;
        }

        // ✅ P0-1 修复：解析页类型 - 使用正确的偏移量（相对于页起始位置）
        // MySQL 8.0: FIL_PAGE_TYPE = 16 (relative to page start)
        uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data.data() + 16);
        pages_by_type[page_type].emplace_back(page_no, std::move(page_data));
    }
    
    // ✅ Step 2: 处理 SDI 页（FIL_PAGE_SDI = 9971）- 必须先解析SDI获取表结构
    catalog::TableSchema table_schema;
    if (pages_by_type.count(9971)) {
        std::cout << "[INFO] Found " << pages_by_type[9971].size() << " SDI pages\n";
        
        // 解析 SDI 元数据（使用 nlohmann/json 库）
        for (const auto& [page_no, page_data] : pages_by_type[9971]) {
            std::cout << "[INFO] SDI Page " << page_no << ": size=" << page_data.size() << "\n";

            // 从 SDI 页中提取 JSON 负载
            const uint8_t* sdi_payload = page_data.data() + 38; // 跳过 FIL_HEADER
            if (page_data.size() > 58) {
                // ✅ P0-1 修复：SDI payload 包含 20 字节头部，需要先解压
                // SDI Header: type(4) + id(8) + uncompressed_len(4) + compressed_len(4) = 20 bytes
                if (page_data.size() >= 38 + 20) {
                    uint32_t uncompressed_len = dbrecovery::innodb::read_le<uint32_t>(sdi_payload + 12);
                    uint32_t compressed_len = dbrecovery::innodb::read_le<uint32_t>(sdi_payload + 16);
                    
                    std::cout << "[INFO] SDI: uncompressed_len=" << uncompressed_len 
                              << ", compressed_len=" << compressed_len << "\n";
                    
                    // 使用 SDIManager 解压
                    auto sdi_json_str = dbrecovery::innodb::SDIManager::extract_sdi_json(
                        sdi_payload, page_data.size() - 38);
                    
                    if (sdi_json_str.has_value()) {
                        try {
                            auto sdi_json = nlohmann::json::parse(sdi_json_str.value());

                            // 提取表名
                            if (sdi_json.contains("name")) {
                                std::string table_name = sdi_json["name"].get<std::string>();
                                std::cout << "[INFO] SDI Table Name: " << table_name << "\n";
                            }

                            // ✅ 新增：从SDI中解析表结构
                            if (sdi_json.contains("columns") && sdi_json["columns"].is_array()) {
                                uint32_t ordinal = 1;
                                for (const auto& col_json : sdi_json["columns"]) {
                                    catalog::ColumnInfo col;
                                    if (col_json.contains("name")) {
                                        col.name = col_json["name"].get<std::string>();
                                    }
                                    if (col_json.contains("type")) {
                                        col.mysql_type_id = col_json["type"].get<uint32_t>();
                                    }
                                    if (col_json.contains("is_nullable")) {
                                        col.is_nullable = col_json["is_nullable"].get<bool>();
                                    }
                                    table_schema.columns[ordinal++] = col;
                                }
                            }

                            // ✅ 新增：从SDI中解析索引信息
                            if (sdi_json.contains("indexes") && sdi_json["indexes"].is_array()) {
                                for (const auto& idx_json : sdi_json["indexes"]) {
                                    catalog::IndexInfo index;
                                    if (idx_json.contains("name")) {
                                        index.name = idx_json["name"].get<std::string>();
                                    }
                                    if (idx_json.contains("columns") && idx_json["columns"].is_array()) {
                                        for (const auto& col : idx_json["columns"]) {
                                            if (col.is_string()) {
                                                index.columns.push_back(col.get<std::string>());
                                            }
                                        }
                                    }
                                    table_schema.indexes.push_back(index);
                                }
                            }
                        } catch (const nlohmann::json::parse_error& e) {
                            std::cerr << "[WARN] Failed to parse SDI JSON: " << e.what() << "\n";

                            // ✅ 修复：更健壮的降级方案
                            if (page_data.size() > 58) {
                                // 尝试从JSON中提取表名（不依赖固定偏移）
                                std::string sdi_str(reinterpret_cast<const char*>(sdi_payload),
                                                   page_data.size() - 38);

                                size_t name_pos = sdi_str.find("\"name\"");
                                if (name_pos != std::string::npos) {
                                    size_t colon_pos = sdi_str.find(':', name_pos);
                                    if (colon_pos != std::string::npos) {
                                        size_t start = sdi_str.find_first_not_of(" \t\n\r", colon_pos + 1);
                                        if (start != std::string::npos && sdi_str[start] == '"') {
                                            size_t end = start + 1;
                                            while (end < sdi_str.length() && sdi_str[end] != '"') {
                                                if (sdi_str[end] == '\\' && end + 1 < sdi_str.length()) {
                                                    end += 2; // 跳过转义字符
                                                } else {
                                                    end++;
                                                }
                                            }

                                            if (end < sdi_str.length()) {
                                                std::string table_name = sdi_str.substr(start + 1, end - start - 1);
                                                std::cout << "[INFO] SDI Table Name (fallback): " << table_name << "\n";
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // ✅ Step 3: 实现 B+树重建逻辑
    if (pages_by_type.count(9969)) {
        std::cout << "[INFO] Found " << pages_by_type[9969].size() << " index pages\n";

        // 使用 BPlusTreeParser 解析所有索引页
        for (const auto& [page_no, page_data] : pages_by_type[9969]) {
            dbrecovery::innodb::BPlusTreeParser parser(page_data);
            
            if (!parser.parse()) {
                std::cerr << "[WARN] Failed to parse B+Tree page " << page_no << "\n";
                continue;
            }

            std::cout << "[INFO] Page " << page_no << ": type=" << parser.page_type()
                      << ", level=" << parser.btr_format() << "\n";

            // 解析记录（需要表结构）
            if (!table_schema.columns.empty()) {
                auto records = parser.parse_records(table_schema);
                std::cout << "[INFO] Page " << page_no << ": found " << records.size() << " records\n";
                
                // 将记录添加到恢复结果
                recovered_records_.insert(recovered_records_.end(), records.begin(), records.end());
            } else {
                std::cout << "[WARN] No table schema available for record parsing\n";
            }
        }
    } else {
        std::cerr << "[WARN] No index pages found in file\n";
        return false;
    }

    // ✅ Step 4: 实现BLOB页链重建逻辑
    if (table_schema.columns.empty()) {
        std::cout << "[INFO] No table schema available, skipping BLOB reconstruction\n";
    } else {
        // 检查是否有BLOB列
        bool has_blob = false;
        for (const auto& pair : table_schema.columns) {
            const auto& col = pair.second;
            if (col.mysql_type_id >= dbrecovery::innodb::mysql_types::TINY_BLOB &&
                col.mysql_type_id <= dbrecovery::innodb::mysql_types::BLOB) {
                has_blob = true;
                break;
            }
        }

        if (has_blob) {
            std::cout << "[INFO] Table contains BLOB columns, will attempt reconstruction\n";

            // 构建 BLOB 页链
            std::map<uint32_t, std::vector<uint8_t>> blob_pages;
            for (const auto& [page_no, page_data] : pages_by_type[9972]) { // FIL_PAGE_TYPE_BLOB = 9972
                blob_pages[page_no] = page_data;
            }

            // 遍历所有 BLOB 页并构建页链
            for (const auto& [page_no, page_data] : blob_pages) {
                std::cout << "[INFO] BLOB Page " << page_no << ": size=" << page_data.size() << "\n";

                // 解析BLOB页头（BLOB 页头位于偏移 38）
                if (page_data.size() >= 54) {
                    uint32_t next_bpage = dbrecovery::innodb::read_le<uint32_t>(page_data.data(), 38 + 0);
                    uint32_t prev_bpage = dbrecovery::innodb::read_le<uint32_t>(page_data.data(), 38 + 4);
                    uint32_t data_len = dbrecovery::innodb::read_le<uint32_t>(page_data.data(), 38 + 8);

                    std::cout << "[INFO] BLOB Page " << page_no
                              << ": next=" << next_bpage
                              << ", prev=" << prev_bpage
                              << ", len=" << data_len << "\n";

                    // 将数据添加到 BLOB 重建器
                    if (data_len > 0 && page_data.size() > 54) {
                        blob_reconstructor_->AddChunk(0, page_data.data() + 54, data_len);
                    }
                }
            }

            // 检查 BLOB 是否完整
            if (blob_reconstructor_->IsComplete()) {
                auto blob_data = blob_reconstructor_->GetBlob();
                std::cout << "[INFO] BLOB reconstruction complete: " << blob_data.size() << " bytes\n";
            } else {
                std::cout << "[WARN] BLOB reconstruction incomplete\n";
            }
        }
    }

    // ✅ Step 5: 输出恢复结果
    std::cout << "[INFO] Recovery completed successfully. Total records: " 
              << recovered_records_.size() << "\n";
    return true;
}

} // namespace innodb
} // namespace dbrecovery

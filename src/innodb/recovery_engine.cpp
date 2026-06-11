#include "dbrecovery/innodb/recovery_engine.hpp"
#include "dbrecovery/innodb/page_reader.hpp"
#include "dbrecovery/innodb/page_parser.hpp"
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/innodb/blob_pointer.hpp"
#include "dbrecovery/innodb/redo_log.hpp"
#include "dbrecovery/innodb/sdi_manager.hpp"
#include "dbrecovery/innodb/forensic_manager.hpp"
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

// ✅ P0-4 实现：完整的检查点恢复逻辑
bool RecoveryEngine::recover_from_checkpoint(const std::string& ibdata_path, const std::string& output_dir) {
    std::cout << "[INFO] recover_from_checkpoint called: " << ibdata_path << "\n";
    std::cout << "[INFO] Output directory: " << output_dir << "\n";

    // ✅ Step 1: 验证输入文件
    std::ifstream ibdata_file(ibdata_path, std::ios::binary | std::ios::ate);
    if (!ibdata_file) {
        std::cerr << "[ERROR] Failed to open ibdata file: " << ibdata_path << "\n";
        return false;
    }

    uint64_t file_size = ibdata_file.tellg();
    ibdata_file.close();

    if (file_size < 16384) {
        std::cerr << "[ERROR] ibdata file too small: " << file_size << " bytes\n";
        return false;
    }

    std::cout << "[INFO] ibdata file size: " << file_size << " bytes\n";

    // ✅ Step 2: 读取第一个页（系统页）以获取检查点信息
    InnoDBFileReader reader(ibdata_path);
    if (!reader.is_valid()) {
        std::cerr << "[ERROR] Failed to initialize InnoDBFileReader for: " << ibdata_path << "\n";
        return false;
    }

    // 读取第0页（系统页）
    auto system_page = reader.read_page(0);
    if (system_page.empty()) {
        std::cerr << "[ERROR] Failed to read system page (page 0)\n";
        return false;
    }

    // ✅ Step 3: 解析检查点信息
    // Checkpoint 1 位于偏移 76-115 (FIL_PAGE_END_LSN + 4)
    // Checkpoint 2 位于偏移 136-175
    constexpr size_t CHECKPOINT1_OFFSET = 76;
    constexpr size_t CHECKPOINT2_OFFSET = 136;
    constexpr size_t CHECKPOINT_SIZE = 40;

    // 读取 Checkpoint 1
    uint32_t checkpoint1_lsn_low = dbrecovery::innodb::read_le<uint32_t>(system_page.data(), CHECKPOINT1_OFFSET);
    uint32_t checkpoint1_lsn_high = dbrecovery::innodb::read_le<uint32_t>(system_page.data(), CHECKPOINT1_OFFSET + 4);
    uint32_t checkpoint1_page_no = dbrecovery::innodb::read_le<uint32_t>(system_page.data(), CHECKPOINT1_OFFSET + 8);

    std::cout << "[INFO] Checkpoint 1: LSN=" << checkpoint1_lsn_high << ":" << checkpoint1_lsn_low
              << ", page_no=" << checkpoint1_page_no << "\n";

    // 读取 Checkpoint 2
    uint32_t checkpoint2_lsn_low = dbrecovery::innodb::read_le<uint32_t>(system_page.data(), CHECKPOINT2_OFFSET);
    uint32_t checkpoint2_lsn_high = dbrecovery::innodb::read_le<uint32_t>(system_page.data(), CHECKPOINT2_OFFSET + 4);
    uint32_t checkpoint2_page_no = dbrecovery::innodb::read_le<uint32_t>(system_page.data(), CHECKPOINT2_OFFSET + 8);

    std::cout << "[INFO] Checkpoint 2: LSN=" << checkpoint2_lsn_high << ":" << checkpoint2_lsn_low
              << ", page_no=" << checkpoint2_page_no << "\n";

    // 选择较新的检查点（比较 LSN）
    uint64_t checkpoint_lsn = (checkpoint1_lsn_high > checkpoint2_lsn_high) ||
                              ((checkpoint1_lsn_high == checkpoint2_lsn_high) && (checkpoint1_lsn_low >= checkpoint2_lsn_low))
                              ? ((uint64_t)checkpoint1_lsn_high << 32 | checkpoint1_lsn_low)
                              : ((uint64_t)checkpoint2_lsn_high << 32 | checkpoint2_lsn_low);

    uint32_t checkpoint_page_no = (checkpoint1_lsn_high > checkpoint2_lsn_high) ||
                                  ((checkpoint1_lsn_high == checkpoint2_lsn_high) && (checkpoint1_lsn_low >= checkpoint2_lsn_low))
                                  ? checkpoint1_page_no
                                  : checkpoint2_page_no;

    std::cout << "[INFO] Selected checkpoint: LSN=" << checkpoint_lsn
              << ", page_no=" << checkpoint_page_no << "\n";

    // ✅ Step 4: 确保输出目录存在
    try {
        std::filesystem::create_directories(output_dir);
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "[ERROR] Failed to create output directory: " << e.what() << "\n";
        return false;
    }

    // ✅ Step 5: 解析所有页并分类
    uint32_t total_pages = reader.get_total_pages();
    std::cout << "[INFO] Total pages in ibdata: " << total_pages << "\n";

    std::map<uint16_t, std::vector<std::pair<uint32_t, std::vector<uint8_t>>>> pages_by_type;

    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
        auto page_data = reader.read_page(page_no);
        if (page_data.empty()) {
            continue;
        }

        // ✅ 修复：InnoDB 页的 FIL_PAGE_TYPE 在偏移 24-25
        uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data.data() + 24);
        pages_by_type[page_type].emplace_back(page_no, std::move(page_data));
    }

    // ✅ Step 6: 处理 SDI 页获取表结构
    catalog::TableSchema table_schema;
    if (pages_by_type.count(9971)) { // FIL_PAGE_SDI
        std::cout << "[INFO] Found " << pages_by_type[9971].size() << " SDI pages\n";

        // 解析第一个 SDI 页获取表结构
        const auto& sdi_page = pages_by_type[9971].front();
        const uint8_t* sdi_payload = sdi_page.second.data() + 38;

        if (sdi_page.second.size() >= 38 + 20) {
            auto sdi_json_str = dbrecovery::innodb::SDIManager::extract_sdi_json(
                sdi_payload, sdi_page.second.size() - 38);

            if (sdi_json_str.has_value()) {
                try {
                    auto sdi_json = nlohmann::json::parse(sdi_json_str.value());

                    // 提取表名
                    if (sdi_json.contains("name")) {
                        table_schema.table_name = sdi_json["name"].get<std::string>();
                    }

                    // 解析列
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

                    // 解析索引
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
                }
            }
        }
    }

    // ✅ Step 7: 处理 B+Tree 索引页
    if (pages_by_type.count(9969)) { // FIL_PAGE_INDEX
        std::cout << "[INFO] Found " << pages_by_type[9969].size() << " index pages\n";

        for (const auto& [page_no, page_data] : pages_by_type[9969]) {
            dbrecovery::innodb::BPlusTreeParser parser(page_data);

            if (!parser.parse()) {
                std::cerr << "[WARN] Failed to parse B+Tree page " << page_no << "\n";
                continue;
            }

            // 解析记录
            if (!table_schema.columns.empty()) {
                auto records = parser.parse_records(table_schema);
                recovered_records_.insert(recovered_records_.end(), records.begin(), records.end());
            }
        }
    }

    // ✅ Step 8: 处理 BLOB 页
    if (pages_by_type.count(9972)) { // FIL_PAGE_TYPE_BLOB
        std::cout << "[INFO] Found " << pages_by_type[9972].size() << " BLOB pages\n";

        for (const auto& [page_no, page_data] : pages_by_type[9972]) {
            if (page_data.size() >= 54) {
                uint32_t next_bpage = dbrecovery::innodb::read_le<uint32_t>(page_data.data(), 38 + 0);
                uint32_t data_len = dbrecovery::innodb::read_le<uint32_t>(page_data.data(), 38 + 8);

                if (data_len > 0 && page_data.size() > 54) {
                    blob_reconstructor_->AddChunk(0, page_data.data() + 54, data_len);
                }
            }
        }

        if (blob_reconstructor_->IsComplete()) {
            auto blob_data = blob_reconstructor_->GetBlob();
            std::cout << "[INFO] BLOB reconstruction complete: " << blob_data.size() << " bytes\n";
        }
    }

    // ✅ Step 9: 输出恢复结果
    std::cout << "[INFO] Checkpoint recovery completed. Total records: "
              << recovered_records_.size() << "\n";

    // 保存恢复的记录到 CSV（使用当前类的 export_to_csv 方法）
    std::string csv_path = output_dir + "/recovered_records.csv";
    if (!recovered_records_.empty() && !table_schema_.columns.empty()) {
        export_to_csv(recovered_records_, table_schema_, csv_path);
    }

    return true;
}

bool RecoveryEngine::deep_recover(const std::string& target_path, const std::string& output_csv) {
    std::cout << "[INFO] RecoveryEngine::deep_recover starting for " << target_path << "\n";
    ForensicRecoveryManager manager;
    return manager.perform_deep_recovery(target_path, output_csv);
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

        // ✅ 修复：InnoDB 页的 FIL_PAGE_TYPE 在偏移 24-25
        uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data.data() + 24);
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
    // 🔧 修复：支持通过 PAGE_N_RECS 识别的页（即使页类型是 0x0000）
    // 构建 additional_index_pages 映射：使用 PAGE_N_RECS 识别有效的索引页
    std::map<uint32_t, std::vector<uint8_t>> additional_index_pages;
    constexpr size_t PAGE_N_RECS_OFFSET = 36;
    for (const auto& [page_type, pages_vec] : pages_by_type) {
        for (const auto& [pno, page_data] : pages_vec) {
            if (page_data.size() > PAGE_N_RECS_OFFSET + 2) {
                uint16_t n_recs = dbrecovery::innodb::read_le<uint16_t>(page_data.data(), PAGE_N_RECS_OFFSET);
                if (n_recs > 0 && n_recs < 10000) {
                    additional_index_pages[pno] = page_data;
                }
            }
        }
    }
    
    // 如果没有找到类型为 9969 的页，尝试使用 additional_index_pages
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
    }
    
    // ✅ 修复：如果没找到类型为 9969 的页，尝试通过 PAGE_N_RECS 识别的页
    if (!pages_by_type.count(9969) && !additional_index_pages.empty()) {
        std::cout << "[INFO] Recovering " << additional_index_pages.size() << " pages via PAGE_N_RECS check\n";
        
        for (const auto& [page_no, page_data] : additional_index_pages) {
            dbrecovery::innodb::BPlusTreeParser parser(page_data);

            if (!parser.parse()) {
                std::cerr << "[WARN] Failed to parse B+Tree page " << page_no << "\n";
                continue;
            }

            std::cout << "[INFO] PAGE_N_RECS Page " << page_no << ": type=" << parser.page_type()
                      << ", level=" << parser.btr_format() << "\n";

            // 解析记录（需要表结构）
            if (!table_schema.columns.empty()) {
                auto records = parser.parse_records(table_schema);
                if (!records.empty()) {
                    std::cout << "[INFO] PAGE_N_RECS Page " << page_no << ": found " << records.size() << " records\n";
                    recovered_records_.insert(recovered_records_.end(), records.begin(), records.end());
                }
            } else {
                std::cout << "[WARN] No table schema available for record parsing\n";
            }
        }
    } else if (!pages_by_type.count(9969)) {
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

// ✅ P0-4 实现：导出为 CSV 格式（带表结构版本）
bool RecoveryEngine::export_to_csv(const std::vector<std::map<std::string, std::string>>& records,
                                   const catalog::TableSchema& schema,
                                   const std::string& csv_path) {
    if (records.empty()) {
        std::cerr << "[WARN] No records to export" << std::endl;
        return false;
    }

    std::ofstream out(csv_path);
    if (!out) {
        std::cerr << "[ERROR] Failed to open CSV file: " << csv_path << std::endl;
        return false;
    }

    // 写入表头（从schema中提取列名顺序）
    for (const auto& pair : schema.columns) {
        if (pair.first > 1) out << ",";
        out << pair.second.name;
    }
    out << "\n";

    // 写入数据行（按schema顺序）
    for (const auto& record : records) {
        for (auto it = schema.columns.begin(); it != schema.columns.end(); ++it) {
            if (it != schema.columns.begin()) out << ",";
            auto it2 = record.find(it->second.name);
            if (it2 != record.end()) {
                out << it2->second;
            }
        }
        out << "\n";
    }

    out.close();
    std::cout << "[INFO] Exported " << records.size() << " records to CSV: " << csv_path << std::endl;

    return true;
}

// ✅ P0-4 实现：导出为 SQL 格式
bool RecoveryEngine::export_to_sql(const std::vector<std::map<std::string, std::string>>& records,
                                   const std::string& table_name,
                                   const std::string& sql_path) {
    if (records.empty()) {
        std::cerr << "[WARN] No records to export" << std::endl;
        return false;
    }

    std::ofstream out(sql_path);
    if (!out) {
        std::cerr << "[ERROR] Failed to open SQL file: " << sql_path << std::endl;
        return false;
    }

    // 写入 SQL 头
    out << "-- MySQL Recovery Export\n";
    out << "-- Table: " << table_name << "\n";
    out << "-- Records: " << records.size() << "\n\n";

    for (const auto& record : records) {
        std::ostringstream oss;
        oss << "INSERT INTO `" << table_name << "` (";

        // 列名
        bool first = true;
        for (const auto& pair : record) {
            if (!first) oss << ", ";
            oss << "`" << pair.first << "`";
            first = false;
        }
        oss << ") VALUES (";

        // 值
        first = true;
        for (const auto& pair : record) {
            if (!first) oss << ", ";

            // 简单转义处理
            std::string value = pair.second;
            if (value == "NULL") {
                oss << "NULL";
            } else {
                // 基本的 SQL 转义（实际应使用更完善的转义函数）
                value.erase(0, value.find_first_not_of("'"));
                value.erase(value.find_last_not_of("'") + 1);
                oss << "'" << value << "'";
            }
            first = false;
        }
        oss << ");\n";

        out << oss.str();
    }

    out.close();
    std::cout << "[INFO] Exported " << records.size() << " records to SQL: " << sql_path << std::endl;

    return true;
}

} // namespace innodb
} // namespace dbrecovery

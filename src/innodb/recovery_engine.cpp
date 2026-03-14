#include "dbrecovery/innodb/recovery_engine.hpp"
#include "dbrecovery/innodb/page_reader.hpp"
#include <iostream>

namespace dbrecovery {
namespace innodb {

RecoveryEngine::RecoveryEngine() {
    std::cout << "[INFO] RecoveryEngine initialized\n";
}

RecoveryEngine::~RecoveryEngine() = default;

bool RecoveryEngine::recover_from_checkpoint(const std::string& ibdata_path, const std::string& output_dir) {
    std::cout << "[WARN] recover_from_checkpoint not yet implemented\n";
    return false;
}

bool RecoveryEngine::recover_table(const std::string& ibd_path, uint64_t table_id) {
    std::cout << "[INFO] recover_table called: " << ibd_path << ", table_id=" << table_id << "\n";
    
    // Use PageReader to scan pages
    dbrecovery::innodb::PageReader reader(ibd_path);
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
        
        // 解析页类型
        uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data.data() + 12);
        pages_by_type[page_type].emplace_back(page_no, std::move(page_data));
    }
    
    // ✅ Step 2: 处理索引页（FIL_PAGE_INDEX = 9969）
    if (pages_by_type.count(9969)) {
        std::cout << "[INFO] Found " << pages_by_type[9969].size() << " index pages\n";
        
        // TODO: 实现 B+树重建逻辑
        for (const auto& [page_no, page_data] : pages_by_type[9969]) {
            dbrecovery::innodb::PageParser parser(page_data.data(), page_data.size());
            if (parser.parse_header()) {
                std::cout << "[INFO] Page " << page_no << ": type=" << parser.page_type() 
                          << ", level=" << parser.page_level() << "\n";
                
                // 解析记录
                auto records = parser.parse_records();
                std::cout << "[INFO] Page " << page_no << ": found " << records.size() << " records\n";
            }
        }
    }
    
    // ✅ Step 3: 处理 SDI 页（FIL_PAGE_SDI = 9971）
    if (pages_by_type.count(9971)) {
        std::cout << "[INFO] Found " << pages_by_type[9971].size() << " SDI pages\n";
        
        // 解析 SDI 元数据（使用 nlohmann/json 库）
        for (const auto& [page_no, page_data] : pages_by_type[9971]) {
            std::cout << "[INFO] SDI Page " << page_no << ": size=" << page_data.size() << "\n";
            
            // 从 SDI 页中提取 JSON 负载
            const uint8_t* sdi_payload = page_data.data() + 38; // 跳过 FIL_HEADER
            if (page_data.size() > 58) {
                std::string json_str(reinterpret_cast<const char*>(sdi_payload), 
                                    page_data.size() - 38);
                
                try {
                    // 使用 nlohmann/json 解析 SDI JSON
                    auto sdi_json = nlohmann::json::parse(json_str);
                    
                    // 提取表名
                    if (sdi_json.contains("name")) {
                        std::string table_name = sdi_json["name"].get<std::string>();
                        std::cout << "[INFO] SDI Table Name: " << table_name << "\n";
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
    
    return true;
}

} // namespace innodb
} // namespace dbrecovery

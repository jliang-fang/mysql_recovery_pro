#include "dbrecovery/recovery/rmrf_recovery.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/page_reader.hpp"
#include "dbrecovery/innodb/btree.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <set>

namespace dbrecovery {
namespace recovery {

// ==================== RmrfRecovery 实现 ====================

RmrfRecovery::RmrfRecovery() : page_size_(16384) {}

RmrfRecovery::~RmrfRecovery() = default;

void RmrfRecovery::set_page_size(uint32_t page_size) {
    if (page_size == 4096 || page_size == 8192 || page_size == 16384 ||
        page_size == 32768 || page_size == 65536) {
        page_size_ = page_size;
    }
}

bool RmrfRecovery::scan_directory(const std::string& dir_path) {
    // TODO: 实现目录扫描功能
    // 这需要：
    // 1. 递归扫描指定目录
    // 2. 查找所有 .ibd 文件
    // 3. 读取每个文件的页并分类
    // 4. 尝试重建表结构

    std::cerr << "[WARN] RmrfRecovery::scan_directory: Not yet implemented" << std::endl;
    return false;
}

bool RmrfRecovery::scan_raw_device(const std::string& device_path) {
    // TODO: 实现原始设备扫描功能
    // 这需要：
    // 1. 以二进制模式打开设备
    // 2. 逐页读取并验证
    // 3. 识别 InnoDB 页类型
    // 4. 构建页链

    std::cerr << "[WARN] RmrfRecovery::scan_raw_device: Not yet implemented" << std::endl;
    return false;
}

std::vector<std::map<std::string, std::string>>
RmrfRecovery::recover_records(const std::vector<uint8_t>& page_data,
                               const catalog::TableSchema& schema) {
    std::vector<std::map<std::string, std::string>> records;

    // 检查页类型
    uint16_t page_type = innodb::read_le<uint16_t>(page_data.data(), innodb::FIL_PAGE_TYPE_ABS);
    if (page_type != innodb::FIL_PAGE_INDEX) {
        std::cerr << "[WARN] RmrfRecovery: Page is not an index page" << std::endl;
        return records;
    }

    // 解析 B+Tree 页
    innodb::BPlusTreeParser parser(page_data);
    if (!parser.parse()) {
        std::cerr << "[WARN] RmrfRecovery: Failed to parse page" << std::endl;
        return records;
    }

    // 解析记录
    records = parser.parse_records(schema);

    return records;
}

bool RmrfRecovery::reconstruct_from_pages(
    const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& pages,
    const catalog::TableSchema& schema,
    std::vector<std::map<std::string, std::string>>& recovered_records) {
    
    if (pages.empty()) {
        std::cerr << "[WARN] RmrfRecovery: No pages to reconstruct" << std::endl;
        return false;
    }

    // 按页号排序
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> sorted_pages = pages;
    std::sort(sorted_pages.begin(), sorted_pages.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // 遍历所有页并提取记录
    std::set<uint32_t> visited_pages;
    for (const auto& [page_no, page_data] : sorted_pages) {
        // 跳过已访问的页（防止循环）
        if (visited_pages.find(page_no) != visited_pages.end()) {
            continue;
        }
        visited_pages.insert(page_no);

        // 检查页类型
        uint16_t page_type = innodb::read_le<uint16_t>(page_data.data(), innodb::FIL_PAGE_TYPE_ABS);
        if (page_type != innodb::FIL_PAGE_INDEX) {
            continue;
        }

        // 解析 B+Tree 页
        innodb::BPlusTreeParser parser(page_data);
        if (!parser.parse()) {
            std::cerr << "[WARN] RmrfRecovery: Failed to parse page " << page_no << std::endl;
            continue;
        }

        // 解析记录
        auto page_records = parser.parse_records(schema);
        recovered_records.insert(recovered_records.end(),
                                 page_records.begin(), page_records.end());
    }

    std::cout << "[INFO] RmrfRecovery: Recovered " << recovered_records.size() 
              << " records from " << visited_pages.size() << " pages" << std::endl;

    return !recovered_records.empty();
}

bool RmrfRecovery::reconstruct_from_file(const std::string& file_path,
                                          const catalog::TableSchema& schema,
                                          std::vector<std::map<std::string, std::string>>& recovered_records) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        std::cerr << "[ERROR] RmrfRecovery: Failed to open file: " << file_path << std::endl;
        return false;
    }

    // 获取文件大小
    file.seekg(0, std::ios::end);
    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    // 计算总页数
    uint32_t total_pages = static_cast<uint32_t>(file_size / page_size_);

    if (total_pages == 0) {
        std::cerr << "[WARN] RmrfRecovery: File is too small" << std::endl;
        return false;
    }

    // 逐页读取
    std::vector<uint8_t> page_data(page_size_);
    uint32_t recovered_pages = 0;

    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
        file.seekg(page_no * page_size_, std::ios::beg);
        file.read(reinterpret_cast<char*>(page_data.data()), page_size_);

        // 检查页类型
        uint16_t page_type = innodb::read_le<uint16_t>(page_data.data(), innodb::FIL_PAGE_TYPE_ABS);
        if (page_type != innodb::FIL_PAGE_INDEX) {
            continue;
        }

        // 解析 B+Tree 页
        innodb::BPlusTreeParser parser(page_data);
        if (!parser.parse()) {
            std::cerr << "[WARN] RmrfRecovery: Failed to parse page " << page_no << std::endl;
            continue;
        }

        // 解析记录
        auto page_records = parser.parse_records(schema);
        recovered_records.insert(recovered_records.end(),
                                 page_records.begin(), page_records.end());
        recovered_pages++;
    }

    std::cout << "[INFO] RmrfRecovery: Recovered " << recovered_records.size() 
              << " records from " << recovered_pages << " pages" << std::endl;

    return !recovered_records.empty();
}

} // namespace recovery
} // namespace dbrecovery

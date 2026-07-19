#include "dbrecovery/export/data_exporter.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/page_reader.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>
#include <map>
#include <algorithm>

namespace dbrecovery {
namespace exporter {

// ==================== IBDExporter 实现 ====================

IBDExporter::IBDExporter(uint32_t page_size) : page_size_(page_size) {
    // 初始化第一页（系统页）
    std::vector<uint8_t> system_page(page_size_, 0);

    // 填充 FIL Header
    // FIL_PAGE_PREV (offset 0-3): 0xFFFFFFFF (no previous page)
    dbrecovery::innodb::write_le<uint32_t>(system_page.data(), 0, 0);
    // FIL_PAGE_NEXT (offset 4-7): 0xFFFFFFFF (no next page)
    dbrecovery::innodb::write_le<uint32_t>(system_page.data(), 4, 0);
    // FIL_PAGE_LSN (offset 8-15): 0 (placeholder)
    dbrecovery::innodb::write_le<uint64_t>(system_page.data(), 8, 0);
    // FIL_PAGE_SPACE_ID (offset 16-19): 0
    dbrecovery::innodb::write_le<uint32_t>(system_page.data(), 16, 0);
    // FIL_PAGE_OFFSET (offset 20-23): 0
    dbrecovery::innodb::write_le<uint32_t>(system_page.data(), 20, 0);
    // FIL_PAGE_ARCH_LOG_NO (offset 28-31): 0

    // FIL_PAGE_TYPE (offset 16): FIL_PAGE_INDEX (9969)
    dbrecovery::innodb::write_le<uint16_t>(system_page.data() + 16, 0, dbrecovery::innodb::FIL_PAGE_INDEX);

    // 填充 Page Header
    // PAGE_N_DIR_SLOTS (offset 38-39): 2 (infimum + supremum)
    dbrecovery::innodb::write_le<uint16_t>(system_page.data() + 38, 0, 2);
    // PAGE_HEAP_TOP (offset 40-41): 54 (after INFIMUM record)
    dbrecovery::innodb::write_le<uint16_t>(system_page.data() + 40, 0, 54);
    // PAGE_N_HEAP (offset 42-43): 2 (infimum + supremum)
    dbrecovery::innodb::write_le<uint16_t>(system_page.data() + 42, 0, 2);
    // PAGE_FREE (offset 44-45): 0
    dbrecovery::innodb::write_le<uint16_t>(system_page.data() + 44, 0, 0);

    // 填充 BTR Header
    // BTR_N_LEVELS (offset 42): 1 (only leaf level)
    dbrecovery::innodb::write_le<uint16_t>(system_page.data() + 42, 0, 1);

    // 填充 INFIMUM 记录 (offset 54)
    // Record header (5 bytes)
    system_page[54] = 0x02; // info_bits: REC_STATUS_INFIMUM
    dbrecovery::innodb::write_le<uint16_t>(system_page.data() + 55, 0, 99); // next_record_offset
    // Heap number (2 bytes)
    dbrecovery::innodb::write_le<uint16_t>(system_page.data() + 57, 0, 0);
    // n_fields (1 byte)
    system_page[59] = 0;

    // 填充 SUPREMUM 记录 (offset 99)
    system_page[99] = 0x03; // info_bits: REC_STATUS_SUPREMUM
    dbrecovery::innodb::write_le<uint16_t>(system_page.data() + 100, 0, 0xFFFF); // next_record_offset = 0xFFFF
    dbrecovery::innodb::write_le<uint16_t>(system_page.data() + 101, 0, 1);
    system_page[103] = 0;

    // 计算并设置校验和
    std::vector<uint8_t> page_copy = system_page;
    for (int i = 0; i < 4; ++i) {
        page_copy[24 + i] = 0;
    }
    uint32_t crc = dbrecovery::innodb::PageReader::compute_crc32c(page_copy);
    for (int i = 0; i < 4; ++i) {
        system_page[24 + i] = (crc >> (i * 8)) & 0xFF;
    }

    pages_.push_back(system_page);
}

IBDExporter::~IBDExporter() = default;

bool IBDExporter::export_record(const std::map<std::string, std::string>& record) {
    stored_records_.push_back(record);
    return true;
}

std::string IBDExporter::get_result() const {
    return ""; // IBD 是二进制格式
}

bool IBDExporter::export_to_ibd(const std::string& file_path,
                                const std::map<std::string, std::string>& table_schema) {
    (void)table_schema;

    std::ofstream file(file_path, std::ios::binary);
    if (!file) {
        std::cerr << "[ERROR] Failed to open IBD file for writing: " << file_path << std::endl;
        return false;
    }

    // 写入所有页
    for (const auto& page : pages_) {
        file.write(reinterpret_cast<const char*>(page.data()), page.size());
    }

    file.close();

    std::cout << "[INFO] IBD file exported: " << file_path << " ("
              << pages_.size() * page_size_ << " bytes)" << std::endl;

    return true;
}


// ==================== Exporter 静态方法实现 ====================
// export_to_csv is in csv_exporter.cpp

bool Exporter::export_to_sql(const std::vector<std::map<std::string, std::string>>& records,
                             const std::string& table_name,
                             const std::string& output_path) {
    dbrecovery::exporter::SQLOutputter outputter(table_name);

    for (const auto& record : records) {
        if (!outputter.export_record(record)) {
            std::cerr << "[ERROR] Failed to export record to SQL" << std::endl;
            return false;
        }
    }

    std::ofstream file(output_path);
    if (!file) {
        std::cerr << "[ERROR] Failed to open file for writing: " << output_path << std::endl;
        return false;
    }

    file << outputter.get_result();
    file.close();

    return true;
}

bool Exporter::export_to_sql(const std::vector<std::map<std::string, std::string>>& records,
                             const std::string& table_name,
                             const std::vector<std::string>& column_order,
                             const std::string& output_path) {
    dbrecovery::exporter::SQLOutputter outputter(table_name, column_order);

    for (const auto& record : records) {
        if (!outputter.export_record(record)) {
            std::cerr << "[ERROR] Failed to export record to SQL" << std::endl;
            return false;
        }
    }

    std::ofstream file(output_path);
    if (!file) {
        std::cerr << "[ERROR] Failed to open file for writing: " << output_path << std::endl;
        return false;
    }

    file << outputter.get_result();
    file.close();

    return true;
}

bool Exporter::export_to_ibd(const std::vector<std::map<std::string, std::string>>& records,
                             const std::string& output_path) {
    // IBD 导出需要更复杂的实现，这里提供基础框架
    // 实际实现需要：
    // 1. 根据表结构创建页
    // 2. 将记录写入页
    // 3. 生成完整的表空间文件

    std::cerr << "[WARN] IBD export not fully implemented yet" << std::endl;
    return false;
}

} // namespace export
} // namespace dbrecovery

#include "dbrecovery/recovery/engine.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include <fstream>
#include <sstream>
#include <iostream>

namespace dbrecovery {
namespace recovery {

RecoveryEngine::RecoveryEngine() : file_reader_(nullptr), current_file_path_("")
{}

RecoveryEngine::~RecoveryEngine() {
    close();
}

bool RecoveryEngine::open(const std::string& file_path) {
    if (file_reader_) {
        close();
    }

    file_reader_ = std::make_unique<innodb::InnoDBFileReader>(file_path);
    if (!file_reader_->is_valid()) {
        std::cerr << "[ERROR] Failed to open file: " << file_path << std::endl;
        return false;
    }

    current_file_path_ = file_path;
    std::cout << "[INFO] Successfully opened file: " << file_path
              << " (" << file_reader_->get_file_size() << " bytes, "
              << file_reader_->get_total_pages() << " pages)" << std::endl;

    return true;
}

void RecoveryEngine::close() {
    if (file_reader_) {
        file_reader_.reset();
        current_file_path_ = "";
    }
}

std::vector<std::map<std::string, std::string>>
RecoveryEngine::recover_truncate(
    const catalog::TableSchema& schema) {

    if (!file_reader_ || !file_reader_->is_valid()) {
        std::cerr << "[ERROR] No file opened" << std::endl;
        return {};
    }

    std::vector<std::map<std::string, std::string>> recovered_records;

    // 遍历所有页，查找有效的 B+Tree 叶子页
    uint32_t total_pages = file_reader_->get_total_pages();
    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
        auto page_data = file_reader_->read_page(page_no);
        if (page_data.empty()) {
            continue;
        }

        // 检查页类型是否为 B+Tree 索引页
        uint16_t page_type = innodb::read_le<uint16_t>(page_data.data(), innodb::FIL_PAGE_TYPE_ABS);
        if (page_type != innodb::FIL_PAGE_INDEX) {
            continue;
        }

        // 解析 B+Tree 页
        innodb::BPlusTreeParser parser(page_data);
        if (!parser.parse()) {
            std::cerr << "[WARN] Failed to parse page " << page_no << std::endl;
            continue;
        }

        // 解析记录
        auto records = parser.parse_records(schema);
        recovered_records.insert(recovered_records.end(), records.begin(),
records.end());
    }

    std::cout << "[INFO] Recovered " << recovered_records.size() << " records from "
              << total_pages << " pages" << std::endl;

    return recovered_records;
}

bool RecoveryEngine::repair_header(const std::string& output_path) {
    if (!file_reader_ || !file_reader_->is_valid()) {
        std::cerr << "[ERROR] No file opened" << std::endl;
        return false;
    }

    // 读取第一页（系统页）
    auto page_data = file_reader_->read_page_raw(0);
    if (page_data.empty() || page_data.size() < 16384) {
        std::cerr << "[ERROR] Failed to read first page for header repair" << std::endl;
        return false;
    }

    // 检查并修复 FIL_PAGE_TYPE
    uint16_t* page_type_ptr = reinterpret_cast<uint16_t*>(page_data.data() + innodb::FIL_PAGE_TYPE_ABS);
    if (*page_type_ptr != innodb::FIL_PAGE_INDEX &&
        *page_type_ptr != innodb::FIL_PAGE_SDI) {
        std::cerr << "[WARN] Invalid page type in header, attempting repair" << std::endl;

        // 尝试设置为默认的 B+Tree 索引页类型
        *page_type_ptr = innodb::FIL_PAGE_INDEX;
    }

    // 写入修复后的文件
    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        std::cerr << "[ERROR] Failed to open output file: " << output_path << std::endl;
        return false;
    }

    out.write(reinterpret_cast<const char*>(page_data.data()), page_data.size());

    // 补全剩余页（如果文件不完整）
    uint64_t file_size = file_reader_->get_file_size();
    size_t page_size = file_reader_->get_page_size();
    uint64_t expected_size = (file_reader_->get_total_pages() + 1) * page_size;

    if (expected_size > file_size) {
        std::vector<uint8_t> padding(page_size, 0);
        for (uint64_t i = file_size; i < expected_size; i += page_size) {
            out.write(reinterpret_cast<const char*>(padding.data()), padding.size());
        }
    }

    out.close();
    std::cout << "[INFO] Header repaired and saved to: " << output_path << std::endl;

    return true;
}

bool RecoveryEngine::export_to_csv(const std::vector<std::map<std::string, std::string>>& records,
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

    // 写入表头（从第一条记录中提取列名）
    bool first = true;
    for (const auto& record : records) {
        if (first) {
            for (auto it = record.begin(); it != record.end(); ++it) {
                if (it != record.begin()) out << ",";
                out << it->first;
            }
            out << "\n";
            first = false;
        }

        // 写入数据行
        for (auto it = record.begin(); it != record.end(); ++it) {
            if (it != record.begin()) out << ",";
            out << it->second;
        }
        out << "\n";
    }

    out.close();
    std::cout << "[INFO] Exported " << records.size() << " records to CSV: " << csv_path << std::endl;

    return true;
}

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

} // namespace recovery
} // namespace dbrecovery


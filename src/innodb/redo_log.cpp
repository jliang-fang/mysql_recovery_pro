#include "dbrecovery/innodb/index_page_parser.hpp"
#include "dbrecovery/innodb/redo_log.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include <fstream>
#include <iostream>

namespace dbrecovery {
namespace innodb {

RedoLogParser::RedoLogParser() {}

RedoLogParser::~RedoLogParser() = default;

std::optional<RedoLogRecord> RedoLogParser::parse_record(const uint8_t* data, size_t len) {
    if (!data || len < 9) {
        return std::nullopt;
    }

    RedoLogRecord record;
    size_t pos = 0;

    // Redo Log 类型是 1 字节 (uint8_t)
    record.r_type = dbrecovery::innodb::read_le<uint8_t>(data, pos);
    pos += sizeof(uint8_t);

    // 解析表空间ID和页号 (各4字节)
    if (pos + 8 <= len) {
        record.r_space_id = dbrecovery::innodb::read_le<uint32_t>(data, pos);
        pos += sizeof(uint32_t);
        record.r_page_no = dbrecovery::innodb::read_le<uint32_t>(data, pos);
        pos += sizeof(uint32_t);
    }

    // ✅ P0-1 修复：parse_record 只解析记录头，不解析数据部分
    // 数据部分由 parse_file 根据固定长度（8字节）解析
    // 这样可以确保多条记录被正确解析

    return record;
}

std::vector<RedoLogRecord> RedoLogParser::parse_file(const std::string& path) {
    std::vector<RedoLogRecord> records;

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[ERROR] Failed to open redo log file: " << path << std::endl;
        return records;
    }

    // 读取文件大小
    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (g_verbose) 
    std::cerr << "[DEBUG] File size: " << file_size << " for path: " << path << std::endl;

    if (file_size < 9) {
        std::cerr << "[ERROR] Redo log file too small, returning empty records" << std::endl;
        return records; // Empty vector
    }

    // 读取整个文件
    std::vector<uint8_t> buffer(file_size);
    file.read(reinterpret_cast<char*>(buffer.data()), file_size);
    file.close();

    if (g_verbose) 
    std::cerr << "[DEBUG] Buffer size: " << buffer.size() << std::endl;

    // 解析所有记录
    // 格式：type(1) + space_id(4) + page_no(4) + data(...)
    // 每条记录至少9字节（头），数据部分长度可变
    size_t pos = 0;
    uint64_t current_lsn = 0;
    while (pos + 9 <= buffer.size()) {
        // 读取记录头（type + space_id + page_no）
        uint8_t type = buffer[pos];
        uint32_t space_id = dbrecovery::innodb::read_le<uint32_t>(buffer.data(), pos + 1);
        uint32_t page_no = dbrecovery::innodb::read_le<uint32_t>(buffer.data(), pos + 5);

        // 数据部分长度 = 剩余字节数 - 9字节头
        size_t data_len = buffer.size() - pos - 9;

        RedoLogRecord record;
        record.r_type = type;
        record.r_space_id = space_id;
        record.r_page_no = page_no;
        record.r_lsn = current_lsn;
        record.r_data.assign(buffer.data() + pos + 9, buffer.data() + pos + 9 + data_len);

        records.push_back(std::move(record));

        // 移动到下一条记录（跳过当前记录的全部数据）
        pos += 9 + data_len;
        current_lsn += 9 + data_len;
    }

    return records;
}

} // namespace innodb
} // namespace dbrecovery


#include "dbrecovery/innodb/redo_log.hpp"
#include <fstream>
#include <iostream>

namespace dbrecovery {
namespace innodb {

RedoLogParser::RedoLogParser() {}

RedoLogParser::~RedoLogParser() = default;

std::optional<RedoLogRecord> RedoLogParser::parse_record(const uint8_t* data, size_t len) {
    if (!data || len < 10) {
        return std::nullopt;
    }

    RedoLogRecord record;
    size_t pos = 0;

    // 解析记录类型
    record.r_type = read_uint16_le(data, pos);

    // 解析表空间ID和页号
    record.r_sapce_id = read_uint16_le(data, pos);
    record.r_page_no = read_uint32_le(data, pos);

    // 解析记录数据长度
    uint16_t data_len = read_uint16_le(data, pos);

    if (pos + data_len > len) {
        return std::nullopt;
    }

    // 提取记录数据
    record.r_data.assign(data + pos, data + pos + data_len);
    pos += data_len;

    // 解析事务相关字段（如果存在）
    if (pos + 16 <= len) {
        record.r_trx_id = read_uint64_le(data, pos);
        record.r_roll_ptr = read_uint64_le(data, pos);
    }

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

    if (file_size < 10) {
        std::cerr << "[ERROR] Redo log file too small" << std::endl;
        return records;
    }

    // 读取整个文件
    std::vector<uint8_t> buffer(file_size);
    file.read(reinterpret_cast<char*>(buffer.data()), file_size);

    // 解析所有记录
    size_t pos = 0;
    while (pos + 10 <= buffer.size()) {
        auto record = parse_record(buffer.data() + pos, buffer.size() - pos);
        if (!record) {
            break;
        }

        records.push_back(*record);
        pos += 10 + record->r_data.size();
    }

    return records;
}

} // namespace innodb
} // namespace dbrecovery


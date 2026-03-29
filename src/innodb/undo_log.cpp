#include "dbrecovery/innodb/undo_log.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include <fstream>
#include <iostream>
#include <vector>

// ✅ 删除：这些函数已在 mach_read.hpp 中实现，此处无需重复定义

namespace dbrecovery {
namespace innodb {

UndoLogParser::UndoLogParser() {}

UndoLogParser::~UndoLogParser() = default;

std::optional<UndoLogRecord> UndoLogParser::parse_record(const uint8_t* data, size_t len) {
    if (!data || len < 20) {
        return std::nullopt;
    }

    UndoLogRecord record;
    size_t pos = 0;

    // 解析事务ID和回滚指针
    record.u_trx_id = dbrecovery::innodb::read_le<uint64_t>(data, pos);
    pos += 8;
    record.u_roll_ptr = dbrecovery::innodb::read_le<uint64_t>(data, pos);
    pos += 8;

    // 解析类型
    record.u_type = dbrecovery::innodb::read_le<uint16_t>(data, pos);
    pos += 2;

    // 剩余数据作为记录数据
    if (pos < len) {
        record.u_data.assign(data + pos, data + len);
    }

    return record;
}

std::optional<UndoLogRecord> UndoLogParser::parse_record_with_data_len(
    const uint8_t* data, size_t len, uint16_t data_len) {
    if (!data || len < 20 + data_len) {
        return std::nullopt;
    }

    UndoLogRecord record;
    size_t pos = 0;

    // 解析事务ID和回滚指针
    record.u_trx_id = dbrecovery::innodb::read_le<uint64_t>(data, pos);
    pos += 8;
    record.u_roll_ptr = dbrecovery::innodb::read_le<uint64_t>(data, pos);
    pos += 8;

    // 解析类型
    record.u_type = dbrecovery::innodb::read_le<uint16_t>(data, pos);
    pos += 2;

    // 解析数据长度
    record.u_data.assign(data + pos, data + pos + data_len);

    return record;
}

std::vector<UndoLogRecord> UndoLogParser::parse_file(const std::string& path) {
    std::vector<UndoLogRecord> records;

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[ERROR] Failed to open undo log file: " << path << std::endl;
        return records;
    }

    // 读取文件大小
    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size < 20) {
        std::cerr << "[ERROR] Undo log file too small" << std::endl;
        return records;
    }

    // 读取整个文件
    std::vector<uint8_t> buffer(file_size);
    file.read(reinterpret_cast<char*>(buffer.data()), file_size);
    file.close();

    // 解析所有记录
    // ✅ P2-1 修复：每条记录格式：trx_id(8) + roll_ptr(8) + type(2) + data_len(2) + data
    // 总大小 = 20 + data_len
    size_t pos = 0;
    while (pos + 22 <= buffer.size()) {
        // 解析事务ID和回滚指针
        uint64_t trx_id = dbrecovery::innodb::read_le<uint64_t>(buffer.data(), pos);
        uint64_t roll_ptr = dbrecovery::innodb::read_le<uint64_t>(buffer.data(), pos + 8);
        uint16_t type = dbrecovery::innodb::read_le<uint16_t>(buffer.data(), pos + 16);
        uint16_t data_len = dbrecovery::innodb::read_le<uint16_t>(buffer.data(), pos + 18);

        UndoLogRecord record;
        record.u_trx_id = trx_id;
        record.u_roll_ptr = roll_ptr;
        record.u_type = type;
        record.u_data.assign(buffer.data() + pos + 20, buffer.data() + pos + 20 + data_len);

        records.push_back(record);

        // ✅ P2-1 修复：移动到下一条记录（20字节头 + data_len字节数据）
        pos += 20 + data_len;
    }

    return records;
}

} // namespace innodb
} // namespace dbrecovery


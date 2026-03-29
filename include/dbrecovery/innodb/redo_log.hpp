#pragma once

#include <cstdint>
#include <vector>
#include <optional>
#include <string>
#include "dbrecovery/innodb/mach_read.hpp"

namespace dbrecovery {
namespace innodb {

// ✅ Redo Log 类型常量（参考 MySQL 8.0 mlog_id_t 枚举）
constexpr uint8_t MLOG_1BYTE = 1;
constexpr uint8_t MLOG_2BYTES = 2;
constexpr uint8_t MLOG_4BYTES = 3;
constexpr uint8_t MLOG_8BYTES = 4;
constexpr uint8_t MLOG_REC_INSERT = 10;
constexpr uint8_t MLOG_REC_UPDATE = 11;
constexpr uint8_t MLOG_REC_DELETE = 12;
constexpr uint8_t MLOG_PAGE_CREATE = 15;
constexpr uint8_t MLOG_PAGE_REINIT = 16;
constexpr uint8_t MLOG_INDEX_ALLOC = 17;
constexpr uint8_t MLOG_COMP_REC_INSERT = 30;
constexpr uint8_t MLOG_COMP_REC_UPDATE = 31;
constexpr uint8_t MLOG_COMP_REC_DELETE = 32;
constexpr uint8_t MLOG_COMP_PAGE_CREATE = 33;
constexpr uint8_t MLOG_COMP_PAGE_REINIT = 34;
constexpr uint8_t MLOG_BLOB_WRITE = 40;
constexpr uint8_t MLOG_MULTI_REC_END = 50;
constexpr uint8_t MLOG_CHECKPOINT = 51;
constexpr uint8_t MLOG_INDEX_LOAD = 52;

// ✅ Redo Log 记录结构（参考 MySQL 8.0 mtr0mtr.h）
struct RedoLogRecord {
    uint16_t r_type;          // 日志类型 (MLOG_*)
    uint32_t r_space_id;      // 表空间ID
    uint32_t r_page_no;       // 页号
    std::vector<uint8_t> r_data; // 原始日志数据

    // 可选事务信息（仅在记录类型支持时存在）
    uint64_t r_trx_id = 0;
    uint64_t r_roll_ptr = 0;

    std::string to_string() const {
        return "RedoLogRecord{type=" + std::to_string(r_type) +
               ", space_id=" + std::to_string(r_space_id) +
               ", page_no=" + std::to_string(r_page_no) +
               ", data_len=" + std::to_string(r_data.size()) + "}";
    }
};

// ✅ 新增：前置声明 parse_redo_record（供模糊测试使用）
inline std::optional<RedoLogRecord> parse_redo_record(
    const uint8_t* data, size_t len) {
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

    return record;
}

class RedoLogParser {
public:
    RedoLogParser();
    ~RedoLogParser();
    
    /**
     * @brief 解析单条 Redo Log 记录
     *
     * @param data 日志数据指针
     * @param len 数据长度
     * @return std::optional<RedoLogRecord> 成功返回记录，否则返回空
     */
    std::optional<RedoLogRecord> parse_record(const uint8_t* data, size_t len);
    
    /**
     * @brief 从文件解析所有 Redo Log 记录
     *
     * @param path 文件路径
     * @return std::vector<RedoLogRecord> 解析出的记录列表
     */
    std::vector<RedoLogRecord> parse_file(const std::string& path);

private:
    // 解析状态（预留扩展）
    bool is_valid_ = true;
};

} // namespace innodb
} // namespace dbrecovery

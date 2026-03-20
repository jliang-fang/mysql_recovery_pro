#pragma once

#include <cstdint>
#include <vector>
#include <optional>
#include <string>

namespace dbrecovery {
namespace innodb {

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

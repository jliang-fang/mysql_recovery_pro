#pragma once
#include <cstdint>
#include <vector>
#include <optional>

namespace dbrecovery {
namespace innodb {

struct UndoLogRecord {
    uint64_t u_trx_id;        // 事务ID
    uint64_t u_roll_ptr;      // 回滚指针
    uint16_t u_type;          // 类型
    std::vector<uint8_t> u_data; // 记录数据
    
    std::string to_string() const {
        return "UndoLogRecord{trx_id=" + std::to_string(u_trx_id) +
               ", roll_ptr=" + std::to_string(u_roll_ptr) +
               ", type=" + std::to_string(u_type) +
               ", data_len=" + std::to_string(u_data.size()) + "}";
    }
};

class UndoLogParser {
public:
    UndoLogParser();
    ~UndoLogParser();
    
    std::optional<UndoLogRecord> parse_record(const uint8_t* data, size_t len);
    std::vector<UndoLogRecord> parse_file(const std::string& path);

private:
    // 解析状态
    bool is_valid_ = true;
};

// 辅助函数：小端序读取（在 undo_log.cpp 中实现）
uint16_t read_uint16_le(const uint8_t* data, size_t& pos);
uint32_t read_uint32_le(const uint8_t* data, size_t& pos);
uint64_t read_uint64_le(const uint8_t* data, size_t& pos);

} // namespace innodb
} // namespace dbrecovery

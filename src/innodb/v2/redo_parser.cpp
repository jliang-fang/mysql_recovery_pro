#include "dbrecovery/innodb/v2/redo_parser.hpp"
#include <cstring>
#include <map>

namespace dbrecovery {
namespace innodb {
namespace v2 {

// ✅ 静态 handler 注册表
std::map<LogType, RedoParser::MLogHandler> RedoParser::handlers_;

RedoParser::RedoParser(const uint8_t* buffer, size_t size)
    : buffer_(buffer), size_(size), checkpoint_lsn_(0) {}

bool RedoParser::parse() noexcept {
    // 基础校验：至少包含日志组头（512 字节）
    if (!buffer_ || size_ < 512) return false;

    // ✅ Step 1: 解析日志组头中的 checkpoint
    parse_header();

    const uint8_t* ptr = buffer_ + 512; // Skip header
    const uint8_t* end = buffer_ + size_;

    while (ptr < end - 7) {  // 至少需要 LSN(8) + type(1) + len(2)
        RedoRecord rec;
        
        // 解析 LSN（8 字节）
        std::memcpy(&rec.lsn, ptr, sizeof(uint64_t));
        ptr += sizeof(uint64_t);
        
        // 解析日志类型（1 字节）
        uint8_t type_byte = *ptr++;
        rec.type = static_cast<LogType>(type_byte);
        
        // 解析日志长度（2 字节）
        uint16_t len = 0;
        std::memcpy(&len, ptr, sizeof(uint16_t));
        if (len == 0 || ptr + len > end) break;  // 防御性校验
        ptr += sizeof(uint16_t);
        
        // 提取日志内容
        rec.data.assign(ptr, ptr + len);
        ptr += len;
        
        records_.push_back(rec);

        // ✅ Special handling for MLOG_CHECKPOINT
        if (rec.type == LogType::MLOG_CHECKPOINT) {
            parse_checkpoint_record(rec.data.data(), rec.data.size());
        }
    }
    
    return !records_.empty();
}

void RedoParser::parse_header() noexcept {
    // Checkpoint1 at offset 32–47: [8B LSN] + [4B checkpoint_no] + [4B reserved]
    std::memcpy(&checkpoint_lsn_, buffer_ + 32, sizeof(uint64_t));
    
    // Also check checkpoint2 (offset 16–31) for redundancy
    uint64_t checkpoint2_lsn = 0;
    std::memcpy(&checkpoint2_lsn, buffer_ + 16, sizeof(uint64_t));
    
    // Use the max LSN as recovery start point
    if (checkpoint2_lsn > checkpoint_lsn_) {
        checkpoint_lsn_ = checkpoint2_lsn;
    }
}

void RedoParser::parse_checkpoint_record(const uint8_t* data, size_t len) noexcept {
    if (len < 16) return; // Minimum: [8B LSN] + [4B checkpoint_no] + [4B reserved]
    
    uint64_t new_lsn;
    std::memcpy(&new_lsn, data, sizeof(uint64_t));
    
    // Update global checkpoint LSN
    if (new_lsn > checkpoint_lsn_) {
        checkpoint_lsn_ = new_lsn;
    }
}

void RedoParser::register_handler(LogType type, MLogHandler handler) {
    handlers_[type] = handler;
}

// ✅ 核心日志类型解析器实现（扩展）
void RedoParser::parse_mlog_rec_insert(const uint8_t* data, size_t len) {
    // MySQL 8.0 mlog_rec_insert 格式：
    // [1 byte] rec_type (MLOG_REC_INSERT=24)
    if (len < 1) return;
    
    uint8_t rec_type = *data;
    // TODO: Parse record-specific fields based on rec_type
}

// ✅ Getters for recovery engine


} // namespace v2
} // namespace innodb
} // namespace dbrecovery

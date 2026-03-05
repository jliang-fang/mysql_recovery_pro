#pragma once

#include <cstdint>
#include <vector>

namespace dbrecovery::innodb::v2 {

// Redo 记录结构（简化版，后续可扩展）
struct RedoRecord {
    uint64_t lsn;              // 记录的 LSN
    uint8_t type;              // MLOG_* 类型
    std::vector<uint8_t> data; // 原始数据负载
    
    bool operator<(const RedoRecord& other) const { return lsn < other.lsn; }
};

// Checkpoint 结构（用于双 checkpoint 校验）
struct Checkpoint {
    uint64_t lsn;
    uint32_t checkpoint_no;
    uint32_t reserved;
};

} // namespace dbrecovery::innodb::v2

// LogType enum for redo log record types
enum class LogType : uint8_t {
    MLOG_REC_INSERT = 1,
    MLOG_REC_DELETE_MARK = 2,
    MLOG_CHECKPOINT = 3,
};

using MLogHandler = void (*)(const uint8_t* data, size_t len);

// RedoParser class declaration (to be implemented in redo_parser.cpp)
class RedoParser {
public:
    explicit RedoParser(const uint8_t* buffer, size_t size);
    ~RedoParser();

    bool parse();
    bool has_records() const;
    
    void register_handler(LogType type, MLogHandler handler);

private:
    void parse_header() noexcept;
    void parse_checkpoint_record(const uint8_t* data, size_t len) noexcept;
    void parse_mlog_rec_insert(const uint8_t* data, size_t len);
    
    const uint8_t* buffer_;
    size_t size_;
    uint64_t checkpoint_lsn_ = 0;
    std::vector<RedoRecord> records_;
};

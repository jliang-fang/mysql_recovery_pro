#pragma once

#include <cstdint>
#include <vector>
#include "dbrecovery/innodb/constants.hpp"
#include "dbrecovery/innodb/v2/types.hpp"

namespace dbrecovery::innodb::v2 {

class RedoParser {
public:
    explicit RedoParser(const uint8_t* buffer, size_t size);
    
    // 解析整个日志缓冲区
    bool parse() noexcept;
    
    // 获取 checkpoint LSN（崩溃恢复起点）
    uint64_t get_checkpoint_lsn() const noexcept { return checkpoint_lsn_; }
    
    // 获取所有解析出的 Redo 记录（用于 LSN 连续性校验）
    const std::vector<RedoRecord>& get_records() const noexcept { return records_; }

private:
    void parse_header() noexcept;
    void parse_checkpoint_record(const uint8_t* data, size_t len) noexcept;
    
    // Handler 注册表
    using MLogHandler = void (RedoParser::*)(const uint8_t*, size_t);
    static std::map<LogType, MLogHandler> handlers_;
    
    const uint8_t* buffer_;
    size_t size_;
    uint64_t checkpoint_lsn_;
    std::vector<RedoRecord> records_;  // ✅ 新增：存储所有解析记录
};

} // namespace dbrecovery::innodb::v2

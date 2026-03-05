// /app/mysql8_recovery/src/innodb/v2/undo_parser.cpp (REWRITE)
#include "dbrecovery/innodb/v2/undo_parser.hpp"
#include <cstring>

namespace DBRecovery::InnoDB::v2 {

UndoParser::UndoParser(const uint8_t* buffer, size_t size)
    : buffer_(buffer), size_(size), state_(0), last_log_offset_(0) {}

bool UndoParser::parse() noexcept {
    // ✅ Step 1: 校验最小长度（Segment Header + 至少一个 Log Header）
    if (!buffer_ || size_ < 38 + 20) return false;

    const uint8_t* ptr = buffer_;
    
    // ✅ Step 2: 解析 Undo Segment Header (offset 0–37)
    parse_segment_header(ptr);
    
    // ✅ Step 3: 遍历所有 Undo Log Headers
    size_t offset = 38;  // Start after segment header
    
    while (offset + 20 <= size_) {
        const uint8_t* log_ptr = ptr + offset;
        
        UndoRecord rec;
        
        // Extract TRX_ID (6 bytes, low 48 bits)
        std::memcpy(&rec.trx_id, log_ptr, sizeof(uint64_t));
        rec.trx_id &= 0xFFFFFFFFFFFF;
        
        // Extract type (1 byte at offset 6)
        rec.type = static_cast<UndoType>(log_ptr[6]);
        
        // Extract TRX_UNDO_PREV_IN_FRM (2 bytes at offset 7)
        uint16_t prev_in_frm = 0;
        std::memcpy(&prev_in_frm, log_ptr + 7, sizeof(uint16_t));
        rec.prev_in_frm_offset = prev_in_frm;
        
        // Extract flags (1 byte at offset 9)
        rec.flags = log_ptr[9];
        
        records_.push_back(rec);
        
        // Move to next log header if not last
        if (rec.prev_in_frm_offset == 0xFFFF || rec.prev_in_frm_offset >= size_) {
            break; // Last undo log in page
        }
        offset = rec.prev_in_frm_offset;
    }
    
    return !records_.empty();
}

void UndoParser::parse_segment_header(const uint8_t* ptr) noexcept {
    // TRX_UNDO_STATE (1 byte at offset 0)
    state_ = ptr[0];
    
    // TRX_UNDO_LAST_LOG (2 bytes at offset 2)
    std::memcpy(&last_log_offset_, ptr + 2, sizeof(uint16_t));
}

// Getters for recovery engine
uint8_t UndoParser::get_state() const noexcept {
    return state_;
}

uint16_t UndoParser::get_last_log_offset() const noexcept {
    return last_log_offset_;
}

} // namespace DBRecovery::InnoDB::v2

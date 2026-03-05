// /app/mysql8_recovery/include/dbrecovery/innodb/v2/undo_parser.hpp (REWRITE)
#pragma once
#include <cstdint>
#include <cstddef>   // For size_t
#include <vector>

namespace DBRecovery::InnoDB::v2 {

enum class UndoType : uint8_t {
    TRX_UNDO_INSERT = 1,
    TRX_UNDO_UPDATE = 2,
};

struct UndoRecord {
    uint64_t trx_id;           // Transaction ID (low 48 bits)
    UndoType type;
    std::vector<uint8_t> data;
    uint16_t prev_in_frm_offset; // TRX_UNDO_PREV_IN_FRM
    uint8_t flags;               // Flags byte
};

class UndoParser {
public:
    UndoParser(const uint8_t* buffer, size_t size);
    
    bool parse() noexcept;
    
    [[nodiscard]] const std::vector<UndoRecord>& records() const { return records_; }
    
    // Segment header accessors
    [[nodiscard]] uint8_t get_state() const noexcept;
    [[nodiscard]] uint16_t get_last_log_offset() const noexcept;

private:
    void parse_segment_header(const uint8_t* ptr) noexcept;
    
    const uint8_t* buffer_;
    size_t size_;
    std::vector<UndoRecord> records_{};
    uint8_t state_;
    uint16_t last_log_offset_;
};

} // namespace DBRecovery::InnoDB::v2

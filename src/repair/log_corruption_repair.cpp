#include "dbrecovery/repair/log_corruption_repair.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include <cstring>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <iostream>

namespace dbrecovery {
namespace repair {

// ==================== RedoLogRepairer ====================

RedoLogRepairer::RedoLogRepairer()
    : block_size_(512) {
}

std::vector<LogCorruption> RedoLogRepairer::detect_corruptions(const uint8_t* data, size_t size) {
    std::vector<LogCorruption> corruptions;

    if (!data || size < block_size_) {
        return corruptions;
    }

    // 1. 检查文件头（第一个块）
    auto header_opt = parse_file_header(data, size);
    if (!header_opt || !header_opt->is_valid) {
        LogCorruption c;
        c.type = LogCorruptionType::REDO_HEADER_CORRUPTED;
        c.offset = 0;
        c.length = block_size_;
        c.description = "Redo log file header is corrupted or invalid";
        c.confidence = 0.95f;
        c.repairable = true;
        corruptions.push_back(c);
    }

    // 2. 检查 Checkpoint 信息
    auto checkpoint_opt = parse_checkpoint(data, size);
    if (!checkpoint_opt || !checkpoint_opt->is_valid) {
        LogCorruption c;
        c.type = LogCorruptionType::REDO_CHECKPOINT_CORRUPTED;
        c.offset = block_size_;  // Checkpoint 通常在第二个块
        c.length = block_size_;
        c.description = "Checkpoint information is corrupted";
        c.confidence = 0.90f;
        c.repairable = true;
        corruptions.push_back(c);
    }

    // 3. 检查每个块的头部
    size_t block_count = size / block_size_;
    for (size_t i = 1; i < block_count; ++i) {
        auto block_opt = parse_block_header(data, size, i);
        if (!block_opt || !block_opt->is_valid) {
            LogCorruption c;
            c.type = LogCorruptionType::REDO_BLOCK_HEADER_CORRUPTED;
            c.offset = i * block_size_;
            c.length = block_size_;
            std::ostringstream oss;
            oss << "Redo block " << i << " header is corrupted";
            c.description = oss.str();
            c.confidence = 0.85f;
            c.repairable = true;
            corruptions.push_back(c);
        }
    }

    return corruptions;
}

size_t RedoLogRepairer::repair(uint8_t* data, size_t size,
                                const std::vector<LogCorruption>& corruptions) {
    size_t repaired = 0;

    for (const auto& c : corruptions) {
        bool success = false;
        size_t block_idx = c.offset / block_size_;

        switch (c.type) {
            case LogCorruptionType::REDO_HEADER_CORRUPTED:
                success = repair_file_header(data, size);
                break;

            case LogCorruptionType::REDO_BLOCK_HEADER_CORRUPTED:
                success = repair_block_header(data, size, block_idx);
                break;

            case LogCorruptionType::REDO_CHECKPOINT_CORRUPTED:
                // 需要提供有效的 checkpoint 信息
                break;

            default:
                break;
        }

        if (success) {
            repaired++;
            repaired_count_++;
        }
    }

    return repaired;
}

std::optional<RedoFileHeader> RedoLogRepairer::parse_file_header(const uint8_t* data, size_t size) {
    if (!data || size < block_size_) {
        return std::nullopt;
    }

    RedoFileHeader header = {};

    // Redo log 文件头结构 (MySQL 8.0):
    // Offset 0-3: LOG_GROUP_ID
    // Offset 4-11: LSN
    // Offset 12-19: LSN_CAPACITY
    // ... 其他字段

    // 检查魔数 (可选)
    // MySQL redo log 没有明确的魔数，但可以检查 LSN 是否合理

    header.log_group_id = dbrecovery::innodb::read_le<uint32_t>(data, 0);
    header.lsn = dbrecovery::innodb::read_le<uint64_t>(data, 4);
    header.block_size = block_size_;
    header.file_size = size;

    // 简单验证
    header.is_valid = (header.lsn > 0 || header.log_group_id == 0);

    return header;
}

std::optional<RedoBlockHeader> RedoLogRepairer::parse_block_header(const uint8_t* data, size_t size, size_t block_idx) {
    if (!data || size < (block_idx + 1) * block_size_) {
        return std::nullopt;
    }

    const uint8_t* block = data + block_idx * block_size_;

    RedoBlockHeader header = {};

    // Redo log 块头结构 (MySQL 8.0):
    // Offset 0-7: LSN (实际上只有部分位)
    // Offset 8-9: 数据长度
    // Offset 10-11: 第一条记录偏移
    // ...

    header.lsn = dbrecovery::innodb::read_le<uint64_t>(block, 0);
    header.data_len = dbrecovery::innodb::read_le<uint16_t>(block, 8);
    header.first_rec_offset = dbrecovery::innodb::read_le<uint16_t>(block, 10);
    header.checkpoint_no = dbrecovery::innodb::read_le<uint32_t>(block, 12);

    // 验证
    header.is_valid = (header.data_len > 0 && header.data_len <= block_size_ - 12);

    return header;
}

std::optional<CheckpointInfo> RedoLogRepairer::parse_checkpoint(const uint8_t* data, size_t size) {
    if (!data || size < block_size_ * 2) {
        return std::nullopt;
    }

    // Checkpoint 信息通常在文件开头的特定偏移
    // MySQL 8.0: checkpoint 在第一个块的特定位置

    CheckpointInfo checkpoint = {};

    // 简化处理：从固定偏移读取
    const uint8_t* cp_data = data + block_size_;

    checkpoint.lsn = dbrecovery::innodb::read_le<uint64_t>(cp_data, 0);
    checkpoint.lsn_offset = dbrecovery::innodb::read_le<uint64_t>(cp_data, 8);
    checkpoint.checkpoint_no = dbrecovery::innodb::read_le<uint32_t>(cp_data, 16);
    checkpoint.arch_log_no = dbrecovery::innodb::read_le<uint32_t>(cp_data, 20);

    // 验证
    checkpoint.is_valid = (checkpoint.lsn > 0);

    return checkpoint;
}

bool RedoLogRepairer::repair_file_header(uint8_t* data, size_t size) {
    if (!data || size < block_size_) {
        return false;
    }

    // 修复文件头
    // 设置默认值
    dbrecovery::innodb::write_le<uint32_t>(data, 0, 0);  // LOG_GROUP_ID
    dbrecovery::innodb::write_le<uint64_t>(data, 4, 16);  // LSN (默认从 16 开始)
    dbrecovery::innodb::write_le<uint64_t>(data, 12, size);  // LSN_CAPACITY

    return true;
}

bool RedoLogRepairer::repair_block_header(uint8_t* data, size_t size, size_t block_idx) {
    if (!data || size < (block_idx + 1) * block_size_) {
        return false;
    }

    uint8_t* block = data + block_idx * block_size_;

    // 修复块头
    // 设置默认值
    uint64_t default_lsn = block_idx * 512;  // 简单递增 LSN
    dbrecovery::innodb::write_le<uint64_t>(block, 0, default_lsn);
    dbrecovery::innodb::write_le<uint16_t>(block, 8, block_size_ - 12);  // 数据长度
    dbrecovery::innodb::write_le<uint16_t>(block, 10, 12);  // 第一条记录偏移

    return true;
}

bool RedoLogRepairer::repair_checkpoint(uint8_t* data, size_t size, const CheckpointInfo& checkpoint) {
    if (!data || size < block_size_ * 2) {
        return false;
    }

    uint8_t* cp_data = data + block_size_;

    // 写入 checkpoint 信息
    dbrecovery::innodb::write_le<uint64_t>(cp_data, 0, checkpoint.lsn);
    dbrecovery::innodb::write_le<uint64_t>(cp_data, 8, checkpoint.lsn_offset);
    dbrecovery::innodb::write_le<uint32_t>(cp_data, 16, checkpoint.checkpoint_no);
    dbrecovery::innodb::write_le<uint32_t>(cp_data, 20, checkpoint.arch_log_no);

    return true;
}

bool RedoLogRepairer::rebuild_lsn_chain(uint8_t* data, size_t size) {
    if (!data || size < block_size_) {
        return false;
    }

    size_t block_count = size / block_size_;
    uint64_t current_lsn = 16;  // 起始 LSN

    for (size_t i = 0; i < block_count; ++i) {
        uint8_t* block = data + i * block_size_;

        // 更新 LSN
        dbrecovery::innodb::write_le<uint64_t>(block, 0, current_lsn);
        current_lsn += block_size_;
    }

    return true;
}

std::vector<size_t> RedoLogRepairer::find_valid_blocks(const uint8_t* data, size_t size) {
    std::vector<size_t> valid_blocks;

    if (!data || size < block_size_) {
        return valid_blocks;
    }

    size_t block_count = size / block_size_;

    for (size_t i = 0; i < block_count; ++i) {
        auto block_opt = parse_block_header(data, size, i);
        if (block_opt && block_opt->is_valid) {
            valid_blocks.push_back(i);
        }
    }

    return valid_blocks;
}

uint32_t RedoLogRepairer::calculate_block_checksum(const uint8_t* block_data) {
    // MySQL 使用 CRC32C 校验
    // 简化实现
    uint32_t checksum = 0;
    for (size_t i = 0; i < block_size_; ++i) {
        checksum ^= block_data[i];
    }
    return checksum;
}

// ==================== UndoLogRepairer ====================

UndoLogRepairer::UndoLogRepairer()
    : page_size_(16384) {
}

std::vector<LogCorruption> UndoLogRepairer::detect_corruptions(const uint8_t* data, size_t size) {
    std::vector<LogCorruption> corruptions;

    if (!data || size < page_size_) {
        return corruptions;
    }

    size_t page_count = size / page_size_;

    for (size_t i = 0; i < page_count; ++i) {
        const uint8_t* page = data + i * page_size_;

        // 检查页类型
        uint16_t page_type = dbrecovery::innodb::read_be<uint16_t>(page, dbrecovery::innodb::FIL_PAGE_TYPE_ABS);

        if (page_type != dbrecovery::innodb::FIL_PAGE_UNDO_LOG) {
            continue;  // 不是 undo 页
        }

        // 检查 Undo Log 头
        // Undo log header 位于页内偏移 38 之后
        // TRX_UNDO_PAGE_TYPE 在偏移 0 (相对于 undo header)
        uint8_t undo_type = page[38];
        if (undo_type != 1 && undo_type != 2) {  // 1 = insert, 2 = update
            LogCorruption c;
            c.type = LogCorruptionType::UNDO_PAGE_CORRUPTED;
            c.offset = i * page_size_;
            c.length = page_size_;
            std::ostringstream oss;
            oss << "Undo page " << i << " has invalid type: " << (int)undo_type;
            c.description = oss.str();
            c.confidence = 0.90f;
            c.repairable = true;
            corruptions.push_back(c);
        }
    }

    return corruptions;
}

size_t UndoLogRepairer::repair(uint8_t* data, size_t size,
                                const std::vector<LogCorruption>& corruptions) {
    size_t repaired = 0;

    for (const auto& c : corruptions) {
        bool success = false;

        switch (c.type) {
            case LogCorruptionType::UNDO_PAGE_CORRUPTED:
                success = repair_undo_page(data + c.offset, page_size_);
                break;

            default:
                break;
        }

        if (success) {
            repaired++;
            repaired_count_++;
        }
    }

    return repaired;
}

std::optional<uint32_t> UndoLogRepairer::parse_segment_header(const uint8_t* data, size_t size) {
    if (!data || size < page_size_) {
        return std::nullopt;
    }

    // Undo segment header 在 page 0 的特定偏移
    // 简化处理
    return std::nullopt;
}

bool UndoLogRepairer::repair_segment_header(uint8_t* data, size_t size) {
    if (!data || size < page_size_) {
        return false;
    }

    // 修复 segment header
    return false;
}

bool UndoLogRepairer::repair_undo_page(uint8_t* page_data, size_t page_size) {
    if (!page_data || page_size < 100) {
        return false;
    }

    // 设置页类型为 UNDO_LOG
    dbrecovery::innodb::write_be<uint16_t>(page_data, dbrecovery::innodb::FIL_PAGE_TYPE_ABS,
                                            dbrecovery::innodb::FIL_PAGE_UNDO_LOG);

    // 设置 TRX_UNDO_PAGE_TYPE 为 update (2)
    page_data[38] = 2;

    return true;
}

// ==================== LogCorruptionHandler ====================

size_t LogCorruptionHandler::analyze_and_repair_redo(uint8_t* data, size_t size) {
    last_corruptions_ = redo_repairer_.detect_corruptions(data, size);
    if (last_corruptions_.empty()) {
        return 0;
    }
    return redo_repairer_.repair(data, size, last_corruptions_);
}

size_t LogCorruptionHandler::analyze_and_repair_undo(uint8_t* data, size_t size) {
    last_corruptions_ = undo_repairer_.detect_corruptions(data, size);
    if (last_corruptions_.empty()) {
        return 0;
    }
    return undo_repairer_.repair(data, size, last_corruptions_);
}

std::vector<LogCorruption> LogCorruptionHandler::analyze_redo(const uint8_t* data, size_t size) {
    return redo_repairer_.detect_corruptions(data, size);
}

std::vector<LogCorruption> LogCorruptionHandler::analyze_undo(const uint8_t* data, size_t size) {
    return undo_repairer_.detect_corruptions(data, size);
}

std::string LogCorruptionHandler::get_report() const {
    std::ostringstream oss;
    oss << "Log Corruption Report\n";
    oss << "=====================\n\n";

    if (last_corruptions_.empty()) {
        oss << "No log corruption detected.\n";
        return oss.str();
    }

    oss << "Detected " << last_corruptions_.size() << " corruption(s):\n\n";

    for (size_t i = 0; i < last_corruptions_.size(); ++i) {
        const auto& c = last_corruptions_[i];
        oss << (i + 1) << ". ";

        switch (c.type) {
            case LogCorruptionType::REDO_HEADER_CORRUPTED:
                oss << "Redo Header Corrupted";
                break;
            case LogCorruptionType::REDO_BLOCK_HEADER_CORRUPTED:
                oss << "Redo Block Header Corrupted";
                break;
            case LogCorruptionType::REDO_CHECKPOINT_CORRUPTED:
                oss << "Redo Checkpoint Corrupted";
                break;
            case LogCorruptionType::UNDO_HEADER_CORRUPTED:
                oss << "Undo Header Corrupted";
                break;
            case LogCorruptionType::UNDO_PAGE_CORRUPTED:
                oss << "Undo Page Corrupted";
                break;
            default:
                oss << "Unknown";
                break;
        }

        oss << "\n   Offset: " << c.offset << "\n";
        oss << "   Confidence: " << std::fixed << std::setprecision(1) << (c.confidence * 100) << "%\n";
        oss << "   Repairable: " << (c.repairable ? "Yes" : "No") << "\n";
        oss << "   Description: " << c.description << "\n\n";
    }

    oss << "Repair Statistics:\n";
    oss << "  Redo repaired: " << redo_repairer_.get_repaired_count() << "\n";
    oss << "  Undo repaired: " << undo_repairer_.get_repaired_count() << "\n";

    return oss.str();
}

} // namespace repair
} // namespace dbrecovery

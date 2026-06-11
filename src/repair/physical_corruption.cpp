#include "dbrecovery/repair/physical_corruption.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/page_reader.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include <cstring>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <iostream>

namespace dbrecovery {
namespace repair {

// ==================== PhysicalCorruptionDetector ====================

PhysicalCorruptionDetector::PhysicalCorruptionDetector()
    : page_size_(16384) {
}

std::vector<PhysicalCorruption> PhysicalCorruptionDetector::detect(const uint8_t* data, size_t size) {
    std::vector<PhysicalCorruption> corruptions;

    // 检测零化区域
    auto zeroed = detect_zeroed_regions(data, size);
    corruptions.insert(corruptions.end(), zeroed.begin(), zeroed.end());

    // 检测比特翻转
    auto bit_flips = detect_bit_flips(data, size);
    corruptions.insert(corruptions.end(), bit_flips.begin(), bit_flips.end());

    // 检测覆盖
    auto overwrites = detect_overwrites(data, size);
    corruptions.insert(corruptions.end(), overwrites.begin(), overwrites.end());

    // 检测截断
    auto truncation = detect_truncation(data, size);
    if (truncation) {
        corruptions.push_back(*truncation);
    }

    // 更新统计
    for (const auto& c : corruptions) {
        stats_[c.type]++;
    }

    return corruptions;
}

std::vector<PhysicalCorruption> PhysicalCorruptionDetector::detect_bit_flips(const uint8_t* data, size_t size) {
    std::vector<PhysicalCorruption> corruptions;
    if (!data || size < page_size_) {
        return corruptions;
    }

    size_t page_count = size / page_size_;

    for (size_t page_idx = 0; page_idx < page_count; ++page_idx) {
        const uint8_t* page = data + page_idx * page_size_;

        // 检查页头是否有效
        uint16_t page_type = dbrecovery::innodb::read_be<uint16_t>(page, dbrecovery::innodb::FIL_PAGE_TYPE_ABS);

        // 如果页类型看起来像是比特翻转的结果
        if (page_type != dbrecovery::innodb::FIL_PAGE_INDEX &&
            page_type != dbrecovery::innodb::FIL_PAGE_SDI &&
            page_type != dbrecovery::innodb::FIL_PAGE_UNDO_LOG &&
            page_type != dbrecovery::innodb::FIL_PAGE_TYPE_BLOB &&
            page_type != dbrecovery::innodb::FIL_PAGE_TYPE_ALLOCATED) {

            // 检查是否可能是单比特翻转
            uint16_t valid_types[] = {
                dbrecovery::innodb::FIL_PAGE_INDEX,      // 0x45BF = 17855
                dbrecovery::innodb::FIL_PAGE_SDI,        // 0x45BE = 17854
                dbrecovery::innodb::FIL_PAGE_UNDO_LOG,   // 0x45B2 = 17842
                dbrecovery::innodb::FIL_PAGE_TYPE_BLOB   // 0x45BD = 17853
            };

            for (uint16_t valid_type : valid_types) {
                // 计算汉明距离 (不同比特的数量)
                uint16_t xor_value = page_type ^ valid_type;
                int bit_diff = __builtin_popcount(xor_value);

                if (bit_diff <= 2) {  // 1-2比特翻转
                    PhysicalCorruption c;
                    c.type = (bit_diff == 1) ? PhysicalCorruptionType::BIT_FLIP : PhysicalCorruptionType::MULTI_BIT_FLIP;
                    c.offset = page_idx * page_size_ + dbrecovery::innodb::FIL_PAGE_TYPE_ABS;
                    c.length = 2;
                    c.original_value = (valid_type >> 8) & 0xFF;  // 高字节
                    c.corrupted_value = (page_type >> 8) & 0xFF;
                    c.confidence = 1.0f - (bit_diff * 0.2f);
                    c.description = "Bit flip detected in page type: 0x" +
                                   std::to_string(page_type) + " -> 0x" + std::to_string(valid_type);
                    corruptions.push_back(c);
                    break;
                }
            }
        }
    }

    return corruptions;
}

std::vector<PhysicalCorruption> PhysicalCorruptionDetector::detect_zeroed_regions(const uint8_t* data, size_t size) {
    std::vector<PhysicalCorruption> corruptions;
    if (!data || size < min_zeroed_length_) {
        return corruptions;
    }

    size_t zero_start = 0;
    size_t zero_count = 0;

    for (size_t i = 0; i < size; ++i) {
        if (data[i] == 0) {
            if (zero_count == 0) {
                zero_start = i;
            }
            zero_count++;
        } else {
            if (zero_count >= min_zeroed_length_) {
                PhysicalCorruption c;
                c.type = PhysicalCorruptionType::ZEROED_REGION;
                c.offset = zero_start;
                c.length = zero_count;
                c.confidence = 0.9f;
                c.description = "Zeroed region detected at offset " + std::to_string(zero_start) +
                               ", length " + std::to_string(zero_count);
                corruptions.push_back(c);
            }
            zero_count = 0;
        }
    }

    // 检查末尾的零化区域
    if (zero_count >= min_zeroed_length_) {
        PhysicalCorruption c;
        c.type = PhysicalCorruptionType::ZEROED_REGION;
        c.offset = zero_start;
        c.length = zero_count;
        c.confidence = 0.9f;
        c.description = "Zeroed region detected at end of file";
        corruptions.push_back(c);
    }

    return corruptions;
}

std::vector<PhysicalCorruption> PhysicalCorruptionDetector::detect_overwrites(const uint8_t* data, size_t size) {
    std::vector<PhysicalCorruption> corruptions;
    if (!data || size < page_size_) {
        return corruptions;
    }

    size_t page_count = size / page_size_;

    // 检测重复模式 (可能表示覆盖)
    for (size_t page_idx = 0; page_idx < page_count; ++page_idx) {
        const uint8_t* page = data + page_idx * page_size_;

        // 检查页是否全是相同字节
        bool all_same = true;
        uint8_t pattern = page[0];
        for (size_t i = 1; i < page_size_; ++i) {
            if (page[i] != pattern) {
                all_same = false;
                break;
            }
        }

        if (all_same && pattern != 0) {  // 0 的情况由 zeroed_regions 处理
            PhysicalCorruption c;
            c.type = PhysicalCorruptionType::PATTERN_OVERWRITE;
            c.offset = page_idx * page_size_;
            c.length = page_size_;
            c.corrupted_value = pattern;
            c.confidence = 0.85f;
            std::ostringstream oss;
            oss << "Pattern overwrite detected: page " << page_idx << " filled with 0x"
                << std::hex << std::setw(2) << std::setfill('0') << (int)pattern << std::dec;
            c.description = oss.str();
            corruptions.push_back(c);
        }
    }

    // 检测交替模式 (如 0xAA 0x55)
    for (size_t page_idx = 0; page_idx < page_count; ++page_idx) {
        const uint8_t* page = data + page_idx * page_size_;

        bool alternating = true;
        for (size_t i = 0; i < page_size_ - 1; ++i) {
            if (page[i] == page[i + 1]) {
                alternating = false;
                break;
            }
        }

        if (alternating && page_size_ > 2) {
            PhysicalCorruption c;
            c.type = PhysicalCorruptionType::REPEATED_PATTERN;
            c.offset = page_idx * page_size_;
            c.length = page_size_;
            c.confidence = 0.7f;
            c.description = "Alternating pattern detected in page " + std::to_string(page_idx);
            corruptions.push_back(c);
        }
    }

    return corruptions;
}

std::optional<PhysicalCorruption> PhysicalCorruptionDetector::detect_truncation(const uint8_t* data, size_t size) {
    if (!data || size < page_size_) {
        return std::nullopt;
    }

    // 检查文件大小是否是页大小的整数倍
    if (size % page_size_ != 0) {
        PhysicalCorruption c;
        c.type = PhysicalCorruptionType::TRUNCATION;
        c.offset = (size / page_size_) * page_size_;
        c.length = size % page_size_;
        c.confidence = 0.95f;
        c.description = "File is truncated: size " + std::to_string(size) +
                       " is not a multiple of page size " + std::to_string(page_size_);
        return c;
    }

    // 检查最后一页是否看起来不完整
    size_t last_page_idx = size / page_size_ - 1;
    const uint8_t* last_page = data + last_page_idx * page_size_;

    // 检查页尾的 LSN 是否与页头匹配
    uint64_t header_lsn = dbrecovery::innodb::read_be<uint64_t>(last_page, 16);
    uint64_t trailer_lsn = dbrecovery::innodb::read_be<uint64_t>(last_page, page_size_ - 8);

    if (header_lsn != trailer_lsn && header_lsn != 0 && trailer_lsn != 0) {
        PhysicalCorruption c;
        c.type = PhysicalCorruptionType::TRUNCATION;
        c.offset = last_page_idx * page_size_;
        c.length = page_size_;
        c.confidence = 0.8f;
        c.description = "Last page LSN mismatch, possible truncation";
        return c;
    }

    return std::nullopt;
}

// ==================== PhysicalCorruptionRepairer ====================

PhysicalCorruptionRepairer::PhysicalCorruptionRepairer()
    : page_size_(16384) {
}

size_t PhysicalCorruptionRepairer::repair(uint8_t* data, size_t size,
                                           const std::vector<PhysicalCorruption>& corruptions) {
    size_t repaired = 0;

    for (const auto& c : corruptions) {
        bool success = false;

        switch (c.type) {
            case PhysicalCorruptionType::BIT_FLIP:
            case PhysicalCorruptionType::MULTI_BIT_FLIP:
                success = repair_bit_flip(data, c);
                break;

            case PhysicalCorruptionType::ZEROED_REGION:
                success = repair_zeroed_region(data, size, c);
                break;

            case PhysicalCorruptionType::TRUNCATION:
                // 截断无法直接修复，需要特殊处理
                failed_count_++;
                continue;

            default:
                // 其他类型的损坏暂不支持自动修复
                failed_count_++;
                continue;
        }

        if (success) {
            repaired++;
            repaired_count_++;
        } else {
            failed_count_++;
        }
    }

    return repaired;
}

bool PhysicalCorruptionRepairer::repair_bit_flip(uint8_t* page_data, const PhysicalCorruption& corruption) {
    if (!page_data || corruption.length < 2) {
        return false;
    }

    // 对于页类型的比特翻转，直接恢复原始值
    if (corruption.offset % page_size_ == dbrecovery::innodb::FIL_PAGE_TYPE_ABS) {
        // 获取期望的有效页类型
        uint16_t valid_types[] = {
            dbrecovery::innodb::FIL_PAGE_INDEX,
            dbrecovery::innodb::FIL_PAGE_SDI,
            dbrecovery::innodb::FIL_PAGE_UNDO_LOG,
            dbrecovery::innodb::FIL_PAGE_TYPE_BLOB
        };

        uint16_t corrupted_type = dbrecovery::innodb::read_be<uint16_t>(page_data, corruption.offset);

        for (uint16_t valid_type : valid_types) {
            uint16_t xor_value = corrupted_type ^ valid_type;
            int bit_diff = __builtin_popcount(xor_value);

            if (bit_diff <= 2) {
                // 修复比特翻转
                dbrecovery::innodb::write_be<uint16_t>(page_data, corruption.offset, valid_type);

                // 重新计算校验和
                // (由 HeaderRepairer 处理)
                return true;
            }
        }
    }

    return false;
}

bool PhysicalCorruptionRepairer::repair_zeroed_region(uint8_t* page_data, size_t page_size,
                                                       const PhysicalCorruption& corruption) {
    if (!page_data) {
        return false;
    }

    // 对于零化区域，我们需要从上下文推断内容
    // 这是一个复杂的问题，这里只处理简单情况

    size_t page_idx = corruption.offset / page_size_;
    size_t offset_in_page = corruption.offset % page_size_;

    // 如果零化的是页头部分
    if (offset_in_page < dbrecovery::innodb::FIL_HEADER_SIZE) {
        // 页头零化是严重损坏，需要从其他信息推断
        // 暂时标记为无法修复
        last_error_ = "Zeroed page header cannot be automatically repaired";
        return false;
    }

    // 如果零化的是数据区域
    // 可以尝试从记录链推断，但这需要更复杂的逻辑
    // 暂时返回 false，表示需要手动处理
    last_error_ = "Zeroed data region requires manual intervention or schema information";

    return false;
}

std::vector<uint8_t> PhysicalCorruptionRepairer::handle_truncation(const uint8_t* file_data, size_t file_size,
                                                                     std::optional<size_t> expected_size) {
    std::vector<uint8_t> result;

    if (!file_data || file_size == 0) {
        return result;
    }

    // 计算完整页数
    size_t complete_pages = file_size / page_size_;
    size_t complete_size = complete_pages * page_size_;

    // 复制完整的数据
    result.assign(file_data, file_data + complete_size);

    // 如果有期望大小且小于当前大小，进行填充
    if (expected_size && *expected_size > complete_size) {
        size_t padding_needed = *expected_size - complete_size;
        result.resize(*expected_size, 0);

        std::cout << "[INFO] Truncated file: recovered " << complete_size << " bytes, "
                  << "padded " << padding_needed << " bytes" << std::endl;
    }

    return result;
}

std::optional<std::vector<uint8_t>> PhysicalCorruptionRepairer::repair_with_ecc(const uint8_t* data, size_t size) {
    // ECC 修复需要预先存储校验数据
    // 这里只是一个占位实现
    // 实际实现需要 RS 码或 LDPC 等纠错码

    if (!data || size == 0) {
        return std::nullopt;
    }

    // 目前不支持 ECC 修复
    last_error_ = "ECC repair not implemented";
    return std::nullopt;
}

std::optional<std::vector<uint8_t>> PhysicalCorruptionRepairer::repair_from_neighbors(
    const uint8_t* corrupted_page,
    const uint8_t* prev_page,
    const uint8_t* next_page) {

    if (!corrupted_page) {
        return std::nullopt;
    }

    std::vector<uint8_t> repaired_page(corrupted_page, corrupted_page + page_size_);

    // 从相邻页推断：
    // 1. 页类型可以从索引结构推断
    // 2. 页链指针可以从相邻页重建
    // 3. 数据内容难以从相邻页推断

    if (prev_page) {
        // 从前一页获取 next_page 指针
        uint32_t prev_next = dbrecovery::innodb::read_be<uint32_t>(prev_page, 12);
        if (prev_next != 0xFFFFFFFF) {
            // 当前页的页号应该是 prev_next
            // 但我们无法恢复实际数据
        }
    }

    if (next_page) {
        // 从后一页获取 prev_page 指针
        uint32_t next_prev = dbrecovery::innodb::read_be<uint32_t>(next_page, 8);
        if (next_prev != 0xFFFFFFFF) {
            // 当前页的页号应该是 next_prev
        }
    }

    // 无法完全修复数据内容
    last_error_ = "Neighbor-based repair can only restore page chain, not data content";
    return std::nullopt;
}

std::optional<uint8_t> PhysicalCorruptionRepairer::try_fix_byte(
    uint8_t corrupted_byte,
    const std::vector<uint8_t>& expected_values) {

    for (uint8_t expected : expected_values) {
        uint8_t xor_value = corrupted_byte ^ expected;
        int bit_diff = __builtin_popcount(xor_value);

        if (bit_diff == 1) {
            return expected;  // 单比特翻转，可以修复
        }
    }

    return std::nullopt;
}

// ==================== PhysicalCorruptionHandler ====================

PhysicalCorruptionHandler::PhysicalCorruptionHandler()
    : page_size_(16384) {
}

size_t PhysicalCorruptionHandler::analyze_and_repair(uint8_t* file_data, size_t file_size) {
    // 先检测损坏
    last_corruptions_ = detector_.detect(file_data, file_size);

    if (last_corruptions_.empty()) {
        return 0;
    }

    // 然后修复
    return repairer_.repair(file_data, file_size, last_corruptions_);
}

std::vector<PhysicalCorruption> PhysicalCorruptionHandler::analyze(const uint8_t* file_data, size_t file_size) {
    last_corruptions_ = detector_.detect(file_data, file_size);
    return last_corruptions_;
}

std::string PhysicalCorruptionHandler::get_report() const {
    std::ostringstream oss;
    oss << "Physical Corruption Analysis Report\n";
    oss << "===================================\n\n";

    if (last_corruptions_.empty()) {
        oss << "No physical corruption detected.\n";
        return oss.str();
    }

    oss << "Detected " << last_corruptions_.size() << " corruption(s):\n\n";

    for (size_t i = 0; i < last_corruptions_.size(); ++i) {
        const auto& c = last_corruptions_[i];
        oss << (i + 1) << ". ";

        switch (c.type) {
            case PhysicalCorruptionType::BIT_FLIP:
                oss << "Bit Flip";
                break;
            case PhysicalCorruptionType::MULTI_BIT_FLIP:
                oss << "Multi-Bit Flip";
                break;
            case PhysicalCorruptionType::PARTIAL_OVERWRITE:
                oss << "Partial Overwrite";
                break;
            case PhysicalCorruptionType::ZEROED_REGION:
                oss << "Zeroed Region";
                break;
            case PhysicalCorruptionType::TRUNCATION:
                oss << "Truncation";
                break;
            case PhysicalCorruptionType::PATTERN_OVERWRITE:
                oss << "Pattern Overwrite";
                break;
            case PhysicalCorruptionType::RANDOM_DATA:
                oss << "Random Data";
                break;
            case PhysicalCorruptionType::REPEATED_PATTERN:
                oss << "Repeated Pattern";
                break;
            default:
                oss << "Unknown";
                break;
        }

        oss << "\n   Offset: " << c.offset << "\n";
        oss << "   Length: " << c.length << "\n";
        oss << "   Confidence: " << std::fixed << std::setprecision(1) << (c.confidence * 100) << "%\n";
        oss << "   Description: " << c.description << "\n\n";
    }

    oss << "Repair Statistics:\n";
    oss << "  Repaired: " << repairer_.get_repaired_count() << "\n";
    oss << "  Failed: " << repairer_.get_failed_count() << "\n";

    return oss.str();
}

} // namespace repair
} // namespace dbrecovery

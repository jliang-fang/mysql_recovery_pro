#include "dbrecovery/repair/metadata_corruption_repair.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/sdi_manager.hpp"
#include <cstring>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <zlib.h>

namespace dbrecovery {
namespace repair {

// ==================== MetadataCorruptionRepairer ====================

MetadataCorruptionRepairer::MetadataCorruptionRepairer()
    : page_size_(16384) {
}

std::vector<MetadataCorruption> MetadataCorruptionRepairer::detect_corruptions(const uint8_t* data, size_t size) {
    std::vector<MetadataCorruption> corruptions;

    if (!data || size < page_size_ * 3) {
        return corruptions;
    }

    // 1. 检查 Page 0 (FSP header)
    auto fsp_opt = parse_fsp_header(data, size);
    if (!fsp_opt || !fsp_opt->is_valid) {
        MetadataCorruption c;
        c.type = MetadataCorruptionType::FSP_HEADER_CORRUPTED;
        c.page_no = 0;
        c.offset = 0;
        c.description = "FSP header (page 0) is corrupted or invalid";
        c.confidence = 0.95f;
        c.repairable = true;
        corruptions.push_back(c);
    } else {
        // 检查 space flags
        uint32_t space_flags = fsp_opt->space_flags;
        // Space flags 格式: [row_format:4bits][zip_size:4bits][其他:24bits]
        uint8_t row_format = (space_flags >> 28) & 0x0F;
        uint8_t zip_size = (space_flags >> 24) & 0x0F;

        if (row_format > 3) {  // 0=REDUNDANT, 1=COMPACT, 2=COMPRESSED, 3=DYNAMIC
            MetadataCorruption c;
            c.type = MetadataCorruptionType::FSP_SPACE_FLAGS_INVALID;
            c.page_no = 0;
            c.offset = 38 + 4;  // FSP_SPACE_FLAGS offset
            std::ostringstream oss;
            oss << "Invalid row format in space flags: " << (int)row_format;
            c.description = oss.str();
            c.confidence = 0.90f;
            c.repairable = true;
            corruptions.push_back(c);
        }
    }

    // 2. 检查 Page 2 (INODE)
    auto inode_opt = parse_inode_page(data, size);
    if (!inode_opt || !inode_opt->is_valid) {
        MetadataCorruption c;
        c.type = MetadataCorruptionType::INODE_PAGE_CORRUPTED;
        c.page_no = 2;
        c.offset = 2 * page_size_;
        c.description = "INODE page (page 2) is corrupted";
        c.confidence = 0.90f;
        c.repairable = true;
        corruptions.push_back(c);
    }

    // 3. 检查表空间 ID 一致性
    auto space_ids = get_all_space_ids(data, size);
    if (space_ids.size() > 1) {
        // 检查是否有不一致的 space_id
        uint32_t first_space_id = space_ids.begin()->second;
        for (const auto& [page_no, space_id] : space_ids) {
            if (space_id != first_space_id && space_id != 0 && space_id != 0xFFFFFFFF) {
                MetadataCorruption c;
                c.type = MetadataCorruptionType::SPACE_ID_MISMATCH;
                c.page_no = page_no;
                c.offset = page_no * page_size_ + 34;  // FIL_PAGE_SPACE_ID
                std::ostringstream oss;
                oss << "Page " << page_no << " has space_id " << space_id
                    << ", expected " << first_space_id;
                c.description = oss.str();
                c.confidence = 0.85f;
                c.repairable = true;
                corruptions.push_back(c);
            }
        }
    }

    // 4. 检查 SDI 页 (Page 1 in MySQL 8.0+)
    auto sdi_opt = parse_sdi_page(data, size, 1);
    if (sdi_opt && !sdi_opt->is_valid) {
        MetadataCorruption c;
        c.type = MetadataCorruptionType::SDI_PAGE_CORRUPTED;
        c.page_no = 1;
        c.offset = page_size_;
        c.description = "SDI page (page 1) is corrupted";
        c.confidence = 0.85f;
        c.repairable = true;
        corruptions.push_back(c);
    }

    return corruptions;
}

size_t MetadataCorruptionRepairer::repair(uint8_t* data, size_t size,
                                            const std::vector<MetadataCorruption>& corruptions) {
    size_t repaired = 0;

    for (const auto& c : corruptions) {
        bool success = false;

        switch (c.type) {
            case MetadataCorruptionType::FSP_HEADER_CORRUPTED:
                success = repair_fsp_header(data, size);
                break;

            case MetadataCorruptionType::FSP_SPACE_FLAGS_INVALID:
                success = repair_space_flags(data, size, 0x20000000);  // DYNAMIC row format
                break;

            case MetadataCorruptionType::INODE_PAGE_CORRUPTED:
                success = repair_inode_page(data, size);
                break;

            case MetadataCorruptionType::SPACE_ID_MISMATCH:
                {
                    auto inferred = infer_space_id(data, size);
                    if (inferred) {
                        success = repair_space_id_mismatch(data, size, *inferred);
                    }
                }
                break;

            case MetadataCorruptionType::SDI_PAGE_CORRUPTED:
                success = repair_sdi_page(data, size, c.page_no);
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

std::optional<FspHeaderInfo> MetadataCorruptionRepairer::parse_fsp_header(const uint8_t* data, size_t size) {
    if (!data || size < page_size_) {
        return std::nullopt;
    }

    const uint8_t* page0 = data;

    FspHeaderInfo fsp = {};

    // FSP Header 结构 (Page 0, offset 38):
    // +0-3: space_id
    // +4-7: space_flags
    // +8-15: size (页数)
    // +16-23: free_limit
    // +24-27: free_len
    // +28-31: frag_n_used
    // ...

    fsp.space_id = dbrecovery::innodb::read_be<uint32_t>(page0, 38 + 0);
    fsp.space_flags = dbrecovery::innodb::read_be<uint32_t>(page0, 38 + 4);
    fsp.size = dbrecovery::innodb::read_be<uint64_t>(page0, 38 + 8);
    fsp.free_limit = dbrecovery::innodb::read_be<uint64_t>(page0, 38 + 16);
    fsp.free_len = dbrecovery::innodb::read_be<uint32_t>(page0, 38 + 24);
    fsp.frag_n_used = dbrecovery::innodb::read_be<uint32_t>(page0, 38 + 28);
    fsp.lsn = dbrecovery::innodb::read_be<uint64_t>(page0, 16);  // FIL_PAGE_LSN

    // 验证
    fsp.is_valid = (fsp.space_id != 0xFFFFFFFF);
    if (fsp.size > size / page_size_) {
        fsp.is_valid = false;
    }

    return fsp;
}

bool MetadataCorruptionRepairer::repair_fsp_header(uint8_t* data, size_t size) {
    if (!data || size < page_size_) {
        return false;
    }

    uint8_t* page0 = data;

    // 获取或推断 space_id
    auto inferred = infer_space_id(data, size);
    uint32_t space_id = inferred.value_or(1);

    // 写入 FSP header
    dbrecovery::innodb::write_be<uint32_t>(page0, 38 + 0, space_id);      // space_id
    dbrecovery::innodb::write_be<uint32_t>(page0, 38 + 4, 0x20000000);    // space_flags (DYNAMIC)
    dbrecovery::innodb::write_be<uint64_t>(page0, 38 + 8, size / page_size_);  // size
    dbrecovery::innodb::write_be<uint64_t>(page0, 38 + 16, size / page_size_);  // free_limit
    dbrecovery::innodb::write_be<uint32_t>(page0, 38 + 24, 0);            // free_len
    dbrecovery::innodb::write_be<uint32_t>(page0, 38 + 28, 0);            // frag_n_used

    return true;
}

bool MetadataCorruptionRepairer::repair_space_flags(uint8_t* data, size_t size, uint32_t correct_flags) {
    if (!data || size < page_size_) {
        return false;
    }

    uint8_t* page0 = data;
    dbrecovery::innodb::write_be<uint32_t>(page0, 38 + 4, correct_flags);

    return true;
}

bool MetadataCorruptionRepairer::repair_space_id_mismatch(uint8_t* data, size_t size, uint32_t correct_space_id) {
    if (!data || size < page_size_) {
        return false;
    }

    size_t page_count = size / page_size_;

    // 更新所有页的 space_id
    for (size_t i = 0; i < page_count; ++i) {
        uint8_t* page = data + i * page_size_;
        dbrecovery::innodb::write_be<uint32_t>(page, 34, correct_space_id);
    }

    // 更新 FSP header 的 space_id
    dbrecovery::innodb::write_be<uint32_t>(data, 38 + 0, correct_space_id);

    return true;
}

std::optional<InodePageInfo> MetadataCorruptionRepairer::parse_inode_page(const uint8_t* data, size_t size) {
    if (!data || size < page_size_ * 3) {
        return std::nullopt;
    }

    const uint8_t* inode_page = data + 2 * page_size_;

    InodePageInfo inode = {};
    inode.page_no = 2;

    // 检查页类型
    uint16_t page_type = dbrecovery::innodb::read_be<uint16_t>(inode_page, dbrecovery::innodb::FIL_PAGE_TYPE_ABS);
    if (page_type != 0x45BB) {  // FIL_PAGE_INODE
        inode.is_valid = false;
        return inode;
    }

    // 解析 INODE 条目
    // INODE 结构：
    // List node for full inode list (12 bytes)
    // List node for free inode list (12 bytes)
    // Inode entries (each 192 bytes in MySQL 8.0)
    // 每个 inode entry 包含 segment information

    // 简化处理：假设基本结构正确
    inode.is_valid = true;

    return inode;
}

bool MetadataCorruptionRepairer::repair_inode_page(uint8_t* data, size_t size) {
    if (!data || size < page_size_ * 3) {
        return false;
    }

    uint8_t* inode_page = data + 2 * page_size_;

    // 设置页类型为 INODE
    dbrecovery::innodb::write_be<uint16_t>(inode_page, dbrecovery::innodb::FIL_PAGE_TYPE_ABS, 0x45BB);

    // 清零其余部分（保留 FIL header）
    std::memset(inode_page + 38, 0, page_size_ - 38 - 8);

    return true;
}

bool MetadataCorruptionRepairer::rebuild_inode_from_scan(uint8_t* data, size_t size,
                                                          const std::vector<uint32_t>& root_pages) {
    if (!data || size < page_size_ * 3 || root_pages.empty()) {
        return false;
    }

    uint8_t* inode_page = data + 2 * page_size_;

    // 设置页类型
    dbrecovery::innodb::write_be<uint16_t>(inode_page, dbrecovery::innodb::FIL_PAGE_TYPE_ABS, 0x45BB);

    // 清零
    std::memset(inode_page + 38, 0, page_size_ - 38 - 8);

    // 写入根页信息
    // 实际实现需要按照 INODE entry 格式写入

    return true;
}

std::optional<SdiPageInfo> MetadataCorruptionRepairer::parse_sdi_page(const uint8_t* data, size_t size, uint32_t page_no) {
    if (!data || size < (page_no + 1) * page_size_) {
        return std::nullopt;
    }

    const uint8_t* sdi_page = data + page_no * page_size_;

    SdiPageInfo sdi = {};
    sdi.page_no = page_no;

    // 检查页类型
    uint16_t page_type = dbrecovery::innodb::read_be<uint16_t>(sdi_page, dbrecovery::innodb::FIL_PAGE_TYPE_ABS);
    if (page_type != dbrecovery::innodb::FIL_PAGE_SDI) {
        sdi.is_valid = false;
        return sdi;
    }

    // SDI 页结构：
    // FIL_HEADER (38 bytes)
    // SDI header (包含版本、类型等)
    // Compressed SDI data

    // 读取 SDI header
    sdi.uncompressed_len = dbrecovery::innodb::read_le<uint32_t>(sdi_page, 38 + 0);
    sdi.compressed_len = dbrecovery::innodb::read_le<uint32_t>(sdi_page, 38 + 4);
    sdi.compression_type = sdi_page[38 + 8];

    // 验证
    sdi.is_valid = (sdi.uncompressed_len > 0 && sdi.compressed_len > 0);

    return sdi;
}

bool MetadataCorruptionRepairer::repair_sdi_page(uint8_t* data, size_t size, uint32_t page_no) {
    if (!data || size < (page_no + 1) * page_size_) {
        return false;
    }

    uint8_t* sdi_page = data + page_no * page_size_;

    // 设置页类型为 SDI
    dbrecovery::innodb::write_be<uint16_t>(sdi_page, dbrecovery::innodb::FIL_PAGE_TYPE_ABS,
                                            dbrecovery::innodb::FIL_PAGE_SDI);

    return true;
}

std::optional<std::vector<uint8_t>> MetadataCorruptionRepairer::decompress_sdi_blob(
    const uint8_t* compressed_data, size_t compressed_len, size_t uncompressed_len) {

    if (!compressed_data || compressed_len == 0 || uncompressed_len == 0) {
        return std::nullopt;
    }

    std::vector<uint8_t> result(uncompressed_len);

    // 使用 zlib 解压
    uLongf dest_len = uncompressed_len;
    int ret = uncompress(result.data(), &dest_len, compressed_data, compressed_len);

    if (ret != Z_OK) {
        last_error_ = "Failed to decompress SDI blob: zlib error " + std::to_string(ret);
        return std::nullopt;
    }

    if (dest_len != uncompressed_len) {
        last_error_ = "Decompressed size mismatch";
        return std::nullopt;
    }

    return result;
}

bool MetadataCorruptionRepairer::rebuild_page_zero(uint8_t* data, size_t size) {
    return repair_fsp_header(data, size);
}

std::optional<uint32_t> MetadataCorruptionRepairer::infer_space_id(const uint8_t* data, size_t size) {
    if (!data || size < page_size_) {
        return std::nullopt;
    }

    auto space_ids = get_all_space_ids(data, size);

    if (space_ids.empty()) {
        return std::nullopt;
    }

    // 找出最常见的 space_id
    std::map<uint32_t, size_t> count;
    for (const auto& [page_no, space_id] : space_ids) {
        if (space_id != 0 && space_id != 0xFFFFFFFF) {
            count[space_id]++;
        }
    }

    if (count.empty()) {
        return 1;  // 默认值
    }

    uint32_t most_common = 0;
    size_t max_count = 0;
    for (const auto& [space_id, cnt] : count) {
        if (cnt > max_count) {
            max_count = cnt;
            most_common = space_id;
        }
    }

    return most_common;
}

bool MetadataCorruptionRepairer::validate_metadata(const uint8_t* data, size_t size) {
    auto corruptions = detect_corruptions(data, size);
    return corruptions.empty();
}

std::map<uint32_t, uint32_t> MetadataCorruptionRepairer::get_all_space_ids(const uint8_t* data, size_t size) const {
    std::map<uint32_t, uint32_t> result;

    if (!data || size < page_size_) {
        return result;
    }

    size_t page_count = size / page_size_;

    for (size_t i = 0; i < page_count; ++i) {
        const uint8_t* page = data + i * page_size_;
        uint32_t space_id = dbrecovery::innodb::read_be<uint32_t>(page, 34);
        result[static_cast<uint32_t>(i)] = space_id;
    }

    return result;
}

// ==================== SdiDataRepairer ====================

SdiDataRepairer::SdiDataRepairer()
    : page_size_(16384) {
}

std::optional<std::string> SdiDataRepairer::extract_sdi_json(const uint8_t* data, size_t size) {
    if (!data || size < page_size_) {
        return std::nullopt;
    }

    // 使用 SDIManager 提取 SDI
    dbrecovery::innodb::SDIManager sdi_manager;
    auto json_opt = sdi_manager.extract_sdi_json(data, size);

    return json_opt;
}

std::optional<std::vector<uint8_t>> SdiDataRepairer::repair_sdi_data(const uint8_t* data, size_t size) {
    if (!data || size < page_size_) {
        return std::nullopt;
    }

    // 尝试提取和修复 SDI 数据
    // 这需要更复杂的实现

    return std::nullopt;
}

std::optional<std::string> SdiDataRepairer::infer_schema_from_pages(const uint8_t* data, size_t size) {
    if (!data || size < page_size_) {
        return std::nullopt;
    }

    // 从页数据推断表结构
    // 这需要分析 INDEX 页中的记录格式

    return std::nullopt;
}

// ==================== MetadataCorruptionHandler ====================

size_t MetadataCorruptionHandler::analyze_and_repair(uint8_t* data, size_t size) {
    last_corruptions_ = metadata_repairer_.detect_corruptions(data, size);
    if (last_corruptions_.empty()) {
        return 0;
    }
    return metadata_repairer_.repair(data, size, last_corruptions_);
}

std::vector<MetadataCorruption> MetadataCorruptionHandler::analyze(const uint8_t* data, size_t size) {
    return metadata_repairer_.detect_corruptions(data, size);
}

std::string MetadataCorruptionHandler::get_report() const {
    std::ostringstream oss;
    oss << "Metadata Corruption Report\n";
    oss << "===========================\n\n";

    if (last_corruptions_.empty()) {
        oss << "No metadata corruption detected.\n";
        return oss.str();
    }

    oss << "Detected " << last_corruptions_.size() << " corruption(s):\n\n";

    for (size_t i = 0; i < last_corruptions_.size(); ++i) {
        const auto& c = last_corruptions_[i];
        oss << (i + 1) << ". ";

        switch (c.type) {
            case MetadataCorruptionType::FSP_HEADER_CORRUPTED:
                oss << "FSP Header Corrupted";
                break;
            case MetadataCorruptionType::FSP_SPACE_FLAGS_INVALID:
                oss << "FSP Space Flags Invalid";
                break;
            case MetadataCorruptionType::SPACE_ID_MISMATCH:
                oss << "Space ID Mismatch";
                break;
            case MetadataCorruptionType::INODE_PAGE_CORRUPTED:
                oss << "INODE Page Corrupted";
                break;
            case MetadataCorruptionType::SDI_PAGE_CORRUPTED:
                oss << "SDI Page Corrupted";
                break;
            case MetadataCorruptionType::SDI_BLOB_CORRUPTED:
                oss << "SDI BLOB Corrupted";
                break;
            default:
                oss << "Unknown";
                break;
        }

        oss << "\n   Page: " << c.page_no << "\n";
        oss << "   Confidence: " << std::fixed << std::setprecision(1) << (c.confidence * 100) << "%\n";
        oss << "   Repairable: " << (c.repairable ? "Yes" : "No") << "\n";
        oss << "   Description: " << c.description << "\n\n";
    }

    oss << "Repair Statistics:\n";
    oss << "  Repaired: " << metadata_repairer_.get_repaired_count() << "\n";

    return oss.str();
}

} // namespace repair
} // namespace dbrecovery

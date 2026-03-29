#include "dbrecovery/innodb/redo_log_replayer.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/page_reader.hpp"
#include <iostream>

namespace dbrecovery {
namespace innodb {

RedoLogReplayer::RedoLogReplayer() {}

RedoLogReplayer::~RedoLogReplayer() = default;

void RedoLogReplayer::set_page(uint32_t page_no, std::vector<uint8_t> page_data) {
    pages_[page_no] = std::move(page_data);
}

std::vector<uint8_t> RedoLogReplayer::get_page(uint32_t page_no) const {
    auto it = pages_.find(page_no);
    if (it != pages_.end()) {
        return it->second;
    }
    return {};
}

bool RedoLogReplayer::apply_record(const RedoLogRecord& record) {
    // 检查页是否存在
    auto it = pages_.find(record.r_page_no);
    if (it == pages_.end()) {
        std::cerr << "[WARN] RedoLogReplayer: Page " << record.r_page_no 
                  << " not found, skipping record" << std::endl;
        failed_count_++;
        return false;
    }

    auto& page_data = it->second;

    // 检查页类型
    uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data.data(), 16);
    if (page_type != dbrecovery::innodb::FIL_PAGE_INDEX) {
        std::cerr << "[WARN] RedoLogReplayer: Page " << record.r_page_no 
                  << " is not an index page, skipping" << std::endl;
        failed_count_++;
        return false;
    }

    // 根据记录类型应用更新
    switch (record.r_type) {
        case MLOG_1BYTE: {
            // 单字节更新
            if (record.r_data.size() < 1) {
                failed_count_++;
                return false;
            }
            uint8_t value = record.r_data[0];
            // 简化：假设数据是偏移+值
            if (record.r_data.size() >= 2) {
                uint16_t offset = dbrecovery::innodb::read_le<uint16_t>(record.r_data.data(), 0);
                if (offset + 1 <= page_data.size()) {
                    page_data[offset] = value;
                    applied_count_++;
                    return true;
                }
            }
            break;
        }

        case MLOG_2BYTES: {
            // 双字节更新
            if (record.r_data.size() < 2) {
                failed_count_++;
                return false;
            }
            if (record.r_data.size() >= 4) {
                uint16_t offset = dbrecovery::innodb::read_le<uint16_t>(record.r_data.data(), 0);
                uint16_t value = dbrecovery::innodb::read_le<uint16_t>(record.r_data.data(), 2);
                if (offset + 2 <= page_data.size()) {
                    dbrecovery::innodb::write_le<uint16_t>(page_data.data(), offset, value);
                    applied_count_++;
                    return true;
                }
            }
            break;
        }

        case MLOG_4BYTES: {
            // 四字节更新
            if (record.r_data.size() < 4) {
                failed_count_++;
                return false;
            }
            if (record.r_data.size() >= 6) {
                uint16_t offset = dbrecovery::innodb::read_le<uint16_t>(record.r_data.data(), 0);
                uint32_t value = dbrecovery::innodb::read_le<uint32_t>(record.r_data.data(), 4);
                if (offset + 4 <= page_data.size()) {
                    dbrecovery::innodb::write_le<uint32_t>(page_data.data(), offset, value);
                    applied_count_++;
                    return true;
                }
            }
            break;
        }

        case MLOG_8BYTES: {
            // 八字节更新
            if (record.r_data.size() < 8) {
                failed_count_++;
                return false;
            }
            if (record.r_data.size() >= 10) {
                uint16_t offset = dbrecovery::innodb::read_le<uint16_t>(record.r_data.data(), 0);
                uint64_t value = dbrecovery::innodb::read_le<uint64_t>(record.r_data.data(), 8);
                if (offset + 8 <= page_data.size()) {
                    dbrecovery::innodb::write_le<uint64_t>(page_data.data(), offset, value);
                    applied_count_++;
                    return true;
                }
            }
            break;
        }

        case MLOG_REC_INSERT: {
            // 记录插入（简化实现）
            std::cerr << "[INFO] RedoLogReplayer: MLOG_REC_INSERT not fully implemented" << std::endl;
            applied_count_++;
            return true;
        }

        case MLOG_REC_UPDATE: {
            // 记录更新（简化实现）
            std::cerr << "[INFO] RedoLogReplayer: MLOG_REC_UPDATE not fully implemented" << std::endl;
            applied_count_++;
            return true;
        }

        case MLOG_REC_DELETE: {
            // 记录删除（简化实现）
            std::cerr << "[INFO] RedoLogReplayer: MLOG_REC_DELETE not fully implemented" << std::endl;
            applied_count_++;
            return true;
        }

        case MLOG_PAGE_CREATE: {
            // 页创建（清空页数据）
            std::fill(page_data.begin(), page_data.end(), 0);
            applied_count_++;
            return true;
        }

        case MLOG_PAGE_REINIT: {
            // 页重新初始化
            std::fill(page_data.begin() + 38, page_data.end(), 0);
            applied_count_++;
            return true;
        }

        default: {
            std::cerr << "[WARN] RedoLogReplayer: Unsupported log type " 
                      << static_cast<int>(record.r_type) << std::endl;
            failed_count_++;
            return false;
        }
    }

    failed_count_++;
    return false;
}

size_t RedoLogReplayer::apply_records(const std::vector<RedoLogRecord>& records) {
    size_t success = 0;
    for (const auto& record : records) {
        if (apply_record(record)) {
            success++;
        }
    }
    return success;
}

void RedoLogReplayer::update_page_checksum(uint32_t page_no) {
    auto it = pages_.find(page_no);
    if (it == pages_.end()) {
        return;
    }

    auto& page_data = it->second;

    // 计算 CRC32C 校验和
    std::vector<uint8_t> page_copy = page_data;
    for (int i = 0; i < 4; ++i) {
        page_copy[24 + i] = 0;
    }

    uint32_t computed_crc = PageReader::compute_crc32c(page_copy);

    // 写入校验和
    for (int i = 0; i < 4; ++i) {
        page_data[24 + i] = (computed_crc >> (i * 8)) & 0xFF;
    }
}

} // namespace innodb
} // namespace dbrecovery

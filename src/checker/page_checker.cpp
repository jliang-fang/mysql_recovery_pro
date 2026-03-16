#include "dbrecovery/checker/page_checker.hpp"
#include <iostream>

namespace dbrecovery {
namespace checker {

PageChecker::PageChecker() {}

PageChecker::~PageChecker() = default;

PageHealth PageChecker::check(const std::vector<uint8_t>& page_data) {
    PageHealth health;

    // 基本校验
    if (page_data.size() < 38) {
        health.is_valid = false;
        health.status_message = "Page too small";
        return health;
    }

    // 校验和检查
    uint32_t stored_checksum = dbrecovery::innodb::read_le<uint32_t>(page_data.data(), 24);
    if (stored_checksum == 0 || stored_checksum == 0xFFFFFFFF) {
        health.checksum_match = true; // NONE校验模式
    } else {
        // 计算实际校验和（简化实现）
        uint32_t computed_crc32c = dbrecovery::innodb::PageReader::compute_crc32c(page_data);
        health.checksum_expected = stored_checksum;
        health.checksum_actual = computed_crc32c;
        health.checksum_match = (stored_checksum == computed_crc32c);
    }

    // B+树结构检查
    uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data.data(), 12);
    if (page_type == dbrecovery::innodb::FIL_PAGE_INDEX) {
        validate_btree_structure(page_data, health);
    }

    return health;
}

bool PageChecker::validate_checksum(const std::vector<uint8_t>& page_data, uint32_t expected) {
    // 使用PageReader的校验和计算方法
    return dbrecovery::innodb::PageReader::validate_checksum(page_data);
}

bool PageChecker::validate_btree_structure(const std::vector<uint8_t>& page_data, PageHealth& health) {
    // 检查INFIMUM和SUPREMUM记录
    if (page_data.size() < 100) {
        return false;
    }

    uint16_t infimum_offset = 54;
    if (infimum_offset + 13 <= page_data.size()) {
        health.has_infimum = true;

        uint16_t supremum_offset = dbrecovery::innodb::read_le<uint16_t>(page_data.data(), infimum_offset + 1);
        if (supremum_offset > 0 && supremum_offset < page_data.size()) {
            health.has_supremum = true;
        }
    }

    // 解析页目录
    if (page_data.size() >= 38 + 36) {
        uint16_t n_dir_slots = dbrecovery::innodb::read_le<uint16_t>(page_data.data(), dbrecovery::innodb::PAGE_N_DIR_SLOTS + 38);
        health.n_dir_slots = n_dir_slots;
    }

    return true;
}

} // namespace checker
} // namespace dbrecovery


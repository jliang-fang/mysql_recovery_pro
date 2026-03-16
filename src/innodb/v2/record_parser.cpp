#include "dbrecovery/innodb/v2/record_parser.hpp"
#include <cstring>

namespace DBRecovery::InnoDB::v2 {

RecordParser::RecordParser(const PageParser& parser)
    : parser_(parser) {}

std::vector<Record> RecordParser::parse() noexcept {
    std::vector<Record> records;
    
    const auto& header = parser_.header();
    
    // PAGE_DIR_SLOTS 紧跟在 PAGE_HEADER 后
    const uint8_t* dir_slots = parser_.data() + 38;
    
    for (uint16_t i = 0; i < header.n_dir_slots; ++i) {
        uint16_t offset = 0;
        std::memcpy(&offset, dir_slots + i * 2, sizeof(uint16_t));
        
        // ✅ 增强：过滤无效 slot
        // 1. 空槽（offset == 0）
        // 2. 超出堆顶（offset >= header.heap_top）
        // 3. 小于页数据区起始位置（FIL_PAGE_DATA = 38 + 最小记录空间 16 = 54）
        if (offset == 0 || offset >= header.heap_top || offset < 54) {
            continue;
        }
        
        Record rec;
        rec.offset = offset;
        records.push_back(rec);
    }
    
    return records;
}

} // namespace DBRecovery::InnoDB::v2

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
        
        // 过滤无效 slot（空槽或超出堆顶）
        if (offset == 0 || offset >= header.heap_top) continue;
        
        Record rec;
        rec.offset = offset;
        records.push_back(rec);
    }
    
    return records;
}

} // namespace DBRecovery::InnoDB::v2

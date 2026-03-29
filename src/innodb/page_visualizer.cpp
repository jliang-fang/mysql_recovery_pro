#include "dbrecovery/innodb/page_visualizer.hpp"
#include "dbrecovery/innodb/btree.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace dbrecovery {
namespace innodb {

PageVisualizer::PageVisualizer(const std::vector<uint8_t>& page_data) {
    set_page(page_data);
}

PageVisualizer::~PageVisualizer() = default;

void PageVisualizer::set_page(const std::vector<uint8_t>& page_data) {
    page_data_ = page_data;
    parsed_ = false;

    // 尝试解析页
    BPlusTreeParser parser(page_data);
    parsed_ = parser.parse();

    if (parsed_) {
        page_type_ = parser.page_type();
        page_level_ = parser.btr_format(); // 注意：这里实际是 flags，需要从 parser 获取
        // 获取实际的页级别
        // page_level_ = parser.page_level(); // 需要添加此方法
        // 获取记录偏移
        // record_offsets_ = parser.record_offsets(); // 需要添加此方法
    }
}

std::string PageVisualizer::visualize_header() const {
    std::ostringstream oss;
    oss << "=== Page Header ===" << std::endl;

    if (page_data_.size() < 38) {
        oss << "Page too small for header" << std::endl;
        return oss.str();
    }

    // FIL Header
    uint32_t prev_page = read_le<uint32_t>(page_data_.data(), 0);
    uint32_t next_page = read_le<uint32_t>(page_data_.data(), 4);
    uint64_t lsn = read_le<uint64_t>(page_data_.data(), 8);
    uint32_t space_id = read_le<uint32_t>(page_data_.data(), 16);
    uint32_t page_no = read_le<uint32_t>(page_data_.data(), 20);
    uint16_t page_type = read_le<uint16_t>(page_data_.data() + 16, 0);

    oss << "FIL Header:" << std::endl;
    oss << "  - Previous Page: " << prev_page << std::endl;
    oss << "  - Next Page: " << next_page << std::endl;
    oss << "  - LSN: " << lsn << std::endl;
    oss << "  - Space ID: " << space_id << std::endl;
    oss << "  - Page No: " << page_no << std::endl;
    oss << "  - Type: " << page_type_to_string(page_type) << " (0x" 
        << std::hex << std::setw(4) << std::setfill('0') << page_type << std::dec << ")" << std::endl;

    // Page Header
    if (page_data_.size() >= 54) {
        uint16_t n_dir_slots = read_le<uint16_t>(page_data_.data(), 38);
        uint16_t heap_top = read_le<uint16_t>(page_data_.data(), 40);
        uint16_t n_heap = read_le<uint16_t>(page_data_.data(), 42) & 0x7FFF;
        uint16_t free_offset = read_le<uint16_t>(page_data_.data(), 44);

        oss << "Page Header:" << std::endl;
        oss << "  - Directory Slots: " << n_dir_slots << std::endl;
        oss << "  - Heap Top: " << heap_top << std::endl;
        oss << "  - Heap Size: " << n_heap << std::endl;
        oss << "  - Free Offset: " << free_offset << std::endl;
    }

    // BTR Header
    if (page_data_.size() >= 58) {
        uint16_t n_levels = read_le<uint16_t>(page_data_.data(), 42);
        uint16_t flags = read_le<uint16_t>(page_data_.data(), 44);

        oss << "BTR Header:" << std::endl;
        oss << "  - Levels: " << n_levels << std::endl;
        oss << "  - Flags: 0x" << std::hex << std::setw(4) << std::setfill('0') 
            << flags << std::dec << std::endl;
        
        // 解析行格式
        uint8_t row_format = flags & 0x07;
        oss << "  - Row Format: ";
        switch (row_format) {
            case 0: oss << "REDUNDANT"; break;
            case 1: oss << "COMPACT"; break;
            case 2: oss << "DYNAMIC"; break;
            case 3: oss << "COMPRESSED"; break;
            default: oss << "UNKNOWN(" << (int)row_format << ")"; break;
        }
        oss << std::endl;
    }

    return oss.str();
}

std::string PageVisualizer::visualize_records() const {
    std::ostringstream oss;
    oss << "=== Records ===" << std::endl;

    if (page_data_.size() < 54) {
        oss << "Page too small for records" << std::endl;
        return oss.str();
    }

    // 查找 INFIMUM 和 SUPREMUM
    uint16_t infimum_offset = 54;
    if (infimum_offset + 13 <= page_data_.size()) {
        uint8_t info_bits = page_data_[infimum_offset];
        uint16_t next_offset = read_le<uint16_t>(page_data_.data(), infimum_offset + 1) & 0x3FFF;

        oss << "INFIMUM:" << std::endl;
        oss << "  - Offset: " << infimum_offset << std::endl;
        oss << "  - Info Bits: 0x" << std::hex << std::setw(2) << std::setfill('0') 
            << static_cast<int>(info_bits) << std::dec << std::endl;
        oss << "  - Next Offset: " << next_offset << std::endl;
    }

    if (infimum_offset + 13 <= page_data_.size()) {
        uint16_t supremum_offset = read_le<uint16_t>(page_data_.data(), infimum_offset + 1) & 0x3FFF;
        if (supremum_offset + 5 <= page_data_.size()) {
            uint8_t info_bits = page_data_[supremum_offset];
            uint16_t next_offset = read_le<uint16_t>(page_data_.data(), supremum_offset + 1) & 0x3FFF;

            oss << "SUPREMUM:" << std::endl;
            oss << "  - Offset: " << supremum_offset << std::endl;
            oss << "  - Info Bits: 0x" << std::hex << std::setw(2) << std::setfill('0') 
                << static_cast<int>(info_bits) << std::dec << std::endl;
            oss << "  - Next Offset: " << next_offset << std::endl;
        }
    }

    return oss.str();
}

std::string PageVisualizer::visualize_directory() const {
    std::ostringstream oss;
    oss << "=== Directory Slots ===" << std::endl;

    if (page_data_.size() < 40) {
        oss << "Page too small for directory" << std::endl;
        return oss.str();
    }

    uint16_t n_dir_slots = read_le<uint16_t>(page_data_.data(), 38);
    oss << "Number of slots: " << n_dir_slots << std::endl;

    if (n_dir_slots > 0 && page_data_.size() >= 38 + n_dir_slots * 2) {
        oss << "Slots:" << std::endl;
        for (uint16_t i = 0; i < n_dir_slots; ++i) {
            size_t slot_offset = 38 + i * 2;
            uint16_t rec_offset = read_le<uint16_t>(page_data_.data(), slot_offset) & 0x7FFF;
            oss << "  [" << i << "] -> " << rec_offset << std::endl;
        }
    }

    return oss.str();
}

std::string PageVisualizer::visualize_btree() const {
    std::ostringstream oss;
    oss << "=== B+Tree Info ===" << std::endl;

    if (page_data_.size() < 58) {
        oss << "Page too small for B+Tree info" << std::endl;
        return oss.str();
    }

    uint16_t n_levels = read_le<uint16_t>(page_data_.data(), 42);
    uint16_t flags = read_le<uint16_t>(page_data_.data(), 44);

    oss << "Levels: " << n_levels << std::endl;
    oss << "Flags: 0x" << std::hex << std::setw(4) << std::setfill('0') << flags << std::dec << std::endl;

    // 节点类型
    uint8_t row_format = flags & 0x07;
    oss << "Row Format: ";
    switch (row_format) {
        case 0: oss << "REDUNDANT"; break;
        case 1: oss << "COMPACT"; break;
        case 2: oss << "DYNAMIC"; break;
        case 3: oss << "COMPRESSED"; break;
        default: oss << "UNKNOWN(" << (int)row_format << ")"; break;
    }
    oss << std::endl;

    oss << "Page Level: " << (int)n_levels << std::endl;
    oss << "Node Type: " << (n_levels == 0 ? "LEAF" : "INTERNAL") << std::endl;

    return oss.str();
}

std::string PageVisualizer::visualize_full() const {
    std::ostringstream oss;
    oss << "╔══════════════════════════════════════════════════════════════════════════════╗" << std::endl;
    oss << "║                         InnoDB Page Visualization                            ║" << std::endl;
    oss << "╚══════════════════════════════════════════════════════════════════════════════╝" << std::endl;
    oss << std::endl;

    oss << "Page Size: " << page_data_.size() << " bytes" << std::endl;
    oss << "Parsed: " << (parsed_ ? "Yes" : "No") << std::endl;
    oss << std::endl;

    oss << visualize_header();
    oss << std::endl;

    oss << visualize_directory();
    oss << std::endl;

    oss << visualize_records();
    oss << std::endl;

    oss << visualize_btree();
    oss << std::endl;

    return oss.str();
}

std::string PageVisualizer::hex_dump(size_t offset, size_t length) const {
    std::ostringstream oss;
    
    if (offset >= page_data_.size()) {
        return "Offset out of range";
    }

    length = std::min(length, page_data_.size() - offset);

    oss << "Hex dump (offset " << offset << " - " << (offset + length) << "):" << std::endl;
    oss << std::hex << std::setfill('0');

    for (size_t i = 0; i < length; i += 16) {
        oss << std::setw(8) << (offset + i) << "  ";
        
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < length) {
                oss << std::setw(2) << static_cast<int>(page_data_[offset + i + j]) << " ";
            } else {
                oss << "   ";
            }
        }
        
        oss << " |";
        for (size_t j = 0; j < 16 && i + j < length; ++j) {
            uint8_t c = page_data_[offset + i + j];
            oss << (c >= 32 && c < 127 ? static_cast<char>(c) : '.');
        }
        oss << "|" << std::endl;
    }

    oss << std::dec;
    return oss.str();
}

std::string PageVisualizer::visualize_health() const {
    std::ostringstream oss;
    oss << "=== Page Health ===" << std::endl;

    if (page_data_.size() < 38) {
        oss << "Status: INVALID - Page too small" << std::endl;
        return oss.str();
    }

    uint16_t page_type = read_le<uint16_t>(page_data_.data() + 16, 0);
    bool valid_type = (page_type == FIL_PAGE_INDEX || 
                       page_type == FIL_PAGE_SDI || 
                       page_type == FIL_PAGE_UNDO_LOG);

    oss << "Page Type: " << page_type_to_string(page_type) << std::endl;
    oss << "Type Valid: " << (valid_type ? "Yes" : "No") << std::endl;

    if (page_data_.size() >= 54) {
        uint16_t heap_top = read_le<uint16_t>(page_data_.data(), 40);
        uint16_t n_heap = read_le<uint16_t>(page_data_.data(), 42) & 0x7FFF;
        
        oss << "Heap Top: " << heap_top << std::endl;
        oss << "Heap Size: " << n_heap << std::endl;
        oss << "Records: " << (n_heap >= 2 ? n_heap - 2 : 0) << " (excluding system records)" << std::endl;
    }

    if (parsed_) {
        oss << "Parse Status: SUCCESS" << std::endl;
    } else {
        oss << "Parse Status: FAILED" << std::endl;
    }

    return oss.str();
}

} // namespace innodb
} // namespace dbrecovery

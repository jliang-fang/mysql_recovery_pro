#pragma once
#include "page_parser.hpp"
#include <vector>
#include <cstddef>

namespace DBRecovery::InnoDB::v2 {

struct Record {
    uint16_t offset;       // 相对页数据起始的偏移
    uint8_t  n_fields;     // 字段数量
    bool     is_deleted;   // 是否已删除
};

class RecordParser {
public:
    RecordParser(const PageParser& parser);
    
    std::vector<Record> parse() noexcept;
    
private:
    const PageParser& parser_;
};

} // namespace DBRecovery::InnoDB::v2

#ifndef DBRECOVERY_INNODB_PAGE_PARSER_HPP
#define DBRECOVERY_INNODB_PAGE_PARSER_HPP
#include <vector>
#include <cstdint>
#include <cstddef>

namespace DBRecovery {
namespace InnoDB {

struct Record {
    uint16_t offset;
};

class PageParser {
public:
    explicit PageParser(const uint8_t* buffer, size_t size);

    bool parse_header();
    
    std::vector<Record> parse_records() const;

    const uint8_t* data() const;
    size_t size() const;

private:
    uint16_t page_type_;
    uint16_t flags_;
    uint16_t page_level_;
    const uint8_t* buffer_;
    size_t size_;
};

} // namespace InnoDB
} // namespace DBRecovery

#endif // DBRECOVERY_INNODB_PAGE_PARSER_HPP

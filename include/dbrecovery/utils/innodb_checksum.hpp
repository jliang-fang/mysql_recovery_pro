#ifndef DBRECOVERY_UTILS_INNODB_CHECKSUM_HPP
#define DBRECOVERY_UTILS_INNODB_CHECKSUM_HPP
#include <cstdint>
#include <cstddef>
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/innodb_checksum.hpp"

namespace dbrecovery {
namespace utils {

class InnoDBChecksum {
public:
    static bool validate_crc32(const uint8_t* page, size_t page_size = 16384) {
        if (page == nullptr || page_size < 12) return false;
        return innodb::is_page_checksum_valid(page, page_size);
    }

    static uint32_t compute_crc32c(const uint8_t* data, size_t size) {
        return innodb::compute_crc32c_checksum(data, size);
    }

    static uint32_t compute_innodb(const uint8_t* data, size_t size) {
        return innodb::compute_innodb_checksum(data, size);
    }
};

} // namespace utils
} // namespace dbrecovery

#endif // DBRECOVERY_UTILS_INNODB_CHECKSUM_HPP

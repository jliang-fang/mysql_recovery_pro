#include "dbrecovery/utils/crc32c.hpp"

namespace dbrecovery {
namespace utils {

uint32_t compute_crc32c(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFF;

    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];

        for (int j = 0; j < 8; ++j) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0x82F63B78; // CRC32C 多项式（Castagnoli）
            } else {
                crc >>= 1;
            }
        }
    }

    return ~crc;
}

uint32_t compute_page_crc32c(const std::vector<uint8_t>& page_data) {
    uint32_t crc = 0xFFFFFFFF;

    for (size_t i = 0; i < page_data.size(); ++i) {
        if (i >= 24 && i < 28) continue; // Skip checksum field at offset 24-27

        uint8_t byte = page_data[i];
        crc ^= byte;

        for (int j = 0; j < 8; ++j) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0x82F63B78; // CRC32C 多项式（Castagnoli）
            } else {
                crc >>= 1;
            }
        }
    }

    return ~crc;
}

} // namespace utils
} // namespace dbrecovery

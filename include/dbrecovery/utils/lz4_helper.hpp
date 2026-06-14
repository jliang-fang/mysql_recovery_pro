#ifndef DBRECOVERY_UTILS_LZ4_HELPER_HPP
#define DBRECOVERY_UTILS_LZ4_HELPER_HPP

#include <vector>
#include <cstdint>
#include <cstring>
#include <lz4.h>

namespace dbrecovery {
namespace utils {

/**
 * @brief LZ4 decompression helper for InnoDB compressed pages
 *
 * InnoDB uses LZ4 compression for:
 * - Compressed row format pages (FIL_PAGE_COMPRESSED, 0x45B5)
 * - ZBLOB pages (FIL_PAGE_TYPE_ZBLOB, 0x45B5)
 *
 * Reference: ibd2sql/utils/lz4.py
 */
class LZ4Helper {
public:
    /// Maximum safe decompressed size (prevent memory exhaustion)
    static constexpr size_t MAX_DECOMPRESSED_SIZE = 16 * 1024 * 1024;  // 16MB per page

    /**
     * @brief Decompress LZ4 compressed data
     * @param src Compressed data pointer
     * @param src_len Compressed data length
     * @param expected_len Expected decompressed length (from page header)
     * @return Decompressed data vector, empty on failure
     */
    static std::vector<uint8_t> decompress(const uint8_t* src, size_t src_len,
                                           size_t expected_len = 0,
                                           size_t max_size = MAX_DECOMPRESSED_SIZE) {
        if (src_len == 0 || src == nullptr) {
            return std::vector<uint8_t>();
        }

        // Sanity check: compressed data should not be larger than decompressed
        if (expected_len > 0 && src_len > expected_len) {
            // Possibly corrupt data
            return std::vector<uint8_t>();
        }

        // Determine output buffer size
        size_t dest_size = expected_len > 0 ? expected_len : src_len * 4;

        // Cap at maximum size to prevent memory exhaustion attacks
        if (dest_size > max_size) {
            dest_size = max_size;
        }

        std::vector<uint8_t> dest(dest_size);

        // LZ4 decompression
        int decompressed_size = LZ4_decompress_safe(
            reinterpret_cast<const char*>(src),
            reinterpret_cast<char*>(dest.data()),
            static_cast<int>(src_len),
            static_cast<int>(dest_size)
        );

        if (decompressed_size < 0) {
            // Decompression failed
            return std::vector<uint8_t>();
        }

        dest.resize(static_cast<size_t>(decompressed_size));
        return dest;
    }

    /**
     * @brief Decompress LZ4 data to existing buffer
     * @param src Compressed data pointer
     * @param src_len Compressed data length
     * @param dest Destination buffer
     * @param dest_size Destination buffer size
     * @param actual_size Actual decompressed size (output)
     * @return true on success, false on failure
     */
    static bool decompress_to_buffer(const uint8_t* src, size_t src_len,
                                     uint8_t* dest, size_t dest_size,
                                     size_t& actual_size) {
        if (src == nullptr || dest == nullptr || src_len == 0) {
            return false;
        }

        int result = LZ4_decompress_safe(
            reinterpret_cast<const char*>(src),
            reinterpret_cast<const char*>(dest),
            static_cast<int>(src_len),
            static_cast<int>(dest_size)
        );

        if (result < 0) {
            return false;
        }

        actual_size = static_cast<size_t>(result);
        return true;
    }

    /**
     * @brief Get maximum compressed size for a given source size
     * @param src_size Source data size
     * @return Maximum compressed size bound
     */
    static size_t compress_bound(size_t src_size) {
        return static_cast<size_t>(LZ4_compressBound(static_cast<int>(src_size)));
    }

    /**
     * @brief Check if data appears to be LZ4 compressed
     * @param data Data pointer
     * @param size Data size
     * @return true if data might be LZ4 compressed
     */
    static bool looks_like_lz4(const uint8_t* data, size_t size) {
        if (size < 10) return false;

        // LZ4 compressed data typically starts with a token byte
        // Token format: [literal_length:4][match_length:4]
        uint8_t token = data[0];

        // Valid tokens have literal_length >= 0 and match_length >= 4
        uint8_t literal_len = token >> 4;
        uint8_t match_len = token & 0x0F;

        // Match length minimum is 4 (stored as match_len - 4)
        // So match_len in token is actual_match_len - 4
        // Valid match_len values: 0-15 (representing 4-19 bytes)
        return match_len <= 15;
    }
};

} // namespace utils
} // namespace dbrecovery

#endif // DBRECOVERY_UTILS_LZ4_HELPER_HPP
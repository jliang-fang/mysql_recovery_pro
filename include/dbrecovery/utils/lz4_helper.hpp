#ifndef DBRECOVERY_UTILS_LZ4_HELPER_HPP
#define DBRECOVERY_UTILS_LZ4_HELPER_HPP

#include <vector>
#include <cstdint>
#include <cstring>
#include <limits>
#include <lz4.h>

namespace dbrecovery {
namespace utils {

/**
 * @brief LZ4 decompression helper for InnoDB compressed pages
 *
 * InnoDB uses LZ4 compression for:
 * - Compressed row format pages (FIL_PAGE_COMPRESSED, 0x45B5)
 * - ZBLOB pages (FIL_PAGE_TYPE_ZBLOB, 0x45B5)
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

        if (max_size == 0 || max_size > MAX_DECOMPRESSED_SIZE) {
            max_size = MAX_DECOMPRESSED_SIZE;
        }

        // Sanity check: compressed data should not be larger than decompressed
        if (expected_len > 0 && (src_len > expected_len || expected_len > max_size)) {
            return std::vector<uint8_t>();
        }

        // Determine output buffer size without overflowing the fallback estimate.
        size_t dest_size = expected_len;
        if (dest_size == 0) {
            if (src_len > max_size / 4) return std::vector<uint8_t>();
            dest_size = src_len * 4;
        }

        if (dest_size == 0 || dest_size > max_size ||
            src_len > static_cast<size_t>(std::numeric_limits<int>::max()) ||
            dest_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
            return std::vector<uint8_t>();
        }

        std::vector<uint8_t> dest(dest_size);

        // LZ4 decompression
        int decompressed_size = LZ4_decompress_safe(
            reinterpret_cast<const char*>(src),
            reinterpret_cast<char*>(dest.data()),
            static_cast<int>(src_len),
            static_cast<int>(dest_size)
        );

        if (decompressed_size < 0 ||
            (expected_len > 0 && static_cast<size_t>(decompressed_size) != expected_len)) {
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

        if (src_len > static_cast<size_t>(std::numeric_limits<int>::max()) ||
            dest_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
            return false;
        }

        int result = LZ4_decompress_safe(
            reinterpret_cast<const char*>(src),
            reinterpret_cast<char*>(dest),
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
        if (src_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
            return 0;
        }
        const int bound = LZ4_compressBound(static_cast<int>(src_size));
        return bound > 0 ? static_cast<size_t>(bound) : 0;
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
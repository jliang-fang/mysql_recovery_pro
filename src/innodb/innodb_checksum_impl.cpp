/**
 * @file innodb_checksum_impl.cpp
 * @brief Canonical InnoDB page checksum implementation
 *
 * Implements the correct InnoDB dual-checksum scheme per MySQL source (buf0buf.cc).
 *
 * InnoDB page trailer layout (last 8 bytes of each page):
 *   [page_size - 8]: FIL_PAGE_END_CHECKSUM (4 bytes, LE) — the checksum value
 *   [page_size - 4]: Checksum algorithm ID (4 bytes, LE) — identifies which algo was used
 *     0xDBCDDAEB = CRC32C
 *     0x3BDC2B7E = innodb (legacy)
 *     0x00000000 = none
 *
 * Note: In MySQL source, the "end LSN old style checksum" is at page_size-8,
 * and the "new checksum" (with algorithm ID) is at page_size-4.
 * When validating, both fields must be zeroed before computing.
 */

#include "dbrecovery/innodb/innodb_checksum.hpp"
#include <cstring>
#include <algorithm>

// Hardware CRC32C support
#if defined(__SSE4_2__)
#include <nmmintrin.h>
#define HAS_CRC32C_HW 1
#elif defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#define HAS_CRC32C_HW 1
#endif

namespace dbrecovery {
namespace innodb {

// ==================== CRC32C Lookup Table (Software Fallback) ====================

static uint32_t crc32c_table[256];
static bool crc32c_table_initialized = false;

static void init_crc32c_table() {
    if (crc32c_table_initialized) return;
    // CRC32C polynomial (Castagnoli): 0x1EDC6F41
    // Reflected: 0x82F63B78
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0x82F63B78;
            } else {
                crc >>= 1;
            }
        }
        crc32c_table[i] = crc;
    }
    crc32c_table_initialized = true;
}

// ==================== CRC32C Implementation ====================

static uint32_t crc32c_sw(const uint8_t* data, size_t length) {
    init_crc32c_table();
    uint32_t crc = 0xFFFFFFFF;

    // Process 8 bytes at a time for speed
    while (length >= 8) {
        uint32_t w1 = static_cast<uint32_t>(data[0]) |
                      (static_cast<uint32_t>(data[1]) << 8) |
                      (static_cast<uint32_t>(data[2]) << 16) |
                      (static_cast<uint32_t>(data[3]) << 24);
        uint32_t w2 = static_cast<uint32_t>(data[4]) |
                      (static_cast<uint32_t>(data[5]) << 8) |
                      (static_cast<uint32_t>(data[6]) << 16) |
                      (static_cast<uint32_t>(data[7]) << 24);
        crc ^= w1;
        crc = crc32c_table[(crc >> 0) & 0xFF] ^ (crc >> 8);
        crc = crc32c_table[(crc >> 0) & 0xFF] ^ (crc >> 8);
        crc = crc32c_table[(crc >> 0) & 0xFF] ^ (crc >> 8);
        crc = crc32c_table[(crc >> 0) & 0xFF] ^ (crc >> 8);
        crc ^= w2;
        crc = crc32c_table[(crc >> 0) & 0xFF] ^ (crc >> 8);
        crc = crc32c_table[(crc >> 0) & 0xFF] ^ (crc >> 8);
        crc = crc32c_table[(crc >> 0) & 0xFF] ^ (crc >> 8);
        crc = crc32c_table[(crc >> 0) & 0xFF] ^ (crc >> 8);
        data += 8;
        length -= 8;
    }

    // Remaining bytes
    while (length > 0) {
        crc = crc32c_table[(crc ^ *data) & 0xFF] ^ (crc >> 8);
        data++;
        length--;
    }

    return ~crc;
}

#if HAS_CRC32C_HW && defined(__SSE4_2__)
static uint32_t crc32c_hw_sse42(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;

    // Process 8 bytes at a time
    while (length >= 8) {
        uint64_t val;
        std::memcpy(&val, data, 8);
        crc = static_cast<uint32_t>(_mm_crc32_u64(crc, val));
        data += 8;
        length -= 8;
    }

    // Process 4 bytes
    while (length >= 4) {
        uint32_t val;
        std::memcpy(&val, data, 4);
        crc = _mm_crc32_u32(crc, val);
        data += 4;
        length -= 4;
    }

    // Process remaining bytes
    while (length > 0) {
        crc = _mm_crc32_u8(crc, *data);
        data++;
        length--;
    }

    return ~crc;
}
#endif

// ==================== InnoDB Legacy Checksum ====================
// From MySQL source: buf/buf0buf.cc buf_calc_page_old_checksum()
// This is the "innodb" checksum algorithm: rotating-add with 4-byte words

static uint32_t innodb_checksum_impl(const uint8_t* data, size_t size) {
    uint32_t sum = 0;

    // Process 4 bytes at a time using the rotating-add algorithm
    // This matches buf_calc_page_old_checksum() in MySQL source
    size_t i = 0;
    while (i + 4 <= size) {
        uint32_t word = static_cast<uint32_t>(data[i]) |
                        (static_cast<uint32_t>(data[i + 1]) << 8) |
                        (static_cast<uint32_t>(data[i + 2]) << 16) |
                        (static_cast<uint32_t>(data[i + 3]) << 24);
        // Rotate sum right by 1 bit and add word
        sum = (sum >> 1) + ((sum & 1) << 31) + word;
        i += 4;
    }

    // Handle remaining bytes
    uint32_t partial = 0;
    size_t shift = 0;
    while (i < size) {
        partial |= static_cast<uint32_t>(data[i]) << shift;
        shift += 8;
        i++;
    }
    if (shift > 0) {
        sum = (sum >> 1) + ((sum & 1) << 31) + partial;
    }

    return sum;
}

// ==================== Public API Implementation ====================

ChecksumAlgo detect_checksum_algorithm(const uint8_t* page, size_t page_size) {
    if (!page || page_size < 8) {
        return ChecksumAlgo::NONE;
    }

    // MySQL 5.7+ stores algorithm ID at page_size-4 as a 4-byte LE magic number
    // MySQL 5.6 does NOT store an algorithm ID; instead, page_size-4 contains
    // the low 4 bytes of the LSN. We detect the version by checking the magic.
    uint32_t algo_id = read_le<uint32_t>(page, page_size - 4);

    if (algo_id == CHECKSUM_CRC32C_MAGIC) {
        return ChecksumAlgo::CRC32C;
    } else if (algo_id == CHECKSUM_INNODB_MAGIC) {
        return ChecksumAlgo::INNODB;
    } else if (algo_id == CHECKSUM_NONE_MAGIC) {
        return ChecksumAlgo::NONE;
    }

    // No recognized algorithm ID — this is likely a MySQL 5.6 page where
    // page_size-4 is part of the LSN, not an algorithm identifier.
    // For MySQL 5.6, the checksum is always the legacy "innodb" checksum.
    return ChecksumAlgo::INNODB;
}

uint32_t read_stored_checksum(const uint8_t* page, size_t page_size) {
    if (!page || page_size < 8) {
        return 0;
    }
    return read_le<uint32_t>(page, page_size - 8);
}

uint32_t read_stored_algorithm_id(const uint8_t* page, size_t page_size) {
    if (!page || page_size < 4) {
        return 0;
    }
    return read_le<uint32_t>(page, page_size - 4);
}

uint32_t compute_crc32c_checksum(const uint8_t* data, size_t size) {
    if (!data || size == 0) {
        return 0;
    }

#if HAS_CRC32C_HW && defined(__SSE4_2__)
    return crc32c_hw_sse42(data, size);
#else
    return crc32c_sw(data, size);
#endif
}

uint32_t compute_innodb_checksum(const uint8_t* data, size_t size) {
    if (!data || size == 0) {
        return 0;
    }
    return innodb_checksum_impl(data, size);
}

/**
 * @brief Zero the checksum fields in a page copy
 *
 * Zeros bytes at [page_size - 8 .. page_size - 1] so that
 * the checksum computation excludes the stored values.
 */
static void zero_checksum_fields(uint8_t* page_copy, size_t page_size) {
    if (page_size < 8) return;
    std::memset(page_copy + page_size - 8, 0, 8);
}

ChecksumValidationResult validate_page_checksum(
    const uint8_t* page, size_t page_size, bool strict) {

    if (!page || page_size < 8) {
        return ChecksumValidationResult::invalid(0, 0, 0, ChecksumAlgo::NONE);
    }

    // Step 1: Read stored values from the ORIGINAL page
    uint32_t stored_checksum = read_stored_checksum(page, page_size);
    uint32_t stored_algo_id = read_stored_algorithm_id(page, page_size);

    // Step 2: Detect algorithm from stored ID
    ChecksumAlgo detected = detect_checksum_algorithm(page, page_size);

    // Step 3: Handle NONE checksum (always valid)
    if (detected == ChecksumAlgo::NONE && stored_algo_id == CHECKSUM_NONE_MAGIC) {
        return ChecksumValidationResult::valid(
            stored_checksum, stored_algo_id, 0, ChecksumAlgo::NONE);
    }

    // Step 4: For MySQL 5.6 pages (no algorithm ID), use the simpler validation:
    // The header checksum (bytes 0-3) should match the trailer checksum (bytes page_size-8 to page_size-4)
    // Both are computed with the legacy innodb algorithm over specific regions.
    // However, the simplest and most reliable check is: compute both CRC32C and innodb
    // checksums over the full page (with checksum fields zeroed) and check against
    // the stored header checksum (bytes 0-3 read as big-endian).

    // Read header checksum (big-endian, like all FIL header fields)
    uint32_t header_checksum = read_be<uint32_t>(page, 0);
    uint32_t trailer_checksum = read_le<uint32_t>(page, page_size - 8);

    // Step 5: Create a copy and zero checksum fields
    uint8_t page_copy[65536]; // Max page size
    if (page_size > sizeof(page_copy)) {
        std::vector<uint8_t> heap_copy(page, page + page_size);
        // Zero both checksum fields
        std::memset(heap_copy.data(), 0, 4); // header checksum
        std::memset(heap_copy.data() + page_size - 8, 0, 8); // trailer

        // Try CRC32C first (MySQL 5.7+)
        if (detected == ChecksumAlgo::CRC32C) {
            uint32_t c1 = compute_crc32c_checksum(heap_copy.data() + 4, 26 - 4);
            uint32_t c2 = compute_crc32c_checksum(heap_copy.data() + 38, page_size - 38 - 8);
            uint32_t crc = c1 ^ c2;
            if (crc == header_checksum || crc == trailer_checksum) {
                return ChecksumValidationResult::valid(
                    stored_checksum, stored_algo_id, crc, ChecksumAlgo::CRC32C);
            }
        }

        // Try innodb legacy checksum
        {
            bool is_mysql56 = (stored_algo_id != CHECKSUM_CRC32C_MAGIC &&
                               stored_algo_id != CHECKSUM_INNODB_MAGIC &&
                               stored_algo_id != CHECKSUM_NONE_MAGIC);

            if (is_mysql56) {
                // MySQL 5.6: full-page computation
                uint32_t old_checksum = compute_innodb_checksum(heap_copy.data() + 4, page_size - 4 - 8);
                if (old_checksum == header_checksum || old_checksum == trailer_checksum) {
                    return ChecksumValidationResult::valid(
                        stored_checksum, stored_algo_id, old_checksum, ChecksumAlgo::INNODB);
                }
                uint32_t new_checksum = compute_crc32c_checksum(heap_copy.data() + 4, page_size - 4 - 8);
                if (new_checksum == trailer_checksum) {
                    return ChecksumValidationResult::valid(
                        stored_checksum, stored_algo_id, new_checksum, ChecksumAlgo::CRC32C);
                }
            } else {
                // MySQL 5.7+: segmented computation
                uint32_t c1 = compute_innodb_checksum(heap_copy.data() + 4, 26 - 4);
                uint32_t c2 = compute_innodb_checksum(heap_copy.data() + 38, page_size - 38 - 8);
                uint32_t innodb_sum = c1 + c2;
                if (innodb_sum == header_checksum || innodb_sum == trailer_checksum) {
                    return ChecksumValidationResult::valid(
                        stored_checksum, stored_algo_id, innodb_sum, ChecksumAlgo::INNODB);
                }
            }
        }

        return ChecksumValidationResult::invalid(
            stored_checksum, stored_algo_id, 0, detected);
    }

    // Standard path: use stack buffer
    std::memcpy(page_copy, page, page_size);
    // Zero header checksum (bytes 0-3) and trailer (bytes page_size-8 to page_size-1)
    std::memset(page_copy, 0, 4);
    std::memset(page_copy + page_size - 8, 0, 8);

    // MySQL 5.7+ with CRC32C: c1=crc32c(data[4:26]) XOR c2=crc32c(data[38:page_size-8])
    if (detected == ChecksumAlgo::CRC32C) {
        uint32_t c1 = compute_crc32c_checksum(page_copy + 4, 26 - 4);
        uint32_t c2 = compute_crc32c_checksum(page_copy + 38, page_size - 38 - 8);
        uint32_t crc = c1 ^ c2;
        if (crc == header_checksum || crc == trailer_checksum) {
            return ChecksumValidationResult::valid(
                stored_checksum, stored_algo_id, crc, ChecksumAlgo::CRC32C);
        }
    }

    // Legacy innodb checksum (MySQL 5.6 default)
    // MySQL 5.6 uses full-page computation (buf_calc_page_old_checksum),
    // NOT the segmented c1+c2 approach used by MySQL 5.7+.
    // For MySQL 5.6 pages (no algorithm ID at page_size-4):
    //   - page[0..3] = old_checksum (big-endian) = rotating-add over entire page (skip bytes 0-3 and page_size-8..page_size-1)
    //   - page[page_size-8..page_size-5] = new_checksum (little-endian) = CRC32C over same range
    {
        // Check if this looks like a MySQL 5.6 page (no algorithm ID magic)
        bool is_mysql56 = (stored_algo_id != CHECKSUM_CRC32C_MAGIC &&
                           stored_algo_id != CHECKSUM_INNODB_MAGIC &&
                           stored_algo_id != CHECKSUM_NONE_MAGIC);

        if (is_mysql56) {
            // MySQL 5.6: compute old-style checksum over entire page (skip header checksum and trailer)
            // buf_calc_page_old_checksum: rotating-add over page[4..page_size-8] (skip 0-3 and last 8 bytes)
            uint32_t old_checksum = compute_innodb_checksum(page_copy + 4, page_size - 4 - 8);
            if (old_checksum == header_checksum) {
                return ChecksumValidationResult::valid(
                    stored_checksum, stored_algo_id, old_checksum, ChecksumAlgo::INNODB);
            }

            // Also try CRC32C for the trailer checksum (MySQL 5.6 stores new_checksum at page_size-8)
            uint32_t new_checksum = compute_crc32c_checksum(page_copy + 4, page_size - 4 - 8);
            if (new_checksum == trailer_checksum) {
                return ChecksumValidationResult::valid(
                    stored_checksum, stored_algo_id, new_checksum, ChecksumAlgo::CRC32C);
            }

            // Try matching trailer with old_checksum (some MySQL 5.6 configs)
            if (old_checksum == trailer_checksum) {
                return ChecksumValidationResult::valid(
                    stored_checksum, stored_algo_id, old_checksum, ChecksumAlgo::INNODB);
            }
        } else {
            // MySQL 5.7+: segmented computation (c1 + c2)
            uint32_t c1 = compute_innodb_checksum(page_copy + 4, 26 - 4);
            uint32_t c2 = compute_innodb_checksum(page_copy + 38, page_size - 38 - 8);
            uint32_t innodb_sum = c1 + c2;
            if (innodb_sum == header_checksum || innodb_sum == trailer_checksum) {
                return ChecksumValidationResult::valid(
                    stored_checksum, stored_algo_id, innodb_sum, ChecksumAlgo::INNODB);
            }
        }
    }

    // If strict mode, we're done
    if (strict) {
        return ChecksumValidationResult::invalid(
            stored_checksum, stored_algo_id, 0, detected);
    }

    // Fallback: try full-page checksum (some implementations compute over full page)
    uint32_t full_crc = compute_crc32c_checksum(page_copy, page_size);
    if (full_crc == header_checksum || full_crc == trailer_checksum) {
        return ChecksumValidationResult::valid(
            stored_checksum, stored_algo_id, full_crc, ChecksumAlgo::CRC32C);
    }

    uint32_t full_innodb = compute_innodb_checksum(page_copy, page_size);
    if (full_innodb == header_checksum || full_innodb == trailer_checksum) {
        return ChecksumValidationResult::valid(
            stored_checksum, stored_algo_id, full_innodb, ChecksumAlgo::INNODB);
    }

    return ChecksumValidationResult::invalid(
        stored_checksum, stored_algo_id, 0, detected);
}

bool is_page_checksum_valid(const uint8_t* page, size_t page_size) {
    if (!page || page_size < 8) {
        return false;
    }
    return validate_page_checksum(page, page_size, false).is_valid;
}

void write_page_checksum(uint8_t* page, size_t page_size, ChecksumAlgo algo) {
    if (!page || page_size < 8) {
        return;
    }

    // Zero the checksum fields first
    std::memset(page + page_size - 8, 0, 8);

    // Compute the checksum over the same FIL regions used by validation.
    // The stored checksum fields and the unused FIL checksum header are excluded.
    uint32_t checksum = 0;
    uint32_t magic = 0;

    if (page_size >= 46) {
        const size_t first_offset = 4;
        const size_t first_size = 26 - first_offset;
        const size_t second_offset = 38;
        const size_t second_size = page_size - 38 - 8;

        switch (algo) {
            case ChecksumAlgo::CRC32C:
            case ChecksumAlgo::STRICT_CRC32C:
                checksum = compute_crc32c_checksum(page + first_offset, first_size) ^
                           compute_crc32c_checksum(page + second_offset, second_size);
                magic = CHECKSUM_CRC32C_MAGIC;
                break;
            case ChecksumAlgo::INNODB:
            case ChecksumAlgo::STRICT_INNODB:
                checksum = compute_innodb_checksum(page + first_offset, first_size) +
                           compute_innodb_checksum(page + second_offset, second_size);
                magic = CHECKSUM_INNODB_MAGIC;
                break;
            case ChecksumAlgo::NONE:
            default:
                checksum = 0;
                magic = CHECKSUM_NONE_MAGIC;
                break;
        }
    } else {
        switch (algo) {
            case ChecksumAlgo::CRC32C:
            case ChecksumAlgo::STRICT_CRC32C:
                checksum = compute_crc32c_checksum(page, page_size);
                magic = CHECKSUM_CRC32C_MAGIC;
                break;
            case ChecksumAlgo::INNODB:
            case ChecksumAlgo::STRICT_INNODB:
                checksum = compute_innodb_checksum(page, page_size);
                magic = CHECKSUM_INNODB_MAGIC;
                break;
            case ChecksumAlgo::NONE:
            default:
                checksum = 0;
                magic = CHECKSUM_NONE_MAGIC;
                break;
        }
    }

    // Write checksum value at page_size - 8
    write_le<uint32_t>(page, page_size - 8, checksum);

    // Write algorithm ID at page_size - 4
    write_le<uint32_t>(page, page_size - 4, magic);
}

} // namespace innodb
} // namespace dbrecovery

/**
 * @file innodb_checksum.hpp
 * @brief Canonical InnoDB page checksum implementation
 *
 * Implements the correct InnoDB dual-checksum scheme as documented in
 * MySQL source code (buf0buf.cc):
 *
 * InnoDB stores TWO values at the end of each page:
 *   page[page_size - 8 .. page_size - 5] = checksum value (4 bytes LE)
 *   page[page_size - 4 .. page_size - 1] = algorithm ID (4 bytes LE)
 *     0xDBCDDAEB = CRC32C
 *     0x3BDC2B7E = innodb (legacy rotating-add)
 *     0x00000000 = none/off
 *
 * Validation flow:
 *   1. Read algorithm ID from page_size - 4
 *   2. Save stored checksum, zero both fields in a temp copy
 *   3. Compute checksum over the entire page (including zeroed region)
 *   4. Compare computed with stored checksum value
 */

#ifndef DBRECOVERY_INNODB_INNODB_CHECKSUM_HPP
#define DBRECOVERY_INNODB_INNODB_CHECKSUM_HPP

#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <optional>
#include "dbrecovery/innodb/mach_read.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== Checksum Magic Constants ====================
// From MySQL 8.0 source: include/buf0checksum.h

/// @brief CRC32C checksum algorithm magic number
constexpr uint32_t CHECKSUM_CRC32C_MAGIC = 0xDBCDDAEB;

/// @brief InnoDB legacy checksum algorithm magic number
constexpr uint32_t CHECKSUM_INNODB_MAGIC = 0x3BDC2B7E;

/// @brief No checksum magic number
constexpr uint32_t CHECKSUM_NONE_MAGIC = 0x00000000;

// ==================== Checksum Algorithm Enum ====================

enum class ChecksumAlgo : uint8_t {
    NONE = 0,       ///< No checksum (innodb_checksum_algorithm=none)
    INNODB = 1,     ///< Legacy InnoDB rotating-add checksum
    CRC32C = 2,     ///< CRC32C (Castagnoli) checksum (MySQL 5.6+ default)
    STRICT_INNODB = 3,  ///< Strict InnoDB (reject if not innodb)
    STRICT_CRC32C = 4   ///< Strict CRC32C (reject if not crc32c)
};

// ==================== Checksum Validation Result ====================

struct ChecksumValidationResult {
    bool is_valid;
    uint32_t stored_checksum;       ///< Checksum stored in page (at page_size-8)
    uint32_t stored_algorithm_id;   ///< Algorithm ID stored in page (at page_size-4)
    uint32_t computed_checksum;     ///< Checksum we computed
    ChecksumAlgo detected_algorithm; ///< Algorithm we detected
    std::string algorithm_name;     ///< Human-readable algorithm name

    static ChecksumValidationResult valid(
        uint32_t stored, uint32_t algo_id, uint32_t computed, ChecksumAlgo algo) {
        return {true, stored, algo_id, computed, algo, algo_to_string(algo)};
    }

    static ChecksumValidationResult invalid(
        uint32_t stored, uint32_t algo_id, uint32_t computed, ChecksumAlgo algo) {
        return {false, stored, algo_id, computed, algo, algo_to_string(algo)};
    }

    static std::string algo_to_string(ChecksumAlgo algo) {
        switch (algo) {
            case ChecksumAlgo::NONE: return "NONE";
            case ChecksumAlgo::INNODB: return "INNODB";
            case ChecksumAlgo::CRC32C: return "CRC32C";
            case ChecksumAlgo::STRICT_INNODB: return "STRICT_INNODB";
            case ChecksumAlgo::STRICT_CRC32C: return "STRICT_CRC32C";
            default: return "UNKNOWN";
        }
    }
};

// ==================== Core Checksum Functions ====================

/**
 * @brief Detect checksum algorithm from page data
 *
 * Reads the algorithm identifier from page[page_size - 4].
 *
 * @param page Pointer to page data (must be at least page_size bytes)
 * @param page_size Size of the page (must be a valid InnoDB page size)
 * @return Detected checksum algorithm
 */
ChecksumAlgo detect_checksum_algorithm(const uint8_t* page, size_t page_size);

/**
 * @brief Read stored checksum value from page
 *
 * Reads the checksum value from page[page_size - 8].
 *
 * @param page Pointer to page data
 * @param page_size Size of the page
 * @return Stored checksum value
 */
uint32_t read_stored_checksum(const uint8_t* page, size_t page_size);

/**
 * @brief Read stored algorithm ID from page
 *
 * Reads the algorithm identifier from page[page_size - 4].
 *
 * @param page Pointer to page data
 * @param page_size Size of the page
 * @return Stored algorithm ID
 */
uint32_t read_stored_algorithm_id(const uint8_t* page, size_t page_size);

/**
 * @brief Compute CRC32C checksum over page data
 *
 * IMPORTANT: The caller MUST zero the checksum fields (page_size-8 through page_size-1)
 * in the data before calling this function, or the result will not match InnoDB's
 * stored checksum.
 *
 * Uses hardware-accelerated CRC32C on x86-64 (SSE4.2) if available.
 *
 * @param data Page data (with checksum fields already zeroed)
 * @param size Page size
 * @return CRC32C checksum value
 */
uint32_t compute_crc32c_checksum(const uint8_t* data, size_t size);

/**
 * @brief Compute InnoDB legacy checksum (rotating-add)
 *
 * This is the "old" InnoDB checksum algorithm from buf_calc_page_old_checksum().
 * It processes the page 4 bytes at a time using a rotating-add algorithm.
 *
 * IMPORTANT: The caller MUST zero the checksum fields before calling.
 *
 * @param data Page data (with checksum fields already zeroed)
 * @param size Page size
 * @return InnoDB legacy checksum value
 */
uint32_t compute_innodb_checksum(const uint8_t* data, size_t size);

/**
 * @brief Validate page checksum using InnoDB's dual-checksum scheme
 *
 * This is the primary checksum validation function. It:
 *   1. Reads the algorithm ID from page[page_size - 4]
 *   2. Reads the stored checksum from page[page_size - 8]
 *   3. Copies the page and zeros the checksum fields
 *   4. Computes the checksum using the detected algorithm
 *   5. Compares computed with stored
 *   6. Falls back to trying other algorithms if strict mode is not required
 *
 * @param page Pointer to page data (must be at least page_size bytes)
 * @param page_size Size of the page (4096, 8192, 16384, 32768, or 65536)
 * @param strict If true, only accept the algorithm indicated by the stored ID.
 *               If false (default), also try CRC32C and INNODB as fallbacks.
 * @return Validation result with detailed information
 */
ChecksumValidationResult validate_page_checksum(
    const uint8_t* page, size_t page_size, bool strict = false);

/**
 * @brief Quick checksum validity check
 *
 * Simplified version that returns true/false without detailed results.
 *
 * @param page Pointer to page data
 * @param page_size Size of the page
 * @return true if checksum is valid (or checksums are disabled)
 */
bool is_page_checksum_valid(const uint8_t* page, size_t page_size);

/**
 * @brief Write checksum to page
 *
 * Computes and writes both the checksum value and algorithm ID to the page.
 *
 * @param page Page data (must be at least page_size bytes, writable)
 * @param page_size Size of the page
 * @param algo Algorithm to use for computing
 */
void write_page_checksum(uint8_t* page, size_t page_size, ChecksumAlgo algo = ChecksumAlgo::CRC32C);

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_INNODB_CHECKSUM_HPP

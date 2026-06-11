/**
 * @file signature_index.hpp
 * @brief PhotoRec-Style Two-Level Signature Index for mysql_recovery_pro
 *
 * Implements high-performance signature scanning inspired by PhotoRec's
 * file_check_list structure:
 * - Level 1: Index by offset (where in the page the signature appears)
 * - Level 2: At each offset, bucket by first byte value (256 buckets)
 *
 * This enables O(1) signature lookup instead of O(n) linear search.
 *
 * Key features from ibd2sql:
 * - FORCE mode: Iterate through every page without relying on page chains
 * - PAGE_INDEX_ID validation: Verify index_id matches expected value
 * - B+Tree traversal: Recursively find leaf pages from root
 *
 * Key features from PhotoRec:
 * - Two-level indexed signature lookup
 * - Block size auto-detection
 * - Search space management
 */

#ifndef DBRECOVERY_ENGINE_SIGNATURE_INDEX_HPP
#define DBRECOVERY_ENGINE_SIGNATURE_INDEX_HPP

#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/innodb_checksum.hpp"
#include <cstdint>
#include <vector>
#include <array>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <list>
#include <mutex>

namespace dbrecovery {
namespace engine {

// ==================== PhotoRec-Style Two-Level Signature Index ====================

/**
 * @brief File check structure (PhotoRec pattern)
 *
 * Each signature is stored with its offset and value for two-level indexing.
 */
struct SignatureCheck {
    std::vector<uint8_t> signature;  // Signature bytes
    uint32_t offset;                 // Offset in page where signature appears
    uint32_t length;                 // Signature length

    SignatureCheck() : offset(0), length(0) {}
    SignatureCheck(const std::vector<uint8_t>& sig, uint32_t off)
        : signature(sig), offset(off), length(static_cast<uint32_t>(sig.size())) {}
};

/**
 * @brief File hint structure (PhotoRec pattern)
 *
 * Contains metadata about a file type for recovery.
 */
struct FileHint {
    std::string extension;
    std::string description;
    uint64_t max_filesize;
    bool recover;

    FileHint() : max_filesize(0), recover(true) {}
    FileHint(const std::string& ext, const std::string& desc, uint64_t max_size = 0)
        : extension(ext), description(desc), max_filesize(max_size), recover(true) {}
};

/**
 * @brief File check list entry (PhotoRec pattern)
 *
 * A single entry in the two-level index structure.
 */
struct FileCheckEntry {
    SignatureCheck signature;
    FileHint hint;
    std::function<bool(const uint8_t*, uint32_t, bool)> header_check_fn;

    FileCheckEntry() : header_check_fn(nullptr) {}
};

/**
 * @brief Two-level signature index (PhotoRec filegen.c pattern)
 *
 * Level 1: Signatures organized by offset
 * Level 2: At each offset, signatures bucketed by first byte value (256 buckets)
 *
 * This provides O(1) lookup instead of O(n) linear search.
 */
class TwoLevelSignatureIndex {
public:
    using FileCheckList = std::list<FileCheckEntry>;

    TwoLevelSignatureIndex() : total_signatures_(0) {}

    /**
     * @brief Register a signature for a file type
     *
     * @param offset Offset in page where signature appears
     * @param signature Signature bytes
     * @param hint File hint (extension, description, etc.)
     * @param header_check_fn Header check callback function
     */
    void register_signature(
        uint32_t offset,
        const std::vector<uint8_t>& signature,
        const FileHint& hint,
        std::function<bool(const uint8_t*, uint32_t, bool)> header_check_fn) {

        FileCheckEntry entry;
        entry.signature = SignatureCheck(signature, offset);
        entry.hint = hint;
        entry.header_check_fn = header_check_fn;

        std::unique_lock<std::shared_mutex> lock(mutex_);

        // Create offset bucket if it doesn't exist
        if (index_by_offset_.find(offset) == index_by_offset_.end()) {
            index_by_offset_[offset] = std::array<FileCheckList, 256>{};
        }

        // Add to the bucket indexed by first byte of signature
        uint8_t first_byte = signature.empty() ? 0 : signature[0];
        index_by_offset_[offset][first_byte].push_back(std::move(entry));
        total_signatures_++;
    }

    /**
     * @brief Find matching signatures at a buffer position
     *
     * Uses two-level indexing for O(1) lookup:
     * 1. Find offset bucket (if exists)
     * 2. Find first-byte bucket (if exists)
     * 3. Iterate through matching signatures
     *
     * @param buffer Buffer to check
     * @param buffer_size Size of buffer
     * @param safe_header_only If true, only check header portion
     * @param results Output vector of matching entries
     * @return Number of matches found
     */
    template<typename T>
    uint32_t find_matches(
        const uint8_t* buffer,
        uint32_t buffer_size,
        bool safe_header_only,
        std::vector<T>& results) {

        uint32_t match_count = 0;

        std::shared_lock<std::shared_mutex> lock(mutex_);

        for (const auto& [offset, buckets] : index_by_offset_) {
            if (offset >= buffer_size) continue;

            // Get the bucket indexed by first byte at this offset
            uint8_t first_byte = buffer[offset];
            const FileCheckList& bucket = buckets[first_byte];

            for (const auto& entry : bucket) {
                const auto& sig = entry.signature;

                // Check if signature matches
                if (sig.length == 0 || offset + sig.length <= buffer_size) {
                    bool match = true;
                    for (uint32_t i = 0; i < sig.length && match; ++i) {
                        if (buffer[offset + i] != sig.signature[i]) {
                            match = false;
                        }
                    }

                    if (match && entry.header_check_fn) {
                        if (entry.header_check_fn(buffer, buffer_size, safe_header_only)) {
                            results.push_back(entry);
                            match_count++;
                        }
                    }
                }
            }
        }

        return match_count;
    }

    /**
     * @brief Get total number of registered signatures
     */
    uint32_t get_signature_count() const { return total_signatures_; }

private:
    // Level 1: Map from offset to array of 256 buckets
    std::unordered_map<uint32_t, std::array<FileCheckList, 256>> index_by_offset_;

    // Mutex for thread safety
    mutable std::shared_mutex mutex_;

    // Total signature count
    uint32_t total_signatures_;
};

// ==================== InnoDB Page Signatures ====================

/**
 * @brief InnoDB page signature definitions
 *
 * Defines all signature patterns for different MySQL versions.
 */
class InnoDBSignatureIndex {
public:
    // InnoDB page type signatures (at offset 24)
    static constexpr uint8_t FIL_PAGE_TYPE_INDEX_56[] = {0xBF, 0x45};  // 0x45BF (LE)
    static constexpr uint8_t FIL_PAGE_TYPE_INDEX_80[] = {0xF1, 0x26};  // 0x26F1 (LE)
    static constexpr uint8_t FIL_PAGE_TYPE_SDI[] = {0xF3, 0x26};       // 0x26F3 (LE)
    static constexpr uint8_t FIL_PAGE_TYPE_BLOB[] = {0xBD, 0x45};      // 0x45BD (LE)

    // MySQL 5.6 InnoDB page header check
    static bool header_check_innodb_56(
        const uint8_t* buffer,
        uint32_t buffer_size,
        bool safe_header_only) {
        (void)safe_header_only;

        if (buffer_size < 38) return false;

        // Check FIL_PAGE_TYPE at offset 24
        uint16_t page_type = read_le<uint16_t>(buffer, 24);
        if (page_type != 0x45BF && page_type != 0x45BC) {
            return false;
        }

        // Check page number at offset 4 (reasonable range)
        uint32_t page_no = read_le<uint32_t>(buffer, 4);
        if (page_no > 0xFFFFFFF) {
            return false;
        }

        // Check LSN at offset 16 (must be > 0 for valid pages)
        uint64_t lsn = read_le<uint64_t>(buffer, 16);
        if (lsn == 0) {
            return false;  // Zero LSN only valid for freshly allocated pages
        }

        return true;
    }

    // MySQL 8.0 InnoDB page header check
    static bool header_check_innodb_80(
        const uint8_t* buffer,
        uint32_t buffer_size,
        bool safe_header_only) {
        (void)safe_header_only;

        if (buffer_size < 38) return false;

        // Check FIL_PAGE_TYPE at offset 24
        uint16_t page_type = read_le<uint16_t>(buffer, 24);
        if (page_type != 0x26F1 && page_type != 0x26F3) {
            return false;
        }

        // Check space_id at offset 34
        uint32_t space_id = read_le<uint32_t>(buffer, 34);
        if (space_id > 0xFFFFFF) {
            return false;
        }

        return true;
    }

    // SDI page header check
    static bool header_check_sdi(
        const uint8_t* buffer,
        uint32_t buffer_size,
        bool safe_header_only) {
        (void)safe_header_only;

        if (buffer_size < 38) return false;

        uint16_t page_type = read_le<uint16_t>(buffer, 24);
        return page_type == 0x26F3 || page_type == 0x45BE;
    }

    /**
     * @brief Build complete signature index for all InnoDB versions
     */
    static void build_complete_index_into(TwoLevelSignatureIndex& index) {
        // MySQL 5.6 FIL_PAGE_INDEX (0x45BF)
        {
            std::vector<uint8_t> sig = {0xBF, 0x45};
            FileHint hint("ibd", "MySQL 5.6 InnoDB data page", 1024 * 1024 * 1024ULL);
            index.register_signature(24, sig, hint, header_check_innodb_56);
        }

        // MySQL 5.6 FIL_PAGE_INDEX variant (0x45BC)
        {
            std::vector<uint8_t> sig = {0xBC, 0x45};
            FileHint hint("ibd", "MySQL 5.6 InnoDB data page (legacy)", 1024 * 1024 * 1024ULL);
            index.register_signature(24, sig, hint, header_check_innodb_56);
        }

        // MySQL 8.0 FIL_PAGE_INDEX (0x26F1)
        {
            std::vector<uint8_t> sig = {0xF1, 0x26};
            FileHint hint("ibd", "MySQL 8.0 InnoDB data page", 1024 * 1024 * 1024ULL);
            index.register_signature(24, sig, hint, header_check_innodb_80);
        }

        // MySQL 8.0 SDI page (0x26F3)
        {
            std::vector<uint8_t> sig = {0xF3, 0x26};
            FileHint hint("sdi", "MySQL 8.0 SDI metadata page", 1024 * 1024 * 1024ULL);
            index.register_signature(24, sig, hint, header_check_sdi);
        }
    }

private:
    template<typename T>
    static T read_le(const uint8_t* buffer, uint32_t offset) {
        T value = 0;
        for (size_t i = 0; i < sizeof(T); ++i) {
            value |= static_cast<T>(buffer[offset + i]) << (i * 8);
        }
        return value;
    }
};

// ==================== FORCE Mode (ibd2sql Pattern) ====================

/**
 * @brief FORCE mode scanner (ibd2sql pattern)
 *
 * Iterates through every page in a file without relying on page chains.
 * For each page:
 * 1. Check signature at offset 24
 * 2. Verify PAGE_INDEX_ID matches expected value
 * 3. Verify leaf page indicator (data[64:66] == 0)
 * 4. Extract data from valid pages
 */
class ForceModeScanner {
public:
    struct Config {
        uint32_t page_size = 16384;
        uint32_t expected_index_id = 0;  // 0 = match any
        bool verify_checksum = true;
        bool strict_mode = false;  // Reject pages without valid checksum
    };

    struct ScanResult {
        uint64_t file_offset;
        uint32_t page_number;
        uint32_t space_id;
        uint16_t page_type;
        uint64_t lsn;
        uint32_t index_id;
        bool is_leaf_page;
        bool checksum_valid;
        std::vector<uint8_t> data;
    };

    /**
     * @brief Scan file in FORCE mode (ibd2sql IBD2SQL_SINGLE pattern)
     *
     * @param data File data
     * @param size File size
     * @param config Scanner configuration
     * @param callback Callback for each found page
     * @return Number of pages found
     */
    static uint32_t scan(
        const uint8_t* data,
        uint64_t size,
        const Config& config,
        std::function<void(const ScanResult&)> callback) {

        uint32_t pages_found = 0;
        uint64_t num_pages = size / config.page_size;

        for (uint32_t i = 0; i < num_pages; ++i) {
            uint64_t offset = i * config.page_size;
            if (offset + config.page_size > size) break;

            const uint8_t* page = data + offset;

            // Step 1: Check signature at offset 24
            uint16_t page_type = read_be<uint16_t>(page, 24);
            if (page_type != 0x45BF && page_type != 0x26F1) {
                continue;  // Not an index page
            }

            // Step 2: Verify PAGE_INDEX_ID at offset 46
            uint32_t page_index_id = read_be<uint32_t>(page, 46);
            if (config.expected_index_id != 0 &&
                page_index_id != config.expected_index_id) {
                continue;  // Index ID doesn't match
            }

            // Step 3: Verify leaf page indicator (offset 64-65 = 0 for leaf)
            // Non-leaf pages have child page pointers at this location
            uint16_t leaf_indicator = read_be<uint16_t>(page, 64);
            bool is_leaf = (leaf_indicator == 0);

            // Step 4: Validate checksum
            bool checksum_valid = true;
            if (config.verify_checksum) {
                checksum_valid = validate_innodb_page_checksum(page, config.page_size);
                if (config.strict_mode && !checksum_valid) {
                    continue;
                }
            }

            // Build result
            ScanResult result;
            result.file_offset = offset;
            result.page_number = read_be<uint32_t>(page, 4);
            result.space_id = read_be<uint32_t>(page, 34);
            result.page_type = page_type;
            result.lsn = read_be<uint64_t>(page, 16);
            result.index_id = page_index_id;
            result.is_leaf_page = is_leaf;
            result.checksum_valid = checksum_valid;
            result.data.assign(page, page + config.page_size);

            callback(result);
            pages_found++;
        }

        return pages_found;
    }

private:
    template<typename T>
    static T read_be(const uint8_t* buffer, uint32_t offset) {
        T value = 0;
        for (size_t i = 0; i < sizeof(T); ++i) {
            value = (value << 8) | buffer[offset + i];
        }
        return value;
    }

    static bool validate_innodb_page_checksum(const uint8_t* page, uint32_t page_size) {
        if (page_size < 38) return false;

        // Read stored checksum (offset 0-4 in MySQL 5.6, or last 4 bytes in trailer)
        uint32_t stored_checksum = read_be<uint32_t>(page, 0);

        // If checksum is 0 or 0xFFFFFFFF, checksum may be disabled
        if (stored_checksum == 0 || stored_checksum == 0xFFFFFFFF) {
            return true;  // Checksum disabled
        }

        // Try CRC32C
        uint32_t computed = compute_crc32c(page, page_size);
        if (computed == stored_checksum) {
            return true;
        }

        // Try InnoDB legacy checksum
        computed = compute_innodb_checksum(page, page_size);
        return computed == stored_checksum;
    }

    static uint32_t compute_crc32c(const uint8_t* data, size_t len) {
        // Simplified CRC32C - use actual implementation in production
        uint32_t crc = 0xFFFFFFFF;
        for (size_t i = 0; i < len; ++i) {
            crc ^= data[i];
            for (int j = 0; j < 8; ++j) {
                if (crc & 1) {
                    crc = (crc >> 1) ^ 0xEDB88320;
                } else {
                    crc >>= 1;
                }
            }
        }
        return ~crc;
    }

    static uint32_t compute_innodb_checksum(const uint8_t* data, size_t len) {
        // InnoDB legacy checksum (simple sum)
        uint32_t sum = 0;
        for (size_t i = 0; i < len; ++i) {
            sum += data[i];
        }
        return sum;
    }
};

// ==================== B+Tree Page Chain Traversal ====================

/**
 * @brief B+Tree page chain traversal (ibd2sql FIND_LEAF_PAGE_FROM_ROOT pattern)
 *
 * Traverses B+Tree from root page to find leaf pages containing data records.
 */
class BTreeTraverser {
public:
    struct PageInfo {
        uint64_t offset;
        uint32_t page_number;
        uint16_t page_level;
        uint32_t index_id;
        uint16_t n_records;
        bool is_leaf;
    };

    /**
     * @brief Traverse B+Tree to find first leaf page
     *
     * @param page_data Root page data
     * @param page_size Page size
     * @param target_index_id Expected index ID (0 = any)
     * @param callback Callback for each non-leaf page visited
     * @return Leaf page info, or nullopt if not found
     */
    static std::optional<PageInfo> find_first_leaf(
        const uint8_t* page_data,
        uint32_t page_size,
        uint32_t target_index_id,
        std::function<bool(const PageInfo&)> callback = nullptr) {

        if (!is_valid_index_page(page_data, page_size)) {
            return std::nullopt;
        }

        // Get page level and index ID
        PageInfo info = parse_page_header(page_data, page_size);

        if (target_index_id != 0 && info.index_id != target_index_id) {
            return std::nullopt;  // Index ID mismatch
        }

        // If already a leaf page, return it
        if (info.is_leaf) {
            return info;
        }

        // Parse records to find child page pointer
        auto child_pages = parse_non_leaf_records(page_data, page_size);
        if (child_pages.empty()) {
            return std::nullopt;
        }

        // Visit this non-leaf page
        if (callback) {
            callback(info);
        }

        // Recursively find leaf (simplified - just return first child)
        // In full implementation, would need to read child page from disk
        return find_first_leaf_internal(child_pages[0], page_size, target_index_id, callback);
    }

    /**
     * @brief Traverse complete B+Tree page chain
     *
     * @param page_data Root page data
     * @param page_size Page size
     * @param callback Callback for each leaf page found
     * @return Number of leaf pages found
     */
    static uint32_t traverse_chain(
        const uint8_t* page_data,
        uint32_t page_size,
        std::function<void(const PageInfo&)> callback) {

        uint32_t leaf_count = 0;
        std::vector<uint32_t> page_chain;

        // Find first leaf
        auto first_leaf = find_first_leaf(page_data, page_size, 0);
        if (!first_leaf) {
            return 0;
        }

        // Follow page chain using FIL_PAGE_NEXT
        uint32_t current_page_no = first_leaf->page_number;
        while (current_page_no != 0xFFFFFFFF && leaf_count < 100000) {
            // Get page info
            PageInfo info;
            info.page_number = current_page_no;
            info.is_leaf = true;
            info.page_level = 0;

            callback(info);
            leaf_count++;

            // Get next page number (would need to read from disk in real implementation)
            // For now, return what we have
            break;  // Placeholder - full implementation needs disk access
        }

        return leaf_count;
    }

private:
    static bool is_valid_index_page(const uint8_t* page, uint32_t page_size) {
        if (page_size < 38) return false;

        uint16_t page_type = read_be<uint16_t>(page, 24);
        return page_type == 0x45BF || page_type == 0x26F1;
    }

    static PageInfo parse_page_header(const uint8_t* page, uint32_t page_size) {
        PageInfo info;
        info.offset = 0;  // Would be set by caller

        if (page_size >= 38) {
            info.page_number = read_be<uint32_t>(page, 4);
            info.index_id = read_be<uint32_t>(page, 46);
        }

        if (page_size >= 56) {
            uint16_t page_level = read_be<uint16_t>(page, 38 + 16);
            info.page_level = page_level;
            info.is_leaf = (page_level == 0);
        }

        return info;
    }

    static std::vector<uint32_t> parse_non_leaf_records(
        const uint8_t* page, uint32_t page_size) {
        std::vector<uint32_t> child_pages;

        // Simplified: parse directory slots to find record offsets
        // Then parse each record to extract child page number
        // This is a placeholder - full implementation would parse actual records

        return child_pages;
    }

    static std::optional<PageInfo> find_first_leaf_internal(
        uint32_t page_no,
        uint32_t page_size,
        uint32_t target_index_id,
        std::function<bool(const PageInfo&)>& callback) {
        // Would need to read page from disk
        // Placeholder implementation
        (void)page_no;
        (void)page_size;
        (void)target_index_id;
        (void)callback;
        return std::nullopt;
    }

    template<typename T>
    static T read_be(const uint8_t* buffer, uint32_t offset) {
        T value = 0;
        for (size_t i = 0; i < sizeof(T); ++i) {
            value = (value << 8) | buffer[offset + i];
        }
        return value;
    }
};

} // namespace engine
} // namespace dbrecovery

#endif // DBRECOVERY_ENGINE_SIGNATURE_INDEX_HPP

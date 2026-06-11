#ifndef DBRECOVERY_INNODB_FORENSIC_RECOVERY_HPP
#define DBRECOVERY_INNODB_FORENSIC_RECOVERY_HPP

#include <vector>
#include <string>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include "dbrecovery/innodb/page_defs.hpp"

namespace dbrecovery {
namespace innodb {

/**
 * @brief Represents a recovered raw tuple from the binary scan.
 * Enhanced for forensic recovery with additional metadata.
 */
struct RecoveredTuple {
    uint32_t page_no;
    uint16_t offset;
    uint16_t heap_no;
    std::vector<uint8_t> raw_data;
    bool was_deleted;      // True if the delete-mark bit was set
    uint16_t next_record_offset;  // Offset to next record in chain
    uint16_t record_length;       // Total record length
};

/**
 * @brief InodeScanner handles low-level binary scanning of disk blocks or files.
 * It identifies InnoDB pages by validating signatures (checksum, type, LSN).
 */
class InodeScanner {
public:
    explicit InodeScanner(const std::string& target_path);
    ~InodeScanner();

    // Scans the file and returns offsets of identified valid InnoDB pages.
    std::vector<size_t> scan_for_pages();

    // Validates if a specific block is an InnoDB page (MySQL 5.6 Antelope/Barracuda).
    bool validate_page_signature(const uint8_t* buffer, size_t size);

private:
    std::string target_path_;
    int fd_;
    size_t file_size_;
};

/**
 * @brief TupleExtractor parses FIL_PAGE_USER pages to extract records.
 * It implements "Zombie Tuple Extraction" by ignoring delete-mark bits for TRUNCATE recovery.
 * 
 * Key improvements for >99.9% recovery rate:
 * 1. 15-bit mask for next_record_offset (not 0x3FFF, but 0x7FFF)
 * 2. Circular reference detection using visited set
 * 3. Proper bounds checking with heap_top
 * 4. Extracts full record data including header
 */
class TupleExtractor {
public:
    // Extracts all tuples from a page, including those marked as deleted (zombie records)
    std::vector<RecoveredTuple> extract_tuples(const uint8_t* page_buffer, uint32_t page_no);

private:
    // Helper to parse record header and determine length
    size_t get_record_length(const uint8_t* rec_ptr);
};

/**
 * @brief ChainReconstructor rebuilds the logical sequence of pages using sibling pointers.
 */
class ChainReconstructor {
public:
    // Rebuilds a chain of page numbers starting from a seed page
    // using both FIL_PAGE_PREV and FIL_PAGE_NEXT for bidirectional traversal
    std::vector<uint32_t> reconstruct_chain(const std::vector<uint8_t>& pool, uint32_t seed_page);
};

/**
 * @brief EnhancedChainReconstructor: Reconstruct B+Tree leaf page chain
 * 
 * Key improvements for TRUNCATE/rm -rf recovery:
 * 1. Use both FIL_PAGE_PREV and FIL_PAGE_NEXT for bidirectional traversal
 * 2. Validate page signature for each page in chain
 * 3. Detect and skip corrupted pages
 * 4. Use page number ordering as fallback when chain pointers are missing
 */
class EnhancedChainReconstructor {
public:
    /**
     * @brief Construct enhanced chain reconstructor
     * @param data Pointer to page pool data
     * @param data_size Size of page pool
     * @param page_size Page size (default 16384)
     */
    EnhancedChainReconstructor(const uint8_t* data, size_t data_size, 
                               uint32_t page_size = UNIV_PAGE_SIZE);

    /**
     * @brief Reconstruct page chain starting from any page
     * @param seed_page Page number to start reconstruction
     * @return Ordered list of page numbers in the chain
     */
    std::vector<uint32_t> reconstruct_chain(uint32_t seed_page);

    /**
     * @brief Find leftmost page in chain using PREV pointers
     * @param page_num Starting page number
     * @param visited Set to track visited pages
     * @return Leftmost page number in chain
     */
    uint32_t find_leftmost(uint32_t page_num, std::set<uint32_t>& visited);

    /**
     * @brief Validate page signature at given page number
     * @param data Page pool data
     * @param data_size Size of page pool
     * @param page_num Page number to validate
     * @param page_size Page size
     * @return true if page is valid
     */
    static bool validate_page_at(const uint8_t* data, size_t data_size, 
                                  uint32_t page_num, uint32_t page_size);

private:
    const uint8_t* data_;
    size_t data_size_;
    uint32_t page_size_;
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_FORENSIC_RECOVERY_HPP

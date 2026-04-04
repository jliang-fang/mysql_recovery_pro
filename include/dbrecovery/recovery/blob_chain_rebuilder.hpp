#ifndef DBRECOVERY_RECOVERY_BLOB_CHAIN_REBUILDER_HPP
#define DBRECOVERY_RECOVERY_BLOB_CHAIN_REBUILDER_HPP

#include <cstdint>
#include <vector>
#include <map>
#include <optional>
#include <string>
#include <memory>

namespace dbrecovery {
namespace recovery {

/**
 * @brief BLOB page structure
 */
struct BlobPage {
    uint32_t space_id;
    uint32_t page_no;
    uint32_t offset;
    std::vector<uint8_t> data;
    uint32_t next_page_no;
    uint32_t total_length;
    uint32_t current_length;
    bool is_first_page;
    bool is_last_page;
};

// Constants for BLOB page parsing
constexpr size_t BLOB_PAGE_HEADER_SIZE = 38;  // FIL header
constexpr size_t BLOB_PAGE_DATA_OFFSET = 38;  // Data starts after FIL header

/**
 * @brief BLOB chain structure
 */
struct BlobChain {
    uint64_t blob_id;
    std::vector<BlobPage> pages;
    uint32_t total_length;
    bool is_complete;
    std::string error_message;
};

/**
 * @brief BLOB Pointer structure
 */
struct BlobPointer {
    uint32_t space_id;
    uint32_t page_no;
    uint32_t offset;
    uint32_t len;           // Current page length
    uint32_t total_len;     // Total BLOB length
    std::vector<uint32_t> chain_pages;  // Chain of pages
};

/**
 * @brief BLOB Chain Rebuilder - Reconstructs BLOB data from fragmented pages
 */
class BlobChainRebuilder {
public:
    BlobChainRebuilder();
    ~BlobChainRebuilder();

    /**
     * @brief Set page size for parsing
     */
    void set_page_size(uint32_t page_size);

    /**
     * @brief Add a BLOB page to the chain
     */
    bool add_blob_page(uint32_t space_id, uint32_t page_no,
                       std::vector<uint8_t> page_data);

    /**
     * @brief Parse BLOB pointer from record data
     */
    std::optional<BlobPointer> parse_blob_pointer(const uint8_t* data, size_t len);

    /**
     * @brief Reconstruct BLOB from pointer
     */
    std::optional<std::vector<uint8_t>> reconstruct_from_pointer(
        const BlobPointer& pointer);

    /**
     * @brief Reconstruct BLOB from file
     */
    std::optional<std::vector<uint8_t>> reconstruct_from_file(
        const std::string& file_path, const BlobPointer& pointer);

    /**
     * @brief Build BLOB chain from pages
     */
    std::optional<BlobChain> build_chain(uint64_t blob_id);

    /**
     * @brief Detect circular references in chain
     */
    bool has_circular_reference(const std::vector<uint32_t>& page_chain);

    /**
     * @brief Get all reconstructed BLOBs
     */
    const std::map<uint64_t, std::vector<uint8_t>>& get_reconstructed_blobs() const;

    /**
     * @brief Get chain statistics
     */
    struct ChainStats {
        uint32_t total_chains;
        uint32_t complete_chains;
        uint32_t partial_chains;
        uint32_t failed_chains;
        uint32_t total_pages;
        uint64_t total_bytes;
    };
    ChainStats get_stats() const;

    /**
     * @brief Clear all data
     */
    void clear();

private:
    uint32_t page_size_;
    std::map<uint32_t, std::map<uint32_t, BlobPage>> pages_;  // space_id -> page_no -> BlobPage
    std::map<uint64_t, std::vector<uint8_t>> reconstructed_blobs_;
    ChainStats stats_;

    /**
     * @brief Parse BLOB page header
     */
    bool parse_blob_page_header(const uint8_t* data, size_t size,
                                uint32_t page_no, BlobPage& page);

    /**
     * @brief Extract BLOB data from page
     */
    std::vector<uint8_t> extract_blob_data(const BlobPage& page);

    /**
     * @brief Follow BLOB chain
     */
    bool follow_chain(uint32_t first_page_no, uint32_t space_id,
                      uint32_t expected_total_len, std::vector<BlobPage>& chain);
};

/**
 * @brief BLOB Page Parser - Parses individual BLOB pages
 */
class BlobPageParser {
public:
    BlobPageParser();
    ~BlobPageParser();

    /**
     * @brief Parse BLOB page
     */
    std::optional<BlobPage> parse_page(uint32_t space_id, uint32_t page_no,
                                        const uint8_t* data, size_t size);

    /**
     * @brief Get next page in chain
     */
    std::optional<uint32_t> get_next_page(const uint8_t* data, size_t size);

    /**
     * @brief Get BLOB data length in page
     */
    uint32_t get_data_length(const uint8_t* data, size_t size);

    /**
     * @brief Extract BLOB data
     */
    std::vector<uint8_t> extract_data(const uint8_t* data, size_t size);

private:
    static constexpr size_t BLOB_PAGE_HEADER_SIZE = 38;  // FIL header
    static constexpr size_t BLOB_PAGE_DATA_OFFSET = 38;  // Data starts after FIL header
};

/**
 * @brief BLOB Recovery Engine - High-level BLOB recovery interface
 */
class BlobRecoveryEngine {
public:
    BlobRecoveryEngine();
    ~BlobRecoveryEngine();

    /**
     * @brief Set page size
     */
    void set_page_size(uint32_t page_size);

    /**
     * @brief Scan file for BLOB pages
     */
    bool scan_file(const std::string& file_path);

    /**
     * @brief Scan directory for BLOB pages
     */
    bool scan_directory(const std::string& dir_path);

    /**
     * @brief Reconstruct all BLOBs
     */
    bool reconstruct_all();

    /**
     * @brief Get reconstructed BLOBs
     */
    const std::map<uint64_t, std::vector<uint8_t>>& get_blobs() const;

    /**
     * @brief Export BLOB to file
     */
    bool export_blob(uint64_t blob_id, const std::string& output_path);

    /**
     * @brief Get statistics
     */
    struct ChainStats {
        uint32_t total_chains = 0;
        uint32_t complete_chains = 0;
        uint32_t partial_chains = 0;
        uint32_t failed_chains = 0;
        uint32_t total_pages = 0;
        uint64_t total_bytes = 0;
    };
    ChainStats get_stats() const;

    /**
     * @brief Clear all data
     */
    void clear();

private:
    uint32_t page_size_;
    BlobChainRebuilder chain_rebuilder_;
    std::map<uint64_t, BlobPointer> blob_pointers_;
};

} // namespace recovery
} // namespace dbrecovery

#endif // DBRECOVERY_RECOVERY_BLOB_CHAIN_REBUILDER_HPP

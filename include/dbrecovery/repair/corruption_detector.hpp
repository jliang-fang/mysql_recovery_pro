#ifndef DBRECOVERY_REPAIR_CORRUPTION_DETECTOR_HPP
#define DBRECOVERY_REPAIR_CORRUPTION_DETECTOR_HPP

#include <cstdint>
#include <vector>
#include <string>
#include <optional>
#include <map>

namespace dbrecovery {
namespace repair {

/**
 * @brief Corruption type enumeration
 */
enum class CorruptionType {
    NONE = 0,
    PAGE_HEADER_CORRUPTED,
    PAGE_TYPE_INVALID,
    PAGE_CHECKSUM_MISMATCH,
    PAGE_CHAIN_BROKEN,
    PAGE_OVERFLOW_CORRUPTED,
    RECORD_HEADER_CORRUPTED,
    RECORD_CHAIN_BROKEN,
    BLOB_CHAIN_BROKEN,
    SDI_CORRUPTED,
    UNKNOWN
};

/**
 * @brief Corruption details structure
 */
struct CorruptionDetails {
    CorruptionType type;
    uint32_t page_no;
    std::string description;
    std::string severity;  // "critical", "warning", "info"
    std::optional<uint64_t> expected_value;
    std::optional<uint64_t> actual_value;
};

/**
 * @brief Page health status structure
 */
struct PageHealth {
    bool is_valid;
    std::string status_message;
    uint32_t page_no;
    uint16_t page_type;
    uint32_t checksum_expected;
    uint32_t checksum_actual;
    bool checksum_match;
    uint32_t next_page;
    uint32_t prev_page;
    uint16_t n_records;
    uint16_t n_dir_slots;
    uint16_t heap_top;
    uint16_t free_offset;
    bool has_infimum;
    bool has_supremum;
    uint32_t record_count;
};

/**
 * @brief Corruption Detector - Detects and reports various types of corruption
 */
class CorruptionDetector {
public:
    CorruptionDetector();
    ~CorruptionDetector();

    /**
     * @brief Set page size for validation
     */
    void set_page_size(uint32_t page_size);

    /**
     * @brief Analyze a single page for corruption
     */
    PageHealth analyze_page(const uint8_t* page_data, size_t page_size, uint32_t page_no);

    /**
     * @brief Detect corruption in a page
     */
    std::vector<CorruptionDetails> detect_corruption(const uint8_t* page_data,
                                                      size_t page_size,
                                                      uint32_t page_no);

    /**
     * @brief Check page header integrity
     */
    bool check_page_header(const uint8_t* page_data, size_t page_size, uint32_t page_no);

    /**
     * @brief Check page checksum
     */
    bool check_page_checksum(const uint8_t* page_data, size_t page_size);

    /**
     * @brief Check page chain consistency
     */
    bool check_page_chain(uint32_t current_page_no,
                          uint32_t next_page_no,
                          uint32_t prev_page_no);

    /**
     * @brief Check record chain integrity
     */
    bool check_record_chain(const uint8_t* page_data,
                            size_t page_size,
                            const std::vector<uint16_t>& record_offsets);

    /**
     * @brief Get all detected corruptions
     */
    const std::vector<CorruptionDetails>& get_corruptions() const;

    /**
     * @brief Get corruption statistics
     */
    std::map<CorruptionType, uint32_t> get_corruption_stats() const;

    /**
     * @brief Clear corruption list
     */
    void clear_corruptions();

    /**
     * @brief Get corruption severity count
     */
    uint32_t get_critical_count() const;
    uint32_t get_warning_count() const;
    uint32_t get_info_count() const;

private:
    uint32_t page_size_;
    std::vector<CorruptionDetails> corruptions_;
    std::map<CorruptionType, uint32_t> corruption_stats_;

    /**
     * @brief Add corruption detail
     */
    void add_corruption(const CorruptionDetails& detail);

    /**
     * @brief Check if page type is valid
     */
    bool is_valid_page_type(uint16_t page_type) const;

    /**
     * @brief Calculate expected next page
     */
    uint32_t expected_next_page(uint32_t current_page_no) const;
};

/**
 * @brief Page Chain Repairer - Repairs broken page chains
 */
class PageChainRepairer {
public:
    PageChainRepairer();
    ~PageChainRepairer();

    /**
     * @brief Add a page to the chain
     */
    void add_page(uint32_t page_no, std::vector<uint8_t> page_data);

    /**
     * @brief Detect and repair broken page chains
     */
    bool repair_chains();

    /**
     * @brief Get repaired pages
     */
    const std::map<uint32_t, std::vector<uint8_t>>& get_repaired_pages() const;

    /**
     * @brief Get chain statistics
     */
    struct ChainStats {
        uint32_t total_pages;
        uint32_t valid_chains;
        uint32_t repaired_chains;
        uint32_t orphan_pages;
    };
    ChainStats get_stats() const;

private:
    std::map<uint32_t, std::vector<uint8_t>> pages_;
    std::map<uint32_t, std::vector<uint8_t>> repaired_pages_;
    ChainStats stats_;

    /**
     * @brief Detect circular references
     */
    bool has_circular_reference(uint32_t start_page,
                                const std::map<uint32_t, uint32_t>& next_map);

    /**
     * @brief Find chain start
     */
    uint32_t find_chain_start(uint32_t page_no,
                               const std::map<uint32_t, uint32_t>& prev_map);
};

/**
 * @brief Record Chain Repairer - Repairs broken record chains
 */
class RecordChainRepairer {
public:
    RecordChainRepairer();
    ~RecordChainRepairer();

    /**
     * @brief Analyze record chain
     */
    bool analyze_chain(const uint8_t* page_data, size_t page_size);

    /**
     * @brief Detect broken records
     */
    std::vector<uint16_t> detect_broken_records(const uint8_t* page_data,
                                                 size_t page_size);

    /**
     * @brief Repair record chain
     */
    bool repair_chain(uint8_t* page_data, size_t page_size);

    /**
     * @brief Get repair statistics
     */
    struct RepairStats {
        uint32_t total_records;
        uint32_t valid_records;
        uint32_t repaired_records;
        uint32_t orphan_records;
    };
    RepairStats get_stats() const;

private:
    RepairStats stats_;

    /**
     * @brief Read next record offset
     */
    std::optional<uint16_t> read_next_offset(const uint8_t* page_data,
                                              size_t page_size,
                                              uint16_t offset);
};

} // namespace repair
} // namespace dbrecovery

#endif // DBRECOVERY_REPAIR_CORRUPTION_DETECTOR_HPP

#ifndef DBRECOVERY_INNODB_UNDO_LOG_PARSER_HPP
#define DBRECOVERY_INNODB_UNDO_LOG_PARSER_HPP

#include <cstdint>
#include <vector>
#include <map>
#include <optional>
#include <string>
#include <memory>

namespace dbrecovery {
namespace innodb {

/**
 * @brief Undo log record structure
 */
struct UndoLogRecord {
    uint64_t u_trx_id;        // Transaction ID
    uint64_t u_roll_ptr;      // Rollback pointer
    uint16_t u_type;          // Record type
    uint16_t u_data_len;      // Data length
    std::vector<uint8_t> u_data; // Record data
    uint32_t u_page_no;       // Page number
    uint32_t u_offset;        // Offset in page

    std::string to_string() const {
        return "UndoLogRecord{trx_id=" + std::to_string(u_trx_id) +
               ", roll_ptr=" + std::to_string(u_roll_ptr) +
               ", type=" + std::to_string(u_type) +
               ", data_len=" + std::to_string(u_data_len) + "}";
    }
};

/**
 * @brief Undo log page structure
 */
struct UndoLogPage {
    uint32_t page_no;
    uint16_t page_type;
    uint64_t trx_id;
    uint64_t roll_ptr;
    std::vector<UndoLogRecord> records;
    uint32_t free_space;
    uint32_t used_space;
    bool is_first_page;
    bool is_last_page;
};

/**
 * @brief Undo log chain structure
 */
struct UndoLogChain {
    uint64_t trx_id;
    std::vector<UndoLogPage> pages;
    uint32_t total_records;
    bool is_complete;
    std::string error_message;
};

/**
 * @brief Transaction rollback information
 */
struct RollbackInfo {
    uint64_t trx_id;
    uint64_t start_lsn;
    uint64_t end_lsn;
    std::vector<UndoLogRecord> undo_records;
    bool is_committed;
    bool is_rolled_back;
    uint32_t total_records;
};

/**
 * @brief Undo Log Parser with transaction rollback support
 */
class UndoLogParser {
public:
    enum class ParseMode {
        PHYSICAL_FLASHBACK,      // Physical flashback mode
        TRANSACTION_ROLLBACK     // Transaction rollback mode
    };

    UndoLogParser();
    ~UndoLogParser();

    /**
     * @brief Set page size
     */
    void set_page_size(uint32_t page_size);

    /**
     * @brief Set parse mode
     */
    void set_mode(ParseMode mode);

    /**
     * @brief Parse undo log page
     */
    std::optional<UndoLogPage> parse_page(const uint8_t* data, size_t size, uint32_t page_no);

    /**
     * @brief Parse undo log file
     */
    std::vector<UndoLogPage> parse_file(const std::string& path);

    /**
     * @brief Extract old versions from undo log (for flashback)
     */
    std::vector<UndoLogRecord> extract_old_versions(
        const std::vector<UndoLogPage>& pages,
        uint64_t table_id);

    /**
     * @brief Build transaction rollback info
     */
    std::optional<RollbackInfo> build_rollback_info(uint64_t trx_id,
                                                     const std::vector<UndoLogPage>& pages);

    /**
     * @brief Get all parsed undo log pages
     */
    const std::vector<UndoLogPage>& get_pages() const;

    /**
     * @brief Get undo log chain by transaction ID
     */
    std::optional<UndoLogChain> get_chain(uint64_t trx_id);

    /**
     * @brief Get statistics
     */
    struct Stats {
        uint32_t total_pages;
        uint32_t total_records;
        uint32_t total_transactions;
        uint32_t complete_chains;
        uint32_t partial_chains;
    };
    Stats get_stats() const;

    /**
     * @brief Clear all data
     */
    void clear();

    /**
     * @brief Parse undo log record (public for testing)
     */
    std::optional<UndoLogRecord> parse_record(const uint8_t* data, size_t size, size_t& pos);

private:
    uint32_t page_size_;
    ParseMode mode_;
    std::vector<UndoLogPage> pages_;
    std::map<uint64_t, std::vector<UndoLogPage>> chains_;
    Stats stats_;

    /**
     * @brief Parse undo log record (private implementation)
     */
    std::optional<UndoLogRecord> parse_record_impl(const uint8_t* data, size_t size, size_t& pos);

    /**
     * @brief Parse undo log page header
     */
    bool parse_page_header(const uint8_t* data, size_t size, UndoLogPage& page);

    /**
     * @brief Follow undo log chain
     */
    bool follow_chain(uint32_t first_page_no, uint64_t trx_id,
                      std::vector<UndoLogPage>& chain);
};

/**
 * @brief Undo Log Replayer - Replays undo log for flashback
 */
class UndoLogReplayer {
public:
    UndoLogReplayer();
    ~UndoLogReplayer();

    /**
     * @brief Set page size
     */
    void set_page_size(uint32_t page_size);

    /**
     * @brief Add undo log page
     */
    bool add_page(uint32_t page_no, std::vector<uint8_t> page_data);

    /**
     * @brief Replay undo log for flashback
     */
    std::vector<std::map<std::string, std::string>> replay(
        const std::string& table_name,
        const std::vector<UndoLogRecord>& undo_records);

    /**
     * @brief Get replay statistics
     */
    struct ReplayStats {
        uint32_t total_records;
        uint32_t replayed_records;
        uint32_t failed_records;
    };
    ReplayStats get_stats() const;

private:
    uint32_t page_size_;
    std::vector<UndoLogPage> pages_;
    ReplayStats stats_;
};

/**
 * @brief Undo Log Chain Builder - Builds undo log chains
 */
class UndoLogChainBuilder {
public:
    UndoLogChainBuilder();
    ~UndoLogChainBuilder();

    /**
     * @brief Add undo log page
     */
    bool add_page(uint32_t page_no, uint64_t trx_id, std::vector<uint8_t> page_data);

    /**
     * @brief Build undo log chains
     */
    bool build_chains();

    /**
     * @brief Get all chains
     */
    const std::map<uint64_t, std::vector<UndoLogPage>>& get_chains() const;

    /**
     * @brief Get chain by transaction ID
     */
    std::optional<std::vector<UndoLogPage>> get_chain(uint64_t trx_id);

    /**
     * @brief Get statistics
     */
    struct ChainStats {
        uint32_t total_pages;
        uint32_t total_chains;
        uint32_t complete_chains;
        uint32_t partial_chains;
    };
    ChainStats get_stats() const;

    /**
     * @brief Clear all data
     */
    void clear();

private:
    uint32_t page_size_;
    std::map<uint32_t, UndoLogPage> pages_;
    std::map<uint64_t, std::vector<UndoLogPage>> chains_;
    ChainStats stats_;

    /**
     * @brief Follow undo log chain
     */
    bool follow_chain(uint32_t first_page_no, uint64_t trx_id);
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_UNDO_LOG_PARSER_HPP

#ifndef DBRECOVERY_RECOVERY_SALVAGE_ENGINE_HPP
#define DBRECOVERY_RECOVERY_SALVAGE_ENGINE_HPP

/**
 * @file salvage_engine.hpp
 * @brief Emergency salvage recovery for severely corrupted InnoDB pages (v2.0)
 *
 * When standard recovery fails due to extensive page corruption, this engine
 * performs brute-force scanning to extract any parseable row fragments.
 *
 * Note: Implementations moved to src/salvage/salvage_engine.cpp
 */

#include <cstdint>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <unordered_set>
#include <sstream>
#include <fstream>
#include <iomanip>

#include "dbrecovery/common/damage_level.hpp"

namespace dbrecovery {
namespace recovery {

// ==================== Damage Classification ====================

// Unified damage classification shared across the recovery and salvage engines.
using DamageLevel = common::DamageLevel;
using common::ForensicEvidence;

struct DamageReport {
    DamageLevel level = DamageLevel::NONE;
    bool fil_header_valid = false;
    bool page_header_valid = false;
    bool checksum_valid = false;
    uint16_t detected_page_type = 0;
    uint32_t detected_space_id = 0;
    uint32_t detected_page_no = 0;
    uint16_t records_found = 0;
    uint16_t records_partial = 0;
    std::string diagnosis;

    std::string to_string() const;
};

// ==================== Salvaged Row ====================

enum class SalvageLevel : uint8_t {
    STRUCTURED = 0,
    FRAGMENT = 1,
    STRING_ONLY = 2
};

struct SalvagedRow {
    uint64_t page_offset = 0;
    uint64_t record_offset = 0;
    uint32_t page_no = 0;          // page number from FIL header (FIL_PAGE_OFFSET)
    uint16_t field_count = 0;
    SalvageLevel salvage_level = SalvageLevel::STRUCTURED;
    std::vector<std::string> fields;
    std::vector<bool> field_nulls;
    std::string content_hash;      // SHA256 of raw record bytes (for dedup)
    double confidence = 0.0;
    std::string recovery_method;
    ForensicEvidence evidence;  // provenance metadata for L3/L4 heuristic results
};

// ==================== Salvage Result ====================

struct SalvageResult {
    bool success = false;
    DamageReport damage;
    std::vector<SalvagedRow> rows;                // STRUCTURED only (L1/L2)
    std::vector<SalvagedRow> evidence_fragments;   // FRAGMENT/STRING_ONLY (L3/L4)
    uint32_t total_pages_scanned = 0;
    uint32_t pages_with_data = 0;
    std::vector<std::string> warnings;

    std::string to_report() const;
};

// ==================== Salvage Engine ====================

class SalvageEngine {
public:
    struct Config {
        uint32_t page_size = 16384;
        uint32_t min_string_len = 4;
        uint32_t max_l4_fragment_bytes = 4096;
        uint32_t max_l4_fragments = 1024;
        uint32_t min_record_fields = 2;
        double min_confidence = 0.3;
        bool enable_signature_scan = true;
        bool enable_pattern_match = true;
        bool enable_string_extract = true;
        // When false (default), FRAGMENT and STRING_ONLY (L3/L4) rows are not
        // promoted into the structured collection (`rows`). Heuristic fragments
        // are high-noise, so structured consumers opt in via this flag rather
        // than receiving them by default. This flag never authorizes export:
        // L3/L4 items always land in `evidence_fragments` and are serialized
        // only through the evidence report, never CSV/SQL export.
        bool include_fragments = false;
        bool verbose = false;
    };

    SalvageEngine() = default;
    explicit SalvageEngine(const Config& config) : config_(config) {}

    void set_config(const Config& config) { config_ = config; }

    static DamageReport assess_damage(const uint8_t* page_data, size_t page_size);

    std::vector<SalvagedRow> salvage_page(
        const uint8_t* page_data, size_t page_size, const DamageReport& damage);

    SalvageResult salvage_pages(
        const std::vector<std::pair<const uint8_t*, size_t>>& pages);

    static bool export_csv(const std::string& path, const std::vector<SalvagedRow>& rows);

private:
    Config config_;

    std::vector<SalvagedRow> salvage_l1(const uint8_t* data, size_t size);
    std::vector<SalvagedRow> salvage_l2(const uint8_t* data, size_t size);
    std::vector<SalvagedRow> salvage_l3(const uint8_t* data, size_t size);
    std::vector<SalvagedRow> salvage_l4(const uint8_t* data, size_t size);

    static bool detect_offset_table(const uint8_t* data, size_t pos, size_t max_len,
                                    uint32_t& field_count, uint32_t& record_len);
};

} // namespace recovery
} // namespace dbrecovery

#endif // DBRECOVERY_RECOVERY_SALVAGE_ENGINE_HPP
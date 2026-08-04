#ifndef DBRECOVERY_RECOVERY_SALVAGE_ENGINE_HPP
#define DBRECOVERY_RECOVERY_SALVAGE_ENGINE_HPP

/**
 * @file salvage_engine.hpp
 * @brief Emergency salvage recovery for severely corrupted InnoDB pages (v2.0)
 *
 * When standard recovery fails due to extensive page corruption, this engine
 * performs brute-force scanning to extract any parseable row fragments.
 *
 * 4 damage levels:
 *   L1 (MINOR):     Valid FIL header, partial page header — standard repair
 *   L2 (MODERATE):  Broken FIL header but recognizable record patterns
 *   L3 (SEVERE):    No FIL header, raw byte scan for row signatures
 *   L4 (CRITICAL):  Only isolated field values recoverable
 *
 * Strategy:
 *   - L1: Repair FIL header checksum/page_type, re-parse records normally
 *   - L2: Scan for COMPACT/DYNAMIC record signatures (offset table patterns)
 *   - L3: Brute-force scan for known data patterns (emails, timestamps, IDs)
 *   - L4: Extract any ASCII/UTF-8 strings above minimum length
 */

#include <cstdint>
#include <vector>
#include <string>
#include <optional>
#include <map>
#include <set>
#include <algorithm>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <regex>

namespace dbrecovery {
namespace recovery {

// ==================== Damage Classification ====================

enum class DamageLevel : uint8_t {
    NONE = 0,
    L1_MINOR = 1,       // Valid FIL header, partial corruption
    L2_MODERATE = 2,    // Broken FIL, recognizable records
    L3_SEVERE = 3,      // No structure, raw byte scan
    L4_CRITICAL = 4     // Only string fragments
};

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

    std::string to_string() const {
        std::ostringstream oss;
        oss << "DamageReport{level=";
        switch (level) {
            case DamageLevel::NONE: oss << "NONE"; break;
            case DamageLevel::L1_MINOR: oss << "L1_MINOR"; break;
            case DamageLevel::L2_MODERATE: oss << "L2_MODERATE"; break;
            case DamageLevel::L3_SEVERE: oss << "L3_SEVERE"; break;
            case DamageLevel::L4_CRITICAL: oss << "L4_CRITICAL"; break;
        }
        oss << ", FIL=" << (fil_header_valid ? "OK" : "BAD")
            << ", checksum=" << (checksum_valid ? "OK" : "BAD")
            << ", records=" << records_found
            << ", partial=" << records_partial
            << ", diagnosis=" << diagnosis << "}";
        return oss.str();
    }
};

// ==================== Salvaged Row ====================

enum class SalvageLevel : uint8_t {
    STRUCTURED = 0,   // L1/L2 — full record structure
    FRAGMENT = 1,     // L3 — pattern-matched evidence
    STRING_ONLY = 2  // L4 — raw string extraction
};

struct SalvagedRow {
    uint64_t page_offset = 0;
    uint16_t record_offset = 0;
    uint16_t field_count = 0;
    SalvageLevel salvage_level = SalvageLevel::STRUCTURED;
    std::vector<std::string> fields;
    double confidence = 0.0;  // 0.0-1.0
    std::string recovery_method; // "header_repair", "signature_scan", "pattern_match", "string_extract"
};

// ==================== Salvage Result ====================

struct SalvageResult {
    bool success = false;
    DamageReport damage;
    std::vector<SalvagedRow> rows;
    uint32_t total_pages_scanned = 0;
    uint32_t pages_with_data = 0;
    std::vector<std::string> warnings;

    std::string to_report() const {
        std::ostringstream oss;
        oss << "Salvage Recovery Report\n";
        oss << "======================\n";
        oss << "Damage level: " << static_cast<int>(damage.level) << "\n";
        oss << "Diagnosis: " << damage.diagnosis << "\n";
        oss << "Pages scanned: " << total_pages_scanned << "\n";
        oss << "Pages with recoverable data: " << pages_with_data << "\n";
        oss << "Rows salvaged: " << rows.size() << "\n";

        std::map<std::string, uint32_t> method_counts;
        for (const auto& row : rows) {
            method_counts[row.recovery_method]++;
        }
        oss << "\nRecovery methods:\n";
        for (const auto& [method, count] : method_counts) {
            oss << "  " << method << ": " << count << " rows\n";
        }

        if (!warnings.empty()) {
            oss << "\nWarnings:\n";
            for (const auto& w : warnings) {
                oss << "  - " << w << "\n";
            }
        }
        return oss.str();
    }
};

// ==================== Salvage Engine ====================

class SalvageEngine {
public:
    struct Config {
        uint32_t page_size = 16384;
        uint32_t min_string_len = 4;       // Min ASCII string length to extract
        uint32_t min_record_fields = 2;    // Min fields for STRUCTURED recovery only (L1/L2)
        double min_confidence = 0.3;       // Min confidence to include row
        bool enable_signature_scan = true; // L2: scan for record signatures
        bool enable_pattern_match = true;  // L3: scan for data patterns
        bool enable_string_extract = true; // L4: extract raw strings
        bool verbose = false;
    };

    SalvageEngine() = default;
    explicit SalvageEngine(const Config& config) : config_(config) {}

    void set_config(const Config& config) { config_ = config; }

    /**
     * @brief Assess damage level for a single page
     */
    static DamageReport assess_damage(const uint8_t* page_data, size_t page_size);

    /**
     * @brief Salvage rows from a damaged page
     * @param page_data Raw page buffer (potentially corrupted)
     * @param page_size Page size
     * @param damage Pre-assessed damage (from assess_damage)
     * @return Salvaged rows
     */
    std::vector<SalvagedRow> salvage_page(
        const uint8_t* page_data, size_t page_size, const DamageReport& damage);

    /**
     * @brief Salvage rows from multiple pages
     */
    SalvageResult salvage_pages(
        const std::vector<std::pair<const uint8_t*, size_t>>& pages);

    /**
     * @brief Export salvaged rows to CSV
     */
    static bool export_csv(const std::string& path, const std::vector<SalvagedRow>& rows);

private:
    Config config_;

    // L1: Repair FIL header and re-parse
    std::vector<SalvagedRow> salvage_l1(const uint8_t* data, size_t size);

    // L2: Scan for COMPACT record signatures
    std::vector<SalvagedRow> salvage_l2(const uint8_t* data, size_t size);

    // L3: Brute-force pattern matching
    std::vector<SalvagedRow> salvage_l3(const uint8_t* data, size_t size);

    // L4: Raw string extraction
    std::vector<SalvagedRow> salvage_l4(const uint8_t* data, size_t size);

    // Detect COMPACT record offset table at a given position
    static bool detect_offset_table(const uint8_t* data, size_t pos, size_t max_len,
                                    uint32_t& field_count, uint32_t& record_len);
};

// ==================== Implementation ====================

inline DamageReport SalvageEngine::assess_damage(const uint8_t* data, size_t size) {
    DamageReport report;
    if (!data || size < 64) {
        report.level = DamageLevel::L4_CRITICAL;
        report.diagnosis = "Data too small for page analysis";
        return report;
    }

    // Check FIL header checksum (offset 0, 4 bytes)
    uint32_t stored_checksum = (static_cast<uint32_t>(data[0]) << 24) |
                               (static_cast<uint32_t>(data[1]) << 16) |
                               (static_cast<uint32_t>(data[2]) << 8) |
                               static_cast<uint32_t>(data[3]);

    // Check LSN (offset 16, 8 bytes)
    uint64_t lsn = (static_cast<uint64_t>(data[16]) << 56) |
                   (static_cast<uint64_t>(data[17]) << 48) |
                   (static_cast<uint64_t>(data[18]) << 40) |
                   (static_cast<uint64_t>(data[19]) << 32) |
                   (static_cast<uint64_t>(data[20]) << 24) |
                   (static_cast<uint64_t>(data[21]) << 16) |
                   (static_cast<uint64_t>(data[22]) << 8) |
                   static_cast<uint64_t>(data[23]);

    // Check page type (offset 24, 2 bytes big-endian)
    uint16_t page_type = (static_cast<uint16_t>(data[24]) << 8) | data[25];

    // Check SPACE_ID (offset 34, 4 bytes big-endian)
    report.detected_space_id = (static_cast<uint32_t>(data[34]) << 24) |
                               (static_cast<uint32_t>(data[35]) << 16) |
                               (static_cast<uint32_t>(data[36]) << 8) |
                               static_cast<uint32_t>(data[37]);

    // Check PAGE_NO (offset 4, 4 bytes big-endian)
    report.detected_page_no = (static_cast<uint32_t>(data[4]) << 24) |
                              (static_cast<uint32_t>(data[5]) << 16) |
                              (static_cast<uint32_t>(data[6]) << 8) |
                              static_cast<uint32_t>(data[7]);

    report.detected_page_type = page_type;

    // Validate FIL header
    bool type_valid = (page_type == 0x45BF || // INDEX
                       page_type == 0x45BE || // SDI
                       page_type == 0x45BD || // SDI_ZIP
                       page_type == 0x45B2 || // UNDO_LOG
                       page_type == 0x45B3 || // UNDO_LOG_NEW
                       page_type == 0x45B5 || // BLOB
                       page_type == 0x0000 || // ALLOCATED
                       page_type == 0x45B8 || // INODE
                       page_type == 0x45B9 || // IBUF_BITMAP
                       page_type == 0x45BA || // SYS
                       page_type == 0x45BB || // TRX_SYS
                       page_type == 0x45BC || // FSP_HDR
                       page_type == 0x45B6);  // XDES

    bool lsn_ok = (lsn > 0 && lsn < UINT64_MAX);

    // Verify trailing checksum (offset -8 from end)
    uint32_t trailing_cksum = 0;
    if (size >= 8) {
        trailing_cksum = (static_cast<uint32_t>(data[size - 8]) << 24) |
                         (static_cast<uint32_t>(data[size - 7]) << 16) |
                         (static_cast<uint32_t>(data[size - 6]) << 8) |
                         static_cast<uint32_t>(data[size - 5]);
    }

    report.checksum_valid = (stored_checksum == trailing_cksum && stored_checksum != 0);

    // Check LSN validity at trailing position
    uint64_t trailing_lsn = 0;
    if (size >= 16) {
        trailing_lsn = (static_cast<uint64_t>(data[size - 16]) << 56) |
                       (static_cast<uint64_t>(data[size - 15]) << 48) |
                       (static_cast<uint64_t>(data[size - 14]) << 40) |
                       (static_cast<uint64_t>(data[size - 13]) << 32) |
                       (static_cast<uint64_t>(data[size - 12]) << 24) |
                       (static_cast<uint64_t>(data[size - 11]) << 16) |
                       (static_cast<uint64_t>(data[size - 10]) << 8) |
                       static_cast<uint64_t>(data[size - 9]);
    }
    bool trailing_lsn_ok = (trailing_lsn == lsn && lsn > 0);

    report.fil_header_valid = type_valid && lsn_ok;
    report.page_header_valid = report.fil_header_valid;

    // Classify damage level
    if (report.fil_header_valid && report.checksum_valid && trailing_lsn_ok) {
        report.level = DamageLevel::NONE;
        report.diagnosis = "Page appears intact";
    } else if (report.fil_header_valid || type_valid) {
        report.level = DamageLevel::L1_MINOR;
        report.diagnosis = "FIL header valid, checksum mismatch — repairable";
    } else if (type_valid || (!type_valid && lsn_ok)) {
        report.level = DamageLevel::L2_MODERATE;
        report.diagnosis = "Partial FIL header — scanning for record signatures";
    } else {
        // Check for any recognizable data patterns
        uint32_t printable = 0;
        for (size_t i = 0; i < std::min(size, size_t(4096)); i++) {
            if (data[i] >= 0x20 && data[i] <= 0x7E) printable++;
        }
        if (printable > size / 4) {
            report.level = DamageLevel::L3_SEVERE;
            report.diagnosis = "No FIL header, but " + std::to_string(printable) +
                               " printable bytes found";
        } else {
            report.level = DamageLevel::L4_CRITICAL;
            report.diagnosis = "Severely corrupted — only string extraction possible";
        }
    }

    return report;
}

inline bool SalvageEngine::detect_offset_table(
    const uint8_t* data, size_t pos, size_t max_len,
    uint32_t& field_count, uint32_t& record_len) {

    field_count = 0;
    record_len = 0;
    if (pos + 4 > max_len) return false;

    // First 2-byte entry is the total record length
    uint16_t first_entry = (static_cast<uint16_t>(data[pos + 1]) << 8) | data[pos];
    if (first_entry < 5 || first_entry > 8192) return false;
    if (pos + first_entry > max_len) return false;

    // Read subsequent offset entries — each must be strictly decreasing
    // In COMPACT format, offsets are stored in descending order
    uint32_t count = 1;
    size_t off = pos + 2;
    uint16_t prev = first_entry;
    std::vector<uint16_t> offsets = {first_entry};

    while (off + 2 <= max_len && count < 200) {
        uint16_t val = (static_cast<uint16_t>(data[off + 1]) << 8) | data[off];
        if (val == 0 || val >= prev) break;
        if (val > first_entry) break;
        offsets.push_back(val);
        prev = val;
        count++;
        off += 2;
    }

    // Need at least 3 fields for a valid record (user columns + system cols)
    if (count < 3) return false;

    // Verify offset table consistency: each offset should be >= previous
    // and the first should be within reasonable range
    if (first_entry > 32768) return false;

    field_count = count;
    record_len = first_entry;
    return true;
}

inline std::vector<SalvagedRow> SalvageEngine::salvage_l1(
    const uint8_t* data, size_t size) {

    std::vector<SalvagedRow> rows;

    // L1: Repair FIL header — fix checksum and re-read records
    // The FIL header is valid so PAGE_N_RECS, PAGE_HEAP_TOP can be read
    if (size < 120) return rows;

    uint16_t n_recs = (static_cast<uint16_t>(data[55]) << 8) | data[54];
    uint16_t heap_top = (static_cast<uint16_t>(data[59]) << 8) | data[58];

    if (n_recs == 0 || n_recs > 1000 || heap_top < 120 || heap_top > size - 8)
        return rows;

    // INFIMUM → first real record
    uint16_t infimum_off = (static_cast<uint16_t>(data[101]) << 8) | data[100];
    if (infimum_off < 99 || infimum_off + 5 > size) return rows;

    uint16_t rec_off = (static_cast<uint16_t>(data[infimum_off + 2]) << 8)
                     | data[infimum_off + 1];

    for (uint16_t i = 0; i < n_recs && rec_off < heap_top && rec_off >= 120; i++) {
        uint32_t field_count, record_len;
        if (detect_offset_table(data, rec_off, heap_top, field_count, record_len)) {
            SalvagedRow row;
            row.page_offset = 0;
            row.record_offset = rec_off;
            row.field_count = field_count;
            row.confidence = 0.7;
            row.recovery_method = "header_repair";

            // Extract field values as hex
            uint16_t data_start = rec_off + field_count * 2 + 1; // +1 for info_bits
            // Skip NULL bitmap: (field_count + 7) / 8
            uint16_t null_bytes = (field_count + 7) / 8;
            data_start += null_bytes;

            // Re-read offset table entries to compute field boundaries
            // offsets[0] = total record length, offsets[1..] = field end positions from end
            std::vector<uint16_t> offsets;
            offsets.push_back(record_len);
            for (uint32_t oe = 0; oe + 1 < field_count; oe++) {
                uint16_t val = (static_cast<uint16_t>(data[rec_off + (oe + 1) * 2 + 1]) << 8)
                             | data[rec_off + (oe + 1) * 2];
                offsets.push_back(val);
            }

            // Extract each field using offset table
            // Field f (0-indexed): position = total_len - offsets[f], size = offsets[f] - offsets[f+1]
            uint16_t total_len = record_len;
            uint32_t n_fields_to_extract = std::min(field_count - 1, uint32_t(20));
            for (uint32_t f = 0; f < n_fields_to_extract; f++) {
                uint16_t field_sz = offsets[f] - offsets[f + 1];
                uint16_t field_off = total_len - offsets[f]; // from data_start

                if (field_sz > 0 && data_start + field_off + field_sz <= size) {
                    std::ostringstream oss;
                    for (uint16_t b = 0; b < field_sz; b++) {
                        oss << std::hex << std::setw(2) << std::setfill('0')
                            << static_cast<int>(data[data_start + field_off + b]);
                    }
                    row.fields.push_back(oss.str());
                } else {
                    row.fields.push_back("NULL");
                }
            }
            rows.push_back(row);
        }

        // Next record
        uint16_t next_rec = (static_cast<uint16_t>(data[rec_off + 1]) << 8)
                         | data[rec_off];
        if (next_rec <= rec_off || next_rec >= heap_top) break;
        rec_off = next_rec;
    }

    return rows;
}

inline std::vector<SalvagedRow> SalvageEngine::salvage_l2(
    const uint8_t* data, size_t size) {

    std::vector<SalvagedRow> rows;

    // L2: Scan for COMPACT/DYNAMIC record signatures
    // A COMPACT record starts with an offset table (2-byte entries)
    // followed by info_bits (1 byte), null bitmap, then field data
    // We slide a window looking for offset table patterns
    for (size_t pos = 0; pos + 6 < size; pos += 2) {
        uint32_t field_count, record_len;
        if (detect_offset_table(data, pos, size, field_count, record_len)) {
            SalvagedRow row;
            row.page_offset = pos;
            row.record_offset = static_cast<uint16_t>(pos);
            row.field_count = field_count;
            row.confidence = 0.5;
            row.recovery_method = "signature_scan";

            // Re-read offset table entries to compute field boundaries
            std::vector<uint16_t> offsets;
            offsets.push_back(record_len);
            for (uint32_t oe = 0; oe + 1 < field_count; oe++) {
                uint16_t val = (static_cast<uint16_t>(data[pos + (oe + 1) * 2 + 1]) << 8)
                             | data[pos + (oe + 1) * 2];
                offsets.push_back(val);
            }

            // Data area: after offset table, info_bits, and NULL bitmap
            uint16_t data_start = pos + field_count * 2 + 1;
            uint16_t null_bytes = (field_count + 7) / 8;
            data_start += null_bytes;

            uint32_t n_fields_to_extract = std::min(field_count - 1, uint32_t(20));
            for (uint32_t f = 0; f < n_fields_to_extract; f++) {
                uint16_t field_sz = offsets[f] - offsets[f + 1];
                uint16_t field_off = record_len - offsets[f];

                if (field_sz > 0 && data_start + field_off + field_sz <= size) {
                    std::ostringstream oss;
                    for (uint16_t b = 0; b < field_sz; b++) {
                        oss << std::hex << std::setw(2) << std::setfill('0')
                            << static_cast<int>(data[data_start + field_off + b]);
                    }
                    row.fields.push_back(oss.str());
                } else {
                    row.fields.push_back("NULL");
                }
            }
            rows.push_back(row);
            pos += record_len; // Skip past this record
        }
    }

    return rows;
}

inline std::vector<SalvagedRow> SalvageEngine::salvage_l3(
    const uint8_t* data, size_t size) {

    std::vector<SalvagedRow> rows;
    std::string buffer(reinterpret_cast<const char*>(data),
                       std::min(size, size_t(65536)));

    // Pattern 1: Email addresses
    std::regex email_re(R"([a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,})");
    std::regex datetime_re(R"(\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2})");
    std::regex url_re(R"(https?://[^\s,;]+)");
    std::regex integer_re(R"(\b\d{5,15}\b)"); // Likely IDs

    auto extract_patterns = [&](const std::regex& re, const std::string& method) {
        auto begin = std::sregex_iterator(buffer.begin(), buffer.end(), re);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            SalvagedRow row;
            row.page_offset = it->position();
            row.field_count = 1;
            row.salvage_level = SalvageLevel::FRAGMENT;
            row.fields.push_back(it->str());
            row.confidence = 0.4;
            row.recovery_method = method;
            rows.push_back(row);
        }
    };

    extract_patterns(email_re, "pattern_match_email");
    extract_patterns(datetime_re, "pattern_match_datetime");
    extract_patterns(url_re, "pattern_match_url");
    extract_patterns(integer_re, "pattern_match_integer");

    return rows;
}

inline std::vector<SalvagedRow> SalvageEngine::salvage_l4(
    const uint8_t* data, size_t size) {

    std::vector<SalvagedRow> rows;

    // L4: Extract any printable ASCII/UTF-8 strings above minimum length
    uint32_t min_len = config_.min_string_len;
    std::string current;
    size_t start_pos = 0;

    for (size_t i = 0; i < size; i++) {
        if (data[i] >= 0x20 && data[i] <= 0x7E) {
            if (current.empty()) start_pos = i;
            current += static_cast<char>(data[i]);
        } else {
            if (current.size() >= min_len) {
                SalvagedRow row;
                row.page_offset = start_pos;
                row.field_count = 1;
                row.salvage_level = SalvageLevel::STRING_ONLY;
                row.fields.push_back(current);
                row.confidence = 0.2;
                row.recovery_method = "string_extract";
                rows.push_back(row);
            }
            current.clear();
        }
    }

    // Don't forget trailing string
    if (current.size() >= min_len) {
        SalvagedRow row;
        row.page_offset = start_pos;
        row.field_count = 1;
        row.salvage_level = SalvageLevel::STRING_ONLY;
        row.fields.push_back(current);
        row.confidence = 0.2;
        row.recovery_method = "string_extract";
        rows.push_back(row);
    }

    return rows;
}

inline std::vector<SalvagedRow> SalvageEngine::salvage_page(
    const uint8_t* data, size_t size, const DamageReport& damage) {

    std::vector<SalvagedRow> rows;

    if (!data || size < 64) return rows;

    switch (damage.level) {
        case DamageLevel::NONE:
        case DamageLevel::L1_MINOR: {
            rows = salvage_l1(data, size);
            if (config_.enable_signature_scan && rows.empty()) {
                auto l2_rows = salvage_l2(data, size);
                rows.insert(rows.end(), l2_rows.begin(), l2_rows.end());
            }
            break;
        }
        case DamageLevel::L2_MODERATE: {
            if (config_.enable_signature_scan) {
                rows = salvage_l2(data, size);
            }
            // Fall through to L3 if no records found
            if (rows.empty() && config_.enable_pattern_match) {
                rows = salvage_l3(data, size);
            }
            break;
        }
        case DamageLevel::L3_SEVERE: {
            if (config_.enable_pattern_match) {
                rows = salvage_l3(data, size);
            }
            if (config_.enable_string_extract) {
                auto l4_rows = salvage_l4(data, size);
                rows.insert(rows.end(), l4_rows.begin(), l4_rows.end());
            }
            break;
        }
        case DamageLevel::L4_CRITICAL: {
            if (config_.enable_string_extract) {
                rows = salvage_l4(data, size);
            }
            break;
        }
    }

    // For L1/L2 structured recovery, enforce min_field_count
    // For L3/L4 fragments, only filter by confidence (fragments are single-field by nature)
    rows.erase(std::remove_if(rows.begin(), rows.end(),
        [this](const SalvagedRow& r) {
            if (r.confidence < config_.min_confidence) return true;
            if (r.salvage_level == SalvageLevel::STRUCTURED &&
                r.field_count < config_.min_record_fields) return true;
            return false;
        }), rows.end());

    return rows;
}

inline SalvageResult SalvageEngine::salvage_pages(
    const std::vector<std::pair<const uint8_t*, size_t>>& pages) {

    SalvageResult result;
    result.success = true;
    result.total_pages_scanned = static_cast<uint32_t>(pages.size());

    DamageLevel worst_level = DamageLevel::NONE;

    for (const auto& [data, size] : pages) {
        auto damage = assess_damage(data, size);

        if (static_cast<int>(damage.level) > static_cast<int>(worst_level)) {
            worst_level = damage.level;
            result.damage = damage;
        }

        if (damage.level > DamageLevel::NONE) {
            auto rows = salvage_page(data, size, damage);
            if (!rows.empty()) {
                result.pages_with_data++;
                for (auto& row : rows) {
                    result.rows.push_back(std::move(row));
                }
            }
        }
    }

    if (result.rows.empty()) {
        result.warnings.push_back("No salvageable data found in any page");
    }

    if (worst_level >= DamageLevel::L3_SEVERE) {
        result.warnings.push_back(
            "Severe corruption detected — recovered data may be incomplete");
    }

    return result;
}

inline bool SalvageEngine::export_csv(
    const std::string& path, const std::vector<SalvagedRow>& rows) {

    std::ofstream file(path);
    if (!file) return false;

    // Determine max fields
    uint16_t max_fields = 0;
    for (const auto& row : rows) {
        max_fields = std::max(max_fields, row.field_count);
    }

    // Header
    file << "offset,method,confidence";
    for (uint16_t i = 0; i < max_fields; i++) {
        file << ",field_" << i;
    }
    file << "\n";

    // Data
    for (const auto& row : rows) {
        file << row.page_offset << "," << row.recovery_method << ","
             << std::fixed << std::setprecision(2) << row.confidence;
        for (uint16_t i = 0; i < max_fields; i++) {
            file << ",";
            if (i < row.fields.size()) {
                // CSV-escape the field
                std::string val = row.fields[i];
                if (val.find(',') != std::string::npos ||
                    val.find('"') != std::string::npos ||
                    val.find('\n') != std::string::npos) {
                    // Escape quotes
                    std::string escaped;
                    for (char c : val) {
                        if (c == '"') escaped += "\"\"";
                        else escaped += c;
                    }
                    file << "\"" << escaped << "\"";
                } else {
                    file << val;
                }
            }
        }
        file << "\n";
    }

    return true;
}

} // namespace recovery
} // namespace dbrecovery

#endif // DBRECOVERY_RECOVERY_SALVAGE_ENGINE_HPP

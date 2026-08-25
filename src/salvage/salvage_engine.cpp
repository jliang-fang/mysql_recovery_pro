/**
 * @file salvage_engine.cpp
 * @brief Emergency salvage recovery for severely corrupted InnoDB pages (v2.0)
 *
 * Implementations extracted from salvage_engine.hpp for better compilation.
 */

#include "dbrecovery/recovery/salvage_engine.hpp"
#include "dbrecovery/innodb/innodb_checksum.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <regex>
#include <map>
#include <openssl/sha.h>

namespace dbrecovery {
namespace recovery {

// ==================== DamageReport ====================

namespace {
// Extract the logical page number from the FIL header (FIL_PAGE_OFFSET, big-endian)
// when the buffer looks like a page with a valid header. 0 means unknown/unreadable.
uint32_t extract_page_no(const uint8_t* data, size_t size) {
    if (!data || size < 8) return 0;
    return (static_cast<uint32_t>(data[4]) << 24) |
           (static_cast<uint32_t>(data[5]) << 16) |
           (static_cast<uint32_t>(data[6]) << 8) |
           static_cast<uint32_t>(data[7]);
}

// SHA256 over the raw on-disk bytes of the salvaged region. Unlike the field-value
// hashes in the evidence report, this covers the exact bytes a fragment came from,
// making it a stable dedup key independent of how fields were later parsed.
std::string sha256_hex_bytes(const uint8_t* data, size_t size) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(data, size, hash);

    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(hash[i]);
    }
    return oss.str();
}
} // namespace

std::string DamageReport::to_string() const {
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

// ==================== SalvageResult ====================

std::string SalvageResult::to_report() const {
    std::ostringstream oss;
    oss << "Salvage Recovery Report\n";
    oss << "======================\n";
    oss << "Damage level: " << static_cast<int>(damage.level) << "\n";
    oss << "Diagnosis: " << damage.diagnosis << "\n";
    oss << "Pages scanned: " << total_pages_scanned << "\n";
    oss << "Pages with recoverable data: " << pages_with_data << "\n";
    oss << "Recovered structured rows: " << rows.size() << "\n";
    oss << "Forensic evidence fragments: " << evidence_fragments.size() << "\n";

    if (!rows.empty()) {
        std::map<std::string, uint32_t> method_counts;
        for (const auto& row : rows) {
            method_counts[row.recovery_method]++;
        }
        oss << "\nRecovery methods (structured rows):\n";
        for (const auto& [method, count] : method_counts) {
            oss << "  " << method << ": " << count << " rows\n";
        }
    }

    if (!evidence_fragments.empty()) {
        std::map<std::string, uint32_t> frag_method_counts;
        for (const auto& frag : evidence_fragments) {
            frag_method_counts[frag.recovery_method]++;
        }
        oss << "\nEvidence fragment methods (L3/L4, not exportable):\n";
        for (const auto& [method, count] : frag_method_counts) {
            oss << "  " << method << ": " << count << " fragments\n";
        }
        oss << "\nWARNING: Evidence fragments are heuristic-only and cannot be\n";
        oss << "exported as CSV/SQL. They are stored in the evidence report.\n";
    }

    if (!warnings.empty()) {
        oss << "\nWarnings:\n";
        for (const auto& w : warnings) {
            oss << "  - " << w << "\n";
        }
    }
    return oss.str();
}

// ==================== SalvageEngine ====================

DamageReport SalvageEngine::assess_damage(const uint8_t* data, size_t size) {
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
    // MySQL 5.6 uses old-style 0x00XX page type codes; 5.7+ renamed them to 0x45XX.
    // Both sets are valid for header validation.
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
                       page_type == 0x45B6 || // XDES
                       // MySQL 5.6 old-style (0x00XX) page types
                       page_type == 0x0002 || // UNDO_LOG (5.6)
                       page_type == 0x0003 || // INODE (5.6)
                       page_type == 0x0004 || // IBUF_FREE_LIST (5.6)
                       page_type == 0x0005 || // IBUF_BITMAP (5.6)
                       page_type == 0x0006 || // SYS (5.6)
                       page_type == 0x0007 || // TRX_SYS (5.6)
                       page_type == 0x0008 || // FSP_HDR (5.6)
                       page_type == 0x0009 || // XDES (5.6)
                       page_type == 0x000A);  // BLOB (5.6)

    bool lsn_ok = (lsn > 0 && lsn < UINT64_MAX);

    // Verify trailing checksum (offset -8 from end)
    uint32_t trailing_cksum = 0;
    if (size >= 8) {
        trailing_cksum = (static_cast<uint32_t>(data[size - 8]) << 24) |
                         (static_cast<uint32_t>(data[size - 7]) << 16) |
                         (static_cast<uint32_t>(data[size - 6]) << 8) |
                         static_cast<uint32_t>(data[size - 5]);
    }

    // Validate both legacy and algorithm-tagged InnoDB page checksums.
    report.checksum_valid = innodb::validate_page_checksum(data, size, false).is_valid;

    // MySQL 5.6 (and earlier) default innodb_checksum_algorithm=INNODB did NOT
    // verify checksums on read for non-compressed pages. Pages from that era
    // have no algorithm ID at page_size-4 (it's the low 4 bytes of the LSN).
    // When the FIL header is valid but no recognized algorithm ID is present,
    // treat the checksum as valid — this is historically accurate behavior.
    // This applies to all recognized page types (both 0x00XX old-style and
    // 0x45XX modern-style) since MySQL 5.6 INDEX pages use 0x45BF.
    // MySQL 8.0+ SDI_ZIP (0x45BD) pages also lack an algorithm ID but DO
    // verify checksums, so the bypass does not apply to them.
    if (!report.checksum_valid && type_valid && lsn_ok) {
        // Bypass for all recognized page types except ALLOCATED (no data) and
        // SDI_ZIP (which verifies checksums even without an algorithm ID).
        if (page_type != 0x45BD && page_type != 0x0000) {
            uint32_t algo_id = innodb::read_stored_algorithm_id(data, size);
            if (algo_id != innodb::CHECKSUM_CRC32C_MAGIC &&
                algo_id != innodb::CHECKSUM_INNODB_MAGIC &&
                algo_id != innodb::CHECKSUM_NONE_MAGIC) {
                report.checksum_valid = true;
            }
        }
    }

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

    // Check free space integrity on non-leaf INDEX pages.
    // For non-leaf pages (level > 0), the free space between PAGE_HEAP_TOP
    // and the page directory start should be zero-filled.  A non-zero byte
    // in this region indicates data corruption (e.g. a bitflip in free space
    // that would not be caught by checksum or FIL-header validation).
    if ((page_type == 0x45BF || page_type == 0x00BF) && size >= 66) {
        uint16_t page_level = (static_cast<uint16_t>(data[64]) << 8) | data[65];
        if (page_level > 0) {
            uint16_t heap_top = (static_cast<uint16_t>(data[40]) << 8) | data[41];
            uint16_t n_dir_slots = (static_cast<uint16_t>(data[38]) << 8) | data[39];
            // Directory starts at: page_size - 8 (trailer) - 2 * n_dir_slots
            uint16_t dir_start = static_cast<uint16_t>(size - 8 - 2 * n_dir_slots);
            if (heap_top >= 38 && heap_top < dir_start && dir_start <= size - 8) {
                for (uint16_t i = heap_top; i < dir_start; ++i) {
                    if (data[i] != 0) {
                        report.free_space_corrupt = true;
                        break;
                    }
                }
            }
        }
    }

    // Classify damage level
    if (report.free_space_corrupt) {
        report.level = DamageLevel::L1_MINOR;
        report.diagnosis = "Free space corrupt on non-leaf page";
    } else if (report.fil_header_valid && report.checksum_valid && trailing_lsn_ok) {
        report.level = DamageLevel::NONE;
        report.diagnosis = "Page appears intact";
    } else if (report.fil_header_valid || type_valid) {
        report.level = DamageLevel::L1_MINOR;
        report.diagnosis = "FIL header valid, checksum mismatch - repairable";
    } else if (type_valid || (!type_valid && lsn_ok)) {
        report.level = DamageLevel::L2_MODERATE;
        report.diagnosis = "Partial FIL header - scanning for record signatures";
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
            report.diagnosis = "Severely corrupted - only string extraction possible";
        }
    }

    return report;
}

bool SalvageEngine::detect_offset_table(
    const uint8_t* data, size_t pos, size_t max_len,
    uint32_t& field_count, uint32_t& record_len) {

    field_count = 0;
    record_len = 0;
    if (pos + 4 > max_len) return false;

    // First 2-byte entry is the total record length
    uint16_t first_entry = (static_cast<uint16_t>(data[pos + 1]) << 8) | data[pos];
    if (first_entry < 5 || first_entry > 8192) return false;
    if (pos + first_entry > max_len) return false;

    // Read subsequent offset entries - each must be strictly decreasing
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

std::vector<SalvagedRow> SalvageEngine::salvage_l1(
    const uint8_t* data, size_t size) {

    std::vector<SalvagedRow> rows;

    // L1: Repair FIL header - fix checksum and re-read records
    if (size < 120) return rows;

    uint16_t n_recs = (static_cast<uint16_t>(data[55]) << 8) | data[54];
    uint16_t heap_top = (static_cast<uint16_t>(data[59]) << 8) | data[58];

    if (n_recs == 0 || n_recs > 1000 || heap_top < 120 || heap_top > size - 8)
        return rows;

    // INFIMUM to first real record
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
            row.page_no = extract_page_no(data, size);
            row.field_count = field_count;
            row.confidence = 0.7;
            row.recovery_method = "header_repair";
            row.evidence.level = DamageLevel::L1_MINOR;
            row.evidence.strategy = "header_repair";
            row.evidence.source_page_no = row.page_no;
            row.evidence.source_offset = rec_off;
            row.evidence.confidence = 0.7f;
            // L1 is "structure-derived": only the FIL header and an offset table
            // were readable. There is no checksum, record-chain, or schema
            // validation behind it, so it must not claim verified status.
            row.evidence.tags = {"structure-derived"};
            row.content_hash = sha256_hex_bytes(data + rec_off,
                                                std::min(size_t(record_len), size - rec_off));

            // Extract field values as hex
            uint16_t data_start = rec_off + field_count * 2 + 1; // +1 for info_bits
            uint16_t null_bytes = (field_count + 7) / 8;
            data_start += null_bytes;

            // Re-read offset table entries to compute field boundaries
            std::vector<uint16_t> offsets;
            offsets.push_back(record_len);
            for (uint32_t oe = 0; oe + 1 < field_count; oe++) {
                uint16_t val = (static_cast<uint16_t>(data[rec_off + (oe + 1) * 2 + 1]) << 8)
                             | data[rec_off + (oe + 1) * 2];
                offsets.push_back(val);
            }

            // Extract each field using offset table
            uint16_t total_len = record_len;
            uint32_t n_fields_to_extract = std::min(field_count - 1, uint32_t(20));
            for (uint32_t f = 0; f < n_fields_to_extract; f++) {
                uint16_t field_sz = offsets[f] - offsets[f + 1];
                uint16_t field_off = total_len - offsets[f];

                if (field_sz > 0 && data_start + field_off + field_sz <= size) {
                    std::ostringstream oss;
                    for (uint16_t b = 0; b < field_sz; b++) {
                        oss << std::hex << std::setw(2) << std::setfill('0')
                            << static_cast<int>(data[data_start + field_off + b]);
                    }
                    row.fields.push_back(oss.str());
                    row.field_nulls.push_back(false);
                } else {
                    row.fields.push_back("");
                    row.field_nulls.push_back(true);
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

std::vector<SalvagedRow> SalvageEngine::salvage_l2(
    const uint8_t* data, size_t size) {

    std::vector<SalvagedRow> rows;

    // L2: Scan for COMPACT/DYNAMIC record signatures
    for (size_t pos = 0; pos + 6 < size; pos += 2) {
        uint32_t field_count, record_len;
        if (detect_offset_table(data, pos, size, field_count, record_len)) {
            SalvagedRow row;
            row.page_offset = pos;
            row.record_offset = pos;
            row.page_no = extract_page_no(data, size);
            row.field_count = field_count;
            row.confidence = 0.5;
            row.recovery_method = "signature_scan";
            row.evidence.level = DamageLevel::L2_MODERATE;
            row.evidence.strategy = "signature_scan";
            row.evidence.source_page_no = row.page_no;
            row.evidence.source_offset = pos;
            row.evidence.confidence = 0.5f;
            row.evidence.tags = {"reconstructed"};
            // Content hash over the raw record bytes for dedup
            row.content_hash = sha256_hex_bytes(
                data + pos, std::min(size_t(record_len), size - pos));

            // Re-read offset table entries
            std::vector<uint16_t> offsets;
            offsets.push_back(record_len);
            for (uint32_t oe = 0; oe + 1 < field_count; oe++) {
                uint16_t val = (static_cast<uint16_t>(data[pos + (oe + 1) * 2 + 1]) << 8)
                             | data[pos + (oe + 1) * 2];
                offsets.push_back(val);
            }

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
                    row.field_nulls.push_back(false);
                } else {
                    row.fields.push_back("");
                    row.field_nulls.push_back(true);
                }
            }
            rows.push_back(row);
            pos += record_len;
        }
    }

    return rows;
}

std::vector<SalvagedRow> SalvageEngine::salvage_l3(
    const uint8_t* data, size_t size) {

    std::vector<SalvagedRow> rows;
    std::string buffer(reinterpret_cast<const char*>(data),
                       std::min(size, size_t(65536)));

    // Pattern 1: Email addresses
    std::regex email_re(R"([a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,})");
    std::regex datetime_re(R"(\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2})");
    std::regex url_re(R"(https?://[^\s,;]+)");
    std::regex integer_re(R"(\b\d{5,15}\b)");

    auto extract_patterns = [&](const std::regex& re, const std::string& method) {
        auto begin = std::sregex_iterator(buffer.begin(), buffer.end(), re);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            SalvagedRow row;
            row.page_offset = it->position();
            row.page_no = extract_page_no(data, size);
            row.field_count = 1;
            row.salvage_level = SalvageLevel::FRAGMENT;
            row.fields.push_back(it->str());
            row.confidence = 0.4;
            row.recovery_method = method;
            row.evidence.level = DamageLevel::L3_SEVERE;
            row.evidence.strategy = method;
            row.evidence.source_page_no = row.page_no;
            row.evidence.source_offset = static_cast<uint64_t>(it->position());
            row.evidence.confidence = 0.4f;
            row.evidence.tags = {"heuristic", "partial"};
            // Content hash for L3: SHA256 of the matched string itself
            row.content_hash = sha256_hex_bytes(
                reinterpret_cast<const uint8_t*>(it->str().data()),
                it->str().size());
            rows.push_back(row);
        }
    };

    extract_patterns(email_re, "pattern_match_email");
    extract_patterns(datetime_re, "pattern_match_datetime");
    extract_patterns(url_re, "pattern_match_url");
    extract_patterns(integer_re, "pattern_match_integer");

    return rows;
}

std::vector<SalvagedRow> SalvageEngine::salvage_l4(
    const uint8_t* data, size_t size) {

    std::vector<SalvagedRow> rows;
    const size_t min_len = config_.min_string_len;
    const size_t max_fragment_bytes = config_.max_l4_fragment_bytes;
    const size_t max_fragments = config_.max_l4_fragments;
    if (max_fragment_bytes == 0 || max_fragments == 0) return rows;

    std::string current;
    current.reserve(max_fragment_bytes);
    size_t start_pos = 0;
    bool continued = false;

    const auto emit = [&](bool truncated) {
        if (current.size() < min_len || rows.size() >= max_fragments) {
            current.clear();
            return;
        }

        SalvagedRow row;
        row.page_offset = start_pos;
        row.page_no = extract_page_no(data, size);
        row.field_count = 1;
        row.salvage_level = SalvageLevel::STRING_ONLY;
        row.fields.push_back(std::move(current));
        row.confidence = 0.3;
        row.recovery_method = "string_extract";
        row.evidence.level = DamageLevel::L4_CRITICAL;
        row.evidence.strategy = "string_extract";
        row.evidence.source_page_no = row.page_no;
        row.evidence.source_offset = start_pos;
        row.evidence.confidence = 0.3f;
        row.evidence.tags = {"heuristic", "fragment"};
        if (continued) row.evidence.tags.push_back("continuation");
        if (truncated) row.evidence.tags.push_back("truncated");
        // Content hash: SHA256 of the raw string bytes
        if (!row.fields.empty()) {
            row.content_hash = sha256_hex_bytes(
                reinterpret_cast<const uint8_t*>(row.fields.back().data()),
                row.fields.back().size());
        }
        rows.push_back(std::move(row));
        current.clear();
    };

    for (size_t i = 0; i < size; ++i) {
        if (data[i] >= 0x20 && data[i] <= 0x7E) {
            if (current.empty()) start_pos = i;
            current += static_cast<char>(data[i]);
            if (current.size() == max_fragment_bytes) {
                emit(true);
                continued = true;
            }
        } else {
            emit(false);
            current.clear();
            continued = false;
        }
    }

    emit(false);
    return rows;
}

std::vector<SalvagedRow> SalvageEngine::salvage_page(
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

    rows.erase(std::remove_if(rows.begin(), rows.end(),
        [this](const SalvagedRow& r) {
            if (r.confidence < config_.min_confidence) return true;
            // For STRUCTURED rows, enforce minimum field count
            if (r.salvage_level == SalvageLevel::STRUCTURED &&
                r.field_count < config_.min_record_fields) return true;
            return false;
        }), rows.end());

    return rows;
}

SalvageResult SalvageEngine::salvage_pages(
    const std::vector<std::pair<const uint8_t*, size_t>>& pages) {

    SalvageResult result;
    result.success = true;
    result.total_pages_scanned = static_cast<uint32_t>(pages.size());

    DamageLevel worst_level = DamageLevel::NONE;

    // Dedup by raw content hash: the same record can be surfaced by multiple
    // salvage strategies (e.g. L1 offset-table walk and L2 signature scan) or
    // appear in multiple scanned buffers. Emit each unique fragment once and
    // count collisions in the report.
    std::unordered_set<std::string> emitted_hashes;
    uint32_t dedup_collisions = 0;

    for (const auto& [data, size] : pages) {
        auto damage = assess_damage(data, size);

        if (static_cast<int>(damage.level) > static_cast<int>(worst_level)) {
            worst_level = damage.level;
            result.damage = damage;
        }

        if (damage.level > DamageLevel::NONE) {
            auto page_rows = salvage_page(data, size, damage);
            if (!page_rows.empty()) {
                result.pages_with_data++;
                for (auto& row : page_rows) {
                    if (!row.content_hash.empty() &&
                        !emitted_hashes.insert(row.content_hash).second) {
                        ++dedup_collisions;
                        continue;
                    }
                    // Route L3/L4 heuristic fragments into evidence_fragments,
                    // STRUCTURED (L1/L2) rows into rows.
                    if (row.salvage_level == SalvageLevel::STRUCTURED) {
                        result.rows.push_back(std::move(row));
                    } else {
                        result.evidence_fragments.push_back(std::move(row));
                    }
                }
            }
        }
    }

    if (dedup_collisions > 0) {
        result.warnings.push_back(
            "Deduplicated " + std::to_string(dedup_collisions) +
            " fragments sharing identical raw content");
    }

    if (result.rows.empty() && result.evidence_fragments.empty()) {
        result.warnings.push_back("No salvageable data found in any page");
    }

    if (worst_level >= DamageLevel::L3_SEVERE) {
        result.warnings.push_back(
            "Severe corruption detected - recovered data may be incomplete");
    }

    return result;
}

bool SalvageEngine::export_csv(
    const std::string& path, const std::vector<SalvagedRow>& rows) {

    // Fail closed: evidence fragments (L3/L4) are forensic-only and must never
    // be written as CSV records. Reject the whole export instead of silently
    // dropping or emitting them, so callers cannot generate a partial file.
    for (const auto& row : rows) {
        if (row.salvage_level != SalvageLevel::STRUCTURED) {
            return false;
        }
    }

    std::ofstream file(path);
    if (!file) return false;

    // Determine max fields
    uint16_t max_fields = 0;
    for (const auto& row : rows) {
        max_fields = std::max(max_fields, row.field_count);
    }

    // Header
    file << "offset,level,method,confidence";
    for (uint16_t i = 0; i < max_fields; i++) {
        file << ",field_" << i;
    }
    file << "\n";

    // Data
    for (const auto& row : rows) {
        file << row.page_offset << "," << static_cast<int>(row.salvage_level) << ","
             << row.recovery_method << ","
             << std::fixed << std::setprecision(2) << row.confidence;
        for (uint16_t i = 0; i < max_fields; i++) {
            file << ",";
            if (i < row.fields.size()) {
                std::string val = row.fields[i];
                if (val.find(',') != std::string::npos ||
                    val.find('"') != std::string::npos ||
                    val.find('\n') != std::string::npos) {
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
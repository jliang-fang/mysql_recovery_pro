/**
 * @file shared_tablespace_recovery.cpp
 * @brief Shared tablespace recovery implementations (Phase 5e)
 *
 * Streaming/indexed scan with memory limits, page-bundle manifest,
 * checksum verification, and zero-page ban.
 */

#include "dbrecovery/innodb/shared_tablespace_recovery.hpp"
#include "dbrecovery/innodb/btree.hpp"

#include <iostream>
#include <algorithm>
#include <set>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <nlohmann/json.hpp>
#include <cstring>

namespace dbrecovery {
namespace innodb {

// ==================== PageBundleManifest ====================

std::string PageBundleManifest::verdict_string() const {
    switch (verdict) {
        case ImportabilityVerdict::IMPORTABLE:           return "IMPORTABLE";
        case ImportabilityVerdict::HAS_HOLES:            return "HAS_HOLES";
        case ImportabilityVerdict::IDENTITY_MISMATCH:    return "IDENTITY_MISMATCH";
        case ImportabilityVerdict::CHECKSUM_FAILURES:    return "CHECKSUM_FAILURES";
        case ImportabilityVerdict::MULTIPLE_SPACES:      return "MULTIPLE_SPACES";
        case ImportabilityVerdict::UNVERIFIABLE:         return "UNVERIFIABLE";
        default:                                          return "UNKNOWN";
    }
}

std::string PageBundleManifest::to_json() const {
    nlohmann::json j;
    j["total_file_pages"] = total_file_pages;
    j["pages_scanned"] = pages_scanned;
    j["pages_with_valid_checksum"] = pages_with_valid_checksum;
    j["pages_with_invalid_checksum"] = pages_with_invalid_checksum;
    j["pages_identity_matched"] = pages_identity_matched;
    j["pages_identity_mismatched"] = pages_identity_mismatched;
    j["holes_found"] = holes_found;
    j["mismatches_found"] = mismatches_found;
    j["file_size_bytes"] = file_size_bytes;
    j["page_size"] = page_size;
    j["verdict"] = verdict_string();

    nlohmann::json spaces_json = nlohmann::json::object();
    for (const auto& [spid, count] : pages_per_space) {
        spaces_json[std::to_string(spid)] = count;
    }
    j["pages_per_space"] = spaces_json;

    nlohmann::json entries_json = nlohmann::json::array();
    for (const auto& entry : entries) {
        nlohmann::json e;
        e["page_no"] = entry.page_no;
        e["space_id"] = entry.space_id;
        e["page_type"] = entry.page_type;
        e["page_type_name"] = entry.page_type_name;
        e["checksum_valid"] = entry.checksum_valid;
        e["checksum_algorithm"] = static_cast<uint8_t>(entry.checksum_algorithm);
        e["identity_matched"] = entry.identity_matched;
        entries_json.push_back(e);
    }
    j["entries"] = entries_json;

    return j.dump(2);
}

// ==================== SharedTablespaceRecovery ====================

SharedTablespaceRecovery::SharedTablespaceRecovery(size_t page_size)
    : page_size_(page_size) {}

void SharedTablespaceRecovery::set_tablespace_path(const std::string& path) {
    ts_path_ = path;
}

void SharedTablespaceRecovery::set_output_dir(const std::string& dir) {
    output_dir_ = dir;
}

void SharedTablespaceRecovery::set_scan_config(const SharedTablespaceScanConfig& config) {
    scan_config_ = config;
}

const SharedTablespaceStats& SharedTablespaceRecovery::get_stats() const {
    return stats_;
}

bool SharedTablespaceRecovery::read_page(std::ifstream& file, uint32_t page_no,
                                          std::vector<uint8_t>& page_buf) const {
    file.seekg(static_cast<std::streamoff>(page_no) * page_size_, std::ios::beg);
    if (!file) return false;
    file.read(reinterpret_cast<char*>(page_buf.data()), page_size_);
    return file.gcount() == static_cast<std::streamsize>(page_size_);
}

PageBundleManifest SharedTablespaceRecovery::scan_pages(uint32_t filter_space_id) {
    PageBundleManifest manifest;
    manifest.page_size = static_cast<uint32_t>(page_size_);

    if (ts_path_.empty()) return manifest;

    std::ifstream file(ts_path_, std::ios::binary | std::ios::ate);
    if (!file) return manifest;

    manifest.file_size_bytes = static_cast<uint64_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    uint64_t total_pages = manifest.file_size_bytes / page_size_;
    manifest.total_file_pages = total_pages;

    // Memory budget: track how many pages we can keep in the manifest
    uint64_t max_entries = scan_config_.max_memory_bytes /
        (sizeof(PageBundleEntry) + 64);  // 64 bytes slop per entry
    bool truncated = false;

    std::vector<uint8_t> page_buf(page_size_);
    uint32_t prev_page_no = 0;
    bool first_page = true;

    for (uint32_t pg = 0; pg < total_pages; pg++) {
        if (!read_page(file, pg, page_buf)) break;

        manifest.pages_scanned++;

        // Read FIL header fields
        uint32_t space_id = read_be<uint32_t>(page_buf.data(), FIL_PAGE_SPACE_ID);
        uint16_t page_type = read_be<uint16_t>(page_buf.data(), FIL_PAGE_TYPE_ABS);
        uint32_t fil_page_no = read_be<uint32_t>(page_buf.data(), FIL_PAGE_OFFSET);

        // Detect holes: the FIL page_no in the header should match the file position
        if (!first_page && fil_page_no != prev_page_no + 1) {
            manifest.holes_found++;
        }
        prev_page_no = fil_page_no;
        first_page = false;

        // Checksum verification
        ChecksumValidationResult csr{false, 0, 0, 0, ChecksumAlgo::NONE, ""};
        if (scan_config_.verify_checksums) {
            csr = validate_page_checksum(page_buf.data(), page_size_, false);
        } else {
            csr.is_valid = true;
        }

        if (csr.is_valid) {
            manifest.pages_with_valid_checksum++;
        } else {
            manifest.pages_with_invalid_checksum++;
        }

        // Identity match
        bool identity_matched = (filter_space_id == 0 || space_id == filter_space_id);
        if (identity_matched) {
            manifest.pages_identity_matched++;
        } else {
            manifest.pages_identity_mismatched++;
        }

        // Track space_id distribution
        manifest.pages_per_space[space_id]++;

        // Build entry (truncate if over memory budget)
        if (!truncated) {
            if (manifest.entries.size() < max_entries) {
                PageBundleEntry entry;
                entry.page_no = pg;
                entry.space_id = space_id;
                entry.page_type = page_type;
                entry.page_type_name = page_type_to_string(page_type);
                entry.checksum_valid = csr.is_valid;
                entry.checksum_algorithm = csr.detected_algorithm;
                entry.identity_matched = identity_matched;
                manifest.entries.push_back(entry);
            } else {
                truncated = true;
            }
        }

        // Detect mismatches: identity mismatch (non-zero space_id mismatch)
        if (filter_space_id != 0 && space_id != 0 && space_id != filter_space_id) {
            manifest.mismatches_found++;
        }
    }

    // Determine importability verdict
    manifest.mismatches_found = manifest.pages_identity_mismatched;
    if (manifest.mismatches_found > 0 && manifest.holes_found > 0) {
        manifest.verdict = ImportabilityVerdict::MULTIPLE_SPACES;
    } else if (manifest.mismatches_found > 0) {
        manifest.verdict = ImportabilityVerdict::IDENTITY_MISMATCH;
    } else if (manifest.pages_with_invalid_checksum > 0) {
        manifest.verdict = ImportabilityVerdict::CHECKSUM_FAILURES;
    } else if (manifest.holes_found > 0) {
        manifest.verdict = ImportabilityVerdict::HAS_HOLES;
    } else if (manifest.pages_scanned > 0) {
        manifest.verdict = ImportabilityVerdict::IMPORTABLE;
    } else {
        manifest.verdict = ImportabilityVerdict::UNVERIFIABLE;
    }

    stats_.total_pages_scanned = manifest.pages_scanned;
    return manifest;
}

std::vector<TableIdentity> SharedTablespaceRecovery::list_tables() {
    std::vector<TableIdentity> tables;
    if (ts_path_.empty()) return tables;

    auto sdi_entries = extract_sdi_entries();
    for (const auto& entry : sdi_entries) {
        TableIdentity id;
        id.schema_name = entry.schema_name;
        id.table_name = entry.table_name;
        id.space_id = entry.space_id;
        id.table_id = entry.table_id;
        tables.push_back(id);
    }
    stats_.tables_found = tables.size();
    return tables;
}

bool SharedTablespaceRecovery::extract_table(uint32_t target_space_id,
                                              const std::string& output_ibd_path) {
    auto start = std::chrono::high_resolution_clock::now();

    if (ts_path_.empty()) return false;

    std::ifstream file(ts_path_, std::ios::binary | std::ios::ate);
    if (!file) return false;

    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    uint64_t total_pages = file_size / page_size_;
    std::vector<uint8_t> page_buf(page_size_);

    // Phase 5e: streaming scan, no page cache, collect matched pages
    // with their page numbers for ordered output
    std::map<uint32_t, std::vector<uint8_t>> matched_pages;
    uint64_t max_matched_pages = scan_config_.max_memory_bytes / page_size_;
    bool memory_limit_reached = false;

    for (uint32_t pg = 0; pg < total_pages; pg++) {
        file.seekg(static_cast<std::streamoff>(pg) * page_size_, std::ios::beg);
        file.read(reinterpret_cast<char*>(page_buf.data()), page_size_);
        if (file.gcount() < static_cast<std::streamsize>(page_size_)) break;

        uint32_t space_id = read_be<uint32_t>(page_buf.data(), FIL_PAGE_SPACE_ID);
        if (space_id != target_space_id) continue;

        if (matched_pages.size() >= max_matched_pages) {
            memory_limit_reached = true;
            break;
        }

        uint16_t page_type = read_be<uint16_t>(page_buf.data(), FIL_PAGE_TYPE_ABS);
        if (page_type == FIL_PAGE_INDEX || page_type == FIL_PAGE_SDI ||
            page_type == FIL_PAGE_SDI_ZIP || pg == 0 || pg == 1) {
            matched_pages[pg] = page_buf;
            if (page_type == FIL_PAGE_INDEX || page_type == FIL_PAGE_SDI) {
                stats_.data_pages_found++;
            }
        }
    }

    if (matched_pages.empty()) {
        std::cerr << "[WARN] No pages found for space_id=" << target_space_id << "\n";
        return false;
    }

    // Phase 5e: Write only actual pages (NO zero-filled holes).
    // The output is a sparse bundle with a companion manifest.
    {
        // Write manifest next to the output file
        std::string manifest_path = output_ibd_path + ".manifest.json";
        PageBundleManifest manifest;
        manifest.page_size = static_cast<uint32_t>(page_size_);
        manifest.file_size_bytes = file_size;
        manifest.total_file_pages = total_pages;

        std::ofstream out(output_ibd_path, std::ios::binary);
        if (!out) {
            std::cerr << "[ERROR] Cannot write " << output_ibd_path << "\n";
            return false;
        }

        // Track page sequence for hole detection
        uint32_t prev_page = 0;
        bool first = true;

        for (const auto& [pg, data] : matched_pages) {
            // Detect holes in the matched page sequence
            if (!first && pg != prev_page + 1) {
                manifest.holes_found++;
            }
            prev_page = pg;
            first = false;

            out.write(reinterpret_cast<const char*>(data.data()), page_size_);
            if (!out) {
                std::cerr << "[ERROR] Write failed at page " << pg << "\n";
                return false;
            }

            // Verify checksum on written page
            auto csr = validate_page_checksum(data.data(), page_size_, false);
            PageBundleEntry entry;
            entry.page_no = pg;
            entry.space_id = read_be<uint32_t>(data.data(), FIL_PAGE_SPACE_ID);
            entry.page_type = read_be<uint16_t>(data.data(), FIL_PAGE_TYPE_ABS);
            entry.page_type_name = page_type_to_string(entry.page_type);
            entry.checksum_valid = csr.is_valid;
            entry.checksum_algorithm = csr.detected_algorithm;
            entry.identity_matched = true;
            manifest.entries.push_back(entry);

            if (csr.is_valid) manifest.pages_with_valid_checksum++;
            else manifest.pages_with_invalid_checksum++;
        }

        manifest.pages_scanned = matched_pages.size();
        manifest.pages_identity_matched = matched_pages.size();

        // Verdict: holes are expected for shared tablespace extraction
        if (manifest.holes_found > 0) {
            manifest.verdict = ImportabilityVerdict::HAS_HOLES;
        } else if (manifest.pages_with_invalid_checksum > 0) {
            manifest.verdict = ImportabilityVerdict::CHECKSUM_FAILURES;
        } else {
            manifest.verdict = ImportabilityVerdict::IMPORTABLE;
        }

        // Write manifest
        std::ofstream mf(manifest_path);
        if (mf) {
            mf << manifest.to_json() << "\n";
        }

        out.close();

        if (memory_limit_reached) {
            std::cerr << "[WARN] Memory limit reached, only "
                      << matched_pages.size() << " pages extracted\n";
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    stats_.recovery_time_ms = std::chrono::duration<double, std::milli>(end - start).count();

    return true;
}

std::vector<std::map<std::string, std::string>> SharedTablespaceRecovery::recover_records(
    uint32_t target_space_id, const catalog::TableSchema& schema) {

    std::vector<std::map<std::string, std::string>> all_records;

    std::string tmp_ibd = output_dir_ + "/_extracted_space_" +
                          std::to_string(target_space_id) + ".ibd";
    if (!extract_table(target_space_id, tmp_ibd)) {
        return all_records;
    }

    // Phase 5e: re-read the extracted sparse IBD with streaming scan
    std::ifstream file(tmp_ibd, std::ios::binary | std::ios::ate);
    if (!file) return all_records;

    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    uint64_t total_pages = file_size / page_size_;
    std::vector<uint8_t> page_buf(page_size_);

    for (uint32_t pg = 0; pg < total_pages; pg++) {
        if (!read_page(file, pg, page_buf)) break;

        uint16_t page_type = read_be<uint16_t>(page_buf.data(), FIL_PAGE_TYPE_ABS);
        if (page_type != FIL_PAGE_INDEX) continue;

        BPlusTreeParser parser(page_buf);
        if (!parser.parse()) continue;

        auto records = parser.parse_records(schema);
        all_records.insert(all_records.end(), records.begin(), records.end());
    }

    stats_.records_recovered = all_records.size();
    return all_records;
}

std::vector<SharedTablespaceRecovery::SDIEntry> SharedTablespaceRecovery::extract_sdi_entries() {
    std::vector<SDIEntry> entries;

    if (ts_path_.empty()) return entries;

    std::ifstream file(ts_path_, std::ios::binary | std::ios::ate);
    if (!file) return entries;

    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    uint64_t total_pages = file_size / page_size_;
    std::vector<uint8_t> page_buf(page_size_);

    for (uint32_t pg = 0; pg < total_pages; pg++) {
        if (!read_page(file, pg, page_buf)) break;

        uint16_t page_type = read_be<uint16_t>(page_buf.data(), FIL_PAGE_TYPE_ABS);
        if (page_type != FIL_PAGE_SDI && page_type != FIL_PAGE_SDI_ZIP) continue;

        stats_.sdi_pages_found++;

        SDIExtractor extractor;
        auto sdi_results = extractor.extract_from_page(
            page_buf.data(), page_size_);
        if (sdi_results.empty()) continue;

        for (const auto& sdi : sdi_results) {
            if (!sdi.is_valid || sdi.sdi_json.empty()) continue;

            auto j = nlohmann::json::parse(sdi.sdi_json, nullptr, false);
            if (j.is_discarded()) continue;

            if (!j.contains("dd_object") || !j["dd_object"].contains("name")) continue;

            SDIEntry entry;
            entry.table_name = j["dd_object"]["name"].get<std::string>();
            entry.schema_name = j.value("schema_name", "");
            entry.space_id = j.value("space_id", 0);
            entry.table_id = j["dd_object"].value("id", 0);

            if (!entry.table_name.empty() && entry.space_id > 0) {
                entries.push_back(entry);
            }
        }
    }

    return entries;
}

} // namespace innodb
} // namespace dbrecovery
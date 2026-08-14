/**
 * @file shared_tablespace_recovery.cpp
 * @brief Shared tablespace recovery implementations
 *
 * Extracted from shared_tablespace_recovery.hpp for better compilation.
 * Note: The header is fully inline, so this file contains the extracted
 * method implementations with the class definition moved here.
 * The header retains only declarations.
 */

#include "dbrecovery/innodb/shared_tablespace_recovery.hpp"
#include "dbrecovery/innodb/btree.hpp"

#include <fstream>
#include <iostream>
#include <algorithm>
#include <set>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <nlohmann/json.hpp>

namespace dbrecovery {
namespace innodb {

SharedTablespaceRecovery::SharedTablespaceRecovery(size_t page_size)
    : page_size_(page_size), cache_populated_(false) {}

void SharedTablespaceRecovery::set_tablespace_path(const std::string& path) {
    ts_path_ = path;
}

void SharedTablespaceRecovery::set_output_dir(const std::string& dir) {
    output_dir_ = dir;
}

const SharedTablespaceStats& SharedTablespaceRecovery::get_stats() const {
    return stats_;
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

void SharedTablespaceRecovery::ensure_cache_populated() {
    if (cache_populated_) return;

    std::ifstream file(ts_path_, std::ios::binary | std::ios::ate);
    if (!file) return;

    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    uint32_t total_pages = static_cast<uint32_t>(file_size / page_size_);
    std::vector<uint8_t> page_buf(page_size_);

    for (uint32_t pg = 0; pg < total_pages; pg++) {
        file.seekg(static_cast<std::streamoff>(pg) * page_size_, std::ios::beg);
        file.read(reinterpret_cast<char*>(page_buf.data()), page_size_);
        if (file.gcount() < static_cast<std::streamsize>(page_size_)) break;
        page_cache_[pg] = page_buf;
    }

    stats_.total_pages_scanned = total_pages;
    cache_populated_ = true;
}

bool SharedTablespaceRecovery::extract_table(uint32_t target_space_id, const std::string& output_ibd_path) {
    auto start = std::chrono::high_resolution_clock::now();

    ensure_cache_populated();

    std::map<uint32_t, std::vector<uint8_t>> pages_by_no;

    for (const auto& [pg, page_buf] : page_cache_) {
        uint32_t space_id = read_le<uint32_t>(page_buf.data(), 34);
        if (space_id != target_space_id) continue;

        uint32_t page_no = read_le<uint32_t>(page_buf.data(), 4);
        uint16_t page_type = read_be<uint16_t>(page_buf.data(), 24);

        if (page_type == FIL_PAGE_INDEX || page_type == FIL_PAGE_SDI ||
            page_type == FIL_PAGE_SDI_ZIP || page_no == 0 || page_no == 1 ||
            page_no == 2 || page_no == 3) {
            pages_by_no[page_no] = page_buf;
            if (page_type == FIL_PAGE_INDEX || page_type == FIL_PAGE_SDI) {
                stats_.data_pages_found++;
            }
        }
    }

    if (pages_by_no.empty()) {
        std::cerr << "[WARN] No pages found for space_id=" << target_space_id << "\n";
        return false;
    }

    if (!pages_by_no.empty()) {
        uint32_t max_page = pages_by_no.rbegin()->first;
        std::ofstream out(output_ibd_path, std::ios::binary);
        std::vector<uint8_t> zero_page(page_size_, 0);

        for (uint32_t pg = 0; pg <= max_page; pg++) {
            auto it = pages_by_no.find(pg);
            if (it != pages_by_no.end()) {
                out.write(reinterpret_cast<const char*>(it->second.data()), page_size_);
            } else {
                out.write(reinterpret_cast<const char*>(zero_page.data()), page_size_);
            }
        }
        out.close();
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

    ensure_cache_populated();

    for (const auto& [pg, page_buf] : page_cache_) {
        uint32_t space_id = read_le<uint32_t>(page_buf.data(), 34);
        if (space_id != target_space_id) continue;

        uint16_t page_type = read_be<uint16_t>(page_buf.data(), 24);
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

    ensure_cache_populated();

    for (const auto& [pg, page_buf] : page_cache_) {
        uint16_t page_type = read_be<uint16_t>(page_buf.data(), 24);
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
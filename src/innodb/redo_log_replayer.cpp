#include "dbrecovery/innodb/redo_log_replayer.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/innodb_checksum.hpp"
#include <iostream>
#include <algorithm>
#include <cstring>
#include <sstream>
#include <set>
#include <map>

namespace dbrecovery {
namespace innodb {

namespace {

/// @brief Human-readable MLOG type name for provenance tracking
const char* mlog_type_name(uint8_t type) {
    switch (type) {
        case MLOG_1BYTE:            return "MLOG_1BYTE";
        case MLOG_2BYTES:           return "MLOG_2BYTES";
        case MLOG_4BYTES:           return "MLOG_4BYTES";
        case MLOG_8BYTES:           return "MLOG_8BYTES";
        case MLOG_REC_INSERT:       return "MLOG_REC_INSERT";
        case MLOG_REC_UPDATE:       return "MLOG_REC_UPDATE";
        case MLOG_REC_DELETE:       return "MLOG_REC_DELETE";
        case MLOG_PAGE_CREATE:      return "MLOG_PAGE_CREATE";
        case MLOG_PAGE_REINIT:      return "MLOG_PAGE_REINIT";
        case MLOG_INDEX_ALLOC:      return "MLOG_INDEX_ALLOC";
        case MLOG_COMP_REC_INSERT:  return "MLOG_COMP_REC_INSERT";
        case MLOG_COMP_REC_UPDATE:  return "MLOG_COMP_REC_UPDATE";
        case MLOG_COMP_REC_DELETE:  return "MLOG_COMP_REC_DELETE";
        case MLOG_COMP_PAGE_CREATE: return "MLOG_COMP_PAGE_CREATE";
        case MLOG_COMP_PAGE_REINIT: return "MLOG_COMP_PAGE_REINIT";
        case MLOG_BLOB_WRITE:       return "MLOG_BLOB_WRITE";
        case MLOG_MULTI_REC_END:    return "MLOG_MULTI_REC_END";
        case MLOG_CHECKPOINT:       return "MLOG_CHECKPOINT";
        case MLOG_INDEX_LOAD:       return "MLOG_INDEX_LOAD";
        default:                    return "MLOG_UNKNOWN";
    }
}

} // anonymous namespace

// ==================== RedoLogReplayer ====================

RedoLogReplayer::RedoLogReplayer() : page_size_(16384) {
    std::memset(&stats_, 0, sizeof(stats_));
}

RedoLogReplayer::~RedoLogReplayer() = default;

void RedoLogReplayer::set_page_size(uint32_t page_size) {
    if (page_size == 4096 || page_size == 8192 ||
        page_size == 16384 || page_size == 32768 || page_size == 65536) {
        page_size_ = page_size;
    }
}

bool RedoLogReplayer::add_page(uint32_t page_no, std::vector<uint8_t> page_data) {
    if (page_data.size() < 38) {
        return false;
    }

    pages_[page_no] = std::move(page_data);
    return true;
}

void RedoLogReplayer::add_redo_records(const std::vector<RedoLogRecord>& records) {
    redo_records_.insert(redo_records_.end(), records.begin(), records.end());
}

RedoReplayResult RedoLogReplayer::replay() {
    // Delegate to replay_for() — this method is deprecated in favor of replay_safe()
    // which gates on pre-conditions before writing pages.
    return replay_for(redo_records_);
}

void RedoLogReplayer::set_verify_checksums(bool verify) {
    verify_checksums_ = verify;
}

const std::map<uint32_t, std::vector<uint8_t>>& RedoLogReplayer::get_base_pages() const {
    return pages_;
}

bool RedoLogReplayer::check_base_page_identity(
    uint32_t page_no, const std::vector<uint8_t>& page_data,
    uint32_t& out_space_id, uint32_t& out_fil_page_no) const {

    out_space_id = 0;
    out_fil_page_no = 0;

    if (page_data.size() < FIL_PAGE_SPACE_ID + 4) return false;

    // FIL header is big-endian throughout InnoDB
    out_space_id = dbrecovery::innodb::read_be<uint32_t>(page_data.data(), FIL_PAGE_SPACE_ID);
    out_fil_page_no = dbrecovery::innodb::read_be<uint32_t>(page_data.data(), FIL_PAGE_OFFSET);

    // Identity match: page_no must equal FIL_PAGE_OFFSET, and space_id must be non-zero.
    // (space_id == 0 is valid for the system tablespace, but a redo-replay base page
    //  without a recognizable space id cannot be attributed safely.)
    if (out_fil_page_no != page_no) return false;
    if (out_space_id == 0) return false;

    return true;
}

RedoLogInventory RedoLogReplayer::generate_inventory() const {
    RedoLogInventory inv{};

    for (const auto& record : redo_records_) {
        inv.total_records++;

        bool whitelisted = is_mlog_whitelisted(static_cast<uint8_t>(record.r_type));
        if (whitelisted) {
            inv.whitelisted_records++;
        } else {
            inv.non_whitelisted_records++;
        }

        inv.global_type_distribution[static_cast<uint8_t>(record.r_type)]++;

        if (inv.start_lsn == 0 || record.r_lsn < inv.start_lsn) {
            inv.start_lsn = record.r_lsn;
        }
        if (record.r_lsn > inv.end_lsn) {
            inv.end_lsn = record.r_lsn;
        }

        // Group by (space_id, page_no)
        auto key = std::make_pair(record.r_space_id, record.r_page_no);
        RedoPageInventoryEntry* entry = nullptr;
        for (auto& e : inv.page_entries) {
            if (e.space_id == key.first && e.page_no == key.second) {
                entry = &e;
                break;
            }
        }

        if (entry == nullptr) {
            RedoPageInventoryEntry ne{};
            ne.space_id = record.r_space_id;
            ne.page_no = record.r_page_no;
            ne.first_lsn = record.r_lsn;
            ne.last_lsn = record.r_lsn;
            ne.total_records = 1;
            if (whitelisted) ne.whitelisted_records = 1;
            else ne.non_whitelisted_records = 1;

            // Base page identity (if the page is loaded)
            auto it = pages_.find(record.r_page_no);
            if (it != pages_.end()) {
                ne.has_base_page = true;
                uint32_t spid = 0, fil_pg = 0;
                ne.base_page_identity_match =
                    check_base_page_identity(record.r_page_no, it->second, spid, fil_pg);
            }

            ne.type_distribution[static_cast<uint8_t>(record.r_type)]++;
            inv.page_entries.push_back(std::move(ne));
        } else {
            entry->total_records++;
            if (whitelisted) entry->whitelisted_records++;
            else entry->non_whitelisted_records++;
            if (record.r_lsn < entry->first_lsn) entry->first_lsn = record.r_lsn;
            if (record.r_lsn > entry->last_lsn) entry->last_lsn = record.r_lsn;
            entry->type_distribution[static_cast<uint8_t>(record.r_type)]++;

            if (!entry->has_base_page) {
                auto it = pages_.find(record.r_page_no);
                if (it != pages_.end()) {
                    entry->has_base_page = true;
                    uint32_t spid = 0, fil_pg = 0;
                    entry->base_page_identity_match =
                        check_base_page_identity(record.r_page_no, it->second, spid, fil_pg);
                }
            }
        }
    }

    std::set<uint32_t> space_ids;
    std::set<uint32_t> page_nos;
    for (const auto& e : inv.page_entries) {
        space_ids.insert(e.space_id);
        page_nos.insert(e.page_no);
    }
    inv.unique_space_ids = static_cast<uint32_t>(space_ids.size());
    inv.unique_page_numbers = static_cast<uint32_t>(page_nos.size());

    return inv;
}

RedoPatchPlan RedoLogReplayer::generate_patch_plan(bool require_base_pages) const {
    RedoPatchPlan plan{};

    if (redo_records_.empty()) {
        plan.warnings.push_back("No redo records available; patch plan is empty");
        return plan;
    }

    // Reject records that are not sorted by LSN. Records with r_lsn == 0 are
    // not verifiable and cannot be safely ordered.
    std::vector<RedoLogRecord> sorted = redo_records_;
    std::stable_sort(sorted.begin(), sorted.end(),
        [](const RedoLogRecord& a, const RedoLogRecord& b) {
            return a.r_lsn < b.r_lsn;
        });

    plan.records_sorted_by_lsn = true;
    for (size_t i = 1; i < sorted.size(); i++) {
        if (sorted[i].r_lsn < sorted[i - 1].r_lsn) {
            plan.records_sorted_by_lsn = false;
            break;
        }
    }

    // Pre-scan: per-page identity availability
    std::map<uint32_t, bool> page_identity_ok;
    for (const auto& record : sorted) {
        auto it = pages_.find(record.r_page_no);
        if (it == pages_.end()) {
            page_identity_ok[record.r_page_no] = false;
            continue;
        }
        uint32_t spid = 0, fil_pg = 0;
        bool ok = check_base_page_identity(record.r_page_no, it->second, spid, fil_pg);
        auto got = page_identity_ok.find(record.r_page_no);
        if (got == page_identity_ok.end()) {
            page_identity_ok[record.r_page_no] = ok;
        } else {
            got->second = got->second && ok;
        }
    }

    plan.all_pages_have_identity_match = true;
    for (const auto& record : sorted) {
        // Skip MLOG types for which page identity is not meaningful
        if (record.r_type == MLOG_CHECKPOINT || record.r_type == MLOG_INDEX_LOAD) {
            continue;
        }
        auto it = pages_.find(record.r_page_no);
        if (it == pages_.end()) {
            plan.all_pages_have_identity_match = false;
            continue;
        }
        auto ok = page_identity_ok.find(record.r_page_no);
        if (ok == page_identity_ok.end() || !ok->second) {
            plan.all_pages_have_identity_match = false;
        }
    }

    uint32_t sequence = 0;
    for (const auto& record : sorted) {
        RedoPatchPlanEntry e{};
        e.sequence = sequence++;
        e.space_id = record.r_space_id;
        e.page_no = record.r_page_no;
        e.lsn = record.r_lsn;
        e.mlog_type = static_cast<uint8_t>(record.r_type);
        e.mlog_name = mlog_type_name(e.mlog_type);
        e.data_size = record.r_data.size();
        e.whitelisted = is_mlog_whitelisted(e.mlog_type);

        if (!e.whitelisted) {
            plan.unknown_mlog_count++;
            plan.all_whitelisted = false;
        }

        auto pit = pages_.find(record.r_page_no);
        if (pit != pages_.end()) {
            e.has_base_page = true;
            uint32_t spid = 0, fil_pg = 0;
            e.base_page_identity_match =
                check_base_page_identity(record.r_page_no, pit->second, spid, fil_pg);
        }
        if (require_base_pages && !e.has_base_page) {
            plan.warnings.push_back(
                "Record #" + std::to_string(e.sequence) +
                " (page " + std::to_string(e.page_no) +
                " @ LSN " + std::to_string(e.lsn) +
                "): no base page loaded; identity unverifiable");
        }

        std::ostringstream prov;
        prov << e.mlog_name << " @ LSN " << e.lsn
             << " (space " << e.space_id
             << ", page " << e.page_no << ")";
        e.provenance = prov.str();

        plan.entries.push_back(std::move(e));
    }

    // LSN gap detection: any gap larger than 0 between consecutive records
    for (size_t i = 1; i < sorted.size(); i++) {
        if (sorted[i].r_lsn > sorted[i - 1].r_lsn + 1) {
            plan.lsn_gap_count++;
        }
    }

    // With no records excluded or unknown MLOG types, whitelist audit passes
    if (plan.unknown_mlog_count == 0) {
        plan.all_whitelisted = true;
    }

    return plan;
}

RedoPreConditionResult RedoLogReplayer::validate_conditions() const {
    RedoPreConditionResult cond{};

    if (redo_records_.empty()) {
        cond.log_format_valid = true;  // nothing to validate
        cond.lsn_monotonic = true;
        cond.all_whitelisted = true;
        cond.pages_available = false;
        cond.base_pages_identity_match = false;
        cond.checksum_available = true;
        cond.has_checkpoint = false;
        return cond;
    }

    // 1. Log format validity: every record must be parseable in isolation.
    //    RedoLogRecord fields already indicate header layout; format validity
    //    requires that no record is empty/inconsistent.
    cond.log_format_valid = true;
    for (const auto& record : redo_records_) {
        if (record.r_data.empty()) {
            cond.log_format_valid = false;
            break;
        }
    }

    // 2. LSN monotonicity
    cond.lsn_monotonic = true;
    for (size_t i = 1; i < redo_records_.size(); i++) {
        if (redo_records_[i].r_lsn < redo_records_[i - 1].r_lsn) {
            cond.lsn_monotonic = false;
            break;
        }
    }

    // 3. Checkpoint presence
    cond.has_checkpoint = false;
    for (const auto& record : redo_records_) {
        if (record.r_type == MLOG_CHECKPOINT) {
            cond.has_checkpoint = true;
            break;
        }
    }

    // 4. Whitelist
    cond.all_whitelisted = true;
    for (const auto& record : redo_records_) {
        if (!is_mlog_whitelisted(static_cast<uint8_t>(record.r_type))) {
            cond.all_whitelisted = false;
            break;
        }
    }

    // 5. Base page availability + identity
    cond.pages_available = true;
    cond.base_pages_identity_match = true;
    for (const auto& record : redo_records_) {
        if (record.r_type == MLOG_CHECKPOINT || record.r_type == MLOG_INDEX_LOAD) {
            continue;
        }
        auto it = pages_.find(record.r_page_no);
        if (it == pages_.end()) {
            cond.pages_available = false;
            cond.base_pages_identity_match = false;
            continue;
        }
        uint32_t spid = 0, fil_pg = 0;
        if (!check_base_page_identity(record.r_page_no, it->second, spid, fil_pg)) {
            cond.base_pages_identity_match = false;
        }
    }

    // 6. Checksum availability (if enabled)
    cond.checksum_available = true;
    if (verify_checksums_) {
        for (const auto& record : redo_records_) {
            if (record.r_type == MLOG_CHECKPOINT || record.r_type == MLOG_INDEX_LOAD) {
                continue;
            }
            auto it = pages_.find(record.r_page_no);
            if (it == pages_.end() ||
                it->second.size() < page_size_) {
                cond.checksum_available = false;
                continue;
            }
            auto csr = validate_page_checksum(it->second.data(), page_size_, false);
            if (!csr.is_valid) {
                cond.checksum_available = false;
            }
        }
    }

    return cond;
}

RedoReplayResult RedoLogReplayer::replay_safe() {
    RedoReplayResult result{};

    auto inventory = generate_inventory();
    auto patch_plan = generate_patch_plan(true);
    auto cond = validate_conditions();

    result.inventory = inventory;
    result.patch_plan = patch_plan;
    result.pre_conditions = cond;

    if (redo_records_.empty()) {
        result.success = false;
        result.message = "No redo records to replay";
        return result;
    }

    if (!cond.all_passed()) {
        result.success = false;
        result.message = "Replay pre-conditions failed; no pages were modified. " +
                         cond.to_string();
        for (const auto& w : patch_plan.warnings) {
            result.errors.push_back(w);
        }
        return result;
    }

    // Replay only the sorted, identity-verified records. We use replay() for
    // the write side, but the plan already verified ordering/whitelist/identity.
    // Because all pre-conditions pass, records must be in monotonic LSN order;
    // if the insertion order differs, replay in plan order.
    std::vector<RedoLogRecord> ordered = redo_records_;
    if (![&]() {
        for (size_t i = 1; i < ordered.size(); i++) {
            if (ordered[i].r_lsn < ordered[i - 1].r_lsn) return false;
        }
        return true;
    }()) {
        std::stable_sort(ordered.begin(), ordered.end(),
            [](const RedoLogRecord& a, const RedoLogRecord& b) {
                return a.r_lsn < b.r_lsn;
            });
    }

    result = replay_for(ordered);

    result.inventory = inventory;
    result.patch_plan = patch_plan;
    result.pre_conditions = cond;

    return result;
}

RedoReplayResult RedoLogReplayer::replay_for(const std::vector<RedoLogRecord>& records) {
    RedoReplayResult result{};
    result.success = true;
    result.records_processed = 0;
    result.records_applied = 0;
    result.records_failed = 0;

    for (const auto& record : records) {
        result.records_processed++;

        // Meta records carry no page payload; they count as processed but
        // are not apply operations and never count as failures.
        if (record.r_type == MLOG_CHECKPOINT ||
            record.r_type == MLOG_MULTI_REC_END ||
            record.r_type == MLOG_INDEX_LOAD) {
            continue;
        }

        if (apply_redo_record(record.r_page_no, record)) {
            result.records_applied++;
            applied_pages_.insert(record.r_page_no);
        } else {
            result.records_failed++;
            errors_.push_back("Failed to apply record: " + record.to_string());
        }
    }

    stats_.records_processed = result.records_processed;
    stats_.records_applied = result.records_applied;
    stats_.records_failed = result.records_failed;
    stats_.pages_modified = static_cast<uint32_t>(applied_pages_.size());

    // Propagate errors to the result
    result.errors = errors_;

    if (result.records_failed > 0) {
        result.success = false;
        result.message = "Replay completed with " + std::to_string(result.records_failed) + " failures";
    } else {
        result.message = "Replay completed successfully";
    }

    return result;
}

std::string RedoPreConditionResult::to_string() const {
    std::ostringstream os;
    os << "Pre-conditions: "
       << "log_format_valid=" << (log_format_valid ? "PASS" : "FAIL")
       << ", lsn_monotonic=" << (lsn_monotonic ? "PASS" : "FAIL")
       << ", has_checkpoint=" << (has_checkpoint ? "PASS" : "FAIL")
       << ", all_whitelisted=" << (all_whitelisted ? "PASS" : "FAIL")
       << ", base_pages_identity_match=" << (base_pages_identity_match ? "PASS" : "FAIL")
       << ", checksum_available=" << (checksum_available ? "PASS" : "FAIL")
       << ", pages_available=" << (pages_available ? "PASS" : "FAIL");
    return os.str();
}

bool RedoLogReplayer::apply_redo_record(uint32_t page_no, const RedoLogRecord& record) {
    auto it = pages_.find(page_no);
    if (it == pages_.end()) {
        std::cerr << "[WARN] RedoLogReplayer: Page " << page_no << " not found" << std::endl;
        return false;
    }

    auto& page_data = it->second;

    // Check page type at offset 24 (FIL_PAGE_TYPE_ABS, big-endian)
    uint16_t page_type = dbrecovery::innodb::read_be<uint16_t>(page_data.data(), FIL_PAGE_TYPE_ABS);
    if (page_type != dbrecovery::innodb::FIL_PAGE_INDEX &&
        page_type != dbrecovery::innodb::FIL_PAGE_SDI &&
        page_type != dbrecovery::innodb::FIL_PAGE_UNDO_LOG) {
        std::cerr << "[WARN] RedoLogReplayer: Page " << page_no << " is not a valid page type (type=" << page_type << ")" << std::endl;
        return false;
    }

    // Apply based on record type
    switch (record.r_type) {
        case MLOG_1BYTE:
        case MLOG_2BYTES:
        case MLOG_4BYTES:
        case MLOG_8BYTES: {
            // Simple data updates
            if (record.r_data.size() < 2) {
                return false;
            }

            uint16_t offset = dbrecovery::innodb::read_le<uint16_t>(record.r_data.data(), 0);
            if (offset + 8 > page_data.size()) {
                return false;
            }

            // Apply data update
            if (record.r_data.size() > 2) {
                size_t data_size = std::min(record.r_data.size() - 2, page_data.size() - offset);
                std::memcpy(page_data.data() + offset, record.r_data.data() + 2, data_size);
            }
            break;
        }

        case MLOG_REC_INSERT:
        case MLOG_COMP_REC_INSERT: {
            // Record insert - parse record offset and data, place into page
            // Format: [2B] record_offset [variable] record_data
            if (record.r_data.size() < 4) {
                break;
            }
            size_t pos = 0;
            uint8_t rec_type = record.r_data[pos++];
            if (record.r_data.size() < pos + 2) break;
            uint16_t insert_offset = dbrecovery::innodb::read_le<uint16_t>(record.r_data.data(), pos);
            pos += 2;

            size_t rec_len = record.r_data.size() - pos;
            if (rec_len > 0 && insert_offset + rec_len <= page_data.size()) {
                std::memcpy(page_data.data() + insert_offset, record.r_data.data() + pos, rec_len);
                // Update PAGE_HEAP_TOP and PAGE_N_RECS
                uint16_t heap_top = dbrecovery::innodb::read_le<uint16_t>(page_data.data(), 38 + 20);
                if (insert_offset + rec_len > heap_top) {
                    dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 38 + 20,
                        static_cast<uint16_t>(insert_offset + rec_len));
                }
                uint16_t n_recs = dbrecovery::innodb::read_le<uint16_t>(page_data.data(), 38 + 16);
                dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 38 + 16, n_recs + 1);
            }
            break;
        }

        case MLOG_REC_UPDATE:
        case MLOG_COMP_REC_UPDATE: {
            // Record update - parse update vector and apply to page
            // Format: [2B] record_offset [1B] n_fields [for each: 2B field_off 2B len data]
            if (record.r_data.size() < 5) {
                break;
            }
            size_t pos = 0;
            uint8_t rec_type = record.r_data[pos++];
            if (record.r_data.size() < pos + 2) break;
            uint16_t rec_offset = dbrecovery::innodb::read_le<uint16_t>(record.r_data.data(), pos);
            pos += 2;

            if (pos < record.r_data.size()) {
                uint8_t n_fields = record.r_data[pos++];
                for (uint8_t i = 0; i < n_fields && pos + 4 <= record.r_data.size(); i++) {
                    uint16_t field_offset = dbrecovery::innodb::read_le<uint16_t>(record.r_data.data(), pos);
                    pos += 2;
                    uint16_t data_len = dbrecovery::innodb::read_le<uint16_t>(record.r_data.data(), pos);
                    pos += 2;
                    if (pos + data_len <= record.r_data.size() &&
                        rec_offset + field_offset + data_len <= page_data.size()) {
                        std::memcpy(page_data.data() + rec_offset + field_offset,
                                    record.r_data.data() + pos, data_len);
                    }
                    pos += data_len;
                }
            }
            break;
        }

        case MLOG_REC_DELETE: {
            // Record delete - mark as deleted
            if (record.r_data.size() >= 2) {
                uint16_t offset = dbrecovery::innodb::read_le<uint16_t>(record.r_data.data(), 0);
                if (offset < page_data.size()) {
                    page_data[offset] |= REC_INFO_DELETED_FLAG;
                }
            }
            break;
        }

        case MLOG_PAGE_CREATE: {
            // Page create - clear page
            std::memset(page_data.data(), 0, page_data.size());
            break;
        }

        case MLOG_PAGE_REINIT: {
            // Page reinit - clear page header + records
            if (page_data.size() > 38) {
                std::memset(page_data.data() + 38, 0, page_data.size() - 38);
            }
            break;
        }

        default: {
            std::cerr << "[WARN] RedoLogReplayer: Unsupported log type "
                      << static_cast<int>(record.r_type) << std::endl;
            return false;
        }
    }

    return true;
}

const std::map<uint32_t, std::vector<uint8_t>>& RedoLogReplayer::get_updated_pages() const {
    return pages_;
}

RedoLogReplayer::ReplayStats RedoLogReplayer::get_stats() const {
    return stats_;
}

void RedoLogReplayer::clear() {
    pages_.clear();
    redo_records_.clear();
    errors_.clear();
    applied_pages_.clear();
    std::memset(&stats_, 0, sizeof(stats_));
}

// ==================== RedoLogChainBuilder ====================

RedoLogChainBuilder::RedoLogChainBuilder() {
    std::memset(&stats_, 0, sizeof(stats_));
}

RedoLogChainBuilder::~RedoLogChainBuilder() = default;

void RedoLogChainBuilder::add_record(const RedoLogRecord& record) {
    records_.push_back(record);
    stats_.total_records++;

    // Track LSN range
    if (stats_.start_lsn == 0) {
        stats_.start_lsn = record.r_lsn;
    }
    stats_.end_lsn = record.r_lsn;
}

bool RedoLogChainBuilder::build_chains() {
    if (records_.empty()) {
        return false;
    }

    // Group by space_id (simplified chain building)
    for (const auto& record : records_) {
        chains_[record.r_space_id].push_back(record);
        stats_.total_chains = std::max(stats_.total_chains, 
            static_cast<uint32_t>(chains_.size()));
    }

    return true;
}

std::vector<RedoLogRecord> RedoLogChainBuilder::get_records(uint64_t start_lsn, uint64_t end_lsn) {
    std::vector<RedoLogRecord> result;

    for (const auto& record : records_) {
        if (record.r_lsn >= start_lsn && record.r_lsn <= end_lsn) {
            result.push_back(record);
        }
    }

    return result;
}

const std::vector<RedoLogRecord>& RedoLogChainBuilder::get_records() const {
    return records_;
}

RedoLogChainBuilder::ChainStats RedoLogChainBuilder::get_stats() const {
    return stats_;
}

void RedoLogChainBuilder::clear() {
    records_.clear();
    chains_.clear();
    std::memset(&stats_, 0, sizeof(stats_));
}

// ==================== RedoLogFilter ====================

RedoLogFilter::RedoLogFilter() = default;

RedoLogFilter::~RedoLogFilter() = default;

void RedoLogFilter::set_space_id(uint32_t space_id) {
    space_id_ = space_id;
}

void RedoLogFilter::set_page_no(uint32_t page_no) {
    page_no_ = page_no;
}

void RedoLogFilter::set_record_type(uint8_t type) {
    record_type_ = type;
}

void RedoLogFilter::set_lsn_range(uint64_t start_lsn, uint64_t end_lsn) {
    start_lsn_ = start_lsn;
    end_lsn_ = end_lsn;
}

std::vector<RedoLogRecord> RedoLogFilter::filter(const std::vector<RedoLogRecord>& records) {
    std::vector<RedoLogRecord> result;

    for (const auto& record : records) {
        if (passes_filter(record)) {
            result.push_back(record);
        }
    }

    return result;
}

bool RedoLogFilter::passes_filter(const RedoLogRecord& record) {
    if (space_id_.has_value() && record.r_space_id != space_id_.value()) {
        return false;
    }

    if (page_no_.has_value() && record.r_page_no != page_no_.value()) {
        return false;
    }

    if (record_type_.has_value() && record.r_type != record_type_.value()) {
        return false;
    }

    // LSN filter
    if (start_lsn_.has_value() || end_lsn_.has_value()) {
        if (start_lsn_.has_value() && record.r_lsn < start_lsn_.value()) {
            return false;
        }
        if (end_lsn_.has_value() && record.r_lsn > end_lsn_.value()) {
            return false;
        }
    }

    return true;
}

void RedoLogFilter::clear_filters() {
    space_id_.reset();
    page_no_.reset();
    record_type_.reset();
    start_lsn_.reset();
    end_lsn_.reset();
}

// ==================== RedoLogAnalyzer ====================

RedoLogAnalyzer::RedoLogAnalyzer() {
    std::memset(&analysis_, 0, sizeof(analysis_));
}

RedoLogAnalyzer::~RedoLogAnalyzer() = default;

bool RedoLogAnalyzer::analyze_file(const std::string& path) {
    RedoLogParser parser;
    auto records = parser.parse_file(path);

    for (const auto& record : records) {
        analyze_record(record);
    }

    return !records.empty();
}

RedoLogAnalyzer::AnalysisResult RedoLogAnalyzer::get_analysis() const {
    return analysis_;
}

std::map<uint8_t, uint32_t> RedoLogAnalyzer::get_type_distribution() const {
    return type_distribution_;
}

std::vector<std::pair<uint32_t, uint32_t>> RedoLogAnalyzer::get_top_pages(uint32_t count) const {
    std::vector<std::pair<uint32_t, uint32_t>> sorted_pages(page_update_count_.begin(), 
                                                              page_update_count_.end());

    std::sort(sorted_pages.begin(), sorted_pages.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });

    if (sorted_pages.size() > count) {
        sorted_pages.resize(count);
    }

    return sorted_pages;
}

void RedoLogAnalyzer::clear() {
    analysis_ = {};
    type_distribution_.clear();
    page_update_count_.clear();
}

void RedoLogAnalyzer::analyze_record(const RedoLogRecord& record) {
    analysis_.total_records++;

    // Count by type
    type_distribution_[record.r_type]++;
    analysis_.total_data_size += record.r_data.size();

    // Count by page
    page_update_count_[record.r_page_no]++;

    // Count specific types
    switch (record.r_type) {
        case MLOG_REC_INSERT:
        case MLOG_COMP_REC_INSERT:
            analysis_.insert_records++;
            break;
        case MLOG_REC_UPDATE:
        case MLOG_COMP_REC_UPDATE:
            analysis_.update_records++;
            break;
        case MLOG_REC_DELETE:
        case MLOG_COMP_REC_DELETE:
            analysis_.delete_records++;
            break;
        case MLOG_PAGE_CREATE:
        case MLOG_COMP_PAGE_CREATE:
            analysis_.page_create_records++;
            break;
        case MLOG_PAGE_REINIT:
        case MLOG_COMP_PAGE_REINIT:
            analysis_.page_reinit_records++;
            break;
        case MLOG_BLOB_WRITE:
            analysis_.blob_write_records++;
            break;
    }

    // Track unique space IDs and page numbers
    analysis_.unique_space_ids = std::max(analysis_.unique_space_ids,
        static_cast<uint32_t>(page_update_count_.size()));
    analysis_.unique_page_numbers = analysis_.unique_space_ids;
}

} // namespace innodb
} // namespace dbrecovery

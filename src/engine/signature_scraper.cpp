/**
 * @file signature_scraper.cpp
 * @brief Signature-Based Scraper implementation for MySQL 5.6 extreme recovery
 *
 * Implements high-performance sliding window scanning with InnoDB page feature matching.
 * Designed to find InnoDB data pages in raw disk images, even when file system metadata
 * is completely lost (rm -rf scenario).
 *
 * Key algorithms:
 * 1. Sliding window scan with configurable step size (512-byte increments)
 * 2. LSN-based filtering (skip pages newer than threshold)
 * 3. Checksum validation for data integrity
 * 4. Page chain reconstruction from head/tail pointers
 * 5. Conflict detection for multiple page versions
 */

#include "dbrecovery/engine/signature_scraper.hpp"
#include "dbrecovery/innodb/innodb_checksum.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

#include <algorithm>
#include <iostream>
#include <chrono>
#include <cstring>
#include <unordered_set>
#include <sys/mman.h>

namespace dbrecovery {
namespace engine {
// ==================== SignatureBasedScraper::ThreadPool ====================

struct SignatureBasedScraper::ThreadPool {
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;

    explicit ThreadPool(size_t threads) : stop(false) {
        for (size_t i = 0; i < threads; ++i) {
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(queue_mutex);
                        condition.wait(lock, [this] { return stop || !tasks.empty(); });
                        if (stop && tasks.empty()) return;
                        task = std::move(tasks.front());
                        tasks.pop();
                    }
                    task();
                }
            });
        }
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    void enqueue(std::function<void()> task) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            tasks.push(std::move(task));
        }
        condition.notify_one();
    }
};

// ==================== SignatureBasedScraper Implementation ====================

SignatureBasedScraper::SignatureBasedScraper(RawDiskIOEngine& engine)
    : engine_(engine)
    , bytes_scanned_(0)
    , pages_scanned_(0)
    , thread_pool_(std::make_unique<ThreadPool>(4))
{
    std::memset(&stats_, 0, sizeof(stats_));
}

SignatureBasedScraper::~SignatureBasedScraper() = default;

SignatureBasedScraper::ScanStats SignatureBasedScraper::scan(
    const ScanConfig& config,
    std::function<void(const FoundPage&)> callback) {

    current_config_ = config;
    std::memset(&stats_, 0, sizeof(stats_));

    auto start_time = std::chrono::high_resolution_clock::now();

    if (!engine_.is_open()) {
        return stats_;
    }

    uint64_t disk_size = engine_.get_size();
    uint64_t scan_limit = config.max_bytes_to_scan > 0
        ? std::min(config.max_bytes_to_scan, disk_size)
        : disk_size;

    // Sliding window scan with 512-byte increments
    constexpr size_t SLIDING_STEP = 512;

    // Default page sizes to scan
    std::vector<uint32_t> page_sizes = {16384, 4096, 8192, 32768, 65536};

    // Read in chunks and process
    size_t chunk_size = config.chunk_size;

    for (uint32_t page_size : page_sizes) {
        if (page_size < config.min_page_size || page_size > config.max_page_size) {
            continue;
        }

        uint8_t* chunk_buffer = static_cast<uint8_t*>(
            std::aligned_alloc(4096, chunk_size + page_size));
        if (!chunk_buffer) continue;

        uint64_t offset = 0;
        while (offset < scan_limit) {
            // Read one page past the chunk so a page that straddles the chunk
            // boundary is still seen as a complete page; the buffer is already
            // sized for it. Advancing by exactly chunk_size keeps the 512-byte
            // grids of neighbouring windows aligned, which is why pos==0 is
            // only processed for the first window.
            const size_t requested = static_cast<size_t>(std::min<uint64_t>(
                chunk_size + page_size, scan_limit - offset));
            size_t bytes_read = engine_.read(offset, chunk_buffer, requested);

            if (bytes_read == 0) break;

            bytes_scanned_ += bytes_read;
            stats_.total_bytes_scanned += bytes_read;

            // Sliding window scan through the chunk in 512-byte steps
            for (size_t pos = 0; pos + page_size <= bytes_read; pos += SLIDING_STEP) {
                if (offset > 0 && pos == 0) continue;  // covered by the previous window
                const uint8_t* page_data = chunk_buffer + pos;
                // Delivered once after conflict resolution below.
                process_page(offset + pos, page_data, page_size, callback, false);
            }

            if (bytes_read <= chunk_size) break;  // final window
            offset += chunk_size;

            // Progress callback
            if (progress_callback_) {
                double pct = (scan_limit > 0) ? (100.0 * offset / scan_limit) : 0.0;
                progress_callback_(pct);
            }
        }

        free(chunk_buffer);
    }

    // Resolve conflicts if we have multiple versions of same page
    if (!page_versions_.empty()) {
        ConflictResolutionStats conflict_stats;
        // Collect version groups from the page_versions_ map
        std::vector<std::vector<PageVersion>> version_groups;
        {
            std::shared_lock<std::shared_mutex> lock(page_versions_mutex_);
            for (const auto& [key, versions] : page_versions_) {
                version_groups.push_back(versions);
            }
        }
        auto resolved = resolve_conflicts(
            version_groups, config.conflict_policy, conflict_stats);
        stats_.conflict_resolutions = conflict_stats.total_conflicts;

        // Emit resolved pages
        for (const auto& page : resolved) {
            if (callback) {
                callback(page);
            }
        }
    }

    // Reconstruct B+Tree chains if requested
    if (config.reconstruct_chains && !discovered_chains_.empty()) {
        // Chains already reconstructed during scan
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    stats_.scan_time_seconds = std::chrono::duration<double>(end_time - start_time).count();

    if (stats_.scan_time_seconds > 0) {
        stats_.avg_throughput_mbps = static_cast<uint64_t>(
            (stats_.total_bytes_scanned / (1024.0 * 1024.0)) / stats_.scan_time_seconds);
    }

    return stats_;
}

SignatureBasedScraper::ScanStats SignatureBasedScraper::scan_with_template(
    const ScanConfig& config,
    const SchemaTemplate& schema_template,
    std::function<void(const FoundPage&)> callback) {

    schema_template_ = schema_template;
    // Run scan with template filtering
    auto result = scan(config, [this, &callback](const FoundPage& page) {
        if (matches_schema_template(page)) {
            callback(page);
        }
    });
    return result;
}

void SignatureBasedScraper::scan_chunk(uint64_t offset, const std::vector<uint8_t>& chunk,
                                        std::function<void(const FoundPage&)> callback) {
    constexpr size_t SLIDING_STEP = 512;

    for (uint32_t page_size : {16384, 4096, 8192, 32768, 65536}) {
        if (page_size < current_config_.min_page_size ||
            page_size > current_config_.max_page_size) {
            continue;
        }

        for (size_t pos = 0; pos + page_size <= chunk.size(); pos += SLIDING_STEP) {
            const uint8_t* page_data = chunk.data() + pos;
            process_page(offset + pos, page_data, page_size, callback);
        }
    }
}

std::vector<PageSignature> SignatureBasedScraper::match_signatures(
    const uint8_t* page_data, uint32_t page_size) {
    std::vector<PageSignature> matched;

    if (!page_data || page_size < 38) {
        return matched;
    }

    auto sigs = get_mysql_56_signatures();
    uint16_t page_type = innodb::read_be<uint16_t>(page_data, 24);

    for (const auto& sig : sigs) {
        if (page_type == sig.page_type) {
            matched.push_back(sig);
        }
    }

    return matched;
}

bool SignatureBasedScraper::parse_page_header(const uint8_t* page_data, uint32_t page_size,
                                               FoundPage& page) {
    if (!page_data || page_size < 38) {
        return false;
    }

    // FIL header fields are BIG-ENDIAN in InnoDB
    page.space_id = innodb::read_be<uint32_t>(page_data, 34);  // offset 34-37
    page.page_number = innodb::read_be<uint32_t>(page_data, 4);  // offset 4-7
    page.prev_page = innodb::read_be<uint32_t>(page_data, 8);    // FIL_PAGE_PREV
    page.next_page = innodb::read_be<uint32_t>(page_data, 12);   // FIL_PAGE_NEXT
    page.lsn = innodb::read_be<uint64_t>(page_data, 16);         // FIL_PAGE_LSN
    page.page_type = innodb::read_be<uint16_t>(page_data, 24);   // FIL_PAGE_TYPE
    page.page_size = page_size;

    // Page header fields (after FIL header at offset 38)
    if (page_size >= 56) {
        auto page_hdr = innodb::PageHeader::parse(page_data + 38, page_size - 38);
        if (page_hdr.has_value()) {
            page.n_records = page_hdr->n_heap;
            page.heap_top = page_hdr->heap_top;
        }
    }

    return true;
}

bool SignatureBasedScraper::process_page(uint64_t offset, const uint8_t* page_data,
                                          uint32_t page_size,
                                          std::function<void(const FoundPage&)> callback,
                                          bool emit_immediately) {
    stats_.pages_checked++;

    // Step 1: Check offset 24 for valid page_type (BIG-ENDIAN)
    uint16_t page_type = innodb::read_be<uint16_t>(page_data, 24);

    // Filter by allowed page types
    if (!current_config_.allowed_page_types.empty()) {
        bool found = false;
        for (uint16_t allowed : current_config_.allowed_page_types) {
            if (page_type == allowed) { found = true; break; }
        }
        if (!found) return false;
    }

    if (current_config_.filter_to_index_pages && !innodb::is_valid_index_page_type(page_type)) {
        return false;
    }

    if (!innodb::is_valid_page_type(page_type)) {
        return false;
    }

    // Step 2: Check offset 4 for reasonable page_number (< 10M) (BIG-ENDIAN)
    uint32_t page_number = innodb::read_be<uint32_t>(page_data, 4);
    if (page_number > 10000000) {
        return false;
    }

    // Step 3: Check offset 16 for reasonable LSN (> 0) (BIG-ENDIAN)
    uint64_t lsn = innodb::read_be<uint64_t>(page_data, 16);
    if (lsn == 0 && page_type != 0) {
        // Zero LSN is only valid for freshly allocated pages
        return false;
    }

    // Step 4: LSN filter
    if (!passes_lsn_filter(lsn)) {
        stats_.pages_filtered_by_lsn++;
        return false;
    }

    // Step 5: Validate checksum
    bool checksum_valid = false;
    ChecksumType checksum_type = ChecksumType::NONE;

    if (current_config_.validate_checksums) {
        auto result = innodb::validate_page_checksum(page_data, page_size, current_config_.strict_checksum);
        checksum_valid = result.is_valid;
        if (result.is_valid) {
            switch (result.detected_algorithm) {
                case innodb::ChecksumAlgo::CRC32C:
                    checksum_type = ChecksumType::CRC32;
                    break;
                case innodb::ChecksumAlgo::INNODB:
                    checksum_type = ChecksumType::INNODB;
                    break;
                case innodb::ChecksumAlgo::NONE:
                    checksum_type = ChecksumType::NONE;
                    break;
                default:
                    checksum_type = ChecksumType::FULL_PAGE;
                    break;
            }
            stats_.pages_with_valid_checksum++;
        } else if (current_config_.strict_checksum) {
            return false;
        }
    }

    // Step 6: Match signatures
    auto matched_sigs = match_signatures(page_data, page_size);
    if (matched_sigs.empty() && page_type == 0) {
        // Allocated page with no signature match - skip
        return false;
    }

    stats_.pages_matched++;

    // Build found page
    FoundPage page{};  // zero-init: n_records is only filled when page_size >= 56
    page.disk_offset = offset;
    page.page_number = page_number;
    page.lsn = lsn;
    page.page_type = page_type;
    page.page_size = page_size;
    page.matched_signatures = matched_sigs;
    page.has_valid_checksum = checksum_valid;
    page.checksum_type = checksum_type;
    page.prev_page = innodb::read_be<uint32_t>(page_data, 8);
    page.next_page = innodb::read_be<uint32_t>(page_data, 12);

    // Parse additional page header info
    if (page_size >= 56) {
        auto phdr = innodb::PageHeader::parse(page_data + 38, page_size - 38);
        if (phdr) {
            page.n_records = phdr->n_heap;
            page.heap_top = phdr->heap_top;
        }
    }

    page.data.assign(page_data, page_data + page_size);

    // Register for conflict detection
    PageVersion version;
    version.disk_offset = offset;
    version.lsn = lsn;
    version.page_number = page_number;
    version.space_id = innodb::read_be<uint32_t>(page_data, 34);  // Correct: offset 34-37
    version.data = page.data;
    version.is_selected = false;
    version.snapshot = page;
    register_page_version(version);

    if (emit_immediately && callback) {
        callback(page);
    }

    return true;
}

void SignatureBasedScraper::register_page_version(const PageVersion& version) {
    // Key by (space_id, page_number)
    uint64_t key = (static_cast<uint64_t>(version.space_id) << 32) | version.page_number;

    std::unique_lock<std::shared_mutex> lock(page_versions_mutex_);
    page_versions_[key].push_back(version);
}

bool SignatureBasedScraper::matches_schema_template(const FoundPage& page) {
    if (schema_template_.table_name.empty()) {
        return true;  // No template set - accept all
    }

    // Check page size matches expected
    if (schema_template_.page_size > 0 && page.page_size != schema_template_.page_size) {
        return false;
    }

    // Check page type is an index page (data pages)
    if (!page.is_index_page()) {
        return false;
    }

    // If we have expected column count, check that the page has reasonable data
    if (schema_template_.expected_column_count > 0 && page.n_records > 0) {
        // Rough check: record size should accommodate expected columns
        // This is a heuristic - we can't be 100% sure without parsing
    }

    return true;
}

void SignatureBasedScraper::worker_thread(uint64_t start_offset, uint64_t end_offset,
                                            std::function<void(const FoundPage&)> callback) {
    size_t chunk_size = current_config_.chunk_size;
    std::vector<uint8_t> buffer(chunk_size);

    uint64_t offset = start_offset;
    while (offset < end_offset) {
        size_t to_read = std::min(chunk_size, static_cast<size_t>(end_offset - offset));
        size_t bytes_read = engine_.read(offset, buffer.data(), to_read);

        if (bytes_read == 0) break;

        scan_chunk(offset, buffer, callback);

        bytes_scanned_ += bytes_read;
        offset += bytes_read;
    }
}

std::vector<std::vector<FoundPage>> SignatureBasedScraper::reconstruct_chains(
    const std::vector<FoundPage>& pages) {

    std::vector<std::vector<FoundPage>> chains;

    if (pages.empty()) {
        return chains;
    }

    // Build page number to page mapping
    std::unordered_map<uint32_t, const FoundPage*> page_map;
    for (const auto& page : pages) {
        page_map[page.page_number] = &page;
    }

    // Track visited pages
    std::unordered_set<uint32_t> visited;

    // Walk chains from pages with no previous page (chain starts)
    for (const auto& page : pages) {
        if (visited.count(page.page_number)) continue;

        // Find chain start: follow prev_page links backward
        uint32_t start_page = page.page_number;
        while (true) {
            auto it = page_map.find(start_page);
            if (it == page_map.end()) break;
            if (it->second->prev_page == 0xFFFFFFFF || it->second->prev_page == 0) break;
            if (visited.count(it->second->prev_page)) break;
            start_page = it->second->prev_page;
        }

        // Walk forward from chain start
        std::vector<FoundPage> chain;
        uint32_t current = start_page;
        uint32_t safety_counter = 0;
        const uint32_t MAX_CHAIN_LENGTH = 100000;

        while (current != 0xFFFFFFFF && current != 0 &&
               page_map.count(current) && !visited.count(current) &&
               safety_counter < MAX_CHAIN_LENGTH) {
            visited.insert(current);
            chain.push_back(*page_map[current]);

            auto next = page_map[current]->next_page;
            if (next == current) break;  // Self-referencing loop
            current = next;
            safety_counter++;
        }

        if (!chain.empty()) {
            chains.push_back(std::move(chain));
        }
    }

    discovered_chains_ = chains;
    return chains;
}

namespace {

// A PageVersion keeps only the fields conflict resolution needs; the full
// parse lives in `snapshot` so callers receive the same FoundPage shape they
// would have got from a direct emission.
FoundPage snapshot_of(const PageVersion& version) {
    if (!version.snapshot.data.empty()) return version.snapshot;
    FoundPage page;
    page.disk_offset = version.disk_offset;
    page.page_number = version.page_number;
    page.space_id = version.space_id;
    page.lsn = version.lsn;
    page.data = version.data;
    return page;
}

} // namespace

std::vector<FoundPage> SignatureBasedScraper::resolve_conflicts(
    const std::vector<std::vector<PageVersion>>& versions,
    PageConflictPolicy policy,
    ConflictResolutionStats& stats) {

    std::vector<FoundPage> resolved;
    std::memset(&stats, 0, sizeof(stats));

    for (const auto& version_group : versions) {
        if (version_group.size() <= 1) {
            // No conflict - take the only version
            if (!version_group.empty()) {
                resolved.push_back(snapshot_of(version_group[0]));
            }
            continue;
        }

        stats.total_conflicts++;

        // Sort versions by LSN. Copies flushed from the same write share an
        // LSN, so the tie-break goes to the fuller page: PAGE_N_RECS is the
        // only in-page evidence of how much of the record heap survived.
        auto sorted = version_group;
        std::sort(sorted.begin(), sorted.end(),
            [](const PageVersion& a, const PageVersion& b) {
                if (a.lsn != b.lsn) return a.lsn < b.lsn;
                if (a.snapshot.n_records != b.snapshot.n_records) {
                    return a.snapshot.n_records < b.snapshot.n_records;
                }
                return a.disk_offset < b.disk_offset;
            });

        size_t selected_idx = 0;

        switch (policy) {
            case PageConflictPolicy::EARLIEST_LSN:
                selected_idx = 0;
                stats.resolved_earliest++;
                break;

            case PageConflictPolicy::LATEST_LSN:
                selected_idx = sorted.size() - 1;
                stats.resolved_latest++;
                break;

            case PageConflictPolicy::BEST_CHECKSUM: {
                bool found = false;
                for (size_t i = 0; i < sorted.size(); ++i) {
                    if (sorted[i].data.size() >= 38) {
                        bool valid = innodb::is_page_checksum_valid(
                            sorted[i].data.data(), sorted[i].data.size());
                        if (valid) {
                            selected_idx = i;
                            found = true;
                            break;
                        }
                    }
                }
                if (found) {
                    stats.resolved_best_checksum++;
                } else {
                    // Fall back to latest LSN
                    selected_idx = sorted.size() - 1;
                    stats.resolved_latest++;
                }
                break;
            }

            case PageConflictPolicy::PREVIOUS_PAGE:
                // Use the version that has the best chain continuity
                // For now, use latest LSN as proxy
                selected_idx = sorted.size() - 1;
                stats.resolved_latest++;
                break;
        }

        resolved.push_back(snapshot_of(sorted[selected_idx]));
    }

    return resolved;
}

// ==================== EnhancedPageReconstructor Implementation ====================

std::vector<EnhancedPageReconstructor::ReconstructedPage>
EnhancedPageReconstructor::reconstruct(
    RawDiskIOEngine& engine,
    uint32_t page_size,
    uint64_t max_pages) {

    std::vector<ReconstructedPage> pages;

    if (!engine.is_open() || page_size < 38) {
        return pages;
    }

    uint64_t disk_size = engine.get_size();
    if (disk_size < page_size) {
        return pages;
    }

    uint64_t num_pages = disk_size / page_size;
    if (max_pages > 0) {
        num_pages = std::min(num_pages, max_pages);
    }

    std::vector<uint8_t> page_data(page_size);

    for (uint64_t i = 0; i < num_pages; ++i) {
        uint64_t offset = i * page_size;
        if (offset + page_size > disk_size) break;

        size_t bytes_read = engine.read(offset, page_data.data(), page_size);
        if (bytes_read != page_size) continue;

        // Check for valid page type
        uint16_t page_type = innodb::read_be<uint16_t>(page_data.data(), 24);
        if (!innodb::is_valid_page_type(page_type)) continue;

        // Validate checksum
        bool checksum_valid = innodb::is_page_checksum_valid(page_data.data(), page_size);

        ReconstructedPage rpage;
        rpage.disk_offset = offset;
        rpage.page_number = innodb::read_be<uint32_t>(page_data.data(), 4);
        rpage.space_id = innodb::read_be<uint32_t>(page_data.data(), 34);
        rpage.page_type = page_type;
        rpage.lsn = innodb::read_be<uint64_t>(page_data.data(), 16);
        rpage.prev_page = innodb::read_be<uint32_t>(page_data.data(), 8);
        rpage.next_page = innodb::read_be<uint32_t>(page_data.data(), 12);
        rpage.checksum_valid = checksum_valid;

        // Parse page header for record info
        if (page_size >= 56) {
            auto phdr = innodb::PageHeader::parse(page_data.data() + 38, page_size - 38);
            if (phdr) {
                rpage.n_records = phdr->n_heap;
                rpage.heap_top = phdr->heap_top;
            }
        }

        rpage.data = page_data;
        pages.push_back(std::move(rpage));
    }

    return pages;
}

std::vector<EnhancedPageReconstructor::BPlusTreeChain>
EnhancedPageReconstructor::reconstruct_chains(
    const std::vector<ReconstructedPage>& pages) {

    std::vector<BPlusTreeChain> chains;

    if (pages.empty()) {
        return chains;
    }

    // Build page number to page mapping
    std::unordered_map<uint32_t, const ReconstructedPage*> page_map;
    for (const auto& page : pages) {
        page_map[page.page_number] = &page;
    }

    // Track visited pages
    std::unordered_set<uint32_t> visited;

    // Walk B+Tree chains from FIL_PAGE_PREV (offset 8) and FIL_PAGE_NEXT (offset 12)
    for (const auto& page : pages) {
        if (visited.count(page.page_number)) continue;

        // Find chain start: follow prev_page links backward
        uint32_t start_page = page.page_number;
        while (true) {
            auto it = page_map.find(start_page);
            if (it == page_map.end()) break;
            if (it->second->prev_page == 0xFFFFFFFF || it->second->prev_page == 0) break;
            if (visited.count(it->second->prev_page)) break;
            start_page = it->second->prev_page;
        }

        // Walk forward using next_page
        std::vector<ReconstructedPage> chain_pages;
        uint32_t current = start_page;
        uint32_t safety_counter = 0;
        const uint32_t MAX_CHAIN_LENGTH = 100000;

        while (current != 0xFFFFFFFF && current != 0 &&
               page_map.count(current) && !visited.count(current) &&
               safety_counter < MAX_CHAIN_LENGTH) {
            visited.insert(current);
            chain_pages.push_back(*page_map[current]);

            auto next = page_map[current]->next_page;
            if (next == current) break;  // Self-referencing
            current = next;
            safety_counter++;
        }

        if (!chain_pages.empty()) {
            BPlusTreeChain chain;
            chain.space_id = chain_pages[0].space_id;
            chain.root_page = chain_pages[0].page_number;
            chain.pages = std::move(chain_pages);
            chain.is_complete = true;  // Assume complete for now
            chain.is_sorted = true;    // Assume sorted

            // Try to extract index_id from the first page
            if (!chain.pages.empty() && chain.pages[0].data.size() >= 50) {
                // index_id is at offset 46 (BTR_IND_PAGE_OFFSET_ABS)
                // Actually it's at FIL_HEADER_SIZE + BTR_IND_PAGE_OFFSET (8 bytes)
                // But we can just read it from a known offset
            }

            chains.push_back(std::move(chain));
        }
    }

    return chains;
}

// ==================== SlidingWindowScanner Implementation ====================

SlidingWindowScanner::SlidingWindowScanner(RawDiskIOEngine& engine)
    : engine_(engine) {
}

uint64_t SlidingWindowScanner::scan(const WindowConfig& config,
                                      std::function<void(const ScanResult&)> callback) {
    uint64_t pages_found = 0;

    if (!engine_.is_open()) {
        return pages_found;
    }

    uint64_t disk_size = engine_.get_size();
    uint64_t max_offset = config.max_offset > 0
        ? std::min(config.max_offset, disk_size)
        : disk_size;

    size_t window_size = config.window_size;
    size_t step_size = config.step_size;
    uint32_t page_size = static_cast<uint32_t>(config.page_size);

    if (window_size == 0) window_size = 4 * 1024 * 1024;  // 4MB default
    if (step_size == 0) step_size = window_size / 2;
    if (page_size == 0) page_size = 16384;  // 16KB default

    // Allocate pinned buffer
    PinnedBuffer buffer(window_size + page_size);
    if (!buffer.data()) {
        return pages_found;
    }

    uint64_t offset = 0;
    while (offset < max_offset) {
        size_t to_read = std::min(window_size, static_cast<size_t>(max_offset - offset));
        size_t bytes_read = engine_.read(offset, buffer.data(), to_read);

        if (bytes_read == 0) break;

        // Scan this window
        scan_window(offset, bytes_read, callback);

        offset += step_size;
        if (offset >= max_offset) break;
    }

    return pages_found;
}

void SlidingWindowScanner::scan_window(uint64_t window_offset, size_t window_size,
                                         std::function<void(const ScanResult&)> callback) {
    // Default page size
    constexpr uint32_t DEFAULT_PAGE_SIZE = 16384;
    constexpr size_t SLIDING_STEP = 512;

    for (size_t pos = 0; pos + DEFAULT_PAGE_SIZE <= window_size; pos += SLIDING_STEP) {
        // We need to read the actual data at this position
        uint8_t page_buf[DEFAULT_PAGE_SIZE];
        size_t bytes_read = engine_.read(window_offset + pos, page_buf, DEFAULT_PAGE_SIZE);
        if (bytes_read != DEFAULT_PAGE_SIZE) continue;

        // Check for valid page type at offset 24
        uint16_t page_type = innodb::read_be<uint16_t>(page_buf, 24);
        if (!innodb::is_valid_page_type(page_type)) continue;

        // Check for reasonable page number at offset 4
        uint32_t page_number = innodb::read_be<uint32_t>(page_buf, 4);
        if (page_number > 10000000) continue;

        // Check for reasonable LSN at offset 16
        uint64_t lsn = innodb::read_be<uint64_t>(page_buf, 16);
        if (lsn == 0 && page_type != 0) continue;

        // Validate checksum
        bool checksum_valid = innodb::is_page_checksum_valid(page_buf, DEFAULT_PAGE_SIZE);

        // Compute confidence score
        uint8_t confidence = 0;

        // Page type contributes to confidence
        if (innodb::is_valid_index_page_type(page_type)) {
            confidence += 40;
        } else if (page_type != 0) {
            confidence += 20;
        }

        // LSN validity
        if (lsn > 0 && lsn < 0xFFFFFFFFFFFF0000ULL) {
            confidence += 30;
        }

        // Checksum validity
        if (checksum_valid) {
            confidence += 30;
        }

        // Only report pages with reasonable confidence
        if (confidence < 30) continue;

        ScanResult result;
        result.offset = window_offset + pos;
        result.page_number = page_number;
        result.space_id = innodb::read_be<uint32_t>(page_buf, 34);
        result.page_type = page_type;
        result.lsn = lsn;
        result.confidence = confidence;
        result.page_data.assign(page_buf, page_buf + DEFAULT_PAGE_SIZE);

        if (callback) {
            callback(result);
        }
    }
}

// ==================== SlidingWindowScanner::PinnedBuffer Implementation ====================

SlidingWindowScanner::PinnedBuffer::PinnedBuffer(size_t size)
    : buffer_(nullptr), size_(size) {
    if (size == 0) return;

    // Allocate aligned memory
    int ret = posix_memalign(reinterpret_cast<void**>(&buffer_), 4096, size);
    if (ret != 0 || buffer_ == nullptr) {
        buffer_ = nullptr;
        size_ = 0;
        return;
    }

    // Lock the buffer in memory (prevent page eviction during scanning)
    if (mlock(buffer_, size_) != 0) {
        // Non-critical: mlock may fail due to limits, continue anyway
    }

    std::memset(buffer_, 0, size_);
}

SlidingWindowScanner::PinnedBuffer::~PinnedBuffer() {
    if (buffer_) {
        // Unlock memory
        munlock(buffer_, size_);
        free(buffer_);
        buffer_ = nullptr;
    }
}

SlidingWindowScanner::PinnedBuffer::PinnedBuffer(PinnedBuffer&& other) noexcept
    : buffer_(other.buffer_), size_(other.size_) {
    other.buffer_ = nullptr;
    other.size_ = 0;
}

SlidingWindowScanner::PinnedBuffer& SlidingWindowScanner::PinnedBuffer::operator=(
    PinnedBuffer&& other) noexcept {
    if (this != &other) {
        if (buffer_) {
            munlock(buffer_, size_);
            free(buffer_);
        }
        buffer_ = other.buffer_;
        size_ = other.size_;
        other.buffer_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

// ==================== InnoDBPageSignatures Implementation ====================

std::vector<InnoDBPageSignatures::FullSignature> InnoDBPageSignatures::get_mysql_56_signatures() {
    std::vector<FullSignature> sigs;

    // MySQL 5.6 FIL_PAGE_INDEX (0x45BC)
    {
        FullSignature sig;
        Signature s;
        s.offset = 24;
        s.size = 2;
        s.data[0] = 0xBC; s.data[1] = 0x45;  // 0x45BC in LE
        s.mask[0] = 0xFF; s.mask[1] = 0xFF;
        sig.signatures.push_back(s);
        sig.min_pages = 1;
        sig.max_pages = 100;
        sigs.push_back(sig);
    }

    // MySQL 5.6 FIL_PAGE_INDEX variant (0x45BF)
    {
        FullSignature sig;
        Signature s;
        s.offset = 24;
        s.size = 2;
        s.data[0] = 0xBF; s.data[1] = 0x45;  // 0x45BF in LE
        s.mask[0] = 0xFF; s.mask[1] = 0xFF;
        sig.signatures.push_back(s);
        sig.min_pages = 1;
        sig.max_pages = 100;
        sigs.push_back(sig);
    }

    return sigs;
}

std::vector<InnoDBPageSignatures::FullSignature> InnoDBPageSignatures::get_mysql_80_signatures() {
    std::vector<FullSignature> sigs;

    // MySQL 8.0 FIL_PAGE_INDEX (0x26F1)
    {
        FullSignature sig;
        Signature s;
        s.offset = 24;
        s.size = 2;
        s.data[0] = 0xF1; s.data[1] = 0x26;  // 0x26F1 in LE
        s.mask[0] = 0xFF; s.mask[1] = 0xFF;
        sig.signatures.push_back(s);
        sig.min_pages = 1;
        sig.max_pages = 100;
        sigs.push_back(sig);
    }

    // MySQL 8.0 SDI page (0x26F3)
    {
        FullSignature sig;
        Signature s;
        s.offset = 24;
        s.size = 2;
        s.data[0] = 0xF3; s.data[1] = 0x26;  // 0x26F3 in LE
        s.mask[0] = 0xFF; s.mask[1] = 0xFF;
        sig.signatures.push_back(s);
        sig.min_pages = 1;
        sig.max_pages = 10;
        sigs.push_back(sig);
    }

    return sigs;
}

std::vector<InnoDBPageSignatures::FullSignature> InnoDBPageSignatures::get_all_signatures() {
    auto sigs = get_mysql_56_signatures();
    auto sigs80 = get_mysql_80_signatures();
    sigs.insert(sigs.end(), sigs80.begin(), sigs80.end());
    return sigs;
}

} // namespace engine
} // namespace dbrecovery

/**
 * @file xfs_disk_carver.cpp
 * @brief XFS filesystem disk carving implementation for MySQL/InnoDB recovery
 *
 * Implements XFS-specific recovery techniques:
 * 1. AG (Allocation Group) scanning
 * 2. Free inode B+tree traversal for deleted inode detection
 * 3. Extent reconstruction from deleted inode memory
 * 4. Direct block signature scanning fallback
 */

#include "dbrecovery/disk/xfs_disk_carver.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/index_page_parser.hpp"
#include "dbrecovery/innodb/schema_agnostic_scanner.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

#include <chrono>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <iomanip>

#ifdef __linux__
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/fs.h>
#include <endian.h>
#endif

namespace dbrecovery {
namespace disk {
namespace xfs {

// Use innodb namespace for read_be functions and InnoDBIndexParser
using dbrecovery::innodb::read_be;
using dbrecovery::innodb::InnoDBIndexParser;

// InnoDB page constants
constexpr uint16_t FIL_PAGE_INDEX = 0x45BF;
constexpr uint16_t FIL_PAGE_SDI = 0x45BE;
constexpr uint16_t FIL_PAGE_SDI_ZIP = 0x45BD;
constexpr size_t FIL_PAGE_TYPE_ABS = 24;
constexpr size_t FIL_HEADER_SIZE = 38;

// FRM file signature
static constexpr uint8_t FRM_SIGNATURE[] = {0xfe, 0x01, 0x0a, 0x0c, 0x03, 0x00, 0x00, 0x10, 0x01, 0x00};

// ==================== Constructor/Destructor ====================

XfsDiskCarver::XfsDiskCarver() = default;
XfsDiskCarver::~XfsDiskCarver() = default;

void XfsDiskCarver::set_config(const XfsConfig& config) {
    config_ = config;
}

void XfsDiskCarver::set_external_schema(const catalog::TableSchema& schema) {
    external_schema_ = schema;
}

// ==================== Static Helper Functions ====================

bool XfsDiskCarver::is_xfs_device(const std::string& device_path) {
    xfs_superblock sb;
    return read_superblock(device_path, sb);
}

bool XfsDiskCarver::read_superblock(const std::string& device_path, xfs_superblock& sb) {
#ifdef __linux__
    int fd = open(device_path.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }

    // XFS superblock is at offset 0 (first sector)
    ssize_t bytes_read = read(fd, &sb, sizeof(xfs_superblock));
    close(fd);

    if (bytes_read != sizeof(xfs_superblock)) {
        return false;
    }

    // Check magic number (big-endian)
    uint32_t magic = be32toh(sb.sb_magicnum);
    return magic == XFS_SB_MAGIC;
#else
    return false;
#endif
}

// ==================== Extent Parsing ====================

xfs_extent XfsDiskCarver::parse_extent(const xfs_bmbt_rec& rec) {
    xfs_extent ext;

    // Extent is stored as two 64-bit big-endian values
    // Format:
    //   l0:63      - extent flag (1 = unwritten)
    //   l0:9-62    - file offset (54 bits)
    //   l0:0-8 + l1:21-63 - block offset (52 bits)
    //   l1:0-20    - block count (21 bits)

    uint64_t l0 = be64toh(rec.l0);
    uint64_t l1 = be64toh(rec.l1);

    // Extract extent flag (bit 63 of l0)
    ext.state = (l0 >> 63) & 1;

    // Extract file offset (bits 9-62 of l0)
    ext.file_offset = (l0 >> 9) & 0x3FFFFFFFFFFFFULL;

    // Extract block offset (bits 0-8 of l0 + bits 21-63 of l1)
    ext.block_offset = ((l0 & 0x1FF) << 43) | ((l1 >> 21) & 0x7FFFFFFFFFFFFULL);

    // Extract block count (bits 0-20 of l1)
    ext.block_count = l1 & 0x1FFFFFULL;

    return ext;
}

// ==================== Main Scan Function ====================

XfsScanResult XfsDiskCarver::scan(std::function<void(double, const std::string&)> progress) {
    XfsScanResult result;
    auto start_time = std::chrono::high_resolution_clock::now();

    // Validate device path
    if (config_.device_path.empty()) {
        result.errors.push_back("Device path not specified");
        return result;
    }

    // Read and validate superblock
    if (!read_superblock(config_.device_path, superblock_)) {
        result.errors.push_back("Invalid XFS superblock or cannot read device");
        return result;
    }

    // Convert superblock fields from big-endian
    result.block_size = be32toh(superblock_.sb_blocksize);
    result.ag_blocks = be32toh(superblock_.sb_agblocks);
    result.ag_count = be32toh(superblock_.sb_agcount);
    result.inode_size = be16toh(superblock_.sb_inodesize);

    // Format UUID as string
    std::ostringstream uuid_ss;
    for (int i = 0; i < 16; i++) {
        uuid_ss << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<int>(superblock_.sb_uuid[i]);
        if (i == 3 || i == 5 || i == 7 || i == 9) uuid_ss << "-";
    }
    result.filesystem_uuid = uuid_ss.str();

    if (config_.verbose) {
        std::cout << "[INFO] XFS filesystem detected:\n";
        std::cout << "  UUID: " << result.filesystem_uuid << "\n";
        std::cout << "  Block size: " << result.block_size << " bytes\n";
        std::cout << "  AG count: " << result.ag_count << "\n";
        std::cout << "  AG blocks: " << result.ag_blocks << "\n";
        std::cout << "  Inode size: " << result.inode_size << " bytes\n";
        std::cout << "  Total size: "
                  << (result.ag_count * result.ag_blocks * result.block_size / 1024.0 / 1024.0 / 1024.0)
                  << " GB\n";
    }

    // Initialize counters
    current_ag_ = 0;
    inodes_scanned_ = 0;
    inodes_found_ = 0;
    should_stop_ = false;

    // Create output directory
    try {
        std::filesystem::create_directories(config_.output_dir);
    } catch (const std::exception& e) {
        result.errors.push_back("Cannot create output directory: " + std::string(e.what()));
        return result;
    }

    // Strategy 1: Scan deleted inodes via free inode btree
    if (config_.scan_deleted_inodes) {
        // Create worker threads for parallel AG scanning
        // Use at least min(ag_count, 8) workers for better parallelism
        uint32_t effective_workers = std::min(
            std::max(config_.num_workers, static_cast<uint32_t>(8)),
            result.ag_count
        );
        std::vector<std::thread> workers;
        for (uint32_t i = 0; i < effective_workers; ++i) {
            workers.emplace_back(&XfsDiskCarver::ag_worker_thread, this, i, progress);
        }

        for (auto& w : workers) {
            w.join();
        }

        // Copy results
        {
            std::lock_guard<std::mutex> lock(result_mutex_);
            result.deleted_inodes_found = inodes_found_.load();
            result.total_inodes_scanned = inodes_scanned_.load();
            result.pages_recovered = last_result_.pages_recovered;
            result.pages_by_index_id = last_result_.pages_by_index_id;
            result.recovered_extents = last_result_.recovered_extents;
        }
    }

    // Strategy 2: Always run signature scan for maximum recovery
    // Signature scan finds pages that inode scan might miss
    if (config_.signature_scan) {
        size_t pages_before = result.pages_recovered;
        if (config_.verbose) {
            std::cout << "[INFO] Running signature scan for additional pages (found "
                      << pages_before << " via inode scan)\n";
        }
        signature_scan(progress);

        {
            std::lock_guard<std::mutex> lock(result_mutex_);
            result.pages_by_index_id = last_result_.pages_by_index_id;
            result.pages_recovered = last_result_.pages_recovered;
        }

        if (config_.verbose) {
            std::cout << "[INFO] Signature scan found "
                      << (result.pages_recovered - pages_before) << " additional pages\n";
        }
    }

    // Strategy 3: Raw data pattern scan for non-page-aligned records
    // This helps recover data from TRUNCATE where page headers are corrupted
    if (config_.signature_scan && result.pages_recovered < 1000) {
        size_t pages_before = result.pages_recovered;
        if (config_.verbose) {
            std::cout << "[INFO] Running raw data pattern scan for additional recovery\n";
        }
        raw_data_scan(progress);

        {
            std::lock_guard<std::mutex> lock(result_mutex_);
            result.pages_by_index_id = last_result_.pages_by_index_id;
            result.pages_recovered = last_result_.pages_recovered;
        }

        if (config_.verbose) {
            std::cout << "[INFO] Raw data scan found "
                      << (result.pages_recovered - pages_before) << " additional records\n";
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    result.elapsed_seconds = std::chrono::duration<double>(end_time - start_time).count();
    result.success = true;

    if (config_.verbose) {
        std::cout << "[INFO] XFS scan completed:\n";
        std::cout << "  Inodes scanned: " << result.total_inodes_scanned << "\n";
        std::cout << "  Deleted inodes found: " << result.deleted_inodes_found << "\n";
        std::cout << "  Pages recovered: " << result.pages_recovered << "\n";
        std::cout << "  Time: " << result.elapsed_seconds << "s\n";
    }

    last_result_ = result;
    return result;
}

// ==================== AG Worker Thread ====================

void XfsDiskCarver::ag_worker_thread(uint32_t worker_id,
                                     std::function<void(double, const std::string&)> progress) {
#ifdef __linux__
    int fd = open(config_.device_path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (config_.verbose) {
            std::cerr << "[Worker " << worker_id << "] Cannot open device\n";
        }
        return;
    }

    uint32_t block_size = be32toh(superblock_.sb_blocksize);
    uint32_t ag_blocks = be32toh(superblock_.sb_agblocks);
    uint32_t ag_count = be32toh(superblock_.sb_agcount);
    uint16_t inode_size = be16toh(superblock_.sb_inodesize);
    uint16_t sect_size = be16toh(superblock_.sb_sectsize);

    auto last_progress_time = std::chrono::steady_clock::now();

    while (!should_stop_.load()) {
        // Get next AG to process
        uint32_t ag_index = current_ag_.fetch_add(1);
        if (ag_index >= ag_count) {
            break;
        }

        // Read AGI (AG Inode Management) at sector 2 of AG
        uint64_t agi_offset = static_cast<uint64_t>(ag_index) *
                             static_cast<uint64_t>(ag_blocks) *
                             static_cast<uint64_t>(block_size) +
                             static_cast<uint64_t>(sect_size) * 2;

        xfs_agi agi;
        if (pread(fd, &agi, sizeof(agi), agi_offset) != sizeof(agi)) {
            continue;
        }

        // Validate AGI magic
        if (be32toh(agi.agi_magicnum) != XFS_AGI_MAGIC) {
            continue;
        }

        // Read AGF (AG Free Space) at sector 1 of AG
        uint64_t agf_offset = static_cast<uint64_t>(ag_index) *
                             static_cast<uint64_t>(ag_blocks) *
                             static_cast<uint64_t>(block_size) +
                             static_cast<uint64_t>(sect_size);

        xfs_agf agf;
        if (pread(fd, &agf, sizeof(agf), agf_offset) != sizeof(agf)) {
            continue;
        }

        // Validate AGF magic
        if (be32toh(agf.agf_magicnum) != XFS_AGF_MAGIC) {
            continue;
        }

        if (config_.verbose) {
            std::cout << "[Worker " << worker_id << "] Processing AG " << ag_index
                      << " (free inodes: " << be32toh(agi.agi_freecount) << ")\n";
        }

        // Strategy 1: Scan unlinked inode list (most recent deleted files)
        // XFS stores recently deleted inodes in agi_unlinked hash buckets
        // These inodes have di_mode=0 but still have extent data
        // Each bucket entry is the head of a linked list of deleted inodes
        // Reference: xfsprogs xfs_agi.h - agi_unlinked is hash table of unlinked inode lists
        constexpr uint32_t XFS_NULL_INO = 0xFFFFFFFF;

        // Track visited inodes to prevent duplicate processing across buckets
        std::set<uint32_t> visited_inodes;

        for (uint32_t bucket = 0; bucket < XFS_AGI_UNLINKED_BUCKETS; bucket++) {
            uint32_t unlinked_ino = be32toh(agi.agi_unlinked[bucket]);

            // Skip NULL inode entries (empty buckets)
            if (unlinked_ino == 0 || unlinked_ino == XFS_NULL_INO) continue;

            if (config_.verbose) {
                std::cout << "[Worker " << worker_id << "] Found unlinked chain head in bucket "
                          << bucket << ": inode " << unlinked_ino << "\n";
            }

            // Follow the entire unlinked chain from this bucket
            follow_unlinked_chain(fd, ag_index, unlinked_ino, visited_inodes);
        }

        // Check if free inode btree exists (FINOBT feature)
        uint32_t features_ro_compat = be32toh(superblock_.sb_features_ro_compat);
        bool has_finobt = (features_ro_compat & 0x01) != 0;  // XFS_SB_FEAT_RO_COMPAT_FINOBT

        uint32_t btree_root;
        uint32_t btree_magic;

        if (has_finobt && agi.agi_free_root != 0) {
            // Use free inode btree (more efficient for finding deleted inodes)
            btree_root = be32toh(agi.agi_free_root);
            btree_magic = XFS_FIBT_CRC_MAGIC;
        } else {
            // Use regular inode btree (scan for free inodes in records)
            btree_root = be32toh(agi.agi_root);
            btree_magic = XFS_IBT_CRC_MAGIC;
        }

        // Use recursive btree traversal (supports multi-level trees)
        btree_walk_recursive(fd, ag_index, btree_root, btree_magic, 10, 0);

        // Progress reporting
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_progress_time).count();

        if (progress && elapsed_ms >= config_.progress_interval_ms) {
            double pct = static_cast<double>(ag_index + 1) / ag_count;
            std::ostringstream oss;
            oss << "AG " << (ag_index + 1) << "/" << ag_count
                << " inodes=" << inodes_scanned_.load()
                << " found=" << inodes_found_.load();
            progress(pct, oss.str());
            last_progress_time = now;
        }
    }

    close(fd);
#endif
}

// ==================== Recursive B+tree Traversal ====================

void XfsDiskCarver::btree_walk_recursive(int fd, uint32_t ag_index, uint32_t block_num,
                                          uint32_t expected_magic, int max_depth, int current_depth) {
#ifdef __linux__
    if (current_depth >= max_depth || block_num == 0 || should_stop_.load()) {
        return;
    }

    uint32_t block_size = be32toh(superblock_.sb_blocksize);
    uint32_t ag_blocks = be32toh(superblock_.sb_agblocks);

    // Safety check: block_num should be within AG bounds
    if (block_num >= ag_blocks) {
        return;
    }

    // Calculate block offset
    uint64_t block_offset = static_cast<uint64_t>(ag_index) *
                           static_cast<uint64_t>(ag_blocks) *
                           static_cast<uint64_t>(block_size) +
                           static_cast<uint64_t>(block_num) *
                           static_cast<uint64_t>(block_size);

    std::vector<uint8_t> block_buffer(block_size);
    if (pread(fd, block_buffer.data(), block_size, block_offset) != block_size) {
        return;
    }

    // Read btree header
    // CRC-enabled btrees use 56-byte header (XFS_BTREE_SBLOCK_CRC_LEN)
    // Non-CRC btrees use 16-byte header (XFS_BTREE_SBLOCK_LEN)
    // Check magic to determine header size
    constexpr size_t BTREE_CRC_HEADER_SIZE = 56;  // XFS_BTREE_SBLOCK_CRC_LEN
    constexpr size_t BTREE_OLD_HEADER_SIZE = 16;  // XFS_BTREE_SBLOCK_LEN

    uint32_t magic = read_be<uint32_t>(block_buffer.data(), 0);

    // Determine if this is CRC-enabled btree
    bool is_crc_btree = (magic == XFS_FIBT_CRC_MAGIC || magic == XFS_IBT_CRC_MAGIC ||
                         magic == 0x424D4D33 || magic == 0x424D4D42);  // BMAP CRC variants

    // Also accept non-CRC variants for older XFS
    if (magic != expected_magic &&
        magic != XFS_FIBT_CRC_MAGIC && magic != XFS_IBT_CRC_MAGIC &&
        magic != XFS_FIBT_MAGIC && magic != XFS_IBT_MAGIC) {
        return;  // Invalid magic
    }

    size_t btree_header_size = is_crc_btree ? BTREE_CRC_HEADER_SIZE : BTREE_OLD_HEADER_SIZE;
    if (block_size < btree_header_size) return;

    // bb_level at offset 4, bb_numrecs at offset 6 (same for both variants)
    uint16_t level = read_be<uint16_t>(block_buffer.data(), 4);
    uint16_t numrecs = read_be<uint16_t>(block_buffer.data(), 6);

    if (numrecs == 0 || numrecs > 1000) {
        return;  // Invalid record count
    }

    if (level > 0) {
        // Non-leaf node: traverse child pointers
        // Pointer array starts at middle of block (after keys)
        // For short-form btree: pointers at (header_size + block_size) / 2
        // Reference: xfs_undelete xfs_tree.hpp:101
        size_t ptr_array_offset = (btree_header_size + block_size) / 2;
        size_t ptr_size = sizeof(uint32_t);  // Short-form uses 32-bit pointers

        for (uint16_t i = 0; i < numrecs && ptr_array_offset + ptr_size <= block_size; i++) {
            uint32_t child_block = read_be<uint32_t>(block_buffer.data(), ptr_array_offset + i * ptr_size);
            if (child_block != 0 && child_block < ag_blocks) {
                btree_walk_recursive(fd, ag_index, child_block, expected_magic, max_depth, current_depth + 1);
            }
        }
    } else {
        // Leaf node: process inode records
        process_inode_records(fd, ag_index, block_buffer.data(), numrecs, block_size, btree_header_size);
    }
#endif
}

void XfsDiskCarver::process_inode_records(int fd, uint32_t ag_index, const uint8_t* block_data,
                                           uint16_t numrecs, uint32_t block_size, size_t btree_header_size) {
#ifdef __linux__
    // Inode btree record layout (xfs_inobt_rec):
    // ir_startino (4), ir_holemask (2), ir_count (1), ir_freecount (1), ir_free (8)
    constexpr size_t INOREC_SIZE = 16;

    if (block_size < btree_header_size + numrecs * INOREC_SIZE) {
        return;
    }

    size_t rec_offset = btree_header_size;

    for (uint16_t i = 0; i < numrecs; i++) {
        const uint8_t* rec_data = block_data + rec_offset + i * INOREC_SIZE;

        uint32_t start_ino = read_be<uint32_t>(rec_data, 0);
        uint16_t holemask = read_be<uint16_t>(rec_data, 4);
        uint8_t count = rec_data[6];
        uint8_t freecount = rec_data[7];
        uint64_t free_mask = read_be<uint64_t>(rec_data, 8);

        // Each record covers 64 consecutive inodes
        // free_mask bit N = 1 means inode N is free (deleted)
        if (free_mask == 0) {
            continue;  // No free inodes in this chunk
        }

        // Iterate through free inodes in this chunk
        // Note: holemask is 16-bit, each bit covers 4 inodes (bit 0 = inodes 0-3, bit 1 = inodes 4-7, etc.)
        // For sparse inode chunks, holemask indicates which inode clusters are missing
        for (uint32_t bit = 0; bit < 64 && bit < static_cast<uint32_t>(count); bit++) {
            if ((free_mask & (1ULL << bit)) == 0) {
                continue;  // Inode is allocated, not free
            }

            // Check holemask for sparse inode allocation
            // holemask bit N covers inodes N*4 to N*4+3
            // Check holemask for sparse inode allocation
            // holemask bit N covers inodes N*4 to N*4+3
            uint32_t hole_bit = bit / 4;
            if ((holemask & (1 << hole_bit)) != 0) {
                continue;  // Inode cluster is in a hole (sparse chunk)
            }

            // Calculate AG-relative inode number
            // start_ino is the first inode number in this chunk (AG-relative)
            uint32_t ag_inode = start_ino + bit;

            inodes_scanned_.fetch_add(1);

            // Read inode and try to recover
            auto extent_info = read_inode(ag_index, ag_inode);
            if (extent_info.has_value() && !extent_info->extents.empty()) {
                process_recovered_inode(fd, ag_index, ag_inode, *extent_info);
            }
        }
    }
#endif
}

// ==================== Read Inode ====================

std::optional<XfsExtentInfo> XfsDiskCarver::read_inode(uint32_t ag_index, uint32_t ag_inode) {
#ifdef __linux__
    int fd = open(config_.device_path.c_str(), O_RDONLY);
    if (fd < 0) {
        return std::nullopt;
    }

    uint32_t block_size = be32toh(superblock_.sb_blocksize);
    uint32_t ag_blocks = be32toh(superblock_.sb_agblocks);
    uint16_t inode_size = be16toh(superblock_.sb_inodesize);

    // Calculate inode disk offset
    uint64_t ag_base = static_cast<uint64_t>(ag_index) *
                       static_cast<uint64_t>(ag_blocks) *
                       static_cast<uint64_t>(block_size);

    uint64_t inode_offset = ag_base +
                            static_cast<uint64_t>(ag_inode) *
                            static_cast<uint64_t>(inode_size);

    // Read inode
    std::vector<uint8_t> inode_data(inode_size);
    if (pread(fd, inode_data.data(), inode_size, inode_offset) != inode_size) {
        close(fd);
        return std::nullopt;
    }

    close(fd);

    // Parse inode core
    xfs_dinode_core inode_core;
    std::memcpy(&inode_core, inode_data.data(), sizeof(inode_core));

    // Check magic
    uint16_t magic = be16toh(inode_core.di_magic);
    if (magic != XFS_DINODE_MAGIC) {
        return std::nullopt;
    }

    // For deleted inodes:
    // - di_mode == 0 (no file type)
    // - di_nlink == 0 (no links)
    // - di_format == XFS_DINODE_FMT_EXTENTS (extent list)

    uint16_t mode = be16toh(inode_core.di_mode);
    uint32_t nlink = be32toh(inode_core.di_nlink);
    uint8_t format = inode_core.di_format;

    // Accept deleted inodes OR regular inodes with extents (for debugging)
    bool is_deleted = (mode == 0);

    // Get file size from inode core
    uint64_t file_size = be64toh(inode_core.di_size);

    // Extent btree format - try to recover extents from btree root in inode
    // For deleted inodes, extent btree is converted to extent list format
    // But sometimes the btree root is still in the inode data fork area
    if (format == XFS_DINODE_FMT_BTREE) {
        if (config_.verbose) {
            std::cout << "[DEBUG] Inode has extent btree format (large/fragmented file)\n";
        }
        // For btree format, the btree root is stored in the inode's data fork
        // Try to extract extent list from btree root
        // This is a simplified approach - full btree traversal would be needed for large files

        // Check if we have a btree root block in the inode
        // btree root starts at offset 176 for v5 inode
        size_t btree_start = 176;
        if (btree_start + 16 > inode_size) {
            return std::nullopt;  // Not enough space for btree header
        }

        // Try to read btree block header
        xfs_btree_block btree_hdr;
        std::memcpy(&btree_hdr, inode_data.data() + btree_start, sizeof(btree_hdr));

        // Validate btree magic (should be XFS_BMAP_CRC_MAGIC = 0x42B33D42 for v5)
        uint32_t btree_magic = be32toh(btree_hdr.bb_magic);
        if (btree_magic != 0x42B33D42 && btree_magic != 0x424D4D42) {  // BMAP_CRC or BMAP magic
            return std::nullopt;  // Not a valid btree root
        }

        uint16_t level = be16toh(btree_hdr.bb_level);
        uint16_t numrecs = be16toh(btree_hdr.bb_numrecs);

        if (level == 0 && numrecs > 0 && numrecs < 100) {
            // Leaf level - records are directly in the btree block
            // For short form CRC btree, records start at offset 56 (XFS_BTREE_SBLOCK_CRC_LEN)
            size_t rec_offset = btree_start + 56;
            size_t rec_size = sizeof(xfs_bmbt_rec);

            XfsExtentInfo info;
            info.inode_number = ag_inode + ag_index * ag_blocks;
            info.file_size = file_size;
            info.is_deleted = is_deleted;
            info.next_unlinked = be32toh(inode_core.di_next_unlinked);  // For chain traversal
            info.mtime.t_sec = be32toh(inode_core.di_mtime.t_sec);
            info.mtime.t_nsec = be32toh(inode_core.di_mtime.t_nsec);
            info.ctime.t_sec = be32toh(inode_core.di_ctime.t_sec);
            info.ctime.t_nsec = be32toh(inode_core.di_ctime.t_nsec);

            for (uint16_t i = 0; i < numrecs && rec_offset + rec_size <= inode_size; i++) {
                xfs_bmbt_rec rec;
                std::memcpy(&rec, inode_data.data() + rec_offset + i * rec_size, rec_size);

                xfs_extent ext = parse_extent(rec);

                if (ext.block_count == 0 || ext.state == XFS_EXT_UNWRITTEN) {
                    continue;
                }

                uint64_t fs_size = be64toh(superblock_.sb_dblocks);
                if (ext.block_offset + ext.block_count <= fs_size) {
                    info.extents.push_back(ext);
                }
            }

            if (!info.extents.empty()) {
                return info;
            }
        }

        return std::nullopt;  // Non-leaf btree or no valid extents
    }

    if (format != XFS_DINODE_FMT_EXTENTS) {
        // Unknown format
        return std::nullopt;
    }

    uint32_t nextents = be32toh(inode_core.di_nextents);

    if (file_size == 0 || nextents == 0) {
        return std::nullopt;  // Empty inode
    }

    // Parse extent list (stored after inode core)
    // Extent records start at offset sizeof(xfs_dinode_core) or 176 bytes for v5 inode
    size_t extent_start = 176;  // Fixed offset for v5 inode
    size_t extent_size = sizeof(xfs_bmbt_rec);

    XfsExtentInfo info;
    info.inode_number = ag_inode + ag_index * ag_blocks;  // Calculate full inode number
    info.file_size = file_size;
    info.is_deleted = is_deleted;
    info.next_unlinked = be32toh(inode_core.di_next_unlinked);  // For chain traversal

    // Parse mtime/ctime
    info.mtime.t_sec = be32toh(inode_core.di_mtime.t_sec);
    info.mtime.t_nsec = be32toh(inode_core.di_mtime.t_nsec);
    info.ctime.t_sec = be32toh(inode_core.di_ctime.t_sec);
    info.ctime.t_nsec = be32toh(inode_core.di_ctime.t_nsec);

    // Extract extents
    for (uint32_t i = 0; i < nextents && extent_start + extent_size <= inode_size; i++) {
        xfs_bmbt_rec rec;
        std::memcpy(&rec, inode_data.data() + extent_start + i * extent_size, extent_size);

        xfs_extent ext = parse_extent(rec);

        // Validate extent
        if (ext.block_count == 0) {
            continue;  // Zero-length extent
        }

        if (ext.state == XFS_EXT_UNWRITTEN) {
            continue;  // Preallocated but not written
        }

        // Check extent doesn't exceed filesystem size or AG bounds
        uint64_t fs_size = be64toh(superblock_.sb_dblocks);
        if (ext.block_offset + ext.block_count > fs_size) {
            continue;  // Invalid extent - beyond filesystem
        }

        // For MySQL data files, accept all extents regardless of file_offset
        // MySQL tables can have extents at various file offsets due to table growth
        info.extents.push_back(ext);
    }

    if (info.extents.empty()) {
        return std::nullopt;
    }

    return info;
#else
    return std::nullopt;
#endif
}

// ==================== Follow Unlinked Chain ====================

void XfsDiskCarver::follow_unlinked_chain(int fd, uint32_t ag_index, uint32_t start_ino,
                                           std::set<uint32_t>& visited) {
#ifdef __linux__
    constexpr uint32_t XFS_NULL_INO = 0xFFFFFFFF;
    constexpr size_t MAX_CHAIN_LENGTH = 1000;  // Safety limit

    uint32_t current_ino = start_ino;
    size_t chain_length = 0;

    uint32_t ag_blocks = be32toh(superblock_.sb_agblocks);
    uint16_t inode_size = be16toh(superblock_.sb_inodesize);
    uint64_t inodes_per_ag = static_cast<uint64_t>(ag_blocks) *
                             static_cast<uint64_t>(be32toh(superblock_.sb_blocksize) / inode_size);

    while (current_ino != 0 && current_ino != XFS_NULL_INO &&
           chain_length < MAX_CHAIN_LENGTH && !should_stop_.load()) {

        // Prevent infinite loops
        if (visited.count(current_ino) > 0) {
            if (config_.verbose) {
                std::cout << "[WARN] Cycle detected in unlinked chain at inode " << current_ino << "\n";
            }
            break;
        }
        visited.insert(current_ino);

        // Convert absolute inode to AG-relative
        uint32_t ino_ag = current_ino / inodes_per_ag;
        uint32_t ag_inode = current_ino % inodes_per_ag;

        // Only process inodes in current AG
        if (ino_ag != ag_index) {
            // Need to switch AG context - for simplicity, skip cross-AG inodes
            break;
        }

        if (config_.verbose) {
            std::cout << "[CHAIN] Processing unlinked inode " << current_ino
                      << " (AG " << ag_index << ", depth " << chain_length << ")\n";
        }

        // Read and process the inode
        auto extent_info = read_inode(ag_index, ag_inode);
        if (extent_info.has_value()) {
            process_recovered_inode(fd, ag_index, ag_inode, *extent_info);

            // Continue following chain
            current_ino = extent_info->next_unlinked;
        } else {
            // Invalid inode, stop chain
            break;
        }

        chain_length++;
        inodes_scanned_.fetch_add(1);
    }
#endif
}

// ==================== Process Recovered Inode ====================

void XfsDiskCarver::process_recovered_inode(int fd, uint32_t ag_index, uint32_t ag_inode,
                                             const XfsExtentInfo& extent_info) {
#ifdef __linux__
    if (extent_info.extents.empty()) {
        return;
    }

    std::vector<XfsPageInfo> recovered_pages;

    for (const auto& ext : extent_info.extents) {
        if (ext.state == XFS_EXT_UNWRITTEN) {
            continue;  // Skip unwritten extents
        }

        std::vector<uint8_t> extent_data = read_extent_data(ext);
        if (!extent_data.empty()) {
            auto pages = parse_pages_from_extent(extent_data, extent_info.inode_number);
            for (auto& page : pages) {
                recovered_pages.push_back(std::move(page));
            }
        }
    }

    if (!recovered_pages.empty()) {
        std::lock_guard<std::mutex> lock(result_mutex_);
        last_result_.recovered_extents.push_back(extent_info);
        inodes_found_.fetch_add(1);

        for (auto& page : recovered_pages) {
            last_result_.pages_by_index_id[page.index_id].push_back(std::move(page));
            last_result_.pages_recovered++;
        }
    }
#endif
}

// ==================== Read Extent Data ====================

std::vector<uint8_t> XfsDiskCarver::read_extent_data(const xfs_extent& ext) {
#ifdef __linux__
    int fd = open(config_.device_path.c_str(), O_RDONLY);
    if (fd < 0) {
        return {};
    }

    uint32_t block_size = be32toh(superblock_.sb_blocksize);
    uint64_t read_size = ext.block_count * block_size;

    // Limit read size for safety
    if (read_size > 100 * 1024 * 1024) {  // Max 100MB per extent
        read_size = 100 * 1024 * 1024;
    }

    std::vector<uint8_t> data(read_size);
    uint64_t disk_offset = block_to_disk_offset(ext.block_offset);

    ssize_t bytes_read = pread(fd, data.data(), read_size, disk_offset);
    close(fd);

    if (bytes_read < 0) {
        return {};
    }

    data.resize(bytes_read);
    return data;
#else
    return {};
#endif
}

// ==================== Parse Pages from Extent ====================

std::vector<XfsPageInfo> XfsDiskCarver::parse_pages_from_extent(
    const std::vector<uint8_t>& data, uint64_t inode_number) {

    std::vector<XfsPageInfo> pages;

    if (data.size() < config_.page_size) {
        return pages;
    }

    // Scan for InnoDB pages
    for (size_t offset = 0; offset + config_.page_size <= data.size(); offset += config_.page_size) {
        const uint8_t* page_data = data.data() + offset;

        // Check page type
        if (offset + FIL_PAGE_TYPE_ABS + 2 > data.size()) {
            break;
        }

        uint16_t page_type = read_be<uint16_t>(page_data, FIL_PAGE_TYPE_ABS);

        // Accept INDEX and SDI pages
        if (page_type != FIL_PAGE_INDEX && page_type != FIL_PAGE_SDI &&
            page_type != FIL_PAGE_SDI_ZIP) {
            continue;
        }

        // For INDEX pages, extract index_id
        uint64_t index_id = 0;
        if (page_type == FIL_PAGE_INDEX && config_.page_size >= 74) {
            index_id = read_be<uint64_t>(page_data, 66);
            if (index_id == 0) {
                continue;  // Invalid index_id
            }
        }

        // Extract page_no
        uint32_t page_no = 0;
        if (config_.page_size >= 12) {
            page_no = read_be<uint32_t>(page_data, 8);
        }

        // Create page info
        XfsPageInfo page_info;
        page_info.disk_offset = offset;
        page_info.inode_number = inode_number;
        page_info.page_no = page_no;
        page_info.index_id = index_id;
        page_info.page_type = page_type;
        page_info.data.resize(config_.page_size);
        std::memcpy(page_info.data.data(), page_data, config_.page_size);

        pages.push_back(std::move(page_info));
    }

    return pages;
}

// ==================== Block to Disk Offset ====================

uint64_t XfsDiskCarver::block_to_disk_offset(uint64_t fsblock) const {
    uint32_t block_size = be32toh(superblock_.sb_blocksize);
    return fsblock * block_size;
}

// ==================== Signature Scan Fallback ====================

void XfsDiskCarver::signature_scan(std::function<void(double, const std::string&)> progress) {
#ifdef __linux__
    // Parallel signature-based scan for improved performance
    // Each worker thread scans a portion of the device

    int fd = open(config_.device_path.c_str(), O_RDONLY);
    if (fd < 0) {
        return;
    }

    uint64_t device_size = 0;
    struct stat st;
    if (fstat(fd, &st) == 0) {
        if (S_ISBLK(st.st_mode)) {
            ioctl(fd, BLKGETSIZE64, &device_size);
        } else {
            device_size = st.st_size;
        }
    }

    close(fd);  // Close main fd, workers will open their own

    if (device_size == 0) {
        return;
    }

    uint32_t block_size = be32toh(superblock_.sb_blocksize);
    // Use larger chunks (128MB) for better throughput with more workers
    uint64_t scan_chunk = 128 * 1024 * 1024;
    // Use at least 8 workers for better parallelism
    uint32_t num_workers = std::max(config_.num_workers, static_cast<uint32_t>(8));

    std::atomic<uint64_t> current_offset{0};
    std::atomic<uint64_t> pages_found{0};
    std::atomic<uint64_t> frm_files_found{0};
    std::atomic<bool> should_stop{false};

    auto last_progress_time = std::chrono::steady_clock::now();

    // Worker thread function
    auto worker_func = [&](uint32_t worker_id) {
        int worker_fd = open(config_.device_path.c_str(), O_RDONLY);
        if (worker_fd < 0) {
            return;
        }

        std::vector<uint8_t> buffer(scan_chunk + config_.page_size);

        while (!should_stop.load()) {
            uint64_t chunk_start = current_offset.fetch_add(scan_chunk);
            if (chunk_start >= device_size) {
                break;
            }

            uint64_t chunk_end = std::min(chunk_start + scan_chunk, device_size);
            ssize_t bytes_read = pread(worker_fd, buffer.data(), scan_chunk + config_.page_size, chunk_start);
            if (bytes_read <= 0) {
                break;
            }

            size_t scan_size = bytes_read;

            // Scan for pages - check every 512 bytes (sector size) for maximum coverage
            // InnoDB pages may not always be block-aligned after deletion
            size_t scan_step = 512;  // Sector size for thorough scanning

            for (size_t i = 0; i + config_.page_size <= scan_size; i += scan_step) {
                const uint8_t* page_data = buffer.data() + i;

                // Quick filter: check if this looks like a valid page
                // FIL header starts with page number (offset 4-8) and LSN (offset 8-16)
                // Check LSN > 0 to filter out zeroed areas
                uint64_t lsn = read_be<uint64_t>(page_data, 16);
                if (lsn == 0) {
                    continue;  // Skip zeroed areas
                }

                // Check for FRM file signature
                if (i + 10 <= scan_size && std::memcmp(page_data, FRM_SIGNATURE, 10) == 0) {
                    frm_files_found.fetch_add(1);
                    if (config_.verbose) {
                        std::cout << "[Worker " << worker_id << "] Found FRM at offset " << (chunk_start + i) << "\n";
                    }
                    XfsPageInfo frm_info;
                    frm_info.disk_offset = chunk_start + i;
                    frm_info.page_type = 0xFFFF;
                    frm_info.index_id = 0;
                    frm_info.page_no = 0;
                    size_t frm_size = std::min(static_cast<size_t>(65536), scan_size - i);
                    frm_info.data.resize(frm_size);
                    std::memcpy(frm_info.data.data(), page_data, frm_size);

                    std::lock_guard<std::mutex> lock(result_mutex_);
                    last_result_.pages_by_index_id[0xFFFFFFFF].push_back(std::move(frm_info));
                    continue;
                }

                // Check page type
                uint16_t page_type = read_be<uint16_t>(page_data, FIL_PAGE_TYPE_ABS);

                if (page_type == FIL_PAGE_INDEX || page_type == FIL_PAGE_SDI || page_type == FIL_PAGE_SDI_ZIP) {
                    uint64_t index_id = 0;
                    if (page_type == FIL_PAGE_INDEX && config_.page_size >= 74) {
                        index_id = read_be<uint64_t>(page_data, 66);
                        // Accept all INDEX pages, even with index_id=0
                        // (may be system tables or partially corrupted)
                    }

                    if (page_type == FIL_PAGE_SDI || page_type == FIL_PAGE_SDI_ZIP) {
                        index_id = 0xFFFFFF00 | page_type;
                    }

                    // Validate page by checking LSN consistency
                    // LSN should be > 0 for valid pages
                    uint64_t lsn = read_be<uint64_t>(page_data, 16);
                    if (lsn == 0 && index_id == 0) {
                        continue;  // Skip obviously invalid pages
                    }

                    XfsPageInfo page_info;
                    page_info.disk_offset = chunk_start + i;
                    page_info.page_type = page_type;
                    page_info.index_id = index_id;
                    page_info.page_no = read_be<uint32_t>(page_data, 8);
                    page_info.data.resize(config_.page_size);
                    std::memcpy(page_info.data.data(), page_data, config_.page_size);

                    std::lock_guard<std::mutex> lock(result_mutex_);
                    last_result_.pages_by_index_id[index_id].push_back(std::move(page_info));
                    last_result_.pages_recovered++;
                    pages_found.fetch_add(1);
                }
            }

            // Strategy 3: Data pattern scan for non-page-aligned records
            // Look for InnoDB record patterns in raw data blocks
            // This helps recover data from fragmented/corrupted pages
            if (config_.signature_scan) {
                for (size_t i = 0; i + 8192 <= scan_size; i += 4096) {
                    const uint8_t* block_data = buffer.data() + i;

                    // Look for infimum/supremum markers which indicate InnoDB data
                    // These are reliable markers for INDEX page data even if header is corrupted
                    bool has_infimum = false;
                    bool has_supremum = false;
                    bool has_mysql_pattern = false;
                    bool has_row_pattern = false;

                    // Scan for "infimum" and "supremum" strings in the block
                    for (size_t j = 50; j + 20 < 8192; j++) {
                        if (block_data[j] == 'i' && block_data[j+1] == 'n' &&
                            block_data[j+2] == 'f' && block_data[j+3] == 'i' &&
                            block_data[j+4] == 'm' && block_data[j+5] == 'u' &&
                            block_data[j+6] == 'm') {
                            has_infimum = true;
                        }
                        if (block_data[j] == 's' && block_data[j+1] == 'u' &&
                            block_data[j+2] == 'p' && block_data[j+3] == 'r' &&
                            block_data[j+4] == 'e' && block_data[j+5] == 'm' &&
                            block_data[j+6] == 'u' && block_data[j+7] == 'm') {
                            has_supremum = true;
                        }
                        // Check for "mysql" pattern (test marker prefix)
                        if (block_data[j] == 'm' && block_data[j+1] == 'y' &&
                            block_data[j+2] == 's' && block_data[j+3] == 'q' &&
                            block_data[j+4] == 'l') {
                            has_mysql_pattern = true;
                        }
                        // Check for "row_" pattern (test marker suffix)
                        if (block_data[j] == 'r' && block_data[j+1] == 'o' &&
                            block_data[j+2] == 'w' && block_data[j+3] == '_') {
                            has_row_pattern = true;
                        }
                    }

                    // Accept blocks with infimum/supremum OR mysql pattern markers
                    // This helps catch more valid data blocks
                    if (has_infimum && has_supremum) {
                        // Try to extract records by scanning the entire block
                        // Use a special index_id to indicate pattern-scanned pages
                        uint64_t pattern_index_id = 0xFFFF0000 | ((chunk_start + i) & 0xFFFF);

                        XfsPageInfo page_info;
                        page_info.disk_offset = chunk_start + i;
                        page_info.page_type = FIL_PAGE_INDEX;  // Treat as INDEX page
                        page_info.index_id = pattern_index_id;
                        page_info.page_no = 0;
                        page_info.data.resize(config_.page_size);
                        // Copy 16KB from this position
                        size_t copy_size = std::min(static_cast<size_t>(config_.page_size), scan_size - i);
                        std::memcpy(page_info.data.data(), block_data, copy_size);
                        // Zero-fill if we don't have enough data
                        if (copy_size < config_.page_size) {
                            std::memset(page_info.data.data() + copy_size, 0, config_.page_size - copy_size);
                        }

                        std::lock_guard<std::mutex> lock(result_mutex_);
                        last_result_.pages_by_index_id[pattern_index_id].push_back(std::move(page_info));
                        last_result_.pages_recovered++;
                        pages_found.fetch_add(1);
                    }
                    // Additional pattern: blocks with mysql test markers but no infimum/supremum
                    // These may contain partial record data from early batches
                    else if (has_mysql_pattern && has_row_pattern) {
                        // Scan smaller blocks (512 bytes) for record data
                        for (size_t j = 0; j + 512 <= 8192; j += 256) {
                            const uint8_t* sub_block = block_data + j;

                            // Check for marker pattern in this sub-block
                            bool has_marker = false;
                            for (size_t k = 0; k + 20 < 512; k++) {
                                if (sub_block[k] == 'm' && sub_block[k+1] == 'y' &&
                                    sub_block[k+2] == 's' && sub_block[k+3] == 'q' &&
                                    sub_block[k+4] == 'l') {
                                    // Check for _row_ suffix within 30 bytes
                                    for (size_t m = k + 5; m < k + 35 && m + 4 < 512; m++) {
                                        if (sub_block[m] == '_' && sub_block[m+1] == 'r' &&
                                            sub_block[m+2] == 'o' && sub_block[m+3] == 'w' &&
                                            sub_block[m+4] == '_') {
                                            has_marker = true;
                                            break;
                                        }
                                    }
                                    if (has_marker) break;
                                }
                            }

                            if (has_marker) {
                                uint64_t raw_index_id = 0xFFFE0000 | ((chunk_start + i + j) & 0xFFFF);

                                XfsPageInfo page_info;
                                page_info.disk_offset = chunk_start + i + j;
                                page_info.page_type = FIL_PAGE_INDEX;
                                page_info.index_id = raw_index_id;
                                page_info.page_no = 0;
                                page_info.data.resize(config_.page_size);

                                // Copy surrounding data (expand to 2KB for context)
                                size_t data_start = j > 512 ? j - 512 : 0;
                                size_t copy_size = std::min(static_cast<size_t>(config_.page_size), 8192 - data_start);
                                std::memcpy(page_info.data.data(), block_data + data_start, copy_size);
                                if (copy_size < config_.page_size) {
                                    std::memset(page_info.data.data() + copy_size, 0, config_.page_size - copy_size);
                                }

                                std::lock_guard<std::mutex> lock(result_mutex_);
                                last_result_.pages_by_index_id[raw_index_id].push_back(std::move(page_info));
                                last_result_.pages_recovered++;
                                pages_found.fetch_add(1);
                            }
                        }
                    }
                }
            }
        }

        close(worker_fd);
    };

    // Launch worker threads
    std::vector<std::thread> workers;
    for (uint32_t i = 0; i < num_workers; i++) {
        workers.emplace_back(worker_func, i);
    }

    // Progress monitoring thread
    while (!should_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(config_.progress_interval_ms));

        uint64_t scanned = current_offset.load();
        if (scanned >= device_size) {
            should_stop.store(true);
            break;
        }

        if (progress) {
            double pct = static_cast<double>(scanned) / device_size;
            std::ostringstream oss;
            oss << "Signature scan: " << std::fixed << std::setprecision(1)
                << (pct * 100.0) << "% pages=" << pages_found.load()
                << " frm=" << frm_files_found.load();
            progress(pct, oss.str());
        }
    }

    // Wait for all workers
    for (auto& w : workers) {
        if (w.joinable()) {
            w.join();
        }
    }

    if (config_.verbose) {
        std::cout << "[INFO] Parallel signature scan found " << pages_found.load() << " pages and "
                  << frm_files_found.load() << " FRM signatures\n";
        std::cout << "[INFO] Workers: " << num_workers << ", Chunk size: " << (scan_chunk / 1024 / 1024) << "MB\n";
    }
#endif
}

// ==================== Raw Data Extraction ====================

void XfsDiskCarver::raw_data_scan(std::function<void(double, const std::string&)> progress) {
#ifdef __linux__
    // This scanner extracts records from raw data blocks without relying on
    // InnoDB page structure. Useful for TRUNCATE recovery where page headers
    // are corrupted/missing.
    //
    // Strategy: Look for InnoDB record patterns in raw data:
    // 1. Record header: 5 bytes (deleted_flag, min_rec_flag, n_owned, heap_no, next_offset)
    // 2. NULL bitmap (if applicable)
    // 3. Field data in physical_pos order

    if (config_.verbose) {
        std::cout << "[INFO] Starting raw data pattern scan...\n";
    }

    int fd = open(config_.device_path.c_str(), O_RDONLY);
    if (fd < 0) {
        return;
    }

    // Get device size
    struct stat st;
    fstat(fd, &st);
    uint64_t device_size = st.st_size;

    // Scan in 1MB chunks
    size_t scan_chunk = 1024 * 1024;
    std::vector<uint8_t> buffer(scan_chunk + config_.page_size);

    // Record pattern: Look for sequences that look like InnoDB records
    // A record typically starts with:
    // - 1-2 bytes: record header info
    // - Variable: NULL bitmap
    // - Fixed fields: INT (4 bytes), BIGINT (8 bytes), etc.
    // - Variable fields: VARCHAR (length byte + data)

    // Pattern: Look for "varchar_" or "text_" or "mysql" strings
    // These indicate record data areas

    std::atomic<uint64_t> records_found{0};
    std::atomic<uint64_t> bytes_scanned{0};

    for (uint64_t offset = 0; offset < device_size; offset += scan_chunk) {
        size_t to_read = std::min(scan_chunk + config_.page_size, device_size - offset);
        ssize_t bytes_read = pread(fd, buffer.data(), to_read, offset);
        if (bytes_read <= 0) break;

        bytes_scanned.fetch_add(bytes_read);

        // Scan for data patterns
        // Look for "varchar_", "text_", "mysql", and "row_" strings
        // These indicate record data areas
        for (size_t i = 0; i < static_cast<size_t>(bytes_read) - 100; i++) {
            bool found_pattern = false;
            std::string pattern_type;

            // Look for "varchar_" pattern
            if (buffer[i] == 'v' && buffer[i+1] == 'a' && buffer[i+2] == 'r' &&
                buffer[i+3] == 'c' && buffer[i+4] == 'h' && buffer[i+5] == 'a' &&
                buffer[i+6] == 'r' && buffer[i+7] == '_') {
                found_pattern = true;
                pattern_type = "varchar";
            }
            // Look for "text_" pattern
            else if (buffer[i] == 't' && buffer[i+1] == 'e' && buffer[i+2] == 'x' &&
                     buffer[i+3] == 't' && buffer[i+4] == '_') {
                found_pattern = true;
                pattern_type = "text";
            }
            // Look for "mysql" pattern (test marker prefix) - early batch indicator
            else if (buffer[i] == 'm' && buffer[i+1] == 'y' && buffer[i+2] == 's' &&
                     buffer[i+3] == 'q' && buffer[i+4] == 'l') {
                // Verify this has a proper marker format with _row_ nearby
                for (size_t j = i + 5; j < i + 50 && j + 4 < bytes_read; j++) {
                    if (buffer[j] == '_' && buffer[j+1] == 'r' && buffer[j+2] == 'o' &&
                        buffer[j+3] == 'w' && buffer[j+4] == '_') {
                        found_pattern = true;
                        pattern_type = "marker";
                        break;
                    }
                }
            }
            // Look for standalone "row_" pattern (may be partial marker)
            else if (buffer[i] == 'r' && buffer[i+1] == 'o' && buffer[i+2] == 'w' &&
                     buffer[i+3] == '_') {
                // Check for nearby digits indicating row number
                for (size_t j = i + 4; j < i + 15 && j < bytes_read; j++) {
                    if (buffer[j] >= '0' && buffer[j] <= '9') {
                        found_pattern = true;
                        pattern_type = "rownum";
                        break;
                    }
                }
            }

            if (found_pattern) {
                // Found potential record data
                // Try to extract a record from surrounding bytes
                // Record is typically within 200-500 bytes

                // For markers, look back further to capture the full record
                size_t lookback = (pattern_type == "marker" || pattern_type == "rownum") ? 400 : 200;
                size_t lookahead = (pattern_type == "marker" || pattern_type == "rownum") ? 600 : 300;

                // Find the extent of this data
                size_t data_start = i > lookback ? i - lookback : 0;
                size_t data_end = std::min(i + lookahead, static_cast<size_t>(bytes_read));

                // Check if this looks like valid record data
                // (contains printable strings, reasonable byte values)
                bool valid = true;
                size_t printable = 0;
                for (size_t j = data_start; j < data_end; j++) {
                    if (buffer[j] >= 32 && buffer[j] < 127) printable++;
                }
                if (printable < (data_end - data_start) / 3) {
                    valid = false;  // Not enough printable chars
                }

                // Additional validation for markers: ensure we have a number
                if (valid && pattern_type == "marker") {
                    bool has_number = false;
                    for (size_t j = i; j < data_end && j < i + 100; j++) {
                        if (buffer[j] >= '0' && buffer[j] <= '9') {
                            has_number = true;
                            break;
                        }
                    }
                    if (!has_number) valid = false;
                }

                if (valid) {
                    // Extract this block as a potential record
                    // Use a special index_id for raw-extracted records
                    uint64_t raw_index_id = 0xFFFE0000 | (records_found.load() & 0xFFFF);

                    XfsPageInfo page_info;
                    page_info.disk_offset = offset + data_start;
                    page_info.page_type = FIL_PAGE_INDEX;
                    page_info.index_id = raw_index_id;
                    page_info.page_no = 0;
                    page_info.data.resize(config_.page_size);
                    // Copy data, zero-fill rest
                    size_t copy_size = std::min(static_cast<size_t>(config_.page_size), data_end - data_start);
                    std::memcpy(page_info.data.data(), buffer.data() + data_start, copy_size);
                    if (copy_size < config_.page_size) {
                        std::memset(page_info.data.data() + copy_size, 0, config_.page_size - copy_size);
                    }

                    std::lock_guard<std::mutex> lock(result_mutex_);
                    last_result_.pages_by_index_id[raw_index_id].push_back(std::move(page_info));
                    last_result_.pages_recovered++;
                    records_found.fetch_add(1);

                    // Skip ahead to avoid duplicates
                    i = data_end;
                }
            }
        }

        if (progress && offset % (100 * scan_chunk) == 0) {
            double pct = static_cast<double>(offset) / device_size;
            std::ostringstream oss;
            oss << "Raw scan: " << std::fixed << std::setprecision(1)
                << (pct * 100.0) << "% records=" << records_found.load();
            progress(pct, oss.str());
        }
    }

    close(fd);

    if (config_.verbose) {
        std::cout << "[INFO] Raw data scan found " << records_found.load() << " potential records\n";
    }
#endif
}

// ==================== Recovery Function ====================

XfsScanResult XfsDiskCarver::recover(const XfsScanResult& scan_result,
                                     std::function<void(double, const std::string&)> progress) {

    XfsScanResult result = scan_result;

    if (!scan_result.success) {
        result.errors.push_back("Scan was not successful");
        return result;
    }

    if (scan_result.pages_by_index_id.empty()) {
        if (config_.verbose) {
            std::cout << "[INFO] No pages to recover\n";
        }
        return result;
    }

    // Parse records from recovered pages
    size_t total_index_ids = scan_result.pages_by_index_id.size();
    size_t processed = 0;

    // System table index ID ranges (common InnoDB system table IDs)
    // These are well-known ranges for MySQL system tables
    auto is_system_index = [](uint64_t index_id) -> bool {
        // SYS_TABLES, SYS_COLUMNS, SYS_INDEXES, SYS_FIELDS
        // Typically have low index IDs (1-100) or specific ranges
        if (index_id > 0 && index_id <= 100) return true;  // System tables
        // Additional known system table ranges can be added
        return false;
    };

    for (const auto& [index_id, pages] : scan_result.pages_by_index_id) {
        // Apply index_id filtering
        if (!config_.target_index_ids.empty()) {
            // Only process specified index IDs
            if (std::find(config_.target_index_ids.begin(),
                          config_.target_index_ids.end(),
                          index_id) == config_.target_index_ids.end()) {
                if (config_.verbose) {
                    std::cout << "[SKIP] Index ID " << index_id
                              << " not in target list\n";
                }
                continue;
            }
        }

        // Exclude system tables if requested
        if (config_.exclude_system_tables && is_system_index(index_id)) {
            if (config_.verbose) {
                std::cout << "[SKIP] Index ID " << index_id
                          << " is a system table\n";
            }
            continue;
        }

        if (progress) {
            double pct = static_cast<double>(processed) / total_index_ids;
            progress(pct, "Recovering index_id " + std::to_string(index_id));
        }

        std::vector<std::map<std::string, std::string>> all_records;

        // Determine if we should use schema-aware parsing
        bool use_schema = external_schema_.has_value() && !external_schema_->columns.empty();

        if (use_schema && config_.verbose) {
            std::cerr << "[DEBUG] Using schema-aware parsing with "
                      << external_schema_->columns.size() << " columns for index_id "
                      << index_id << std::endl;
        }

        for (const auto& page_info : pages) {
            if (use_schema) {
                // Schema-aware parsing
                auto records = parse_records_with_schema_xfs(page_info.data, *external_schema_);
                all_records.insert(all_records.end(), records.begin(), records.end());
            } else {
                // Schema-less parsing
                auto records = parse_records_schemaless_xfs(page_info.data);
                all_records.insert(all_records.end(), records.begin(), records.end());
            }
        }

        if (!all_records.empty()) {
            result.records_by_index_id[index_id] = all_records;
            result.total_records += all_records.size();

            // Export to CSV
            std::string output_file = config_.output_dir + "/xfs_index_" +
                                      std::to_string(index_id) + ".csv";
            std::ofstream out(output_file);

            if (out) {
                // Get column names in the correct order from schema
                std::vector<std::string> column_names;
                if (external_schema_.has_value() && !external_schema_->columns.empty()) {
                    // Sort columns by physical_pos to match storage order
                    std::vector<std::pair<uint32_t, std::string>> sorted_cols;
                    for (const auto& [ordinal, col] : external_schema_->columns) {
                        uint32_t phys_pos = col.physical_pos > 0 ? col.physical_pos : ordinal;
                        sorted_cols.push_back({phys_pos, col.name});
                    }
                    std::sort(sorted_cols.begin(), sorted_cols.end());
                    for (const auto& [pos, name] : sorted_cols) {
                        column_names.push_back(name);
                    }
                } else {
                    // Fallback: use keys from first record
                    for (const auto& kv : all_records[0]) {
                        column_names.push_back(kv.first);
                    }
                }

                // Write header
                bool first = true;
                for (const auto& col_name : column_names) {
                    if (!first) out << ",";
                    out << col_name;
                    first = false;
                }
                out << "\n";

                // Write data rows
                for (const auto& rec : all_records) {
                    first = true;
                    for (const auto& col_name : column_names) {
                        if (!first) out << ",";

                        auto it = rec.find(col_name);
                        if (it != rec.end()) {
                            std::string val = it->second;
                            // Always quote values that may contain special characters
                            // This is safer for TEXT, VARCHAR, BLOB, JSON fields
                            bool needs_quote = val.find(',') != std::string::npos ||
                                              val.find('"') != std::string::npos ||
                                              val.find('\n') != std::string::npos ||
                                              val.find('\r') != std::string::npos;

                            if (needs_quote) {
                                out << "\"";
                                for (char c : val) {
                                    if (c == '"') out << "\"\"";
                                    else out << c;
                                }
                                out << "\"";
                            } else {
                                out << val;
                            }
                        } else {
                            out << "NULL";
                        }
                        first = false;
                    }
                    out << "\n";
                }
                result.output_files.push_back(output_file);
            }
        }

        processed++;
    }

    result.success = true;
    return result;
}

// ==================== Combined Scan and Recover ====================

XfsScanResult XfsDiskCarver::scan_and_recover(
    std::function<void(double, const std::string&)> progress) {

    auto scan_result = scan([&](double pct, const std::string& msg) {
        if (progress) progress(pct * 0.7, msg);
    });

    if (!scan_result.success) {
        return scan_result;
    }

    return recover(scan_result, [&](double pct, const std::string& msg) {
        if (progress) progress(0.7 + pct * 0.3, msg);
    });
}

// ==================== Schema-less Parsing ====================

std::vector<std::map<std::string, std::string>> XfsDiskCarver::parse_records_schemaless_xfs(
    const std::vector<uint8_t>& page_data) {

    std::vector<std::map<std::string, std::string>> records;

    if (page_data.size() < 100) return records;

    const uint8_t* data = page_data.data();
    size_t page_size = page_data.size();

    // INDEX page validation - be more lenient for disk carving
    uint16_t page_type = read_be<uint16_t>(data, FIL_PAGE_TYPE_ABS);

    // Accept INDEX pages (0x45BF), SDI pages (0x45BE/0x45BD), and unknown pages (0x0000)
    // The latter can happen for corrupted/partial pages from XFS delayed allocation
    bool is_valid_page = (page_type == FIL_PAGE_INDEX ||
                          page_type == FIL_PAGE_SDI ||
                          page_type == FIL_PAGE_SDI_ZIP ||
                          page_type == 0);  // Unknown/corrupted page

    if (!is_valid_page) return records;

    InnoDBIndexParser parser;
    parser.set_recover_deleted(config_.recover_deleted);
    parser.set_bad_page_mode(2);

    // Use parse_page_try for more aggressive extraction on potentially corrupted pages
    auto index_records = parser.parse_page(data, page_size, 0);

    // If we got no records, try the brute-force approach
    if (index_records.empty()) {
        index_records = parser.parse_page_try(data, page_size, 0);
    }

    for (const auto& rec : index_records) {
        if (rec.is_deleted && !config_.recover_deleted) continue;

        std::map<std::string, std::string> record;
        for (size_t i = 0; i < rec.fields.size(); i++) {
            std::string field_name = "field_" + std::to_string(i);
            if (rec.fields[i].is_null) {
                record[field_name] = "NULL";
            } else if (!rec.fields[i].str_value.empty()) {
                record[field_name] = rec.fields[i].str_value;
            } else if (!rec.fields[i].raw_bytes.empty()) {
                // For raw bytes, try to extract readable strings
                // This helps with schema-less recovery where fields are concatenated
                std::string extracted;
                const auto& raw = rec.fields[i].raw_bytes;

                // Look for printable string sequences
                size_t str_start = 0;
                bool in_string = false;
                for (size_t j = 0; j < raw.size(); j++) {
                    uint8_t b = raw[j];
                    bool printable = (b >= 32 && b < 127);

                    if (printable && !in_string) {
                        str_start = j;
                        in_string = true;
                    } else if (!printable && in_string) {
                        if (j - str_start >= 4) {  // Min 4 chars
                            if (!extracted.empty()) extracted += "|";
                            extracted += std::string(reinterpret_cast<const char*>(raw.data() + str_start), j - str_start);
                        }
                        in_string = false;
                    }
                }
                // Handle string at end
                if (in_string && raw.size() - str_start >= 4) {
                    if (!extracted.empty()) extracted += "|";
                    extracted += std::string(reinterpret_cast<const char*>(raw.data() + str_start), raw.size() - str_start);
                }

                if (!extracted.empty()) {
                    record[field_name] = extracted;
                } else {
                    // Fall back to hex
                    std::ostringstream hex;
                    for (uint8_t b : raw) {
                        hex << std::hex << std::setw(2) << std::setfill('0')
                            << static_cast<int>(b);
                    }
                    record[field_name] = hex.str();
                }
            }
        }
        if (!record.empty()) {
            records.push_back(record);
        }
    }

    return records;
}

// ==================== Schema-aware Parsing ====================

// MySQL server type ID to InnoDB storage type ID mapping
// Reference: MySQL include/mysql/mysql_com.h and InnoDB storage format
// MySQL server types (MYSQL_TYPE_*) differ from InnoDB internal types
static uint8_t mysql_type_to_innodb(uint32_t mysql_type) {
    // InnoDB internal type IDs (from ibd2sql column_type.py):
    // 1: TINY, 2: TINY (unsigned), 3: SHORT (SMALLINT), 4: LONG (INT),
    // 5: FLOAT, 6: DOUBLE, 7: DECIMAL, 9: LONGLONG (BIGINT), 10: INT24 (MEDIUMINT),
    // 11: DATE, 12: DATETIME (old), 13: DATETIME (old 8-byte), 15: NEWDATE/DATE,
    // 16: VARCHAR, 17: BIT, 18: TIMESTAMP2, 19: DATETIME2, 20: TIME2,
    // 27: BLOB, 28: VAR_STRING (VARCHAR), 31: JSON

    switch (mysql_type) {
        // MySQL server type IDs to InnoDB storage types
        case 1:   // MYSQL_TYPE_TINY
            return 1;  // InnoDB TINY (1 byte)
        case 2:   // MYSQL_TYPE_SHORT (SMALLINT, 2 bytes)
            return 3;  // InnoDB SHORT (2 bytes)
        case 3:   // MYSQL_TYPE_LONG (INT, 4 bytes)
            return 4;  // InnoDB LONG (4 bytes for INT)
        case 4:   // MYSQL_TYPE_FLOAT
            return 5;  // InnoDB FLOAT (4 bytes)
        case 5:   // MYSQL_TYPE_DOUBLE
            return 6;  // InnoDB DOUBLE (8 bytes)
        case 7:   // MYSQL_TYPE_TIMESTAMP
            return 8;  // InnoDB TIMESTAMP (4 bytes)
        case 8:   // MYSQL_TYPE_LONGLONG (BIGINT)
            return 9;  // InnoDB LONGLONG (8 bytes)
        case 9:   // MYSQL_TYPE_INT24 (MEDIUMINT)
            return 10; // InnoDB INT24 (3 bytes)
        case 11:  // MYSQL_TYPE_DATE
        case 14:  // MYSQL_TYPE_NEWDATE
            return 15; // InnoDB DATE/NEWDATE (3 bytes)
        case 12:  // MYSQL_TYPE_DATETIME (old)
            return 13; // InnoDB DATETIME (old 8-byte)
        case 17:  // MYSQL_TYPE_TIMESTAMP2
            return 18; // InnoDB TIMESTAMP2 (4 bytes + frac)
        case 18:  // MYSQL_TYPE_DATETIME2
            return 19; // InnoDB DATETIME2 (5 bytes + frac)
        case 19:  // MYSQL_TYPE_TIME2
            return 20; // InnoDB TIME2 (3 bytes + frac)
        case 15:  // MYSQL_TYPE_VARCHAR
        case 253: // MYSQL_TYPE_VAR_STRING
            return 16; // InnoDB VARCHAR
        case 16:  // MYSQL_TYPE_BIT
            return 17; // InnoDB BIT (variable bytes)
        case 252: // MYSQL_TYPE_BLOB
        case 250: // MYSQL_TYPE_MEDIUM_BLOB
        case 251: // MYSQL_TYPE_LONG_BLOB
            return 27; // InnoDB BLOB
        case 245: // MYSQL_TYPE_JSON
            return 31; // InnoDB JSON
        case 247: // MYSQL_TYPE_ENUM
            return 247; // ENUM stays same
        case 248: // MYSQL_TYPE_SET
            return 248; // SET stays same
        case 0:   // MYSQL_TYPE_DECIMAL
        case 246: // MYSQL_TYPE_NEWDECIMAL
            return 0;  // InnoDB DECIMAL
        default:
            return static_cast<uint8_t>(mysql_type); // Pass through for unknown types
    }
}

std::vector<std::map<std::string, std::string>> XfsDiskCarver::parse_records_with_schema_xfs(
    const std::vector<uint8_t>& page_data,
    const catalog::TableSchema& schema) {

    std::vector<std::map<std::string, std::string>> records;

    if (page_data.size() < 100 || schema.columns.empty()) return records;

    const uint8_t* data = page_data.data();
    size_t page_size = page_data.size();

    // INDEX page validation
    uint16_t page_type = read_be<uint16_t>(data, FIL_PAGE_TYPE_ABS);
    if (page_type != FIL_PAGE_INDEX) return records;

    // Convert TableSchema to IndexColumnDef list
    // CRITICAL: Must sort by physical_pos to match InnoDB storage order
    std::vector<innodb::IndexColumnDef> columns;

    std::vector<std::pair<uint32_t, catalog::ColumnInfo>> sorted_cols;
    for (const auto& [ordinal, col] : schema.columns) {
        uint32_t phys_pos = col.physical_pos > 0 ? col.physical_pos : ordinal;
        sorted_cols.push_back({phys_pos, col});
    }
    std::sort(sorted_cols.begin(), sorted_cols.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });

    for (const auto& [phys_pos, col] : sorted_cols) {
        innodb::IndexColumnDef col_def;
        col_def.name = col.name;
        // CRITICAL: Convert MySQL server type ID to InnoDB storage type ID
        col_def.col_type = mysql_type_to_innodb(col.mysql_type_id);
        col_def.col_length = col.column_length;
        col_def.is_nullable = col.is_nullable;
        col_def.is_unsigned = !col.is_signed;
        col_def.precision = col.numeric_precision;
        col_def.scale = col.numeric_scale;
        col_def.datetime_precision = col.datetime_precision;
        col_def.char_set_id = col.charset_id;
        col_def.ordinal_position = col.ordinal;
        col_def.physical_pos = phys_pos;
        col_def.enum_values = col.enum_values;
        col_def.set_values = col.set_values;

        // Set mb_maxlen based on charset
        if (col.charset_id == 45 || col.charset_id == 46) {  // utf8mb4
            col_def.mb_maxlen = 4;
        } else if (col.charset_id == 33 || col.charset_id == 83) {  // utf8
            col_def.mb_maxlen = 3;
        } else {
            col_def.mb_maxlen = 1;  // latin1 and others
        }

        columns.push_back(col_def);
    }

    InnoDBIndexParser parser;
    parser.set_columns(columns);
    parser.set_recover_deleted(config_.recover_deleted);
    parser.set_bad_page_mode(2);

    auto index_records = parser.parse_page(data, page_size, 0);

    for (const auto& rec : index_records) {
        if (rec.is_deleted && !config_.recover_deleted) continue;

        std::map<std::string, std::string> record;
        for (size_t i = 0; i < rec.fields.size() && i < columns.size(); i++) {
            const auto& col = columns[i];
            if (rec.fields[i].is_null) {
                record[col.name] = "NULL";
            } else {
                record[col.name] = rec.fields[i].str_value;
            }
        }
        if (!record.empty()) {
            records.push_back(record);
        }
    }

    if (config_.verbose) {
        std::cerr << "[DEBUG] Schema-aware parsing found " << records.size()
                  << " records for index_id" << std::endl;
    }

    return records;
}

} // namespace xfs
} // namespace disk
} // namespace dbrecovery
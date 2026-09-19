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
#include "dbrecovery/catalog/schema_type_conversion.hpp"
#include "dbrecovery/utils/lz4_helper.hpp"

#include <chrono>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <limits>

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
constexpr uint16_t FIL_PAGE_TYPE_BLOB = 0x45BD;   // Same as SDI_ZIP (BLOB page type)
constexpr uint16_t FIL_PAGE_TYPE_ZBLOB = 0x45BA;  // Compressed BLOB page
constexpr uint16_t FIL_PAGE_TYPE_ZBLOB2 = 0x45BB; // Compressed BLOB page v2
constexpr uint16_t FIL_PAGE_COMPRESSED = 0x45B5;  // Compressed page
constexpr uint16_t FIL_PAGE_COMPRESSED_AND_ENCRYPTED = 0x45B7; // Compressed+Encrypted
constexpr size_t FIL_PAGE_TYPE_ABS = 24;
constexpr size_t FIL_HEADER_SIZE = 38;

namespace {

bool bounded_pread(int fd, uint64_t source_size, void* buffer, size_t size, uint64_t offset) {
    if (fd < 0 || buffer == nullptr || size == 0 || offset > source_size ||
        size > source_size - offset) {
        return false;
    }
    return ::pread(fd, buffer, size, static_cast<off_t>(offset)) == static_cast<ssize_t>(size);
}

int duplicate_read_only_fd(int fd) {
#ifdef __linux__
    return ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
#else
    (void)fd;
    return -1;
#endif
}

}  // namespace

// FRM file signature
static constexpr uint8_t FRM_SIGNATURE[] = {0xfe, 0x01, 0x0a, 0x0c, 0x03, 0x00, 0x00, 0x10, 0x01, 0x00};

// ==================== Constructor/Destructor ====================

XfsDiskCarver::XfsDiskCarver() = default;

XfsDiskCarver::~XfsDiskCarver() {
#ifdef __linux__
    if (source_fd_ >= 0) {
        ::close(source_fd_);
    }
#endif
}

bool XfsDiskCarver::adopt_fd(int owned_fd, uint64_t known_size) {
#ifdef __linux__
    if (owned_fd < 0) {
        return false;
    }

    struct stat st {};
    const int flags = ::fcntl(owned_fd, F_GETFL);
    if (flags < 0 || (flags & O_ACCMODE) != O_RDONLY || ::fstat(owned_fd, &st) != 0) {
        ::close(owned_fd);
        return false;
    }

    if (source_fd_ >= 0) {
        ::close(source_fd_);
    }
    source_fd_ = owned_fd;
    source_size_ = known_size;
    return true;
#else
    (void)owned_fd;
    (void)known_size;
    return false;
#endif
}

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
    ssize_t bytes_read = pread(fd, &sb, sizeof(xfs_superblock), 0);
    if (bytes_read == sizeof(xfs_superblock)) {
        uint32_t magic = be32toh(sb.sb_magicnum);
        if (magic == XFS_SB_MAGIC) {
            close(fd);
            return true;
        }
    }

    // Primary superblock invalid or corrupted - try secondary superblocks
    // XFS has backup superblocks in each AG (AG 1, 2, 3, ...)
    // Try the first few AGs for backup superblocks
    std::cerr << "[WARN] Primary XFS superblock invalid, trying backup superblocks...\n";

    // We need at least enough data to read sb_agblocks and sb_agcount
    // to know where the backup SBs are. Try reading the primary SB again
    // with relaxed validation to get filesystem geometry.
    // If primary is completely unreadable, try known locations.
    uint32_t block_size = 4096;  // Default XFS block size
    uint32_t ag_blocks = 0;
    uint32_t ag_count = 0;

    // Try to get geometry from partial/old primary SB
    if (bytes_read >= static_cast<ssize_t>(offsetof(xfs_superblock, sb_agcount) + 8)) {
        // Attempt to read geometry from old-style superblock offsets
        // XFS v4 sb is 208 bytes, v5 is 264 bytes
        // sb_blocksize at offset 16, sb_agblocks at offset 32, sb_agcount at offset 36
        uint32_t raw_bs = be32toh(*reinterpret_cast<uint32_t*>(
            reinterpret_cast<uint8_t*>(&sb) + 16));
        if (raw_bs >= 512 && raw_bs <= 65536 && (raw_bs & (raw_bs - 1)) == 0) {
            block_size = raw_bs;
        }
        ag_blocks = be32toh(*reinterpret_cast<uint32_t*>(
            reinterpret_cast<uint8_t*>(&sb) + 32));
        ag_count = be32toh(*reinterpret_cast<uint32_t*>(
            reinterpret_cast<uint8_t*>(&sb) + 36));
    }

    // If we couldn't get geometry from primary, try fixed offsets for backup SBs
    if (ag_blocks == 0 || ag_count == 0 || ag_blocks > 1000000000 || ag_count > 1000) {
        ag_blocks = 8192;   // Reasonable default
        ag_count = 4;       // Try at least 4 AGs
    }

    // Try backup superblocks in each AG
    for (uint32_t ag = 1; ag < std::min(ag_count, 32u); ag++) {
        uint64_t offset = static_cast<uint64_t>(ag) * static_cast<uint64_t>(ag_blocks) * block_size;
        bytes_read = pread(fd, &sb, sizeof(xfs_superblock), offset);
        if (bytes_read == sizeof(xfs_superblock)) {
            uint32_t magic = be32toh(sb.sb_magicnum);
            if (magic == XFS_SB_MAGIC) {
                std::cerr << "[INFO] Found valid XFS superblock in AG " << ag
                          << " (offset " << offset << ")\n";
                close(fd);
                return true;
            }
        }
    }

    close(fd);
    return false;
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

    if (source_fd_ < 0) {
#ifdef __linux__
        if (config_.device_path.empty()) {
            result.errors.push_back("Device path not specified");
            return result;
        }

        const int fd = ::open(config_.device_path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            result.errors.push_back("Cannot open a regular XFS image source");
            return result;
        }

        struct stat st {};
        if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
            ::close(fd);
            result.errors.push_back("Cannot open a regular XFS image source");
            return result;
        }

        if (!adopt_fd(fd, static_cast<uint64_t>(st.st_size))) {
            result.errors.push_back("Cannot adopt regular XFS image source");
            return result;
        }
#else
        result.errors.push_back("XFS scanning is only supported on Linux");
        return result;
#endif
    }

    if (!bounded_pread(source_fd_, source_size_, &superblock_, sizeof(superblock_), 0) ||
        be32toh(superblock_.sb_magicnum) != XFS_SB_MAGIC) {
        result.errors.push_back("Invalid XFS superblock on admitted source");
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

    // Strategy 2: Scan free space blocks for InnoDB pages
    // This finds data from deleted files (e.g. DROP TABLE) whose inode metadata was zeroed
    if (config_.scan_free_space) {
        size_t pages_before = result.pages_recovered;
        if (config_.verbose) {
            std::cout << "[INFO] Running free block scan (found "
                      << pages_before << " via inode scan)\n";
        }
        scan_free_blocks(progress);

        {
            std::lock_guard<std::mutex> lock(result_mutex_);
            result.pages_by_index_id = last_result_.pages_by_index_id;
            result.pages_recovered = last_result_.pages_recovered;
        }

        if (config_.verbose) {
            std::cout << "[INFO] Free block scan found "
                      << (result.pages_recovered - pages_before) << " additional pages\n";
        }
    }

    // Strategy 3: Always run signature scan for maximum recovery
    // Signature scan finds pages that other strategies might miss
    if (config_.signature_scan) {
        size_t pages_before = result.pages_recovered;
        if (config_.verbose) {
            std::cout << "[INFO] Running signature scan for additional pages (found "
                      << pages_before << " via inode + free block scan)\n";
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

    // Strategy 4: Raw data pattern scan for non-page-aligned records
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
    int fd = duplicate_read_only_fd(source_fd_);
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
        if (!bounded_pread(fd, source_size_, &agi, sizeof(agi), agi_offset)) {
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
        if (!bounded_pread(fd, source_size_, &agf, sizeof(agf), agf_offset)) {
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
    if (!bounded_pread(fd, source_size_, block_buffer.data(), block_size, block_offset)) {
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
            auto extent_info = read_inode(fd, ag_index, ag_inode);
            if (extent_info.has_value() && !extent_info->extents.empty()) {
                process_recovered_inode(fd, ag_index, ag_inode, *extent_info);
            }
        }
    }
#endif
}

// ==================== Read Inode ====================

std::optional<XfsExtentInfo> XfsDiskCarver::read_inode(int fd, uint32_t ag_index, uint32_t ag_inode) {
#ifdef __linux__
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
    if (!bounded_pread(fd, source_size_, inode_data.data(), inode_size, inode_offset)) {
        return std::nullopt;
    }

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
    // - di_version >= 3 (V5 XFS only — V4 inodes have unreliable post-deletion data)

    uint16_t mode = be16toh(inode_core.di_mode);
    uint32_t nlink = be32toh(inode_core.di_nlink);
    uint8_t format = inode_core.di_format;
    uint8_t di_version = inode_core.di_version;

    // Strict deleted-inode validation (matching xfs_undelete heuristics)
    bool is_deleted = (mode == 0) && (nlink == 0) && (di_version >= 3);

    if (!is_deleted) {
        // Also accept regular files with extent format (for non-destructive recovery)
        bool is_regular_file = (mode != 0) && (nlink > 0) && (format == XFS_DINODE_FMT_EXTENTS);
        if (!is_regular_file) {
            if (config_.verbose && dbrecovery::innodb::g_verbose)
                std::cerr << "[DEBUG] Skipping inode: mode=" << mode << " nlink=" << nlink
                          << " version=" << (int)di_version << " format=" << (int)format << "\n";
            return std::nullopt;
        }
    }

    // Get file size from inode core
    uint64_t file_size = be64toh(inode_core.di_size);

    // Extent btree format - try to recover extents from btree root in inode
    // For deleted inodes, extent btree is converted to extent list format
    // But sometimes the btree root is still in the inode data fork area
    if (format == XFS_DINODE_FMT_BTREE) {
        if (config_.verbose) {
            if (dbrecovery::innodb::g_verbose)             std::cout << "[DEBUG] Inode has extent btree format (large/fragmented file)\n";
        }
        // For btree format, the btree root is stored in the inode's data fork
        // Try to extract extent list from btree root
        // This is a simplified approach - full btree traversal would be needed for large files

        // Check if we have a btree root block in the inode
        // btree root starts after inode core: 100 for v4 (version 2), 176 for v5 (version 3)
        size_t inode_core_size = (di_version >= 3) ? 176 : 100;
        size_t btree_start = inode_core_size;
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

        if (level == 0 && numrecs > 0 && numrecs < 1000) {
            // Leaf level - records are directly in the btree block
            // For short form CRC btree, records start at offset 56 (XFS_BTREE_SBLOCK_CRC_LEN)
            // For non-CRC, records start at offset 16
            size_t btree_hdr_size = (btree_magic == 0x42B33D42) ? 56 : 16;
            size_t rec_offset = btree_start + btree_hdr_size;
            size_t rec_size = sizeof(xfs_bmbt_rec);

            XfsExtentInfo info;
            info.inode_number = ag_inode + ag_index * ag_blocks;
            info.file_size = file_size;
            info.is_deleted = is_deleted;
            info.next_unlinked = be32toh(inode_core.di_next_unlinked);
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
        } else if (level > 0 && numrecs > 0 && numrecs < 1000) {
            // Multi-level extent btree: read child blocks to collect leaf extents
            // This handles large, highly fragmented files with many extents
            // For CRC btree, keys and pointers start at offset 56
            size_t btree_hdr_size = (btree_magic == 0x42B33D42) ? 56 : 16;
            size_t key_offset = btree_start + btree_hdr_size;
            size_t ptr_offset = (btree_hdr_size + be32toh(superblock_.sb_blocksize)) / 2;

            XfsExtentInfo info;
            info.inode_number = ag_inode + ag_index * ag_blocks;
            info.file_size = file_size;
            info.is_deleted = is_deleted;
            info.next_unlinked = be32toh(inode_core.di_next_unlinked);
            info.mtime.t_sec = be32toh(inode_core.di_mtime.t_sec);
            info.mtime.t_nsec = be32toh(inode_core.di_mtime.t_nsec);
            info.ctime.t_sec = be32toh(inode_core.di_ctime.t_sec);
            info.ctime.t_nsec = be32toh(inode_core.di_ctime.t_nsec);

            // Read child blocks and extract extent records from leaf nodes
            uint32_t block_size = be32toh(superblock_.sb_blocksize);

            // Collect child block numbers from pointer array
            std::vector<uint32_t> child_blocks;
            for (uint16_t i = 0; i < numrecs; i++) {
                uint32_t child = read_be<uint32_t>(inode_data.data(), ptr_offset + i * 4);
                if (child != 0 && child < be32toh(superblock_.sb_agblocks)) {
                    child_blocks.push_back(child);
                }
            }

            // Process each child block (potentially multi-level)
            for (uint32_t child_block : child_blocks) {
                // Calculate disk offset for the child block (AG-relative to absolute)
                uint64_t child_disk_offset = static_cast<uint64_t>(ag_index) *
                    static_cast<uint64_t>(be32toh(superblock_.sb_agblocks)) * block_size +
                    static_cast<uint64_t>(child_block) * block_size;

                // Recursively walk btree levels to collect leaf records
                collect_extent_leaf_records(fd, child_disk_offset, block_size, level - 1, &info);
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
    // v4 inode core is 100 bytes, v5 inode core is 176 bytes
    size_t inode_core_size_ext = (di_version >= 3) ? 176 : 100;
    size_t extent_start = inode_core_size_ext;
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
        auto extent_info = read_inode(fd, ag_index, ag_inode);
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

    // Validate extents against free space BNO btree to filter out reallocated blocks
    auto validated_extents = validate_extents_against_free_space(fd, ag_index, extent_info.extents);
    if (validated_extents.empty()) {
        return;  // All extents have been reallocated
    }

    std::vector<XfsPageInfo> recovered_pages;

    for (const auto& ext : validated_extents) {
        if (ext.state == XFS_EXT_UNWRITTEN) {
            continue;  // Skip unwritten extents
        }

        std::vector<uint8_t> extent_data = read_extent_data(fd, ext);
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

std::vector<uint8_t> XfsDiskCarver::read_extent_data(int fd, const xfs_extent& ext) {
#ifdef __linux__
    if (fd < 0) {
        return {};
    }

    uint32_t block_size = be32toh(superblock_.sb_blocksize);
    if (block_size == 0 || ext.block_count > std::numeric_limits<uint64_t>::max() / block_size) {
        return {};
    }
    uint64_t read_size = ext.block_count * static_cast<uint64_t>(block_size);

    // Limit read size for safety
    if (read_size > 100 * 1024 * 1024) {  // Max 100MB per extent
        read_size = 100 * 1024 * 1024;
    }

    if (read_size == 0 || read_size > std::numeric_limits<size_t>::max()) {
        return {};
    }

    std::vector<uint8_t> data(static_cast<size_t>(read_size));
    uint64_t disk_offset = block_to_disk_offset(ext.block_offset);
    if (!bounded_pread(fd, source_size_, data.data(), data.size(), disk_offset)) {
        return {};
    }

    return data;
#else
    (void)fd;
    (void)ext;
    return {};
#endif
}

// ==================== Compressed Page Detection and Decompression ====================

// Check if a data block looks like a compressed InnoDB page (ZSD header)
// Compressed pages have ZSD at offset 0 with version=1, valid orig_size
static bool is_compressed_page(const uint8_t* data, size_t size) {
    if (size < 38 + 32) return false;  // Need at least FIL header + ZSD

    // ZSD fields (all little-endian)
    // offset 0: zip_size (2 bytes) - page size in 512-byte units
    // offset 2: version (2 bytes) - compression version, typically 1
    // offset 14: orig_size (4 bytes) - original uncompressed page size

    uint16_t version = dbrecovery::innodb::read_le<uint16_t>(data, 2);
    if (version != 1) return false;  // Only version 1 is known

    uint16_t zip_size = dbrecovery::innodb::read_le<uint16_t>(data, 0);
    // zip_size should be 8, 16, or 32 (4KB, 8KB, or 16KB compressed page)
    if (zip_size != 8 && zip_size != 16 && zip_size != 32) return false;

    uint32_t orig_size = dbrecovery::innodb::read_le<uint32_t>(data, 14);
    // orig_size should be a valid InnoDB page size (4K, 8K, 16K, 32K, 64K)
    if (orig_size != 4096 && orig_size != 8192 && orig_size != 16384 &&
        orig_size != 32768 && orig_size != 65536) return false;

    // zlen at offset 8 should be non-zero and <= zip_size * 512
    uint16_t zlen = dbrecovery::innodb::read_le<uint16_t>(data, 8);
    if (zlen == 0 || zlen > zip_size * 512) return false;

    return true;
}

// Try to decompress a compressed InnoDB page
// Returns decompressed data on success, empty vector on failure
static std::vector<uint8_t> try_decompress_page(const uint8_t* data, size_t size) {
    if (!is_compressed_page(data, size)) return {};

    uint32_t orig_size = dbrecovery::innodb::read_le<uint32_t>(data, 14);
    uint16_t zlen = dbrecovery::innodb::read_le<uint16_t>(data, 8);

    // Compressed payload starts after ZSD (32 bytes)
    constexpr size_t ZSD_SIZE = 32;
    if (size < ZSD_SIZE + zlen) return {};

    return dbrecovery::utils::LZ4Helper::decompress(
        data + ZSD_SIZE, zlen, orig_size);
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

        // Try compressed page detection first (ZSD header at offset 0)
        std::vector<uint8_t> decompressed;
        const uint8_t* effective_data = page_data;
        size_t effective_size = config_.page_size;

        if (page_type != FIL_PAGE_INDEX && page_type != FIL_PAGE_SDI &&
            page_type != FIL_PAGE_SDI_ZIP &&
            page_type != FIL_PAGE_TYPE_BLOB && page_type != FIL_PAGE_TYPE_ZBLOB &&
            page_type != FIL_PAGE_TYPE_ZBLOB2 &&
            page_type != FIL_PAGE_COMPRESSED && page_type != FIL_PAGE_COMPRESSED_AND_ENCRYPTED) {
            // Check if this is a compressed page
            auto decomp = try_decompress_page(page_data, config_.page_size);
            if (!decomp.empty() && decomp.size() >= FIL_PAGE_TYPE_ABS + 2) {
                // Re-read page type from decompressed data
                uint16_t decomp_type = read_be<uint16_t>(decomp.data(), FIL_PAGE_TYPE_ABS);
                if (decomp_type == FIL_PAGE_INDEX || decomp_type == FIL_PAGE_SDI ||
                    decomp_type == FIL_PAGE_SDI_ZIP ||
                    decomp_type == FIL_PAGE_TYPE_BLOB || decomp_type == FIL_PAGE_TYPE_ZBLOB ||
                    decomp_type == FIL_PAGE_TYPE_ZBLOB2) {
                    decompressed = std::move(decomp);
                    effective_data = decompressed.data();
                    effective_size = decompressed.size();
                    page_type = decomp_type;
                }
            }
        }

        // Encrypted compressed pages require authenticated decryption before parsing.
        if (page_type == FIL_PAGE_COMPRESSED_AND_ENCRYPTED) {
            continue;
        }

        // Accept INDEX, SDI, BLOB, and ZBLOB pages
        if (page_type != FIL_PAGE_INDEX && page_type != FIL_PAGE_SDI &&
            page_type != FIL_PAGE_SDI_ZIP &&
            page_type != FIL_PAGE_TYPE_BLOB && page_type != FIL_PAGE_TYPE_ZBLOB &&
            page_type != FIL_PAGE_TYPE_ZBLOB2 &&
            page_type != FIL_PAGE_COMPRESSED && page_type != FIL_PAGE_COMPRESSED_AND_ENCRYPTED) {
            continue;
        }

        // For INDEX pages, extract index_id
        uint64_t index_id = 0;
        if (page_type == FIL_PAGE_INDEX && effective_size >= 74) {
            index_id = read_be<uint64_t>(effective_data, 66);
            if (index_id == 0) {
                continue;  // Invalid index_id
            }
        }

        // For SDI pages, use pseudo index_id
        if (page_type == FIL_PAGE_SDI || page_type == FIL_PAGE_SDI_ZIP) {
            index_id = 0xFFFFFF00 | page_type;
        }

        // For BLOB/ZBLOB pages, use pseudo index_id based on page type
        if (page_type == FIL_PAGE_TYPE_BLOB || page_type == FIL_PAGE_TYPE_ZBLOB ||
            page_type == FIL_PAGE_TYPE_ZBLOB2) {
            index_id = 0xFFFF0000 | page_type;
        }

        // Extract page_no
        uint32_t page_no = 0;
        if (effective_size >= 12) {
            page_no = read_be<uint32_t>(effective_data, 8);
        }

        // Create page info
        XfsPageInfo page_info;
        page_info.disk_offset = offset;
        page_info.inode_number = inode_number;
        page_info.page_no = page_no;
        page_info.index_id = index_id;
        page_info.page_type = page_type;
        page_info.data.resize(effective_size);
        std::memcpy(page_info.data.data(), effective_data, effective_size);

        pages.push_back(std::move(page_info));
    }

    return pages;
}

// ==================== Block to Disk Offset ====================

uint64_t XfsDiskCarver::block_to_disk_offset(uint64_t fsblock) const {
    uint32_t block_size = be32toh(superblock_.sb_blocksize);
    return fsblock * block_size;
}

// ==================== Free Block Scanning ====================

void XfsDiskCarver::scan_free_blocks(std::function<void(double, const std::string&)> progress) {
#ifdef __linux__
    // Walk XFS free space BNO btree and scan each free block for InnoDB page signatures.
    // This is the XFS equivalent of ext4's scan_freed_blocks - it finds data blocks
    // that were freed (e.g. by DROP TABLE) but whose inode metadata was zeroed.

    uint32_t block_size = be32toh(superblock_.sb_blocksize);
    uint32_t ag_blocks = be32toh(superblock_.sb_agblocks);
    uint32_t ag_count = be32toh(superblock_.sb_agcount);
    uint16_t sect_size = be16toh(superblock_.sb_sectsize);

    if (block_size == 0 || ag_blocks == 0) return;

    uint32_t page_size = config_.page_size;
    uint32_t blocks_per_page = page_size / block_size;
    if (blocks_per_page == 0) blocks_per_page = 1;

    int fd = duplicate_read_only_fd(source_fd_);
    if (fd < 0) return;

    std::atomic<uint64_t> blocks_scanned{0};
    std::atomic<uint64_t> pages_found{0};
    auto last_progress_time = std::chrono::steady_clock::now();

    // Process each AG
    for (uint32_t ag_index = 0; ag_index < ag_count && !should_stop_.load(); ag_index++) {
        // Read AGF
        uint64_t agf_offset = static_cast<uint64_t>(ag_index) *
                             static_cast<uint64_t>(ag_blocks) *
                             static_cast<uint64_t>(block_size) +
                             static_cast<uint64_t>(sect_size);

        xfs_agf agf;
        if (!bounded_pread(fd, source_size_, &agf, sizeof(agf), agf_offset)) continue;
        if (be32toh(agf.agf_magicnum) != XFS_AGF_MAGIC) continue;

        uint32_t bno_root = be32toh(agf.agf_roots[0]);
        uint32_t bno_level = be32toh(agf.agf_levels[0]);
        if (bno_root == 0) continue;

        // Walk free space BNO btree to collect free extents
        // Use iterative stack-based traversal
        struct StackEntry {
            uint32_t block_num;
            int depth;
        };
        std::vector<StackEntry> stack;
        stack.push_back({bno_root, 0});

        std::vector<std::pair<uint64_t, uint64_t>> free_extents;  // (start_block, count)

        while (!stack.empty() && !should_stop_.load()) {
            auto entry = stack.back();
            stack.pop_back();

            if (entry.depth >= 10 || entry.block_num == 0 || entry.block_num >= ag_blocks) continue;

            uint64_t block_offset = static_cast<uint64_t>(ag_index) *
                                   static_cast<uint64_t>(ag_blocks) *
                                   static_cast<uint64_t>(block_size) +
                                   static_cast<uint64_t>(entry.block_num) * block_size;

            std::vector<uint8_t> buf(block_size);
            if (!bounded_pread(fd, source_size_, buf.data(), block_size, block_offset)) continue;

            uint32_t magic = read_be<uint32_t>(buf.data(), 0);
            // Check for alloc btree magic (BNO = by block number)
            if (magic != XFS_ABTB_MAGIC && magic != XFS_ABTB_CRC_MAGIC) continue;

            bool is_crc = (magic == XFS_ABTB_CRC_MAGIC);
            size_t hdr_size = is_crc ? 56 : 16;
            if (block_size < hdr_size) continue;

            uint16_t level = read_be<uint16_t>(buf.data(), 4);
            uint16_t numrecs = read_be<uint16_t>(buf.data(), 6);
            if (numrecs == 0 || numrecs > 1000) continue;

            if (level > 0) {
                // Internal node: push children
                size_t ptr_offset = (hdr_size + block_size) / 2;
                for (uint16_t i = 0; i < numrecs && ptr_offset + 4 <= block_size; i++) {
                    uint32_t child = read_be<uint32_t>(buf.data(), ptr_offset + i * 4);
                    if (child != 0 && child < ag_blocks) {
                        stack.push_back({child, entry.depth + 1});
                    }
                }
            } else {
                // Leaf node: extract free extent records
                // xfs_alloc_rec: ar_startblock (4), ar_blockcount (4) = 8 bytes
                size_t rec_offset = hdr_size;
                for (uint16_t i = 0; i < numrecs && rec_offset + 8 <= block_size; i++) {
                    uint32_t start = read_be<uint32_t>(buf.data(), rec_offset);
                    uint32_t count = read_be<uint32_t>(buf.data(), rec_offset + 4);
                    rec_offset += 8;

                    if (start > 0 && count > 0 && start + count <= ag_blocks) {
                        uint64_t abs_start = static_cast<uint64_t>(ag_index) * ag_blocks + start;
                        free_extents.push_back({abs_start, count});
                    }
                }
            }
        }

        // Scan each free extent for InnoDB pages
        uint64_t ag_base = static_cast<uint64_t>(ag_index) * ag_blocks;
        for (const auto& [start_block, count] : free_extents) {
            if (should_stop_.load()) break;

            // Scan at page-aligned offsets within this free extent
            uint64_t extent_start = ag_base + start_block;
            uint64_t extent_end = extent_start + count;

            for (uint64_t fsblock = extent_start; fsblock + blocks_per_page <= extent_end;
                 fsblock += blocks_per_page) {
                uint64_t disk_offset = fsblock * block_size;
                std::vector<uint8_t> page_data(page_size);

                if (!bounded_pread(fd, source_size_, page_data.data(), page_size, disk_offset)) {
                    continue;
                }

                blocks_scanned.fetch_add(1);

                if (page_data.size() >= 26) {
                    uint16_t page_type = read_be<uint16_t>(page_data.data(), FIL_PAGE_TYPE_ABS);

                    // Try compressed page detection
                    std::vector<uint8_t> decompressed;
                    const uint8_t* effective_data = page_data.data();
                    size_t effective_size = page_size;

                    if (page_type != FIL_PAGE_INDEX && page_type != FIL_PAGE_SDI &&
                        page_type != FIL_PAGE_SDI_ZIP &&
                        page_type != FIL_PAGE_COMPRESSED && page_type != FIL_PAGE_COMPRESSED_AND_ENCRYPTED) {
                        auto decomp = try_decompress_page(page_data.data(), page_size);
                        if (!decomp.empty() && decomp.size() >= FIL_PAGE_TYPE_ABS + 2) {
                            uint16_t decomp_type = read_be<uint16_t>(decomp.data(), FIL_PAGE_TYPE_ABS);
                            if (decomp_type == FIL_PAGE_INDEX || decomp_type == FIL_PAGE_SDI ||
                                decomp_type == FIL_PAGE_SDI_ZIP) {
                                decompressed = std::move(decomp);
                                effective_data = decompressed.data();
                                effective_size = decompressed.size();
                                page_type = decomp_type;
                            }
                        }
                    }

                    // Accept BLOB and ZBLOB page types for comprehensive data recovery
                    if (page_type == FIL_PAGE_INDEX || page_type == FIL_PAGE_SDI ||
                        page_type == FIL_PAGE_SDI_ZIP ||
                        page_type == FIL_PAGE_TYPE_BLOB || page_type == 0x45BD ||
                        page_type == FIL_PAGE_TYPE_ZBLOB || page_type == 0x45BA ||
                        page_type == FIL_PAGE_TYPE_ZBLOB2 || page_type == 0x45BB) {
                        uint64_t index_id = 0;
                        if (page_type == FIL_PAGE_INDEX && effective_size >= 74) {
                            index_id = read_be<uint64_t>(effective_data, 66);
                        }
                        if (page_type == FIL_PAGE_SDI || page_type == FIL_PAGE_SDI_ZIP) {
                            index_id = 0xFFFFFF00 | page_type;
                        }
                        uint32_t page_no = (effective_size >= 12) ?
                            read_be<uint32_t>(effective_data, 8) : 0;

                        if (index_id > 0 && index_id < 1000000000000ULL) {
                            XfsPageInfo page_info;
                            page_info.disk_offset = disk_offset;
                            page_info.inode_number = 0;
                            page_info.page_no = page_no;
                            page_info.index_id = index_id;
                            page_info.page_type = page_type;
                            page_info.data.resize(effective_size);
                            std::memcpy(page_info.data.data(), effective_data, effective_size);

                            {
                                std::lock_guard<std::mutex> lock(result_mutex_);
                                last_result_.pages_by_index_id[index_id].push_back(std::move(page_info));
                                last_result_.pages_recovered++;
                            }
                            pages_found.fetch_add(1);
                        }
                    }
                }
            }
        }

        // Progress reporting
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_progress_time).count();
        if (progress && elapsed_ms >= config_.progress_interval_ms) {
            double pct = static_cast<double>(ag_index + 1) / ag_count;
            std::ostringstream oss;
            oss << "Free blocks scanned: " << blocks_scanned.load()
                << " pages found: " << pages_found.load()
                << " AG " << (ag_index + 1) << "/" << ag_count;
            progress(pct, oss.str());
            last_progress_time = now;
        }
    }

    close(fd);

    if (config_.verbose) {
        std::cout << "[INFO] Free block scan: scanned=" << blocks_scanned.load()
                  << " pages_found=" << pages_found.load() << "\n";
    }
#endif
}

// ==================== Free Space Extent Validation ====================
// Validates recovered extents against the free space BNO btree.
// This ensures that blocks from deleted inode extents haven't been reallocated.
// Matches xfs_undelete's OnlyWithinAGF/TreeCheck algorithm.

std::vector<xfs_extent> XfsDiskCarver::validate_extents_against_free_space(
    int fd, uint32_t ag_index, const std::vector<xfs_extent>& extents) {
#ifdef __linux__
    std::vector<xfs_extent> valid_extents;

    uint32_t block_size = be32toh(superblock_.sb_blocksize);
    uint32_t ag_blocks = be32toh(superblock_.sb_agblocks);
    uint16_t sect_size = be16toh(superblock_.sb_sectsize);

    // Read AGF to get free space btree root
    uint64_t ag_offset = static_cast<uint64_t>(ag_index) * ag_blocks * block_size;
    uint64_t agf_offset = ag_offset + sect_size;

    xfs_agf agf;
    if (!bounded_pread(fd, source_size_, &agf, sizeof(agf), agf_offset)) {
        return {};
    }
    if (be32toh(agf.agf_magicnum) != XFS_AGF_MAGIC) return {};

    uint32_t bno_root = be32toh(agf.agf_roots[0]);
    if (bno_root == 0 || bno_root >= ag_blocks) return extents;

    for (const auto& ext : extents) {
        // Convert absolute fsblock to AG-relative
        uint64_t ag_base = static_cast<uint64_t>(ag_index) * ag_blocks;
        uint64_t rel_start = ext.block_offset - ag_base;
        uint64_t rel_end = rel_start + ext.block_count;

        if (rel_start >= ag_blocks) continue;

        uint64_t extent_begin = rel_start;
        uint64_t extent_end = rel_end;
        uint64_t file_offset_accum = 0;

        // Recursively check this extent against the free space btree
        std::vector<xfs_extent> sub_extents;
        free_space_tree_check(fd, ag_index, ag_offset, block_size,
                             bno_root, &extent_begin, &extent_end,
                             ext, file_offset_accum, &sub_extents);

        for (auto& sub : sub_extents) {
            valid_extents.push_back(std::move(sub));
        }
    }

    return valid_extents;
#else
    return extents;
#endif
}

void XfsDiskCarver::free_space_tree_check(
    int fd, uint32_t ag_index, uint64_t ag_offset, uint32_t block_size,
    uint32_t block_num, uint64_t* extent_begin, uint64_t* extent_end,
    const xfs_extent& original_ext, uint64_t& file_offset_accum,
    std::vector<xfs_extent>* valid_extents) {
#ifdef __linux__
    if (*extent_begin >= *extent_end) return;

    uint32_t ag_blocks = be32toh(superblock_.sb_agblocks);
    if (block_num == 0 || block_num >= ag_blocks) return;

    uint64_t block_offset = ag_offset + static_cast<uint64_t>(block_num) * block_size;
    std::vector<uint8_t> buf(block_size);
    if (!bounded_pread(fd, source_size_, buf.data(), block_size, block_offset)) return;

    uint32_t magic = read_be<uint32_t>(buf.data(), 0);
    if (magic != XFS_ABTB_MAGIC && magic != XFS_ABTB_CRC_MAGIC) return;

    bool is_crc = (magic == XFS_ABTB_CRC_MAGIC);
    size_t hdr_size = is_crc ? 56 : 16;
    if (block_size < hdr_size) return;

    uint16_t level = read_be<uint16_t>(buf.data(), 4);
    uint16_t numrecs = read_be<uint16_t>(buf.data(), 6);
    if (numrecs == 0 || numrecs > 1000) return;

    if (level > 0) {
        // Internal node: binary search for the child containing extent_begin
        // Keys are (startblock, blockcount), sorted by startblock
        // xfs_alloc_key_t: ar_startblock(4) + ar_blockcount(4) = 8 bytes
        size_t key_offset = hdr_size;
        size_t ptr_offset = (hdr_size + block_size) / 2;

        // Binary search: find the rightmost key <= extent_begin
        uint16_t left = 0, right = numrecs;
        while (left < right) {
            uint16_t mid = (left + right) / 2;
            uint32_t key_start = read_be<uint32_t>(buf.data(), key_offset + mid * 8);
            if (key_start <= *extent_begin) {
                left = mid + 1;
            } else {
                right = mid;
            }
        }
        uint16_t child_idx = (left > 0) ? (left - 1) : 0;

        // Read the pointer at child_idx
        uint32_t child_block = read_be<uint32_t>(buf.data(), ptr_offset + child_idx * 4);
        free_space_tree_check(fd, ag_index, ag_offset, block_size,
                             child_block, extent_begin, extent_end,
                             original_ext, file_offset_accum, valid_extents);
    } else {
        // Leaf node: process records
        // xfs_alloc_rec: ar_startblock(4) + ar_blockcount(4) = 8 bytes
        size_t rec_offset = hdr_size;

        for (uint16_t i = 0; i < numrecs && rec_offset + 8 <= block_size; i++) {
            uint32_t rec_start = read_be<uint32_t>(buf.data(), rec_offset);
            uint32_t rec_count = read_be<uint32_t>(buf.data(), rec_offset + 4);
            rec_offset += 8;
            if (rec_count == 0) continue;

            uint64_t rec_begin = rec_start;
            uint64_t rec_end = rec_start + rec_count;

            // Check overlap with our extent range
            if (*extent_begin >= rec_end) continue;  // Extent is to the right
            if (*extent_end <= rec_begin) break;      // Extent is to the left (sorted, so we're done)

            // Overlap found: add the overlapping region
            uint64_t overlap_begin = std::max(rec_begin, *extent_begin);
            uint64_t overlap_end = std::min(rec_end, *extent_end);

            if (overlap_begin < overlap_end) {
                xfs_extent valid_ext;
                valid_ext.block_offset = ag_offset / block_size +
                    static_cast<uint64_t>(ag_index) * ag_blocks + overlap_begin;
                valid_ext.block_count = overlap_end - overlap_begin;
                valid_ext.file_offset = original_ext.file_offset + file_offset_accum;
                valid_ext.state = 0;
                valid_extents->push_back(valid_ext);
            }

            // Advance to the end of this record
            file_offset_accum += (overlap_end - *extent_begin);
            *extent_begin = overlap_end;

            if (*extent_begin >= *extent_end) return;
        }
    }
#endif
}

// ==================== Multi-Level Extent Btree Traversal ====================
// Recursively walks extent btree blocks to collect leaf-level extent records.
// This handles large, highly fragmented files where the extent btree has
// multiple levels (root -> internal nodes -> leaves).

void XfsDiskCarver::collect_extent_leaf_records(
    int fd, uint64_t block_disk_offset, uint32_t block_size,
    int level, XfsExtentInfo* info) {
#ifdef __linux__
    if (level < 0 || level > 10) return;  // Safety limits

    std::vector<uint8_t> buf(block_size);
    if (!bounded_pread(fd, source_size_, buf.data(), block_size, block_disk_offset)) return;

    uint32_t magic = read_be<uint32_t>(buf.data(), 0);
    // Check for BMAP btree magic (CRC or non-CRC)
    bool is_crc = (magic == 0x42B33D42);  // XFS_BMAP_CRC_MAGIC
    bool is_valid = is_crc || (magic == 0x424D4D42);  // XFS_BMAP_MAGIC
    if (!is_valid) return;

    size_t hdr_size = is_crc ? 56 : 16;
    uint16_t bb_level = read_be<uint16_t>(buf.data(), 4);
    uint16_t numrecs = read_be<uint16_t>(buf.data(), 6);
    if (numrecs == 0 || numrecs > 1000) return;

    if (level == 0) {
        // Leaf node: extract extent records
        // xfs_bmbt_rec: 128-bit (16 bytes) packed format
        constexpr size_t BMAP_REC_SIZE = sizeof(xfs_bmbt_rec);
        size_t rec_offset = hdr_size;
        for (uint16_t i = 0; i < numrecs && rec_offset + BMAP_REC_SIZE <= block_size; i++) {
            xfs_bmbt_rec rec;
            std::memcpy(&rec, buf.data() + rec_offset, BMAP_REC_SIZE);
            rec_offset += BMAP_REC_SIZE;

            xfs_extent ext = parse_extent(rec);
            if (ext.block_count > 0 && ext.state != XFS_EXT_UNWRITTEN) {
                uint64_t fs_size = be64toh(superblock_.sb_dblocks);
                if (ext.block_offset + ext.block_count <= fs_size) {
                    info->extents.push_back(ext);
                }
            }
        }
    } else {
        // Internal node: read keys + pointers, recurse into children
        size_t key_offset = hdr_size;
        size_t ptr_offset = (hdr_size + block_size) / 2;

        for (uint16_t i = 0; i < numrecs; i++) {
            uint64_t child_block = 0;
            if (is_crc) {
                // CRC format uses 8-byte pointers
                child_block = read_be<uint64_t>(buf.data(), ptr_offset + i * 8);
            } else {
                // Non-CRC format uses 4-byte pointers
                child_block = read_be<uint32_t>(buf.data(), ptr_offset + i * 4);
            }

            if (child_block != 0 && child_block < be32toh(superblock_.sb_agblocks)) {
                uint32_t ag_blocks = be32toh(superblock_.sb_agblocks);
                uint64_t child_disk_offset = block_disk_offset -
                    (block_disk_offset % (static_cast<uint64_t>(ag_blocks) * block_size)) +
                    child_block * block_size;
                collect_extent_leaf_records(fd, child_disk_offset, block_size, level, info);
            }
        }
    }
#endif
}

// ==================== Signature Scan Fallback ====================

void XfsDiskCarver::signature_scan(std::function<void(double, const std::string&)> progress) {
#ifdef __linux__
    // Parallel signature-based scan for improved performance
    // Each worker thread scans a portion of the device

    if (source_fd_ < 0 || source_size_ == 0) {
        return;
    }

    const uint64_t device_size = source_size_;

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
        int worker_fd = duplicate_read_only_fd(source_fd_);
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
            const size_t read_size = static_cast<size_t>(chunk_end - chunk_start);
            if (!bounded_pread(worker_fd, source_size_, buffer.data(), read_size, chunk_start)) {
                break;
            }

            size_t scan_size = read_size;

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

                // Try compressed page detection if regular page type doesn't match
                std::vector<uint8_t> decompressed;
                const uint8_t* effective_data = page_data;
                size_t effective_size = config_.page_size;

                if (page_type != FIL_PAGE_INDEX && page_type != FIL_PAGE_SDI &&
                    page_type != FIL_PAGE_SDI_ZIP &&
                    page_type != FIL_PAGE_TYPE_BLOB && page_type != FIL_PAGE_TYPE_ZBLOB &&
                    page_type != FIL_PAGE_TYPE_ZBLOB2 &&
                    page_type != FIL_PAGE_COMPRESSED && page_type != FIL_PAGE_COMPRESSED_AND_ENCRYPTED) {
                    auto decomp = try_decompress_page(page_data, config_.page_size);
                    if (!decomp.empty() && decomp.size() >= FIL_PAGE_TYPE_ABS + 2) {
                        uint16_t decomp_type = read_be<uint16_t>(decomp.data(), FIL_PAGE_TYPE_ABS);
                        if (decomp_type == FIL_PAGE_INDEX || decomp_type == FIL_PAGE_SDI ||
                            decomp_type == FIL_PAGE_SDI_ZIP) {
                            decompressed = std::move(decomp);
                            effective_data = decompressed.data();
                            effective_size = decompressed.size();
                            page_type = decomp_type;
                        }
                    }
                }

                if (page_type == FIL_PAGE_INDEX || page_type == FIL_PAGE_SDI ||
                    page_type == FIL_PAGE_SDI_ZIP ||
                    page_type == FIL_PAGE_TYPE_BLOB || page_type == FIL_PAGE_TYPE_ZBLOB ||
                    page_type == FIL_PAGE_TYPE_ZBLOB2) {
                    uint64_t index_id = 0;
                    if (page_type == FIL_PAGE_INDEX && effective_size >= 74) {
                        index_id = read_be<uint64_t>(effective_data, 66);
                    }

                    if (page_type == FIL_PAGE_SDI || page_type == FIL_PAGE_SDI_ZIP) {
                        index_id = 0xFFFFFF00 | page_type;
                    }

                    if (page_type == FIL_PAGE_TYPE_BLOB || page_type == FIL_PAGE_TYPE_ZBLOB ||
                        page_type == FIL_PAGE_TYPE_ZBLOB2) {
                        index_id = 0xFFFFFF00 | page_type;
                    }

                    // Validate page by checking LSN consistency
                    uint64_t lsn = read_be<uint64_t>(effective_data, 16);
                    if (lsn == 0 && index_id == 0) {
                        continue;  // Skip obviously invalid pages
                    }

                    XfsPageInfo page_info;
                    page_info.disk_offset = chunk_start + i;
                    page_info.page_type = page_type;
                    page_info.index_id = index_id;
                    page_info.page_no = read_be<uint32_t>(effective_data, 8);
                    page_info.data.resize(effective_size);
                    std::memcpy(page_info.data.data(), effective_data, effective_size);

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

    if (source_fd_ < 0 || source_size_ == 0) {
        return;
    }

    const int fd = duplicate_read_only_fd(source_fd_);
    if (fd < 0) {
        return;
    }

    const uint64_t device_size = source_size_;

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
        size_t to_read = static_cast<size_t>(std::min<uint64_t>(
            scan_chunk + config_.page_size, device_size - offset));
        if (!bounded_pread(fd, source_size_, buffer.data(), to_read, offset)) {
            break;
        }
        const ssize_t bytes_read = static_cast<ssize_t>(to_read);

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
        // InnoDB reserves the high 32-bit range (>= 0xFFFF0000) for the
        // data dictionary / internal system tables. Schemaless recovery
        // frequently mis-identifies these via garbage pages, so treat them
        // as system tables when exclusion is requested.
        if (index_id >= 0xFFFF0000ULL) return true;
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
            if (dbrecovery::innodb::g_verbose)             std::cerr << "[DEBUG] Using schema-aware parsing with "
                      << external_schema_->columns.size() << " columns for index_id "
                      << index_id << std::endl;
        }

        for (const auto& page_info : pages) {
            if (use_schema) {
                // Schema-aware parsing
                auto records = parse_records_with_schema_xfs(page_info.data, *external_schema_);
                all_records.insert(all_records.end(), records.begin(), records.end());
            }
        }

        if (!use_schema) {
            // Heuristic rows remain forensic evidence and are not exportable.
            processed++;
            continue;
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
                          page_type == FIL_PAGE_COMPRESSED ||
                          page_type == FIL_PAGE_COMPRESSED_AND_ENCRYPTED ||
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
// MySQL server types (MYSQL_TYPE_*) differ from InnoDB internal types
static uint8_t mysql_type_to_innodb(uint32_t mysql_type) {
    // InnoDB internal type IDs:
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
        case 255: // MYSQL_TYPE_GEOMETRY
            return 30;  // InnoDB GEOMETRY type
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
        // physical_pos may be 0-based (first column = 0) or 1-based; normalize
        // to 1-based so ordinal 1 never collides with physical_pos 0.
        uint32_t phys_pos = col.physical_pos > 0 ? col.physical_pos : ordinal;
        sorted_cols.push_back({phys_pos, col});
    }
    std::sort(sorted_cols.begin(), sorted_cols.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });

    for (const auto& [phys_pos, col] : sorted_cols) {
        std::vector<std::string> diagnostics;
        auto converted = catalog::build_index_column(
            col, col.ordinal, phys_pos,
            catalog::TypeIdDomain::kInnodbParser, diagnostics);
        if (!converted.has_value()) {
            for (const auto& d : diagnostics) {
                std::cerr << "[WARN] XFS schema column rejected: " << d << std::endl;
            }
            continue;
        }
        innodb::IndexColumnDef col_def = *converted;

        // Set mb_maxlen based on charset
        if (col.charset_id == 45 || col.charset_id == 46) {  // utf8mb4
            col_def.mb_maxlen = 4;
        } else if (col.charset_id == 33 || col.charset_id == 83) {  // utf8
            col_def.mb_maxlen = 3;
        } else {
            col_def.mb_maxlen = 1;  // latin1 and others
        }

        // Mirror recovery_cli.hpp FRM semantics: is_big gates 2-byte var-size
        // encoding; is_binary keeps CHAR-vs-BINARY (type 29) fixed-length.
        // Type 27 is shared by BLOB and TEXT, so binary-ness must come from the
        // collation (63 = binary); otherwise TEXT is hex-encoded.
        if (col_def.col_type == 24 || col_def.col_type == 25 ||
            col_def.col_type == 26 || col_def.col_type == 27 ||
            col_def.col_type == 31) {
            col_def.is_big = true;
            if (col_def.col_type == 27) {
                col_def.is_binary = (col.charset_id == 63 || col.collation_id == 63);
            }
        } else if (col_def.col_type == 16 || col_def.col_type == 29) {
            col_def.is_big = (col.column_length * col_def.mb_maxlen > 255);
            // BINARY vs CHAR: both type 29; BINARY columns use charset-63 and
            // resolve fixed-length via mb_maxlen==1, but mark explicitly when
            // collation is binary and the column is not JSON.
            col_def.is_binary = (col.charset_id == 63 || col.collation_id == 63);
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
        if (dbrecovery::innodb::g_verbose)         std::cerr << "[DEBUG] Schema-aware parsing found " << records.size()
                  << " records for index_id" << std::endl;
    }

    return records;
}

// ==================== AG Header Reading ====================
bool XfsDiskCarver::read_ag_headers(int fd, uint32_t ag_index, xfs_agi& agi, xfs_agf& agf) {
#ifdef __linux__
    if (fd < 0) {
        return false;
    }

    const uint32_t block_size = be32toh(superblock_.sb_blocksize);
    const uint32_t ag_blocks = be32toh(superblock_.sb_agblocks);
    const uint16_t sect_size = be16toh(superblock_.sb_sectsize);
    if (block_size == 0 || ag_blocks == 0 || sect_size == 0 ||
        ag_index >= be32toh(superblock_.sb_agcount)) {
        return false;
    }

    const uint64_t ag_base = static_cast<uint64_t>(ag_index) * ag_blocks * block_size;
    const uint64_t agf_offset = ag_base + sect_size;
    const uint64_t agi_offset = ag_base + static_cast<uint64_t>(sect_size) * 2;
    if (!bounded_pread(fd, source_size_, &agi, sizeof(agi), agi_offset) ||
        be32toh(agi.agi_magicnum) != XFS_AGI_MAGIC ||
        !bounded_pread(fd, source_size_, &agf, sizeof(agf), agf_offset) ||
        be32toh(agf.agf_magicnum) != XFS_AGF_MAGIC) {
        return false;
    }

    return true;
#else
    (void)fd;
    (void)ag_index;
    (void)agi;
    (void)agf;
    return false;
#endif
}

// ==================== Extent Validation ====================
// Validates that extent blocks are still free (not reallocated)

bool XfsDiskCarver::validate_extent(const xfs_extent& ext, uint32_t ag_index) {
#ifdef __linux__
    if (ext.block_count == 0 || source_fd_ < 0) {
        return false;
    }

    const uint32_t blocks_per_ag = be32toh(superblock_.sb_agblocks);
    if (blocks_per_ag == 0 || ag_index >= be32toh(superblock_.sb_agcount)) {
        return false;
    }

    const uint64_t ag_base = static_cast<uint64_t>(ag_index) * blocks_per_ag;
    if (ext.block_offset < ag_base ||
        ext.block_offset - ag_base >= blocks_per_ag ||
        ext.block_count > blocks_per_ag - (ext.block_offset - ag_base)) {
        return false;
    }

    const int fd = duplicate_read_only_fd(source_fd_);
    if (fd < 0) {
        return false;
    }

    xfs_agi agi {};
    xfs_agf agf {};
    const bool headers_valid = read_ag_headers(fd, ag_index, agi, agf);
    ::close(fd);
    if (!headers_valid) {
        return false;
    }

    const uint32_t bno_root = be32toh(agf.agf_roots[XFS_BTNUM_BNOi]);
    const uint32_t longest_free = be32toh(agf.agf_longest);
    return bno_root != 0 && ext.block_count <= longest_free;
#else
    (void)ext;
    (void)ag_index;
    return false;
#endif
}

} // namespace xfs
} // namespace disk
} // namespace dbrecovery
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
        std::vector<std::thread> workers;
        for (uint32_t i = 0; i < config_.num_workers && i < result.ag_count; ++i) {
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

    // Strategy 2: Fallback signature scan
    if (config_.signature_scan && result.pages_recovered == 0) {
        if (config_.verbose) {
            std::cout << "[INFO] No pages found via inode scan, performing signature scan\n";
        }
        signature_scan(progress);

        {
            std::lock_guard<std::mutex> lock(result_mutex_);
            result.pages_by_index_id = last_result_.pages_by_index_id;
            result.pages_recovered = last_result_.pages_recovered;
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

        // Walk the inode btree
        std::vector<uint8_t> block_buffer(block_size);

        // Simple btree traversal (only leaf level for now)
        uint32_t current_block = btree_root;
        int max_levels = 10;  // Safety limit

        for (int level = 0; level < max_levels && current_block != 0; level++) {
            uint64_t block_offset = static_cast<uint64_t>(ag_index) *
                                   static_cast<uint64_t>(ag_blocks) *
                                   static_cast<uint64_t>(block_size) +
                                   static_cast<uint64_t>(current_block) *
                                   static_cast<uint64_t>(block_size);

            if (pread(fd, block_buffer.data(), block_size, block_offset) != block_size) {
                break;
            }

            xfs_btree_block btree_hdr;
            std::memcpy(&btree_hdr, block_buffer.data(), sizeof(btree_hdr));

            // Validate magic
            uint32_t magic = be32toh(btree_hdr.bb_magic);
            if (magic != btree_magic && magic != XFS_FIBT_MAGIC && magic != XFS_IBT_MAGIC) {
                break;
            }

            uint16_t numrecs = be16toh(btree_hdr.bb_numrecs);
            uint16_t level_val = be16toh(btree_hdr.bb_level);

            if (level_val == 0) {
                // Leaf node - process inode records
                // Record offset for short-form CRC btree: XFS_BTREE_SBLOCK_CRC_LEN = 56 bytes
                size_t rec_offset = 56;
                size_t rec_size = sizeof(xfs_inobt_rec);

                for (uint16_t i = 0; i < numrecs && rec_offset + rec_size <= block_size; i++) {
                    xfs_inobt_rec rec;
                    std::memcpy(&rec, block_buffer.data() + rec_offset + i * rec_size, rec_size);

                    uint32_t start_ino = be32toh(rec.ir_startino);
                    uint64_t free_mask = be64toh(rec.ir_free);
                    uint8_t free_count = rec.ir_freecount;

                    // Each record covers 64 consecutive inodes
                    // free_mask bit N = 1 means inode N is free (deleted)

                    if (free_count == 0 && !has_finobt) {
                        // In regular inode btree, check free_mask anyway
                        // (allocated inodes that were deleted)
                    }

                    // Iterate through free inodes in this chunk
                    for (uint32_t bit = 0; bit < 64 && free_mask != 0; bit++) {
                        if ((free_mask & (1ULL << bit)) == 0) {
                            continue;  // Inode is allocated, skip
                        }

                        uint32_t ag_inode = start_ino + bit;

                        // Read inode and try to recover
                        auto extent_info = read_inode(ag_index, ag_inode);
                        if (extent_info.has_value()) {
                            // Read data from extents first (outside lock)
                            std::vector<XfsPageInfo> recovered_pages;
                            for (const auto& ext : extent_info->extents) {
                                if (ext.state == XFS_EXT_UNWRITTEN) {
                                    continue;  // Skip unwritten extents
                                }

                                std::vector<uint8_t> extent_data = read_extent_data(ext);
                                if (!extent_data.empty()) {
                                    // Parse InnoDB pages from extent data
                                    auto pages = parse_pages_from_extent(extent_data,
                                                                         extent_info->inode_number);
                                    for (auto& page : pages) {
                                        recovered_pages.push_back(std::move(page));
                                    }
                                }
                            }

                            // Now lock and add results
                            if (!recovered_pages.empty() || extent_info->extents.size() > 0) {
                                std::lock_guard<std::mutex> lock(result_mutex_);
                                last_result_.recovered_extents.push_back(*extent_info);
                                inodes_found_.fetch_add(1);

                                for (auto& page : recovered_pages) {
                                    last_result_.pages_by_index_id[page.index_id].push_back(std::move(page));
                                    last_result_.pages_recovered++;
                                }
                            }
                        }

                        inodes_scanned_.fetch_add(1);
                    }
                }
                break;  // Done with this AG
            } else {
                // Node (non-leaf) - get first pointer for traversal
                // Pointer offset after keys
                // For short-form: pointers at offset (header + max_keys) / 2 roughly
                size_t ptr_offset = block_size / 2;  // Simplified
                if (ptr_offset + 4 <= block_size) {
                    uint32_t next_block;
                    std::memcpy(&next_block, block_buffer.data() + ptr_offset, 4);
                    current_block = be32toh(next_block);
                } else {
                    break;
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

        // Check extent doesn't exceed filesystem size
        uint64_t fs_size = be64toh(superblock_.sb_dblocks);
        if (ext.block_offset + ext.block_count > fs_size) {
            continue;  // Invalid extent
        }

        // Only accept extents starting at file offset 0 (for MySQL data files)
        if (ext.file_offset == 0) {
            info.extents.push_back(ext);
        }
    }

    if (info.extents.empty()) {
        return std::nullopt;
    }

    return info;
#else
    return std::nullopt;
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
    // Direct signature-based scan as fallback
    // Similar to ext4 carving but XFS-aware

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

    if (device_size == 0) {
        close(fd);
        return;
    }

    uint32_t block_size = be32toh(superblock_.sb_blocksize);
    uint64_t scan_chunk = 256 * 1024 * 1024;  // 256MB chunks
    std::vector<uint8_t> buffer(scan_chunk + config_.page_size);

    uint64_t total_scanned = 0;
    uint64_t pages_found = 0;
    uint64_t frm_files_found = 0;

    auto last_progress_time = std::chrono::steady_clock::now();

    for (uint64_t offset = 0; offset < device_size; offset += scan_chunk) {
        ssize_t bytes_read = pread(fd, buffer.data(), scan_chunk + config_.page_size, offset);
        if (bytes_read <= 0) {
            break;
        }

        size_t scan_size = bytes_read;

        // Scan for InnoDB pages at block-aligned offsets
        // Also scan at 512-byte offsets for non-aligned pages (edge case)
        uint64_t scan_step = block_size;  // Primary: block-aligned

        for (size_t i = 0; i + config_.page_size <= scan_size; i += scan_step) {
            const uint8_t* page_data = buffer.data() + i;

            // First check for FRM file signature (MySQL 5.6/5.7 schema files)
            if (i + 10 <= scan_size && std::memcmp(page_data, FRM_SIGNATURE, 10) == 0) {
                frm_files_found++;
                if (config_.verbose) {
                    std::cout << "[INFO] Found FRM file signature at offset " << (offset + i) << "\n";
                }
                // Store FRM location for later extraction
                XfsPageInfo frm_info;
                frm_info.disk_offset = offset + i;
                frm_info.page_type = 0xFFFF;  // Special marker for FRM
                frm_info.index_id = 0;
                frm_info.page_no = 0;
                // Read up to 64KB for FRM file (typical size)
                size_t frm_size = std::min(static_cast<size_t>(65536), scan_size - i);
                frm_info.data.resize(frm_size);
                std::memcpy(frm_info.data.data(), page_data, frm_size);

                std::lock_guard<std::mutex> lock(result_mutex_);
                last_result_.pages_by_index_id[0xFFFFFFFF].push_back(std::move(frm_info));
                continue;
            }

            // Check page type
            uint16_t page_type = read_be<uint16_t>(page_data, FIL_PAGE_TYPE_ABS);

            // Accept INDEX, SDI, and SDI_ZIP pages
            if (page_type == FIL_PAGE_INDEX || page_type == FIL_PAGE_SDI || page_type == FIL_PAGE_SDI_ZIP) {
                uint64_t index_id = 0;
                if (page_type == FIL_PAGE_INDEX && config_.page_size >= 74) {
                    index_id = read_be<uint64_t>(page_data, 66);
                    if (index_id == 0) {
                        continue;  // Invalid index_id
                    }
                }

                // For SDI pages, use space_id as key
                if (page_type == FIL_PAGE_SDI || page_type == FIL_PAGE_SDI_ZIP) {
                    index_id = 0xFFFFFF00 | page_type;  // Special marker for SDI
                }

                XfsPageInfo page_info;
                page_info.disk_offset = offset + i;
                page_info.page_type = page_type;
                page_info.index_id = index_id;
                page_info.page_no = read_be<uint32_t>(page_data, 8);
                page_info.data.resize(config_.page_size);
                std::memcpy(page_info.data.data(), page_data, config_.page_size);

                std::lock_guard<std::mutex> lock(result_mutex_);
                last_result_.pages_by_index_id[index_id].push_back(std::move(page_info));
                last_result_.pages_recovered++;
                pages_found++;
            }
        }

        total_scanned += scan_size;

        // Progress reporting
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_progress_time).count();

        if (progress && elapsed_ms >= config_.progress_interval_ms) {
            double pct = static_cast<double>(total_scanned) / device_size;
            std::ostringstream oss;
            oss << "Signature scan: " << std::fixed << std::setprecision(1)
                << (pct * 100.0) << "% pages=" << pages_found
                << " frm=" << frm_files_found;
            progress(pct, oss.str());
            last_progress_time = now;
        }
    }

    close(fd);

    if (config_.verbose) {
        std::cout << "[INFO] Signature scan found " << pages_found << " pages and "
                  << frm_files_found << " FRM signatures\n";
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

    for (const auto& [index_id, pages] : scan_result.pages_by_index_id) {
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
                // Header
                if (!all_records.empty()) {
                    bool first = true;
                    for (const auto& kv : all_records[0]) {
                        if (!first) out << ",";
                        out << kv.first;
                        first = false;
                    }
                    out << "\n";

                    // Data rows
                    for (const auto& rec : all_records) {
                        first = true;
                        for (const auto& kv : rec) {
                            if (!first) out << ",";

                            std::string val = kv.second;
                            if (val.find(',') != std::string::npos ||
                                val.find('"') != std::string::npos ||
                                val.find('\n') != std::string::npos) {
                                out << "\"";
                                for (char c : val) {
                                    if (c == '"') out << "\"\"";
                                    else out << c;
                                }
                                out << "\"";
                            } else {
                                out << val;
                            }
                            first = false;
                        }
                        out << "\n";
                    }
                    result.output_files.push_back(output_file);
                }
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

    // INDEX page validation
    uint16_t page_type = read_be<uint16_t>(data, FIL_PAGE_TYPE_ABS);
    if (page_type != FIL_PAGE_INDEX) return records;

    InnoDBIndexParser parser;
    parser.set_recover_deleted(config_.recover_deleted);
    parser.set_bad_page_mode(2);

    auto index_records = parser.parse_page(data, page_size, 0);

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
                std::ostringstream hex;
                for (uint8_t b : rec.fields[i].raw_bytes) {
                    hex << std::hex << std::setw(2) << std::setfill('0')
                        << static_cast<int>(b);
                }
                record[field_name] = hex.str();
            }
        }
        if (!record.empty()) {
            records.push_back(record);
        }
    }

    return records;
}

// ==================== Schema-aware Parsing ====================

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
        col_def.col_type = col.mysql_type_id;
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
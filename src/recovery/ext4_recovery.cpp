/**
 * @file ext4_recovery.cpp
 * @brief ext4 filesystem-specific recovery implementation for MySQL/InnoDB data
 *
 * Implements ext4-specific recovery techniques based on testdisk/ext2p.c
 */

#include "dbrecovery/recovery/ext4_recovery.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/index_page_parser.hpp"

#include <chrono>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>
#include <fstream>

#ifdef __linux__
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/fs.h>
#include <endian.h>
#endif

namespace dbrecovery {
namespace recovery {
namespace ext4 {

using dbrecovery::innodb::read_be;
using dbrecovery::innodb::InnoDBIndexParser;

// InnoDB page constants
constexpr uint16_t FIL_PAGE_INDEX = 0x45BF;
constexpr uint16_t FIL_PAGE_SDI = 0x45BE;
constexpr uint16_t FIL_PAGE_SDI_ZIP = 0x45BD;
constexpr size_t FIL_PAGE_TYPE_ABS = 24;

// ext4 extent magic
constexpr uint16_t EXT4_EXTENT_MAGIC = 0xF30A;

// ==================== Constructor/Destructor ====================

Ext4Recovery::Ext4Recovery() = default;
Ext4Recovery::~Ext4Recovery() = default;

void Ext4Recovery::set_config(const Ext4Config& config) {
    config_ = config;
}

void Ext4Recovery::set_external_schema(const catalog::TableSchema& schema) {
    external_schema_ = schema;
}

// ==================== Static Helper Functions ====================

bool Ext4Recovery::is_ext4_device(const std::string& device_path) {
    ext4_superblock sb;
    return read_superblock(device_path, sb);
}

bool Ext4Recovery::read_superblock(const std::string& device_path, ext4_superblock& sb) {
#ifdef __linux__
    int fd = open(device_path.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }

    // ext4 superblock is at offset 1024 (0x400)
    ssize_t bytes_read = pread(fd, &sb, sizeof(ext4_superblock), EXT4_SUPERBLOCK_OFFSET);
    close(fd);

    if (bytes_read != sizeof(ext4_superblock)) {
        return false;
    }

    // Check magic number (little-endian)
    uint16_t magic = le16toh(sb.s_magic);
    return magic == EXT2_SUPER_MAGIC;
#else
    return false;
#endif
}

// ==================== Group Descriptor Reading ====================

bool Ext4Recovery::read_group_descriptor(uint32_t group_index, ext4_group_desc& desc) const {
#ifdef __linux__
    int fd = open(config_.device_path.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }

    // Use cached block_size and desc_size (thread-safe)
    uint32_t block_size = block_size_;
    uint32_t desc_size = desc_size_;

    // Group descriptor table starts at block 1 (or 2 for 1KB blocks)
    uint32_t gdb_block = (block_size == 1024) ? 2 : 1;

    // Calculate descriptor offset
    uint64_t desc_offset = static_cast<uint64_t>(gdb_block) * block_size +
                         static_cast<uint64_t>(group_index) * desc_size;

    ssize_t bytes_read = pread(fd, &desc, sizeof(ext4_group_desc), desc_offset);
    close(fd);

    return bytes_read >= static_cast<ssize_t>(desc_size);
#else
    return false;
#endif
}

// ==================== Block Bitmap Reading ====================

std::vector<uint8_t> Ext4Recovery::read_block_bitmap(uint32_t group_index) {
#ifdef __linux__
    ext4_group_desc desc;
    if (!read_group_descriptor(group_index, desc)) {
        return {};
    }

    int fd = open(config_.device_path.c_str(), O_RDONLY);
    if (fd < 0) {
        return {};
    }

    // Use cached values (thread-safe)
    uint32_t block_size = block_size_;
    uint32_t blocks_per_group = blocks_per_group_;

    // Calculate block bitmap block number (combine lo and hi for 64-bit)
    uint64_t bitmap_block = static_cast<uint64_t>(le32toh(desc.bg_block_bitmap_lo)) |
                          (static_cast<uint64_t>(le32toh(desc.bg_block_bitmap_hi)) << 32);

    if (bitmap_block == 0) {
        close(fd);
        return {};
    }

    // Block bitmap size = blocks_per_group / 8 bytes
    size_t bitmap_size = (blocks_per_group + 7) / 8;
    std::vector<uint8_t> bitmap(bitmap_size);

    uint64_t disk_offset = bitmap_block * block_size;
    ssize_t bytes_read = pread(fd, bitmap.data(), bitmap_size, disk_offset);
    close(fd);

    if (bytes_read != static_cast<ssize_t>(bitmap_size)) {
        return {};
    }

    return bitmap;
#else
    return {};
#endif
}

// ==================== Inode Bitmap Reading ====================

std::vector<uint8_t> Ext4Recovery::read_inode_bitmap(uint32_t group_index) {
#ifdef __linux__
    ext4_group_desc desc;
    if (!read_group_descriptor(group_index, desc)) {
        return {};
    }

    int fd = open(config_.device_path.c_str(), O_RDONLY);
    if (fd < 0) {
        return {};
    }

    // Use cached values (thread-safe)
    uint32_t block_size = block_size_;
    uint32_t inodes_per_group = inodes_per_group_;

    uint64_t bitmap_block = static_cast<uint64_t>(le32toh(desc.bg_inode_bitmap_lo)) |
                          (static_cast<uint64_t>(le32toh(desc.bg_inode_bitmap_hi)) << 32);

    if (bitmap_block == 0) {
        close(fd);
        return {};
    }

    size_t bitmap_size = (inodes_per_group + 7) / 8;
    std::vector<uint8_t> bitmap(bitmap_size);

    uint64_t disk_offset = bitmap_block * block_size;
    ssize_t bytes_read = pread(fd, bitmap.data(), bitmap_size, disk_offset);
    close(fd);

    if (bytes_read != static_cast<ssize_t>(bitmap_size)) {
        return {};
    }

    return bitmap;
#else
    return {};
#endif
}

// ==================== Inode Reading ====================

uint64_t Ext4Recovery::inode_to_disk_offset(uint32_t group_index, uint32_t local_inode) const {
#ifdef __linux__
    // Safety check: local_inode must be at least 1
    if (local_inode == 0) {
        return 0;
    }

    // Use cached values (thread-safe)
    uint32_t block_size = block_size_;
    uint16_t inode_size = inode_size_;
    uint32_t blocks_per_group = blocks_per_group_;

    if (block_size == 0 || inode_size == 0 || blocks_per_group == 0) {
        return 0;
    }

    ext4_group_desc desc;
    if (!read_group_descriptor(group_index, desc)) {
        return 0;
    }

    uint64_t inode_table_block = static_cast<uint64_t>(le32toh(desc.bg_inode_table_lo)) |
                                (static_cast<uint64_t>(le32toh(desc.bg_inode_table_hi)) << 32);

    if (inode_table_block == 0) {
        return 0;
    }

    // Safety check: inode_table_block should be within the group
    // Each group has blocks_per_group blocks, starting at group_index * blocks_per_group
    uint64_t group_start_block = static_cast<uint64_t>(group_index) * blocks_per_group;
    uint64_t group_end_block = group_start_block + blocks_per_group;

    if (inode_table_block < group_start_block || inode_table_block >= group_end_block) {
        // Invalid inode table block - outside group bounds
        // This can happen with corrupted filesystems
        return 0;
    }

    // Calculate offset within inode table
    uint64_t table_offset = inode_table_block * block_size;
    uint64_t inode_offset = static_cast<uint64_t>(local_inode - 1) * inode_size;

    return table_offset + inode_offset;
#else
    return 0;
#endif
}

std::optional<Ext4InodeInfo> Ext4Recovery::read_inode(uint32_t group_index, uint32_t local_inode) {
#ifdef __linux__
    // Safety check: ensure cached values are valid
    if (inode_size_ == 0 || inodes_per_group_ == 0) {
        return std::nullopt;
    }

    int fd = open(config_.device_path.c_str(), O_RDONLY);
    if (fd < 0) {
        return std::nullopt;
    }

    // Use cached values (thread-safe)
    uint16_t inode_size = inode_size_;
    uint32_t inodes_per_group = inodes_per_group_;

    uint64_t inode_offset = inode_to_disk_offset(group_index, local_inode);

    if (inode_offset == 0) {
        close(fd);
        return std::nullopt;
    }

    ext4_inode inode_raw;
    memset(&inode_raw, 0, sizeof(inode_raw));
    ssize_t bytes_read = pread(fd, &inode_raw, inode_size, inode_offset);
    close(fd);

    if (bytes_read != inode_size) {
        return std::nullopt;
    }

    // Parse inode information
    Ext4InodeInfo info;
    info.inode_number = group_index * inodes_per_group + local_inode;
    info.block_group = group_index;

    // Check if inode is deleted (mode = 0 or dtime > 0)
    uint16_t mode = le16toh(inode_raw.i_mode);
    uint32_t dtime = le32toh(inode_raw.i_dtime);
    uint16_t links_count = le16toh(inode_raw.i_links_count);

    info.is_deleted = (mode == 0) || (dtime > 0) || (links_count == 0);
    info.dtime = dtime;

    // Get file size (combine lo and hi)
    uint64_t size_lo = le32toh(inode_raw.i_size_lo);
    uint64_t size_hi = static_cast<uint64_t>(le32toh(inode_raw.i_size_hi)) << 32;
    info.file_size = size_lo | size_hi;

    if (info.file_size == 0) {
        return std::nullopt;  // Empty inode
    }

    // Parse extents
    info.extents = parse_extents(inode_raw);

    if (info.extents.empty()) {
        return std::nullopt;  // No data extents
    }

    return info;
#else
    return std::nullopt;
#endif
}

// ==================== Extent Parsing ====================

std::vector<ext4_extent> Ext4Recovery::parse_extents(const ext4_inode& inode) {
    std::vector<ext4_extent> extents;

    // Check if inode uses extents (flag bit 4)
    uint32_t flags = le32toh(inode.i_flags);
    bool uses_extents = (flags & 0x80000) != 0;  // EXT4_EXTENTS_FL

    if (!uses_extents) {
        // Legacy block mapping - not implemented
        return extents;
    }

    // Read extent header from i_block
    const ext4_extent_header* header = reinterpret_cast<const ext4_extent_header*>(inode.i_block);

    if (le16toh(header->eh_magic) != EXT4_EXTENT_MAGIC) {
        return extents;
    }

    uint16_t entries = le16toh(header->eh_entries);
    uint16_t depth = le16toh(header->eh_depth);

    if (depth == 0 && entries > 0 && entries <= 4) {
        // Direct extents in inode
        const ext4_extent* ext_array = reinterpret_cast<const ext4_extent*>(
            inode.i_block + sizeof(ext4_extent_header));

        for (uint16_t i = 0; i < entries; i++) {
            ext4_extent ext;
            ext.ee_block = le32toh(ext_array[i].ee_block);
            ext.ee_len = le16toh(ext_array[i].ee_len);
            ext.ee_start_hi = le16toh(ext_array[i].ee_start_hi);
            ext.ee_start_lo = le32toh(ext_array[i].ee_start_lo);

            if (ext.ee_len > 0) {
                extents.push_back(ext);
            }
        }
    }

    // Handle multi-level extent tree (depth > 0)
    // For large files, extent tree can have multiple levels of index nodes
    if (depth > 0 && entries > 0 && entries <= 4) {
        // This is an index node, need to traverse down to leaf nodes
        // ext4_extent_idx structure: ei_block (4), ei_leaf_lo (4), ei_leaf_hi (2)
        // Same size as ext4_extent (12 bytes each)
        const uint8_t* idx_array = reinterpret_cast<const uint8_t*>(inode.i_block) + sizeof(ext4_extent_header);

        // For multi-level extent trees, we need to read external blocks
        // This requires device access, which is not available in this context
        // The caller should handle this by calling traverse_extent_tree()
        // For now, we return empty and let the caller handle multi-level trees
    }

    return extents;
}

// ==================== Multi-level Extent Tree Traversal ====================

std::vector<ext4_extent> Ext4Recovery::traverse_extent_tree(
    int fd, uint64_t root_block, uint32_t block_size, int max_depth) {

    std::vector<ext4_extent> extents;

    if (max_depth <= 0 || root_block == 0) {
        return extents;
    }

#ifdef __linux__
    std::vector<uint8_t> block_buffer(block_size);

    // Read the extent block
    uint64_t block_offset = root_block * block_size;
    if (pread(fd, block_buffer.data(), block_size, block_offset) != block_size) {
        return extents;
    }

    // Parse extent header
    const ext4_extent_header* header = reinterpret_cast<const ext4_extent_header*>(block_buffer.data());

    if (le16toh(header->eh_magic) != EXT4_EXTENT_MAGIC) {
        return extents;
    }

    uint16_t entries = le16toh(header->eh_entries);
    uint16_t depth = le16toh(header->eh_depth);

    if (entries == 0 || entries > 340) {  // Max entries in a 4K block
        return extents;
    }

    if (depth == 0) {
        // Leaf node: extract extents
        const ext4_extent* ext_array = reinterpret_cast<const ext4_extent*>(
            block_buffer.data() + sizeof(ext4_extent_header));

        for (uint16_t i = 0; i < entries; i++) {
            ext4_extent ext;
            ext.ee_block = le32toh(ext_array[i].ee_block);
            ext.ee_len = le16toh(ext_array[i].ee_len);
            ext.ee_start_hi = le16toh(ext_array[i].ee_start_hi);
            ext.ee_start_lo = le32toh(ext_array[i].ee_start_lo);

            if (ext.ee_len > 0) {
                extents.push_back(ext);
            }
        }
    } else {
        // Index node: traverse child nodes
        // ext4_extent_idx has same layout as ext4_extent but different semantics
        const uint8_t* idx_array = block_buffer.data() + sizeof(ext4_extent_header);

        for (uint16_t i = 0; i < entries; i++) {
            // ext4_extent_idx: ei_block (4), ei_leaf_lo (4), ei_leaf_hi (2)
            uint32_t ei_block = le32toh(*reinterpret_cast<const uint32_t*>(idx_array + i * 12));
            uint32_t ei_leaf_lo = le32toh(*reinterpret_cast<const uint32_t*>(idx_array + i * 12 + 4));
            uint16_t ei_leaf_hi = le16toh(*reinterpret_cast<const uint16_t*>(idx_array + i * 12 + 8));

            uint64_t child_block = (static_cast<uint64_t>(ei_leaf_hi) << 32) | ei_leaf_lo;

            if (child_block > 0) {
                auto child_extents = traverse_extent_tree(fd, child_block, block_size, max_depth - 1);
                extents.insert(extents.end(), child_extents.begin(), child_extents.end());
            }
        }
    }
#endif

    return extents;
}

// ==================== Block Offset Calculation ====================

uint64_t Ext4Recovery::block_to_disk_offset(uint64_t block) const {
    // Use cached block_size (thread-safe)
    return block * block_size_;
}

// ==================== Main Scan Function ====================

Ext4ScanResult Ext4Recovery::scan(std::function<void(double, const std::string&)> progress) {
    Ext4ScanResult result;
    auto start_time = std::chrono::high_resolution_clock::now();

    // Debug: Log entry
    std::cout << "[DEBUG] Ext4Recovery::scan() starting\n" << std::flush;

    if (!read_superblock(config_.device_path, superblock_)) {
        result.errors.push_back("Invalid ext4 superblock or cannot read device");
        return result;
    }

    std::cout << "[DEBUG] Superblock read successfully\n" << std::flush;

    // Parse superblock information
    result.block_size = 1024 << le32toh(superblock_.s_log_block_size);
    result.inode_size = le16toh(superblock_.s_inode_size);
    result.blocks_per_group = le32toh(superblock_.s_blocks_per_group);
    result.inodes_per_group = le32toh(superblock_.s_inodes_per_group);

    // Calculate number of block groups
    uint64_t total_blocks = static_cast<uint64_t>(le32toh(superblock_.s_blocks_count_lo)) |
                           (static_cast<uint64_t>(le32toh(superblock_.s_blocks_count_hi)) << 32);
    result.group_count = total_blocks / result.blocks_per_group;

    // Cache all superblock values to member variables for thread-safe access
    group_count_ = result.group_count;
    block_size_ = result.block_size;
    blocks_per_group_ = result.blocks_per_group;
    inodes_per_group_ = result.inodes_per_group;
    inode_size_ = result.inode_size;
    desc_size_ = le16toh(superblock_.s_desc_size);
    if (desc_size_ == 0) desc_size_ = 32;  // Legacy default

    if (config_.verbose) {
        std::cout << "[INFO] ext4 filesystem detected:\n";
        std::cout << "  Block size: " << result.block_size << " bytes\n";
        std::cout << "  Inode size: " << result.inode_size << " bytes\n";
        std::cout << "  Block groups: " << result.group_count << "\n";
        std::cout << "  Blocks per group: " << result.blocks_per_group << "\n";
        std::cout << "  Inodes per group: " << result.inodes_per_group << "\n";
        std::cout << "  Stored group_count_: " << group_count_ << "\n";
        std::cout << "  Stored block_size_: " << block_size_ << "\n";
    }

    // Validate group count
    if (group_count_ == 0 || group_count_ > 100000) {
        result.errors.push_back("Invalid group count: " + std::to_string(group_count_));
        return result;
    }

    // Initialize counters
    current_group_ = 0;
    blocks_scanned_ = 0;
    blocks_found_ = 0;
    should_stop_ = false;

    // Clear last_result_ and queue before starting threads
    {
        std::lock_guard<std::mutex> lock(result_mutex_);
        last_result_ = Ext4ScanResult();
        while (!result_queue_.empty()) {
            result_queue_.pop();
        }
    }

    // Create output directory
    try {
        std::filesystem::create_directories(config_.output_dir);
    } catch (const std::exception& e) {
        result.errors.push_back("Cannot create output directory");
        return result;
    }

    std::cout << "[DEBUG] Output directory created\n" << std::flush;

    // Create worker threads for parallel group scanning
    std::vector<std::thread> workers;
    std::cout << "[DEBUG] Creating " << config_.num_workers << " worker threads for " << result.group_count << " groups\n" << std::flush;

    for (uint32_t i = 0; i < config_.num_workers && i < result.group_count; ++i) {
        workers.emplace_back(&Ext4Recovery::group_worker_thread, this, i, progress);
    }

    std::cout << "[DEBUG] Worker threads created, joining...\n" << std::flush;

    for (auto& w : workers) {
        w.join();
    }

    // Merge results from queue into map (single-threaded, safe)
    {
        std::lock_guard<std::mutex> lock(result_mutex_);
        while (!result_queue_.empty()) {
            auto& item = result_queue_.front();
            uint64_t index_id = item.first;
            Ext4BlockInfo block_info = std::move(item.second);
            result_queue_.pop();

            result.pages_by_index_id[index_id].push_back(std::move(block_info));
            result.pages_recovered++;
        }
        result.deleted_inodes_found = blocks_found_.load();
        result.total_blocks_scanned = blocks_scanned_.load();
        result.recovered_inodes = last_result_.recovered_inodes;
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    result.elapsed_seconds = std::chrono::duration<double>(end_time - start_time).count();
    result.success = true;

    last_result_ = result;
    return result;
}

// ==================== Group Worker Thread ====================

void Ext4Recovery::group_worker_thread(uint32_t worker_id,
                                       std::function<void(double, const std::string&)> progress) {
#ifdef __linux__
    std::cout << "[DEBUG] Worker " << worker_id << " starting\n" << std::flush;

    int fd = open(config_.device_path.c_str(), O_RDONLY);
    if (fd < 0) {
        std::cout << "[DEBUG] Worker " << worker_id << " failed to open device\n" << std::flush;
        return;
    }

    // Use cached superblock values (thread-safe)
    uint32_t block_size = block_size_;
    uint32_t group_count = group_count_;
    uint32_t blocks_per_group = blocks_per_group_;
    uint32_t inodes_per_group = inodes_per_group_;
    uint16_t inode_size = inode_size_;
    uint16_t desc_size = desc_size_;

    std::cout << "[DEBUG] Worker " << worker_id << ": block_size=" << block_size
              << " group_count=" << group_count << " blocks_per_group=" << blocks_per_group
              << " inodes_per_group=" << inodes_per_group << "\n" << std::flush;

    if (group_count == 0) {
        close(fd);
        return;  // Safety check
    }

    auto last_progress_time = std::chrono::steady_clock::now();

    while (!should_stop_.load()) {
        uint64_t group_idx = current_group_.fetch_add(1);
        uint32_t group_index = static_cast<uint32_t>(group_idx);

        if (group_index >= group_count || group_idx >= group_count) {
            break;
        }

        // Read block bitmap to identify freed blocks
        if (config_.scan_freed_blocks) {
            std::vector<uint8_t> bitmap = read_block_bitmap(group_index);
            if (!bitmap.empty()) {
                // Scan freed blocks
                scan_freed_blocks(group_index, bitmap);
            }
        }

        // Read inode bitmap and scan deleted inodes
        // This is now safer with bounds checking in inode_to_disk_offset
        if (config_.scan_deleted_inodes) {
            try {
                std::vector<uint8_t> inode_bitmap = read_inode_bitmap(group_index);
                if (!inode_bitmap.empty()) {
                    // Scan deleted inodes (those with bit=0 in bitmap but have data)
                    scan_deleted_inodes_safe(group_index, inode_bitmap);
                }
            } catch (const std::exception& e) {
                // Log and continue on error
                if (config_.verbose) {
                    std::cout << "[WARN] Inode scan failed for group " << group_index
                              << ": " << e.what() << "\n";
                }
            }
        }

        // Progress reporting
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_progress_time).count();

        if (progress && elapsed_ms >= config_.progress_interval_ms) {
            double pct = static_cast<double>(group_index + 1) / group_count;
            std::ostringstream oss;
            oss << "Group " << (group_index + 1) << "/" << group_count
                << " blocks=" << blocks_scanned_.load()
                << " found=" << blocks_found_.load();
            progress(pct, oss.str());
            last_progress_time = now;
        }
    }

    close(fd);
#endif
}

// ==================== Freed Block Scanning ====================

void Ext4Recovery::scan_freed_blocks(uint32_t group_index, const std::vector<uint8_t>& bitmap) {
#ifdef __linux__
    int fd = open(config_.device_path.c_str(), O_RDONLY);
    if (fd < 0) {
        return;
    }

    // Use cached values (thread-safe)
    uint32_t block_size = block_size_;
    uint32_t blocks_per_group = blocks_per_group_;
    uint32_t group_count = group_count_;

    // Calculate group start offset
    uint64_t group_start_block = static_cast<uint64_t>(group_index) * blocks_per_group;

    // Safety check: ensure we don't read beyond device
    uint64_t max_offset = static_cast<uint64_t>(group_count_) * blocks_per_group * block_size;

    // InnoDB pages are 16KB, spanning multiple 4KB blocks
    uint32_t blocks_per_page = config_.page_size / block_size;
    if (blocks_per_page == 0) blocks_per_page = 4;  // Default: 16KB page / 4KB block = 4

    // Scan at page-aligned offsets within this group
    // Limit the number of pages we scan per group to avoid overflow
    uint32_t max_pages_per_group = blocks_per_group / blocks_per_page;
    if (max_pages_per_group > 8192) max_pages_per_group = 8192;  // Safety limit

    for (uint32_t page_idx = 0; page_idx < max_pages_per_group; page_idx++) {
        uint32_t block_offset = page_idx * blocks_per_page;
        if (block_offset >= blocks_per_group) break;

        uint64_t block_num = group_start_block + block_offset;
        uint64_t disk_offset = block_to_disk_offset(block_num);

        // Safety check: don't read beyond device
        if (disk_offset + config_.page_size > max_offset) break;

        // Read full page (16KB)
        std::vector<uint8_t> page_data(config_.page_size);
        ssize_t bytes_read = pread(fd, page_data.data(), config_.page_size, disk_offset);

        if (bytes_read == config_.page_size) {
            blocks_scanned_.fetch_add(1);

            // Check for InnoDB page type at offset 24
            if (page_data.size() >= 26) {
                uint16_t page_type = read_be<uint16_t>(page_data.data(), FIL_PAGE_TYPE_ABS);

                // Check for INDEX page type (0x45BF)
                if (page_type == FIL_PAGE_INDEX) {
                    uint64_t index_id = read_be<uint64_t>(page_data.data(), 66);

                    if (index_id > 0 && index_id < 1000000000000) {  // Sanity check for index_id
                        Ext4BlockInfo block_info;
                        block_info.block_number = block_num;
                        block_info.disk_offset = disk_offset;
                        block_info.block_group = group_index;
                        block_info.is_freed = true;
                        block_info.data = std::move(page_data);

                        // Thread-safe: push to queue, merge later in main thread
                        std::lock_guard<std::mutex> lock(result_mutex_);
                        result_queue_.push({index_id, std::move(block_info)});
                        blocks_found_.fetch_add(1);
                    }
                }
            }
        }
    }

    close(fd);
#endif
}

// ==================== Safe Deleted Inode Scanning ====================

void Ext4Recovery::scan_deleted_inodes_safe(uint32_t group_index, const std::vector<uint8_t>& inode_bitmap) {
#ifdef __linux__
    int fd = open(config_.device_path.c_str(), O_RDONLY);
    if (fd < 0) {
        return;
    }

    // Use cached values (thread-safe)
    uint32_t inodes_per_group = inodes_per_group_;
    uint16_t inode_size = inode_size_;
    uint32_t block_size = block_size_;

    // Safety checks
    if (inodes_per_group == 0 || inode_size == 0 || block_size == 0) {
        close(fd);
        return;
    }

    // Calculate maximum disk offset for safety
    uint64_t max_offset = static_cast<uint64_t>(group_count_) * blocks_per_group_ * block_size;

    // Scan for potential deleted inodes
    // Focus on inodes that were regular files (potential MySQL data files)
    for (uint32_t local_inode = 1; local_inode < inodes_per_group; local_inode++) {
        // Check if inode is marked as free in bitmap
        uint32_t byte_idx = local_inode / 8;
        uint32_t bit_idx = local_inode % 8;

        if (byte_idx >= inode_bitmap.size()) {
            break;  // Safety check
        }

        bool is_free = (inode_bitmap[byte_idx] & (1 << bit_idx)) == 0;

        if (!is_free) {
            continue;  // Skip allocated inodes
        }

        // Try to read the inode safely
        uint64_t inode_offset = inode_to_disk_offset(group_index, local_inode);
        if (inode_offset == 0 || inode_offset >= max_offset) {
            continue;  // Invalid or out-of-bounds offset
        }

        ext4_inode inode_raw;
        memset(&inode_raw, 0, sizeof(inode_raw));

        // Read with safety limit
        ssize_t bytes_read = pread(fd, &inode_raw, std::min(inode_size, static_cast<uint16_t>(sizeof(inode_raw))), inode_offset);

        if (bytes_read <= 0) {
            continue;  // Read failed
        }

        // Check if this was a regular file with data
        uint16_t mode = le16toh(inode_raw.i_mode);
        uint32_t dtime = le32toh(inode_raw.i_dtime);
        uint16_t links_count = le16toh(inode_raw.i_links_count);
        uint64_t file_size = le32toh(inode_raw.i_size_lo) |
                            (static_cast<uint64_t>(le32toh(inode_raw.i_size_hi)) << 32);

        // Check for deleted regular files with size matching InnoDB data
        constexpr uint16_t EXT4_S_IFREG = 0x8000;  // Regular file type
        bool is_regular_file = (mode & EXT4_S_IFREG) != 0;
        bool is_deleted = (dtime > 0) || (links_count == 0) || (mode == 0);

        // Only process deleted regular files with potential InnoDB data
        if (is_deleted && file_size >= config_.page_size) {
            // Parse extents to find data blocks
            std::vector<ext4_extent> extents = parse_extents(inode_raw);

            for (const auto& ext : extents) {
                uint32_t ext_len = le16toh(ext.ee_len);
                uint32_t ext_start_lo = le32toh(ext.ee_start_lo);
                uint16_t ext_start_hi = le16toh(ext.ee_start_hi);

                uint64_t ext_start = ext_start_lo | (static_cast<uint64_t>(ext_start_hi) << 32);

                if (ext_len == 0 || ext_start == 0) {
                    continue;
                }

                // Read blocks from this extent
                for (uint32_t blk = 0; blk < ext_len && blk < 1000; blk++) {
                    uint64_t block_num = ext_start + blk;
                    uint64_t disk_offset = block_num * block_size;

                    if (disk_offset + config_.page_size > max_offset) {
                        break;  // Safety check
                    }

                    std::vector<uint8_t> page_data(config_.page_size);
                    ssize_t page_read = pread(fd, page_data.data(), config_.page_size, disk_offset);

                    if (page_read == config_.page_size) {
                        // Check for InnoDB INDEX page
                        if (page_data.size() >= 26) {
                            uint16_t page_type = read_be<uint16_t>(page_data.data(), FIL_PAGE_TYPE_ABS);

                            if (page_type == FIL_PAGE_INDEX) {
                                uint64_t index_id = read_be<uint64_t>(page_data.data(), 66);

                                if (index_id > 0 && index_id < 1000000000000) {
                                    Ext4BlockInfo block_info;
                                    block_info.block_number = block_num;
                                    block_info.disk_offset = disk_offset;
                                    block_info.block_group = group_index;
                                    block_info.is_freed = true;
                                    block_info.data = std::move(page_data);

                                    std::lock_guard<std::mutex> lock(result_mutex_);
                                    result_queue_.push({index_id, std::move(block_info)});
                                    blocks_found_.fetch_add(1);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    close(fd);
#endif
}

// ==================== Recovery Function ====================

Ext4ScanResult Ext4Recovery::recover(const Ext4ScanResult& scan_result,
                                     std::function<void(double, const std::string&)> progress) {
    Ext4ScanResult result = scan_result;

    if (!scan_result.success) {
        result.errors.push_back("Scan was not successful");
        return result;
    }

    // Check if schema-aware recovery is enabled
    bool use_schema = external_schema_.has_value() && !external_schema_->columns.empty();

    if (use_schema && config_.verbose) {
        std::cout << "[INFO] Using schema-aware recovery with "
                  << external_schema_->columns.size() << " columns\n";
    }

    // Prepare IndexColumnDef list if schema is available
    std::vector<dbrecovery::innodb::IndexColumnDef> index_columns;
    if (use_schema) {
        // Convert TableSchema to IndexColumnDef list
        std::vector<std::pair<uint32_t, catalog::ColumnInfo>> sorted_cols;
        for (const auto& [ordinal, col] : external_schema_->columns) {
            uint32_t phys_pos = col.physical_pos > 0 ? col.physical_pos : ordinal;
            sorted_cols.push_back({phys_pos, col});
        }
        std::sort(sorted_cols.begin(), sorted_cols.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

        for (const auto& [phys_pos, col] : sorted_cols) {
            dbrecovery::innodb::IndexColumnDef col_def;
            col_def.name = col.name;
            // Convert MySQL server type to InnoDB storage type
            // Reference: xfs_disk_carver.cpp mysql_type_to_innodb()
            switch (col.mysql_type_id) {
                case 1: col_def.col_type = 1; col_def.col_length = 1; break;  // TINYINT
                case 2: col_def.col_type = 3; col_def.col_length = 2; break;  // SMALLINT
                case 3: col_def.col_type = 4; col_def.col_length = 4; break;  // INT
                case 4: col_def.col_type = 5; col_def.col_length = 4; break;  // FLOAT
                case 5: col_def.col_type = 6; col_def.col_length = 8; break;  // DOUBLE
                case 8: col_def.col_type = 9; col_def.col_length = 8; break;  // BIGINT
                case 9: col_def.col_type = 10; col_def.col_length = 3; break;  // MEDIUMINT
                case 10: col_def.col_type = 15; col_def.col_length = 3; break; // DATE
                case 11: col_def.col_type = 20; col_def.col_length = 3; break; // TIME
                case 12: col_def.col_type = 19; col_def.col_length = 5; break; // DATETIME
                case 15: // VARCHAR
                case 253: col_def.col_type = 16; break;  // VARCHAR (InnoDB type 16 is variable)
                case 245: col_def.col_type = 31; break; // JSON (InnoDB type 31)
                case 252: col_def.col_type = 27; break; // TEXT/BLOB (InnoDB type 27)
                case 251: col_def.col_type = 27; break; // BLOB (InnoDB type 27)
                case 247: col_def.col_type = 247; col_def.col_length = 1; break; // ENUM
                case 248: col_def.col_type = 248; col_def.col_length = 1; break; // SET
                default: col_def.col_type = 16; break; // Default to VARCHAR type
            }
            col_def.is_nullable = col.is_nullable;
            col_def.is_unsigned = !col.is_signed;
            col_def.ordinal_position = col.ordinal;
            col_def.physical_pos = phys_pos;
            index_columns.push_back(col_def);
        }
    }

    // Export recovered pages to CSV files with record parsing
    uint64_t total_records = 0;
    uint32_t processed_index = 0;

    for (const auto& [index_id, pages] : scan_result.pages_by_index_id) {
        if (pages.empty()) continue;

        processed_index++;

        // Progress reporting
        if (progress) {
            double pct = static_cast<double>(processed_index) / scan_result.pages_by_index_id.size();
            std::ostringstream oss;
            oss << "Exporting index " << index_id << " (" << pages.size() << " pages)";
            progress(pct, oss.str());
        }

        std::string output_file = config_.output_dir + "/index_" +
                                  std::to_string(index_id) + ".csv";
        std::ofstream out(output_file, std::ios::binary);
        if (!out) {
            result.errors.push_back("Cannot create output file: " + output_file);
            continue;
        }

        // Write header
        if (use_schema) {
            // Write column names from schema
            bool first = true;
            for (const auto& col : index_columns) {
                if (!first) out << ",";
                out << col.name;
                first = false;
            }
            out << "\n";
        } else {
            // Write default header
            out << "page_no,index_id,trx_id,roll_ptr,record_data\n";
        }

        // Parse each page and extract records
        for (const auto& page : pages) {
            if (page.data.size() < config_.page_size) continue;

            try {
                dbrecovery::innodb::InnoDBIndexParser parser;
                uint32_t page_no = static_cast<uint32_t>(page.disk_offset / config_.page_size);

                if (use_schema) {
                    // Schema-aware parsing
                    parser.set_columns(index_columns);
                    auto records = parser.parse_page(page.data.data(), page.data.size(), page_no);

                    for (const auto& rec : records) {
                        bool first = true;
                        for (size_t i = 0; i < rec.fields.size() && i < index_columns.size(); i++) {
                            if (!first) out << ",";
                            first = false;

                            if (rec.fields[i].is_null) {
                                out << "NULL";
                            } else {
                                // Quote string values and escape special characters
                                std::string val = rec.fields[i].str_value;
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
                            }
                        }
                        out << "\n";
                        total_records++;
                    }
                } else {
                    // Schema-less parsing (original behavior)
                    auto records = parser.parse_page(page.data.data(), page.data.size(), page_no);

                    for (const auto& rec : records) {
                        // Write record info
                        out << (page.disk_offset / config_.page_size) << ","
                            << index_id << ","
                            << rec.trx_id << ","
                            << rec.roll_ptr << ",\"";

                        // Escape and write record data
                        for (uint8_t byte : rec.raw_data) {
                            if (byte >= 32 && byte < 127 && byte != '"' && byte != '\\') {
                                out << static_cast<char>(byte);
                            } else if (byte == '"' || byte == '\\') {
                                out << '\\' << static_cast<char>(byte);
                            } else {
                                out << '.';
                            }
                        }
                        out << "\"\n";
                        total_records++;
                    }
                }
            } catch (const std::exception& e) {
                if (!use_schema) {
                    // Fallback: export raw page data if parsing fails
                    out << (page.disk_offset / config_.page_size) << ","
                        << index_id << ","
                        << "0,0,\"";

                    // Find infimum/supremum and extract readable content
                    constexpr uint16_t INFIMUM_STRING_OFFSET = 59;
                    for (size_t i = INFIMUM_STRING_OFFSET; i < page.data.size(); i++) {
                        uint8_t byte = page.data[i];
                        if (byte >= 32 && byte < 127 && byte != '"' && byte != '\\') {
                            out << static_cast<char>(byte);
                        }
                    }
                    out << "\"\n";
                }
            }
        }
        out.close();
    }

    result.total_records = total_records;
    result.success = true;

    if (config_.verbose) {
        std::cout << "[INFO] Exported " << total_records << " records to CSV files\n";
    }

    return result;
}

Ext4ScanResult Ext4Recovery::scan_and_recover(
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

} // namespace ext4
} // namespace recovery
} // namespace dbrecovery
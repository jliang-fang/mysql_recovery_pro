/**
 * @file ext4_recovery.cpp
 * @brief ext4 filesystem-specific recovery implementation for MySQL/InnoDB data
 */

#include "dbrecovery/recovery/ext4_recovery.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/index_page_parser.hpp"
#include "dbrecovery/innodb/innodb_checksum.hpp"
#include "dbrecovery/common/mysql_version_detector.hpp"
#include "dbrecovery/common/version_compat.hpp"
#include "dbrecovery/catalog/schema_type_conversion.hpp"
#include "dbrecovery/export/csv_cell.hpp"

#include <chrono>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>
#include <fstream>
#include <functional>

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
constexpr uint16_t EXT4_MAX_EXTENT_DEPTH = 5;

bool checked_add(uint64_t left, uint64_t right, uint64_t& result) {
    if (left > std::numeric_limits<uint64_t>::max() - right) {
        return false;
    }
    result = left + right;
    return true;
}

bool checked_multiply(uint64_t left, uint64_t right, uint64_t& result) {
    if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

bool contains_range(uint64_t source_size, uint64_t offset, uint64_t length) {
    return offset <= source_size && length <= source_size - offset;
}

bool block_range(uint64_t block, uint32_t block_size, uint64_t source_size,
                 uint64_t& offset, uint64_t length = 0) {
    return checked_multiply(block, block_size, offset) &&
           contains_range(source_size, offset, length);
}

bool block_to_disk_offset_checked(uint64_t block, uint32_t block_size,
                                  uint64_t source_size, uint64_t& offset) {
    return checked_multiply(block, block_size, offset) &&
           offset <= source_size;
}

bool valid_extent_header(const ext4_extent_header& header, size_t buffer_size,
                         uint16_t expected_depth) {
    if (buffer_size < sizeof(ext4_extent_header)) {
        return false;
    }
    if (le16toh(header.eh_magic) != EXT4_EXTENT_MAGIC ||
        le16toh(header.eh_depth) != expected_depth) {
        return false;
    }

    const uint16_t entries = le16toh(header.eh_entries);
    const uint16_t maximum = le16toh(header.eh_max);
    const size_t capacity = (buffer_size - sizeof(ext4_extent_header)) / sizeof(ext4_extent);
    return entries > 0 && entries <= maximum && entries <= capacity;
}

bool read_superblock_from_fd(int fd, uint64_t source_size, ext4_superblock& sb) {
#ifdef __linux__
    if (fd < 0 || source_size < EXT4_SUPERBLOCK_OFFSET + sizeof(ext4_superblock)) {
        return false;
    }

    auto read_at = [&](uint64_t offset) -> ssize_t {
        if (offset > source_size || sizeof(ext4_superblock) > source_size - offset) {
            return -1;
        }
        return ::pread(fd, &sb, sizeof(ext4_superblock), offset);
    };

    ssize_t bytes_read = read_at(EXT4_SUPERBLOCK_OFFSET);
    if (bytes_read == sizeof(ext4_superblock) &&
        le16toh(sb.s_magic) == EXT2_SUPER_MAGIC) {
        return true;
    }

    std::cerr << "[WARN] Primary ext4 superblock invalid, trying backup superblocks...\n";

    uint32_t block_size = 1024;
    uint32_t blocks_per_group = 8192;
    if (bytes_read >= static_cast<ssize_t>(offsetof(ext4_superblock, s_log_block_size) + 8)) {
        const uint32_t log_block_size = le32toh(sb.s_log_block_size);
        if (log_block_size <= 6) {
            block_size = EXT4_SUPERBLOCK_OFFSET << log_block_size;
        }
        const uint32_t bpg = le32toh(sb.s_blocks_per_group);
        if (bpg > 0 && bpg < 100000000) {
            blocks_per_group = bpg;
        }
    }

    for (int backup_group : {1, 3, 5, 7, 9, 25, 27, 32, 49, 64, 81, 100, 125, 128, 243, 256, 343, 512, 625, 729}) {
        const uint64_t backup_offset = static_cast<uint64_t>(backup_group) *
                                       static_cast<uint64_t>(blocks_per_group) * block_size +
                                       EXT4_SUPERBLOCK_OFFSET;
        bytes_read = read_at(backup_offset);
        if (bytes_read == sizeof(ext4_superblock) &&
            le16toh(sb.s_magic) == EXT2_SUPER_MAGIC) {
            std::cerr << "[INFO] Found valid ext4 superblock in group " << backup_group
                      << " (offset " << backup_offset << ")\n";
            return true;
        }
    }
#endif
    return false;
}

Ext4Recovery::Ext4Recovery() = default;

Ext4Recovery::~Ext4Recovery() {
#ifdef __linux__
    if (source_fd_ >= 0) {
        ::close(source_fd_);
        source_fd_ = -1;
    }
#endif
}

bool Ext4Recovery::adopt_fd(int owned_fd, uint64_t known_size) {
#ifdef __linux__
    if (owned_fd < 0 || known_size == 0) {
        if (owned_fd >= 0) ::close(owned_fd);
        return false;
    }

    const int flags = ::fcntl(owned_fd, F_GETFL);
    struct stat status {};
    if (flags < 0 || (flags & O_ACCMODE) != O_RDONLY ||
        ::fstat(owned_fd, &status) != 0) {
        ::close(owned_fd);
        return false;
    }

    if (source_fd_ >= 0) ::close(source_fd_);
    source_fd_ = owned_fd;
    source_size_ = known_size;
    return true;
#else
    (void)owned_fd;
    (void)known_size;
    return false;
#endif
}

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
    struct stat status {};
    if (::stat(device_path.c_str(), &status) != 0 || S_ISBLK(status.st_mode)) {
        return false;
    }

    int fd = open(device_path.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }

    // ext4 superblock is at offset 1024 (0x400)
    ssize_t bytes_read = pread(fd, &sb, sizeof(ext4_superblock), EXT4_SUPERBLOCK_OFFSET);

    if (bytes_read == sizeof(ext4_superblock)) {
        uint16_t magic = le16toh(sb.s_magic);
        if (magic == EXT2_SUPER_MAGIC) {
            close(fd);
            return true;
        }
    }

    // Primary superblock invalid - try backup superblocks
    // ext4 backup superblocks exist at the start of each block group:
    // Groups 0, 1, and powers of 3, 5, 7
    // The most reliable backup is at group 1 (block_size * blocks_per_group)
    // If we can't read from primary, try reading geometry from partial primary SB
    std::cerr << "[WARN] Primary ext4 superblock invalid, trying backup superblocks...\n";

    uint32_t block_size = 1024;   // Default ext4 block size
    uint32_t blocks_per_group = 8192;  // Default

    // Try to get geometry from primary SB even if magic is wrong
    if (bytes_read >= static_cast<ssize_t>(offsetof(ext4_superblock, s_log_block_size) + 8)) {
        uint32_t log_block_size = le32toh(sb.s_log_block_size);
        if (log_block_size <= 6) {
            block_size = EXT4_SUPERBLOCK_OFFSET << log_block_size;
        }
        uint32_t bpg = le32toh(sb.s_blocks_per_group);
        if (bpg > 0 && bpg < 100000000) {
            blocks_per_group = bpg;
        }
    }

    // Try backup superblock at group 1 (most common backup location)
    for (int backup_group : {1, 3, 5, 7, 9, 25, 27, 32, 49, 64, 81, 100, 125, 128, 243, 256, 343, 512, 625, 729}) {
        uint64_t backup_offset = static_cast<uint64_t>(backup_group) *
                                static_cast<uint64_t>(blocks_per_group) * block_size +
                                EXT4_SUPERBLOCK_OFFSET;
        bytes_read = pread(fd, &sb, sizeof(ext4_superblock), backup_offset);
        if (bytes_read == sizeof(ext4_superblock)) {
            uint16_t magic = le16toh(sb.s_magic);
            if (magic == EXT2_SUPER_MAGIC) {
                std::cerr << "[INFO] Found valid ext4 superblock in group " << backup_group
                          << " (offset " << backup_offset << ")\n";
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

// ==================== Group Descriptor Reading ====================

bool Ext4Recovery::read_group_descriptor(int fd, uint32_t group_index, ext4_group_desc& desc) const {
#ifdef __linux__
    if (fd < 0 || block_size_ == 0 || desc_size_ < 32 ||
        desc_size_ > sizeof(ext4_group_desc) || group_index >= group_count_) {
        return false;
    }

    const uint32_t gdb_block = (block_size_ == 1024) ? 2 : 1;
    uint64_t gdb_offset = 0;
    uint64_t index_offset = 0;
    uint64_t desc_offset = 0;
    if (!checked_multiply(gdb_block, block_size_, gdb_offset) ||
        !checked_multiply(group_index, desc_size_, index_offset) ||
        !checked_add(gdb_offset, index_offset, desc_offset) ||
        !contains_range(source_size_, desc_offset, desc_size_)) {
        return false;
    }

    std::memset(&desc, 0, sizeof(desc));
    return ::pread(fd, &desc, desc_size_, desc_offset) == desc_size_;
#else
    (void)fd;
    (void)group_index;
    (void)desc;
    return false;
#endif
}

// ==================== Block Bitmap Reading ====================

std::vector<uint8_t> Ext4Recovery::read_block_bitmap(int fd, uint32_t group_index) {
#ifdef __linux__
    ext4_group_desc desc {};
    if (!read_group_descriptor(fd, group_index, desc)) {
        return {};
    }

    const uint64_t bitmap_block = static_cast<uint64_t>(le32toh(desc.bg_block_bitmap_lo)) |
                                  (static_cast<uint64_t>(le32toh(desc.bg_block_bitmap_hi)) << 32);
    if (bitmap_block == 0 || blocks_per_group_ == 0 || block_size_ == 0) {
        return {};
    }

    const size_t bitmap_size = (blocks_per_group_ + 7) / 8;
    uint64_t disk_offset = 0;
    if (!checked_multiply(bitmap_block, block_size_, disk_offset) ||
        !contains_range(source_size_, disk_offset, bitmap_size)) {
        return {};
    }

    std::vector<uint8_t> bitmap(bitmap_size);
    if (::pread(fd, bitmap.data(), bitmap_size, disk_offset) !=
        static_cast<ssize_t>(bitmap_size)) {
        return {};
    }
    return bitmap;
#else
    (void)fd;
    (void)group_index;
    return {};
#endif
}

// ==================== Inode Bitmap Reading ====================

std::vector<uint8_t> Ext4Recovery::read_inode_bitmap(int fd, uint32_t group_index) {
#ifdef __linux__
    ext4_group_desc desc {};
    if (!read_group_descriptor(fd, group_index, desc)) {
        return {};
    }

    const uint64_t bitmap_block = static_cast<uint64_t>(le32toh(desc.bg_inode_bitmap_lo)) |
                                  (static_cast<uint64_t>(le32toh(desc.bg_inode_bitmap_hi)) << 32);
    if (bitmap_block == 0 || inodes_per_group_ == 0 || block_size_ == 0) {
        return {};
    }

    const size_t bitmap_size = (inodes_per_group_ + 7) / 8;
    uint64_t disk_offset = 0;
    if (!checked_multiply(bitmap_block, block_size_, disk_offset) ||
        !contains_range(source_size_, disk_offset, bitmap_size)) {
        return {};
    }

    std::vector<uint8_t> bitmap(bitmap_size);
    if (::pread(fd, bitmap.data(), bitmap_size, disk_offset) !=
        static_cast<ssize_t>(bitmap_size)) {
        return {};
    }
    return bitmap;
#else
    (void)fd;
    (void)group_index;
    return {};
#endif
}

// ==================== Inode Reading ====================

uint64_t Ext4Recovery::inode_to_disk_offset(int fd, uint32_t group_index, uint32_t local_inode) const {
#ifdef __linux__
    if (local_inode == 0 || local_inode > inodes_per_group_ ||
        block_size_ == 0 || inode_size_ == 0 || blocks_per_group_ == 0) {
        return 0;
    }

    ext4_group_desc desc {};
    if (!read_group_descriptor(fd, group_index, desc)) {
        return 0;
    }

    const uint64_t inode_table_block = static_cast<uint64_t>(le32toh(desc.bg_inode_table_lo)) |
                                       (static_cast<uint64_t>(le32toh(desc.bg_inode_table_hi)) << 32);
    uint64_t group_start_block = 0;
    if (!checked_multiply(group_index, blocks_per_group_, group_start_block)) {
        return 0;
    }
    const uint64_t group_end_block = group_start_block + blocks_per_group_;
    if (inode_table_block < group_start_block || inode_table_block >= group_end_block) {
        return 0;
    }

    uint64_t table_offset = 0;
    uint64_t inode_offset = 0;
    if (!checked_multiply(inode_table_block, block_size_, table_offset) ||
        !checked_multiply(local_inode - 1, inode_size_, inode_offset) ||
        !contains_range(source_size_, table_offset, inode_offset)) {
        return 0;
    }
    return table_offset + inode_offset;
#else
    (void)fd;
    (void)group_index;
    (void)local_inode;
    return 0;
#endif
}

std::optional<Ext4InodeInfo> Ext4Recovery::read_inode(int fd, uint32_t group_index, uint32_t local_inode) {
#ifdef __linux__
    if (fd < 0 || inode_size_ == 0 || inode_size_ > sizeof(ext4_inode) ||
        inodes_per_group_ == 0) {
        return std::nullopt;
    }

    const uint64_t inode_offset = inode_to_disk_offset(fd, group_index, local_inode);
    if (inode_offset == 0 || inode_offset > source_size_ ||
        inode_size_ > source_size_ - inode_offset) {
        return std::nullopt;
    }

    ext4_inode inode_raw {};
    if (::pread(fd, &inode_raw, inode_size_, inode_offset) != inode_size_) {
        return std::nullopt;
    }

    Ext4InodeInfo info;
    info.inode_number = group_index * inodes_per_group_ + local_inode;
    info.block_group = group_index;

    const uint16_t mode = le16toh(inode_raw.i_mode);
    const uint32_t dtime = le32toh(inode_raw.i_dtime);
    const uint16_t links_count = le16toh(inode_raw.i_links_count);
    info.is_deleted = (mode == 0) || (dtime > 0) || (links_count == 0);
    info.dtime = dtime;
    info.file_size = le32toh(inode_raw.i_size_lo) |
                     (static_cast<uint64_t>(le32toh(inode_raw.i_size_hi)) << 32);
    if (info.file_size == 0) {
        return std::nullopt;
    }

    info.extents = parse_extents(fd, inode_raw, block_size_);
    if (info.extents.empty() && info.file_size >= config_.page_size) {
        info.extents = traverse_extent_tree_from_inode(fd, inode_raw, block_size_);
    }
    return info.extents.empty() ? std::nullopt : std::optional<Ext4InodeInfo>(std::move(info));
#else
    (void)fd;
    (void)group_index;
    (void)local_inode;
    return std::nullopt;
#endif
}

// ==================== Extent Parsing ====================

std::vector<ext4_extent> Ext4Recovery::parse_extents(int fd, const ext4_inode& inode, uint32_t block_size) {
    std::vector<ext4_extent> extents;

    // Check if inode uses extents (flag bit 4)
    uint32_t flags = le32toh(inode.i_flags);
    bool uses_extents = (flags & 0x80000) != 0;  // EXT4_EXTENTS_FL

    if (!uses_extents) {
        // Legacy block mapping — direct/indirect block pointers
        // i_block[0..11]: direct blocks
        // i_block[12]: single indirect
        // i_block[13]: double indirect
        // i_block[14]: triple indirect
        uint32_t blk_sz = block_size;

        auto read_block = [&](uint64_t block_num) -> std::vector<uint8_t> {
            uint64_t disk_offset = 0;
            if (!block_to_disk_offset_checked(block_num, blk_sz, source_size_, disk_offset) ||
                !contains_range(source_size_, disk_offset, blk_sz)) {
                return {};
            }
            std::vector<uint8_t> buf(blk_sz);
            ssize_t n = pread(fd, buf.data(), blk_sz, static_cast<off_t>(disk_offset));
            if (n != static_cast<ssize_t>(blk_sz)) buf.clear();
            return buf;
        };

        // Helper: read indirect block pointers (4 bytes each, little-endian)
        std::function<std::vector<uint64_t>(uint64_t, int, uint32_t)> read_indirect_blocks;
        read_indirect_blocks = [&](uint64_t indirect_block, int level,
                                   uint32_t bsz) -> std::vector<uint64_t> {
            std::vector<uint64_t> result;
            if (level <= 0) return result;

            auto blk_data = read_block(indirect_block);
            if (blk_data.empty()) return result;

            uint32_t entries_per_block = bsz / 4;
            for (uint32_t i = 0; i < entries_per_block && i * 4 + 3 < blk_data.size(); i++) {
                uint64_t entry = blk_data[i * 4] |
                                 (static_cast<uint64_t>(blk_data[i * 4 + 1]) << 8) |
                                 (static_cast<uint64_t>(blk_data[i * 4 + 2]) << 16) |
                                 (static_cast<uint64_t>(blk_data[i * 4 + 3]) << 24);
                if (entry == 0) continue;

                if (level == 1) {
                    result.push_back(entry);
                } else {
                    auto sub_blocks = read_indirect_blocks(entry, level - 1, bsz);
                    result.insert(result.end(), sub_blocks.begin(), sub_blocks.end());
                }
            }
            return result;
        };

        // Collect all block numbers
        std::vector<uint64_t> all_blocks;

        // Direct blocks: i_block[0..11]
        for (int i = 0; i < 12; i++) {
            uint32_t blk = le32toh(inode.i_block[i]);
            if (blk != 0) all_blocks.push_back(blk);
        }

        // Single indirect: i_block[12]
        uint32_t s_indirect = le32toh(inode.i_block[12]);
        if (s_indirect != 0) {
            auto indirect_blocks = read_indirect_blocks(s_indirect, 1, blk_sz);
            all_blocks.insert(all_blocks.end(), indirect_blocks.begin(), indirect_blocks.end());
        }

        // Double indirect: i_block[13]
        uint32_t d_indirect = le32toh(inode.i_block[13]);
        if (d_indirect != 0) {
            auto indirect_blocks = read_indirect_blocks(d_indirect, 2, blk_sz);
            all_blocks.insert(all_blocks.end(), indirect_blocks.begin(), indirect_blocks.end());
        }

        // Triple indirect: i_block[14]
        uint32_t t_indirect = le32toh(inode.i_block[14]);
        if (t_indirect != 0) {
            auto indirect_blocks = read_indirect_blocks(t_indirect, 3, blk_sz);
            all_blocks.insert(all_blocks.end(), indirect_blocks.begin(), indirect_blocks.end());
        }

        // Convert to extents (contiguous runs)
        if (!all_blocks.empty()) {
            std::sort(all_blocks.begin(), all_blocks.end());

            ext4_extent ext;
            ext.ee_block = 0;
            ext.ee_start_lo = static_cast<uint32_t>(all_blocks[0] & 0xFFFFFFFF);
            ext.ee_start_hi = static_cast<uint16_t>((all_blocks[0] >> 32) & 0xFFFF);
            ext.ee_len = 1;

            for (size_t i = 1; i < all_blocks.size(); i++) {
                if (all_blocks[i] == all_blocks[i - 1] + 1) {
                    ext.ee_len++;
                } else {
                    extents.push_back(ext);
                    ext.ee_block += ext.ee_len;
                    ext.ee_start_lo = static_cast<uint32_t>(all_blocks[i] & 0xFFFFFFFF);
                    ext.ee_start_hi = static_cast<uint16_t>((all_blocks[i] >> 32) & 0xFFFF);
                    ext.ee_len = 1;
                }
            }
            extents.push_back(ext);
        }

        return extents;
    }

    // Read extent header from i_block
    const ext4_extent_header* header = reinterpret_cast<const ext4_extent_header*>(
        reinterpret_cast<const uint8_t*>(inode.i_block));

    if (le16toh(header->eh_magic) != EXT4_EXTENT_MAGIC) {
        return extents;
    }

    uint16_t entries = le16toh(header->eh_entries);
    uint16_t depth = le16toh(header->eh_depth);

    if (depth == 0 && entries > 0 && entries <= 4) {
        // Direct extents in inode
        const ext4_extent* ext_array = reinterpret_cast<const ext4_extent*>(
            reinterpret_cast<const uint8_t*>(inode.i_block) + sizeof(ext4_extent_header));

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

        // We need fd and block_size to traverse — caller should provide via overload
        // Mark that multi-level traversal is needed
    }

    return extents;
}

// ==================== Multi-level Extent Tree Traversal from Inode ====================

std::vector<ext4_extent> Ext4Recovery::traverse_extent_tree_from_inode(
    int fd, const ext4_inode& inode, uint32_t block_size) {

    std::vector<ext4_extent> extents;

#ifdef __linux__
    uint32_t flags = le32toh(inode.i_flags);
    if (!(flags & 0x80000)) return extents;  // EXT4_EXTENTS_FL

    const ext4_extent_header* header = reinterpret_cast<const ext4_extent_header*>(
        reinterpret_cast<const uint8_t*>(inode.i_block));
    if (le16toh(header->eh_magic) != EXT4_EXTENT_MAGIC) return extents;

    uint16_t entries = le16toh(header->eh_entries);
    uint16_t depth = le16toh(header->eh_depth);

    if (depth == 0) {
        // Direct extents — same as parse_extents but with fd context
        const ext4_extent* ext_array = reinterpret_cast<const ext4_extent*>(
            reinterpret_cast<const uint8_t*>(inode.i_block) + sizeof(ext4_extent_header));
        for (uint16_t i = 0; i < entries && i <= 4; i++) {
            ext4_extent ext;
            ext.ee_block = le32toh(ext_array[i].ee_block);
            ext.ee_len = le16toh(ext_array[i].ee_len);
            ext.ee_start_hi = le16toh(ext_array[i].ee_start_hi);
            ext.ee_start_lo = le32toh(ext_array[i].ee_start_lo);
            if (ext.ee_len > 0) extents.push_back(ext);
        }
        return extents;
    }

    // depth > 0: read index entries from inode's i_block, recurse into child blocks
    const uint8_t* idx_array = reinterpret_cast<const uint8_t*>(inode.i_block) + sizeof(ext4_extent_header);
    for (uint16_t i = 0; i < entries && i <= 4; i++) {
        uint32_t ei_leaf_lo = le32toh(*reinterpret_cast<const uint32_t*>(idx_array + i * 12 + 4));
        uint16_t ei_leaf_hi = le16toh(*reinterpret_cast<const uint16_t*>(idx_array + i * 12 + 8));
        uint64_t child_block = (static_cast<uint64_t>(ei_leaf_hi) << 32) | ei_leaf_lo;

        if (child_block > 0) {
            auto child_extents = traverse_extent_tree(fd, child_block, block_size, depth);
            extents.insert(extents.end(), child_extents.begin(), child_extents.end());
        }
    }
#endif

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
    uint64_t block_offset = 0;
    if (!block_to_disk_offset_checked(root_block, block_size, source_size_, block_offset) ||
        !contains_range(source_size_, block_offset, block_size)) {
        return extents;
    }
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

    const size_t capacity = (block_size - sizeof(ext4_extent_header)) / sizeof(ext4_extent);
    if (entries == 0 || entries > capacity) {
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

int Ext4Recovery::duplicate_source_fd() const {
#ifdef __linux__
    return source_fd_ >= 0 ? ::fcntl(source_fd_, F_DUPFD_CLOEXEC, 0) : -1;
#else
    return -1;
#endif
}

Ext4ScanResult Ext4Recovery::scan(std::function<void(double, const std::string&)> progress) {
    Ext4ScanResult result;
    auto start_time = std::chrono::high_resolution_clock::now();

    // Debug: Log entry
    if (dbrecovery::innodb::g_verbose)     std::cout << "[DEBUG] Ext4Recovery::scan() starting\n" << std::flush;

    if (source_fd_ < 0 || !read_superblock_from_fd(source_fd_, source_size_, superblock_)) {
        result.errors.push_back("Invalid ext4 superblock or cannot read admitted source");
        return result;
    }

    if (dbrecovery::innodb::g_verbose) 
    std::cout << "[DEBUG] Superblock read successfully\n" << std::flush;

    // Parse superblock information with fail-closed geometry validation
    const uint32_t log_block_size = le32toh(superblock_.s_log_block_size);
    if (log_block_size > 6) {
        result.errors.push_back("Invalid ext4 geometry: block size shift exceeds limit");
        return result;
    }
    result.block_size = 1024 << log_block_size;
    result.inode_size = le16toh(superblock_.s_inode_size);
    result.blocks_per_group = le32toh(superblock_.s_blocks_per_group);
    result.inodes_per_group = le32toh(superblock_.s_inodes_per_group);

    // Calculate number of block groups
    uint64_t total_blocks = static_cast<uint64_t>(le32toh(superblock_.s_blocks_count_lo)) |
                           (static_cast<uint64_t>(le32toh(superblock_.s_blocks_count_hi)) << 32);
    if (result.block_size == 0 ||
        result.inode_size < 128 || result.inode_size > sizeof(ext4_inode) ||
        result.blocks_per_group == 0 || result.inodes_per_group == 0 ||
        total_blocks == 0) {
        result.errors.push_back("Invalid ext4 geometry");
        return result;
    }
    result.group_count = static_cast<uint32_t>(
        (total_blocks / result.blocks_per_group) +
        ((total_blocks % result.blocks_per_group != 0) ? 1 : 0));

    // Cache all superblock values to member variables for thread-safe access
    group_count_ = result.group_count;
    block_size_ = result.block_size;
    blocks_per_group_ = result.blocks_per_group;
    inodes_per_group_ = result.inodes_per_group;
    inode_size_ = result.inode_size;
    desc_size_ = le16toh(superblock_.s_desc_size);
    if (desc_size_ == 0) desc_size_ = 32;  // Legacy default

    // Fail-closed: reject geometry exceeding admitted source capacity
    uint64_t total_bytes = 0;
    if (!checked_multiply(total_blocks, result.block_size, total_bytes) ||
        total_bytes > source_size_) {
        result.errors.push_back("Invalid ext4 geometry: filesystem larger than admitted source");
        return result;
    }

    // Fail-closed: reject descriptor sizes outside valid range
    if (desc_size_ < 32 || desc_size_ > sizeof(ext4_group_desc)) {
        result.errors.push_back("Invalid ext4 geometry: descriptor size out of range");
        return result;
    }

    // Fail-closed: verify group descriptor table fits within admitted source
    const uint32_t gdb_block = (block_size_ == 1024) ? 2 : 1;
    uint64_t gdb_offset = 0;
    uint64_t gdb_span = 0;
    if (!checked_multiply(gdb_block, block_size_, gdb_offset) ||
        !checked_multiply(group_count_, desc_size_, gdb_span) ||
        !contains_range(source_size_, gdb_offset, gdb_span)) {
        result.errors.push_back("Invalid ext4 geometry: group descriptor table exceeds source");
        return result;
    }

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

    if (dbrecovery::innodb::g_verbose) 
    std::cout << "[DEBUG] Output directory created\n" << std::flush;

    // Create worker threads for parallel group scanning
    std::vector<std::thread> workers;
    if (dbrecovery::innodb::g_verbose)     std::cout << "[DEBUG] Creating " << config_.num_workers << " worker threads for " << result.group_count << " groups\n" << std::flush;

    for (uint32_t i = 0; i < config_.num_workers && i < result.group_count; ++i) {
        workers.emplace_back(&Ext4Recovery::group_worker_thread, this, i, progress);
    }

    if (dbrecovery::innodb::g_verbose) 
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
    if (dbrecovery::innodb::g_verbose)     std::cout << "[DEBUG] Worker " << worker_id << " starting\n" << std::flush;

    const int fd = duplicate_source_fd();
    if (fd < 0) {
        if (dbrecovery::innodb::g_verbose) {
            std::cout << "[DEBUG] Worker " << worker_id << " could not duplicate admitted source\n" << std::flush;
        }
        return;
    }

    // Use cached superblock values (thread-safe)
    uint32_t block_size = block_size_;
    uint32_t group_count = group_count_;
    uint32_t blocks_per_group = blocks_per_group_;
    uint32_t inodes_per_group = inodes_per_group_;
    uint16_t inode_size = inode_size_;
    uint16_t desc_size = desc_size_;

    if (dbrecovery::innodb::g_verbose) 
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
        if (config_.scan_freed_blocks || config_.scan_allocated_innodb_blocks) {
            std::vector<uint8_t> bitmap = read_block_bitmap(fd, group_index);
            if (!bitmap.empty()) {
                // Scan freed blocks
                if (config_.scan_freed_blocks) {
                    scan_freed_blocks(fd, group_index, bitmap);
                }
                // Scan allocated blocks for InnoDB INDEX pages (DELETE recovery)
                if (config_.scan_allocated_innodb_blocks) {
                    scan_allocated_blocks(fd, group_index, bitmap);
                }
            }
        }

        // Read inode bitmap and scan deleted inodes
        // This is now safer with bounds checking in inode_to_disk_offset
        if (config_.scan_deleted_inodes) {
            try {
                std::vector<uint8_t> inode_bitmap = read_inode_bitmap(fd, group_index);
                if (!inode_bitmap.empty()) {
                    // Scan deleted inodes (those with bit=0 in bitmap but have data)
                    scan_deleted_inodes_safe(fd, group_index, inode_bitmap);
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

void Ext4Recovery::scan_freed_blocks(int fd, uint32_t group_index, const std::vector<uint8_t>& bitmap) {
#ifdef __linux__
    if (fd < 0 || block_size_ == 0 || config_.page_size == 0 ||
        config_.page_size > source_size_) {
        return;
    }

    // Use cached values (thread-safe)
    uint32_t block_size = block_size_;
    uint32_t blocks_per_group = blocks_per_group_;
    uint32_t group_count = group_count_;

    // Calculate group start offset
    uint64_t group_start_block = static_cast<uint64_t>(group_index) * blocks_per_group;

    // Safety check: ensure we don't read beyond the admitted source
    const uint64_t max_offset = source_size_;

    // InnoDB pages span several ext4 blocks, but a file's first block can start
    // at any group block, so the physical page start is NOT a multiple of
    // blocks_per_page. Step by one filesystem block and accept only offsets
    // whose FIL header validates, otherwise pages whose start is off by one
    // block are silently skipped.
    uint32_t blocks_per_page = config_.page_size / block_size;
    if (blocks_per_page == 0) blocks_per_page = 16;

    uint32_t scan_step = 1;  // one filesystem block at a time
    uint32_t max_offsets = (blocks_per_group > 524288) ? 524288 : blocks_per_group;

    for (uint32_t blk = 0; blk < max_offsets; blk += scan_step) {
        // Only scan blocks that are freed (bit=0 in bitmap)
        uint32_t byte_idx = blk / 8;
        uint32_t bit_idx = blk % 8;
        if (byte_idx < bitmap.size() && (bitmap[byte_idx] & (1 << bit_idx)) != 0) {
            continue;  // Skip allocated blocks
        }

        uint64_t group_offset = 0;
        uint64_t blk_offset = 0;
        uint64_t disk_offset = 0;
        if (!checked_multiply(group_index, blocks_per_group, group_offset) ||
            !checked_add(group_offset, blk, group_offset) ||
            !checked_multiply(group_offset, block_size, disk_offset) ||
            !contains_range(max_offset, disk_offset, config_.page_size)) {
            break;
        }

        // Read full page (16KB)
        std::vector<uint8_t> page_data(config_.page_size);
        ssize_t bytes_read = pread(fd, page_data.data(), config_.page_size, disk_offset);

        if (bytes_read == static_cast<ssize_t>(config_.page_size)) {
            blocks_scanned_.fetch_add(1);

            // Check for InnoDB page type at offset 24
            if (page_data.size() >= 74) {
                uint16_t page_type = read_be<uint16_t>(page_data.data(), FIL_PAGE_TYPE_ABS);

                // Accept ALL valid InnoDB page types
                bool is_valid_type = false;
                switch (page_type) {
                    case 2:    // FIL_PAGE_INODE
                    case 5:    // FIL_PAGE_IBUF_BITMAP
                    case 8:    // FIL_PAGE_TYPE_FSP_HDR
                    case 11:   // FIL_PAGE_TYPE_XDES
                    case 0x45BD: // FIL_PAGE_TYPE_BLOB
                    case 0x45BF: // FIL_PAGE_INDEX
                    case 0x45BA: // FIL_PAGE_TYPE_ZBLOB (compressed BLOB)
                    case 0x45BB: // FIL_PAGE_TYPE_ZBLOB2 (compressed BLOB v2)
                    case 0x45B5: // FIL_PAGE_COMPRESSED
                    case 0x45B7: // FIL_PAGE_COMPRESSED_AND_ENCRYPTED
                        is_valid_type = true;
                        break;
                    default:
                        break;
                }

                if (is_valid_type) {
                    // Try to read index_id from page (offset 66 for INDEX pages)
                    // For non-INDEX pages, use page_no (offset 4) as identifier
                    uint64_t index_id = read_be<uint64_t>(page_data.data(), 66);
                    if (index_id == 0 || index_id > 1000000000000) {
                        // For non-INDEX pages, use space_id/page_no as identifier
                        uint32_t space_id = read_be<uint32_t>(page_data.data(), 34);
                        index_id = space_id > 0 ? space_id : page_type;
                    }

                    if (index_id > 0 && index_id < 1000000000000) {
                        // Filter system tables if configured
                        if (config_.exclude_system_tables) {
                            if ((index_id > 0 && index_id <= 100) ||
                                index_id >= 0xFFFF0000ULL) {
                                continue;
                            }
                        }
                        // Filter to only target index IDs if specified
                        if (!config_.target_index_ids.empty() &&
                            std::find(config_.target_index_ids.begin(),
                                      config_.target_index_ids.end(),
                                      index_id) == config_.target_index_ids.end()) {
                            continue;
                        }
                        Ext4BlockInfo block_info;
                        block_info.block_number = disk_offset / block_size;
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

#endif
}

// ==================== Allocated Block Scanning (DELETE recovery) ====================

void Ext4Recovery::scan_allocated_blocks(int fd, uint32_t group_index, const std::vector<uint8_t>& bitmap) {
#ifdef __linux__
    if (fd < 0 || block_size_ == 0 || config_.page_size == 0 ||
        config_.page_size > source_size_) {
        return;
    }

    // Use cached values (thread-safe)
    uint32_t block_size = block_size_;
    uint32_t blocks_per_group = blocks_per_group_;
    uint32_t group_count = group_count_;

    // Safety check: ensure we don't read beyond the admitted source
    const uint64_t max_offset = source_size_;

    // One filesystem block at a time: a tablespace does not necessarily start on
    // a page-size boundary inside its block group (see scan_freed_blocks).
    uint32_t blocks_per_page = config_.page_size / block_size;
    if (blocks_per_page == 0) blocks_per_page = 16;
    const uint32_t scan_step = 1;

    uint32_t max_offsets = (blocks_per_group > 524288) ? 524288 : blocks_per_group;

    for (uint32_t blk = 0; blk < max_offsets; blk += scan_step) {
        // Only scan blocks that are allocated (bit=1 in bitmap)
        uint32_t byte_idx = blk / 8;
        uint32_t bit_idx = blk % 8;
        if (byte_idx < bitmap.size() && (bitmap[byte_idx] & (1 << bit_idx)) == 0) {
            continue;  // Skip freed blocks
        }

        uint64_t group_offset = 0;
        uint64_t blk_offset = 0;
        uint64_t disk_offset = 0;
        if (!checked_multiply(group_index, blocks_per_group, group_offset) ||
            !checked_add(group_offset, blk, group_offset) ||
            !checked_multiply(group_offset, block_size, disk_offset) ||
            !contains_range(max_offset, disk_offset, config_.page_size)) {
            break;
        }

        // Read full page (16KB)
        std::vector<uint8_t> page_data(config_.page_size);
        ssize_t bytes_read = pread(fd, page_data.data(), config_.page_size, disk_offset);

        if (bytes_read == static_cast<ssize_t>(config_.page_size)) {
            blocks_scanned_.fetch_add(1);

            // Check for InnoDB page type at offset 24
            if (page_data.size() >= 74) {
                uint16_t page_type = read_be<uint16_t>(page_data.data(), FIL_PAGE_TYPE_ABS);

                // Only accept FIL_PAGE_INDEX (0x45BF) for allocated block scanning
                // Allocated blocks with other page types are not useful for DELETE recovery
                if (page_type == 0x45BF) {
                    uint64_t index_id = read_be<uint64_t>(page_data.data(), 66);

                    if (index_id > 0 && index_id < 1000000000000) {
                        // Filter system tables if configured
                        if (config_.exclude_system_tables) {
                            if ((index_id > 0 && index_id <= 100) ||
                                index_id >= 0xFFFF0000ULL) {
                                continue;
                            }
                        }
                        // Filter to only target index IDs if specified
                        if (!config_.target_index_ids.empty() &&
                            std::find(config_.target_index_ids.begin(),
                                      config_.target_index_ids.end(),
                                      index_id) == config_.target_index_ids.end()) {
                            continue;
                        }
                        Ext4BlockInfo block_info;
                        block_info.block_number = disk_offset / block_size;
                        block_info.disk_offset = disk_offset;
                        block_info.block_group = group_index;
                        block_info.is_freed = false;
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

#endif
}

// ==================== Safe Deleted Inode Scanning ====================

void Ext4Recovery::scan_deleted_inodes_safe(int fd, uint32_t group_index, const std::vector<uint8_t>& inode_bitmap) {
#ifdef __linux__
    if (fd < 0 || block_size_ == 0 || inode_size_ == 0 ||
        config_.page_size == 0) {
        return;
    }

    // Use cached values (thread-safe)
    uint32_t inodes_per_group = inodes_per_group_;
    uint16_t inode_size = inode_size_;
    uint32_t block_size = block_size_;

    // Safety checks
    if (inodes_per_group == 0 || inode_size == 0 || block_size == 0) {
        return;
    }

    // Calculate maximum disk offset for safety
    uint64_t max_offset = source_size_;

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
        uint64_t inode_offset = inode_to_disk_offset(fd, group_index, local_inode);
        if (inode_offset == 0 || inode_offset > max_offset ||
            inode_size > max_offset - inode_offset) {
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
            std::vector<ext4_extent> extents = parse_extents(fd, inode_raw, block_size);
            if (extents.empty() && file_size >= config_.page_size) {
                extents = traverse_extent_tree_from_inode(fd, inode_raw, block_size);
            }

            for (const auto& ext : extents) {
                uint32_t ext_len = le16toh(ext.ee_len);
                uint32_t ext_start_lo = le32toh(ext.ee_start_lo);
                uint16_t ext_start_hi = le16toh(ext.ee_start_hi);

                uint64_t ext_start = ext_start_lo | (static_cast<uint64_t>(ext_start_hi) << 32);

                if (ext_len == 0 || ext_start == 0) {
                    continue;
                }

                // Read at page-aligned offsets within this extent
                // InnoDB pages are 16KB-aligned within the file, but the file
                // itself may start at any 1024-byte block boundary in ext4.
                // We scan every page_size (16KB) offset within the extent.
                uint32_t ext_blocks = ext_len;
                uint32_t blocks_per_page = config_.page_size / block_size;
                if (blocks_per_page == 0) blocks_per_page = 16;

                for (uint32_t blk = 0; blk < ext_blocks && blk < 100000; blk += blocks_per_page) {
                    uint64_t block_num = ext_start + blk;
                    uint64_t disk_offset = 0;
                    if (!block_to_disk_offset_checked(block_num, block_size, max_offset, disk_offset) ||
                        !contains_range(max_offset, disk_offset, config_.page_size)) {
                        break;  // Safety check
                    }

                    std::vector<uint8_t> page_data(config_.page_size);
                    ssize_t page_read = pread(fd, page_data.data(), config_.page_size, disk_offset);

                    if (page_read == static_cast<ssize_t>(config_.page_size)) {
                        // Check for valid InnoDB page types
                        if (page_data.size() >= 74) {
                            uint16_t page_type = read_be<uint16_t>(page_data.data(), FIL_PAGE_TYPE_ABS);

                            bool is_valid_type = false;
                            switch (page_type) {
                                case 2:    // FIL_PAGE_INODE
                                case 5:    // FIL_PAGE_IBUF_BITMAP
                                case 8:    // FIL_PAGE_TYPE_FSP_HDR
                                case 11:   // FIL_PAGE_TYPE_XDES
                                case 0x45BD: // FIL_PAGE_TYPE_BLOB
                                case 0x45BF: // FIL_PAGE_INDEX
                                case 0x45BA: // FIL_PAGE_TYPE_ZBLOB
                                case 0x45BB: // FIL_PAGE_TYPE_ZBLOB2
                                    is_valid_type = true;
                                    break;
                                default:
                                    break;
                            }

                            if (is_valid_type) {
                                uint64_t index_id = read_be<uint64_t>(page_data.data(), 66);
                                if (index_id == 0 || index_id > 1000000000000) {
                                    uint32_t space_id = read_be<uint32_t>(page_data.data(), 34);
                                    index_id = space_id > 0 ? space_id : page_type;
                                }

                                if (index_id > 0 && index_id < 1000000000000) {
                                    // Filter system tables if configured
                                    if (config_.exclude_system_tables) {
                                        if ((index_id > 0 && index_id <= 100) ||
                                            index_id >= 0xFFFF0000ULL) {
                                            continue;
                                        }
                                    }
                                    // Filter to only target index IDs if specified
                                    if (!config_.target_index_ids.empty() &&
                                        std::find(config_.target_index_ids.begin(),
                                                  config_.target_index_ids.end(),
                                                  index_id) == config_.target_index_ids.end()) {
                                        continue;
                                    }
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

    bool use_schema = external_schema_.has_value() && !external_schema_->columns.empty();
    std::vector<dbrecovery::innodb::IndexColumnDef> index_columns;
    std::vector<std::string> schema_diagnostics;
    uint64_t interior_pages_skipped = 0;
    uint64_t checksum_rejected_pages = 0;

    if (use_schema && config_.target_index_ids.empty()) {
        schema_diagnostics.push_back(
            "External schema ignored: schema-aware recovery requires explicit target index IDs");
        use_schema = false;
    }

    if (use_schema) {
        std::vector<std::pair<uint32_t, catalog::ColumnInfo>> sorted_columns;
        sorted_columns.reserve(external_schema_->columns.size());
        for (const auto& [ordinal, column] : external_schema_->columns) {
            const uint32_t physical_pos = column.physical_pos > 0 ? column.physical_pos : ordinal;
            sorted_columns.push_back({physical_pos, column});
        }
        std::sort(sorted_columns.begin(), sorted_columns.end(),
                  [](const auto& left, const auto& right) { return left.first < right.first; });

        std::vector<catalog::IndexColumnBuildRequest> requests;
        requests.reserve(sorted_columns.size());
        for (const auto& [physical_pos, column] : sorted_columns) {
            requests.push_back({column, column.ordinal, physical_pos});
        }

        auto converted = catalog::build_index_columns(
            requests, catalog::TypeIdDomain::kInnodbParser);
        if (converted.ok()) {
            index_columns = std::move(converted.columns);
        } else {
            schema_diagnostics = std::move(converted.diagnostics);
            use_schema = false;
        }
    }

    result.errors.insert(result.errors.end(), schema_diagnostics.begin(), schema_diagnostics.end());

    if (use_schema && config_.verbose) {
        std::cout << "[INFO] Using schema-aware recovery with "
                  << index_columns.size() << " columns\n";
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

        const bool use_schema_for_index = use_schema &&
            std::find(config_.target_index_ids.begin(), config_.target_index_ids.end(), index_id) !=
                config_.target_index_ids.end();

        // A single page can be reachable twice on a raw source (live file plus
        // a write-ahead copy); byte-identical rows are collapsed so exported
        // row counts can match the source table.
        std::set<std::string> seen_rows;

        // Write header
        if (use_schema_for_index) {
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

        // Calibrate the checksum validator against this index's pages before
        // decoding any of them. See ChecksumGateBatch: rejecting on a
        // validator that does not model the input would delete every row of a
        // MySQL 5.6 tablespace, while rejecting an outlier on an input it does
        // model still stops one body-flipped page from emitting a fabricated
        // row.
        std::vector<uint8_t> checksum_fails(pages.size(), 0);
        dbrecovery::innodb::ChecksumGateBatch checksum_batch;
        if (use_schema_for_index) {
            for (size_t index = 0; index < pages.size(); ++index) {
                const auto& page = pages[index];
                if (page.data.size() < config_.page_size) continue;
                if (page.data.size() >= 66 && read_be<uint16_t>(page.data.data(), 64) != 0) continue;
                const bool page_valid = dbrecovery::innodb::validate_page_checksum(
                    page.data.data(), config_.page_size, /*strict=*/false).is_valid;
                checksum_batch.observe(page_valid);
                if (!page_valid) checksum_fails[index] = 1;
            }
        }

        // Parse each page and extract records
        for (size_t page_index = 0; page_index < pages.size(); ++page_index) {
            const auto& page = pages[page_index];
            if (page.data.size() < config_.page_size) continue;

            // With a declared schema only leaf pages (PAGE_LEVEL == 0) may be
            // decoded as rows. Interior pages hold node pointers, and decoding
            // them as user records produces high-confidence garbage rows.
            if (use_schema_for_index && page.data.size() >= 66) {
                const uint16_t page_level = read_be<uint16_t>(page.data.data(), 64);
                if (page_level != 0) {
                    interior_pages_skipped++;
                    continue;
                }
            }

            // Fail closed on a checksum mismatch that the batch calibration
            // proved meaningful: a page altered after it was written must not
            // contribute a row that never existed.
            if (use_schema_for_index && checksum_batch.should_reject(checksum_fails[page_index] == 0)) {
                checksum_rejected_pages++;
                continue;
            }

            try {
                dbrecovery::innodb::InnoDBIndexParser parser;
                parser.set_recover_deleted(true);
                parser.set_bad_page_mode(2);  // Try aggressive recovery for potentially corrupted pages
                uint32_t page_no = static_cast<uint32_t>(page.disk_offset / config_.page_size);

                // Detect MySQL version from page data
                dbrecovery::MySQLVersion version = dbrecovery::MySQLVersionDetector::detect(
                    page.data.data(), page.data.size());
                if (version != dbrecovery::MySQLVersion::UNKNOWN) {
                    uint32_t version_int = dbrecovery::mysql_version_to_number(version);
                    parser.set_mysql_version(version_int);
                } else {
                    // Default to MySQL 8.0 if version cannot be detected
                    parser.set_mysql_version(80000);
                }

                if (use_schema_for_index) {
                    parser.set_columns(index_columns);
                    // Compacting an allocated tablespace keeps history records in
                    // the page chain; they must not be exported as live rows.
                    parser.set_recover_deleted(false);
                    // Schema-aware export must only take rows proven by the on-page
                    // record chain; brute-force scans fabricate plausible rows.
                    parser.set_strict(true);
                    parser.set_target_index_id(index_id);
                }
                auto records = parser.parse_page(page.data.data(), page.data.size(), page_no);

                for (const auto& rec : records) {
                    // Skip empty records
                    if (rec.fields.empty() && rec.raw_data.empty()) continue;
                    // Skip records the parser itself marks as deleted history.
                    if (rec.is_deleted) continue;

                    if (use_schema_for_index && !rec.fields.empty()) {
                        // Schema-aware row: emit exactly one CSV field per declared
                        // column so the artifact always has index_columns.size()
                        // columns, and so embedded NUL bytes / commas cannot
                        // corrupt the textual layout.
                        std::string row;
                        for (size_t i = 0; i < index_columns.size(); i++) {
                            if (i > 0) row += ",";
                            if (i >= rec.fields.size()) {
                                continue;
                            }
                            const auto& field = rec.fields[i];
                            if (field.is_null) {
                                row += "NULL";
                            } else {
                                row += dbrecovery::exporter::csv_column_value(
                                    field, index_columns[i].is_binary);
                            }
                        }
                        if (seen_rows.insert(row).second) {
                            out << row << "\n";
                            total_records++;
                        }
                        continue;
                    }

                    bool first_field = true;
                    if (!rec.fields.empty()) {

                        // If we have parsed fields, output them
                        if (!rec.fields.empty()) {
                            for (const auto& field : rec.fields) {
                                if (!first_field) out << ",";
                                first_field = false;

                                if (field.is_null) {
                                    out << "NULL";
                                } else {
                                    out << dbrecovery::exporter::csv_column_value(field, false);
                                }
                            }
                        } else {
                            // No parsed fields: output raw_data as hex
                            out << "\"";
                            for (uint8_t byte : rec.raw_data) {
                                if (byte >= 32 && byte < 127 && byte != '"' && byte != '\\') {
                                    out << static_cast<char>(byte);
                                } else {
                                    out << '.';
                                }
                            }
                            out << "\"";
                        }
                        out << "\n";
                        total_records++;
                    }
                }
            } catch (const std::exception& e) {
                if (!use_schema_for_index) {
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
    result.checksum_rejected_pages = checksum_rejected_pages;
    result.success = true;

    if (config_.verbose) {
        std::cout << "[INFO] Exported " << total_records << " records to CSV files\n";
        if (interior_pages_skipped > 0) {
            std::cout << "[INFO] Skipped " << interior_pages_skipped
                      << " interior (non-leaf) index page(s) for schema-aware decoding\n";
        }
        if (checksum_rejected_pages > 0) {
            std::cout << "[INFO] Rejected " << checksum_rejected_pages
                      << " index page(s): InnoDB checksum does not match page contents\n";
        }
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
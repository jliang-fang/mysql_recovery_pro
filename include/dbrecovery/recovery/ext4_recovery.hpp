/**
 * @file ext4_recovery.hpp
 * @brief ext4 filesystem-specific recovery for MySQL/InnoDB data
 *
 * This module implements ext4-specific recovery techniques:
 * 1. Superblock detection and parsing
 * 2. Block group descriptor table scanning
 * 3. Block bitmap analysis for recently freed blocks
 * 4. Inode table scanning for deleted inodes
 *
 * Reference: ext4 filesystem structure documentation
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>
#include <map>
#include <optional>
#include <functional>
#include <mutex>
#include <atomic>
#include <queue>

#include "dbrecovery/catalog/schema_defs.hpp"

#ifdef __linux__
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace dbrecovery {
namespace recovery {
namespace ext4 {

// ext4 magic number
constexpr uint16_t EXT2_SUPER_MAGIC = 0xEF53;

// ext4 superblock offsets
constexpr uint64_t EXT4_SUPERBLOCK_OFFSET = 1024;  // 0x400

// ext4 inode types
constexpr uint16_t EXT4_S_IFREG = 0x8000;  // Regular file
constexpr uint16_t EXT4_S_IFDIR = 0x4000;  // Directory

// ==================== ext4 Superblock Structure ====================

struct ext4_superblock {
    uint32_t s_inodes_count;      // Total inodes count
    uint32_t s_blocks_count_lo;   // Total blocks count (low 32 bits)
    uint32_t s_r_blocks_count_lo; // Reserved blocks count (low 32 bits)
    uint32_t s_free_blocks_count_lo; // Free blocks count (low 32 bits)
    uint32_t s_free_inodes_count; // Free inodes count
    uint32_t s_first_data_block;  // First data block
    uint32_t s_log_block_size;    // Block size = 1024 << s_log_block_size
    uint32_t s_log_frag_size;     // Fragment size
    uint32_t s_blocks_per_group;  // Blocks per group
    uint32_t s_frags_per_group;   // Fragments per group
    uint32_t s_inodes_per_group;  // Inodes per group
    uint32_t s_mtime;             // Mount time
    uint32_t s_wtime;             // Write time
    uint16_t s_mnt_count;         // Mount count
    uint16_t s_max_mnt_count;     // Max mount count
    uint16_t s_magic;             // Magic signature (0xEF53)
    uint16_t s_state;             // File system state
    uint16_t s_errors;            // Behaviour when detecting errors
    uint16_t s_minor_rev_level;   // Minor revision level
    uint32_t s_lastcheck;         // Time of last check
    uint32_t s_checkinterval;     // Max. time between checks
    uint32_t s_creator_os;        // Creator OS
    uint32_t s_rev_level;         // Revision level
    uint16_t s_def_resuid;        // Default uid for reserved blocks
    uint16_t s_def_resgid;        // Default gid for reserved blocks
    // EXT4_DYNAMIC_REV specific fields
    uint32_t s_first_ino;         // First non-reserved inode
    uint16_t s_inode_size;        // Size of inode structure
    uint16_t s_block_group_nr;    // Block group number of this superblock
    uint32_t s_feature_compat;    // Compatible feature set
    uint32_t s_feature_incompat;  // Incompatible feature set
    uint32_t s_feature_ro_compat; // Read-only compatible feature set
    uint8_t  s_uuid[16];          // 128-bit UUID for volume
    char     s_volume_name[16];   // Volume name
    char     s_last_mounted[64];  // Directory where last mounted
    uint32_t s_algorithm_usage_bitmap; // For compression
    // Performance hints
    uint8_t  s_prealloc_blocks;   // Number of blocks to preallocate
    uint8_t  s_prealloc_dir_blocks; // Number to preallocate for dirs
    uint16_t s_reserved_gdt_blocks; // Per group desc table blocks
    // Journaling support
    uint32_t s_journal_uuid[4];   // UUID of journal superblock
    uint32_t s_journal_inum;      // Inode number of journal file
    uint32_t s_journal_dev;       // Device number of journal file
    uint32_t s_last_orphan;       // Head of list of inodes to delete
    // EXT4 specific fields
    uint32_t s_hash_seed[4];      // HTREE hash seed
    uint8_t  s_def_hash_version;  // Default hash version
    uint8_t  s_jnl_backup_type;   // Journal backup type
    uint16_t s_desc_size;         // Size of group descriptor
    uint32_t s_default_mount_opts; // Default mount options
    uint32_t s_first_meta_bg;     // First metablock block group
    uint32_t s_mkfs_time;         // When filesystem was created
    uint32_t s_jnl_blocks[17];    // Backup of journal superblock
    // 64-bit mode
    uint32_t s_blocks_count_hi;   // Blocks count high 32 bits
    uint32_t s_r_blocks_count_hi; // Reserved blocks count high 32 bits
    uint32_t s_free_blocks_count_hi; // Free blocks count high 32 bits
    uint16_t s_min_extra_isize;   // All inodes have at least this extra space
    uint16_t s_want_extra_isize;  // New inodes should reserve this extra space
    uint32_t s_flags;             // Miscellaneous flags
    uint16_t s_raid_stride;       // RAID stride
    uint16_t s_mmp_interval;      // MMP interval in seconds
    uint64_t s_mmp_block;         // MMP block number
    uint32_t s_raid_stripe_width; // RAID stripe width
    uint8_t  s_log_groups_per_flex; // Groups per flex group
    uint8_t  s_reserved_char_pad;
    uint16_t s_reserved_pad;
    uint32_t s_kbytes_written;    // Number of KB written
    uint32_t s_snapshot_inum;     // Inode number of active snapshot
    uint32_t s_snapshot_id;       // Sequential ID of active snapshot
    uint64_t s_snapshot_r_blocks_count; // Reserved blocks for snapshot
    uint32_t s_snapshot_list;     // Inode number of the head of snapshot list
    uint32_t s_error_count;       // Number of errors seen
    uint32_t s_first_error_time;  // First time an error happened
    uint32_t s_first_error_ino;   // Inode involved in first error
    uint64_t s_first_error_block; // Block involved of first error
    uint8_t  s_first_error_func[32]; // Function where error happened
    uint32_t s_first_error_line;  // Line where error happened
    uint32_t s_last_error_time;   // Most recent time of error
    uint32_t s_last_error_ino;    // Inode involved in last error
    uint64_t s_last_error_block;  // Block involved of last error
    uint8_t  s_last_error_func[32]; // Function where error happened
    uint32_t s_last_error_line;   // Line where error happened
    uint32_t s_mount_opts[8];     // Mount options
    uint32_t s_usr_quota_inum;    // Inode number of user quota file
    uint32_t s_grp_quota_inum;    // Inode number of group quota file
    uint32_t s_overhead_blocks;   // Overhead blocks count
    uint32_t s_backup_bgs[2];     // Backup block groups
    uint8_t  s_encrypt_algos[4];  // Encryption algorithms in use
    uint8_t  s_encrypt_pw_salt[16]; // Salt for password encryption
    uint32_t s_lpf_ino;           // Inode number of lost+found
    uint32_t s_prj_quota_inum;    // Inode number of project quota file
    uint32_t s_checksum_seed;     // Checksum seed
};

// ==================== ext4 Block Group Descriptor ====================

struct ext4_group_desc {
    uint32_t bg_block_bitmap_lo;     // Lower 32 bits of block bitmap block
    uint32_t bg_inode_bitmap_lo;     // Lower 32 bits of inode bitmap block
    uint32_t bg_inode_table_lo;      // Lower 32 bits of inode table start block
    uint16_t bg_free_blocks_count_lo; // Lower 16 bits of free blocks count
    uint16_t bg_free_inodes_count_lo; // Lower 16 bits of free inodes count
    uint16_t bg_used_dirs_count_lo;  // Lower 16 bits of used directories count
    uint16_t bg_flags;               // Block group flags
    uint32_t bg_exclude_bitmap_lo;   // Lower 32 bits of exclude bitmap
    uint16_t bg_block_bitmap_csum_lo; // Block bitmap checksum (lower)
    uint16_t bg_inode_bitmap_csum_lo; // Inode bitmap checksum (lower)
    uint16_t bg_itable_unused_lo;    // Lower 16 bits of unused inodes count
    uint16_t bg_checksum;            // Group descriptor checksum
    // 64-bit mode fields
    uint32_t bg_block_bitmap_hi;     // Higher 32 bits of block bitmap block
    uint32_t bg_inode_bitmap_hi;     // Higher 32 bits of inode bitmap block
    uint32_t bg_inode_table_hi;      // Higher 32 bits of inode table start block
    uint16_t bg_free_blocks_count_hi; // Higher 16 bits of free blocks count
    uint16_t bg_free_inodes_count_hi; // Higher 16 bits of free inodes count
    uint16_t bg_used_dirs_count_hi;  // Higher 16 bits of used directories count
    uint16_t bg_itable_unused_hi;    // Higher 16 bits of unused inodes count
    uint32_t bg_exclude_bitmap_hi;   // Higher 32 bits of exclude bitmap
    uint16_t bg_block_bitmap_csum_hi; // Block bitmap checksum (higher)
    uint16_t bg_inode_bitmap_csum_hi; // Inode bitmap checksum (higher)
    uint32_t bg_reserved;
};

// ==================== ext4 Inode Structure ====================

// On-disk ext4 inode (little-endian). Byte-for-byte identical to the kernel
// layout: i_block is 60 bytes (15 32-bit entries holding either direct block
// pointers or an inline extent tree), NOT 60 entries. The struct is padded to
// the 256-byte maximum inode size so that reads bounded by inode_size_ can
// never overrun the interpreted fields.
struct ext4_inode {
    uint16_t i_mode;                // 0   File mode
    uint16_t i_uid;                 // 2   Owner user ID (low 16 bits)
    uint32_t i_size_lo;             // 4   File size (low 32 bits)
    uint32_t i_atime;               // 8   Access time
    uint32_t i_ctime;               // 12  Creation time
    uint32_t i_mtime;               // 16  Modification time
    uint32_t i_dtime;               // 20  Deletion time
    uint16_t i_gid;                 // 24  Owner group ID (low 16 bits)
    uint16_t i_links_count;         // 26  Links count
    uint32_t i_blocks_lo;           // 28  Blocks count (low 32 bits)
    uint32_t i_flags;               // 32  Inode flags
    uint32_t i_osd1;                // 36  OS-dependent value
    uint32_t i_block[15];           // 40  Block pointers or inline extent tree (60 bytes)
    uint32_t i_generation;          // 100 File version (for NFS)
    uint32_t i_file_acl_lo;         // 104 Extended attribute block (low 32 bits)
    uint32_t i_size_hi;             // 108 File size (high 32 bits)
    uint32_t i_obso_faddr;          // 112 Obsolete fragment address
    uint16_t i_blocks_high;         // 116 Blocks count (high 16 bits)
    uint16_t i_file_acl_high;       // 118 Extended attribute block (high 16 bits)
    uint16_t i_uid_high;            // 120 Owner user ID (high 16 bits)
    uint16_t i_gid_high;            // 122 Owner group ID (high 16 bits)
    uint16_t i_checksum_lo;         // 124 Inode checksum (low 16 bits)
    uint16_t i_reserved;            // 126 Reserved
    uint16_t i_extra_isize;         // 128 Extra inode size
    uint16_t i_checksum_hi;         // 130 Inode checksum (high 16 bits)
    uint32_t i_ctime_extra;         // 132 Creation time (extra bits)
    uint32_t i_mtime_extra;         // 136 Modification time (extra bits)
    uint32_t i_atime_extra;         // 140 Access time (extra bits)
    uint32_t i_crtime;              // 144 Creation time (low 32 bits)
    uint32_t i_crtime_extra;        // 148 Creation time (extra bits)
    uint32_t i_version_hi;          // 152 File version (high 32 bits)
    uint32_t i_projid;              // 156 Project ID
    uint8_t  i_pad[96];             // 160..255 Pad to the 256-byte maximum inode size
};

static_assert(sizeof(ext4_inode) == 256,
              "ext4_inode must match the 256-byte on-disk inode image");
static_assert(offsetof(ext4_inode, i_block) == 40,
              "ext4 inode block map must begin at byte 40");
static_assert(offsetof(ext4_inode, i_generation) == 100,
              "ext4 inode generation must begin at byte 100");
static_assert(offsetof(ext4_inode, i_size_hi) == 108,
              "ext4 inode high size must begin at byte 108");

// ext4 extent structure
struct ext4_extent {
    uint32_t ee_block;    // First logical block
    uint16_t ee_len;      // Number of blocks
    uint16_t ee_start_hi; // High 16 bits of physical block
    uint32_t ee_start_lo; // Low 32 bits of physical block
};

// ext4 extent header
struct ext4_extent_header {
    uint16_t eh_magic;    // Magic number (0xF30A)
    uint16_t eh_entries;  // Number of entries
    uint16_t eh_max;      // Maximum number of entries
    uint16_t eh_depth;    // Depth of extent tree
    uint32_t eh_generation; // Generation number
};

// ==================== ext4 Recovery Configuration ====================

struct Ext4Config {
    std::string device_path;              // Block device or image file
    std::string output_dir;               // Output directory for recovered files
    uint32_t page_size = 16384;           // InnoDB page size (16KB default)
    uint32_t num_workers = 8;             // Worker threads
    bool scan_freed_blocks = true;        // Scan recently freed blocks
    bool scan_deleted_inodes = true;      // Scan deleted inode slots
    bool scan_allocated_innodb_blocks = false; // Scan allocated blocks for InnoDB INDEX pages (for DELETE recovery)
    bool verbose = false;
    uint64_t progress_interval_ms = 500;
    std::vector<uint64_t> target_index_ids;
    std::vector<uint32_t> target_space_ids;
    bool exclude_system_tables = false;
};

// ==================== ext4 Recovery Result ====================

struct Ext4BlockInfo {
    uint64_t block_number;
    uint64_t disk_offset;
    uint32_t block_group;
    bool is_freed;
    std::vector<uint8_t> data;
};

struct Ext4InodeInfo {
    uint64_t inode_number;
    uint32_t block_group;
    uint64_t file_size;
    uint32_t dtime;  // Deletion time
    std::vector<ext4_extent> extents;
    bool is_deleted;
};

struct Ext4ScanResult {
    bool success = false;
    std::string filesystem_uuid;
    uint32_t block_size = 0;
    uint32_t inode_size = 0;
    uint32_t blocks_per_group = 0;
    uint32_t inodes_per_group = 0;
    uint32_t group_count = 0;

    uint64_t total_blocks_scanned = 0;
    uint64_t freed_blocks_found = 0;
    uint64_t deleted_inodes_found = 0;
    uint64_t pages_recovered = 0;
    uint64_t total_records = 0;
    /// Index pages rejected before structured decoding because their stored
    /// InnoDB checksum does not match the page contents.  A page in this state
    /// cannot support row-level claims: decoding it produced a fabricated row.
    uint64_t checksum_rejected_pages = 0;
    /// Rows refused because a LOB column still pointed off-page and the
    /// external chain was never reassembled.  These rows are recorded as
    /// unrecoverable rather than exported with pointer bytes as data.
    uint64_t overflow_records_dropped = 0;

    std::map<uint64_t, std::vector<Ext4BlockInfo>> pages_by_index_id;
    std::map<uint32_t, std::vector<uint8_t>> blob_pages_by_page_no;
    std::vector<Ext4InodeInfo> recovered_inodes;
    std::vector<std::string> errors;

    double elapsed_seconds = 0;
};

// ==================== Ext4 Recovery Class ====================

class Ext4Recovery {
public:
    Ext4Recovery();
    ~Ext4Recovery();

    Ext4Recovery(const Ext4Recovery&) = delete;
    Ext4Recovery& operator=(const Ext4Recovery&) = delete;

    bool adopt_fd(int owned_fd, uint64_t known_size);

    void set_config(const Ext4Config& config);

    // Set external schema for schema-aware recovery
    void set_external_schema(const catalog::TableSchema& schema);

    // Check if device has valid ext4 superblock
    static bool is_ext4_device(const std::string& device_path);

    // Read ext4 superblock
    static bool read_superblock(const std::string& device_path, ext4_superblock& sb);

    // Scan ext4 filesystem for deleted MySQL data
    Ext4ScanResult scan(std::function<void(double, const std::string&)> progress = nullptr);

    // Recover data from scan result
    Ext4ScanResult recover(const Ext4ScanResult& scan_result,
                          std::function<void(double, const std::string&)> progress = nullptr);

    // Combined scan and recover
    Ext4ScanResult scan_and_recover(std::function<void(double, const std::string&)> progress = nullptr);

private:
    Ext4Config config_;
    int source_fd_ = -1;
    uint64_t source_size_ = 0;
    ext4_superblock superblock_;
    Ext4ScanResult last_result_;
    std::optional<catalog::TableSchema> external_schema_;  // External schema for schema-aware recovery

    // Cached superblock values for thread-safe access
    uint32_t group_count_ = 0;
    uint32_t block_size_ = 4096;
    uint32_t blocks_per_group_ = 0;
    uint32_t inodes_per_group_ = 0;
    uint16_t inode_size_ = 256;
    uint16_t desc_size_ = 32;

    std::atomic<uint64_t> current_group_{0};
    std::atomic<uint64_t> blocks_scanned_{0};
    std::atomic<uint64_t> blocks_found_{0};
    std::atomic<bool> should_stop_{false};
    std::mutex result_mutex_;

    // Thread-safe result collection queue
    std::queue<std::pair<uint64_t, Ext4BlockInfo>> result_queue_;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> blob_queue_;

    // Read block group descriptor
    bool read_group_descriptor(int fd, uint32_t group_index, ext4_group_desc& desc) const;

    // Read block bitmap for a group
    std::vector<uint8_t> read_block_bitmap(int fd, uint32_t group_index);

    // Read inode bitmap for a group
    std::vector<uint8_t> read_inode_bitmap(int fd, uint32_t group_index);

    /**
     * @brief Check if a block is free in the block bitmap
     * @param bitmap Block bitmap for the group
     * @param block_offset Block offset within the group
     * @return true if block is free (bit is 0)
     */
    static bool is_block_free(const std::vector<uint8_t>& bitmap, uint32_t block_offset) {
        if (bitmap.empty() || block_offset >= bitmap.size() * 8) {
            return false;  // Out of bounds - treat as used
        }
        uint32_t byte_index = block_offset / 8;
        uint32_t bit_index = block_offset % 8;
        // Bit is 0 = free, Bit is 1 = used
        return (bitmap[byte_index] & (1 << bit_index)) == 0;
    }

    /**
     * @brief Check if a block is used in the block bitmap
     * @param bitmap Block bitmap for the group
     * @param block_offset Block offset within the group
     * @return true if block is used (bit is 1)
     */
    static bool is_block_used(const std::vector<uint8_t>& bitmap, uint32_t block_offset) {
        return !is_block_free(bitmap, block_offset);
    }

    /**
     * @brief Count free blocks in a bitmap
     * @param bitmap Block bitmap for the group
     * @param blocks_per_group Number of blocks in the group
     * @return Number of free blocks
     */
    static uint32_t count_free_blocks(const std::vector<uint8_t>& bitmap, uint32_t blocks_per_group) {
        uint32_t free_count = 0;
        uint32_t blocks_to_check = std::min(blocks_per_group, static_cast<uint32_t>(bitmap.size() * 8));
        for (uint32_t i = 0; i < blocks_to_check; i++) {
            if (is_block_free(bitmap, i)) {
                free_count++;
            }
        }
        return free_count;
    }

    // Read inode from inode table
    std::optional<Ext4InodeInfo> read_inode(int fd, uint32_t group_index, uint32_t local_inode);

    // Parse extents from inode (fd and block_size needed for legacy indirect block mapping)
    std::vector<ext4_extent> parse_extents(int fd, const ext4_inode& inode, uint32_t block_size);

    // Traverse multi-level extent tree (for large files)
    std::vector<ext4_extent> traverse_extent_tree(int fd, uint64_t root_block, uint32_t block_size, int max_depth);

    // Traverse extent tree starting from inode's i_block (handles first level in inode)
    std::vector<ext4_extent> traverse_extent_tree_from_inode(int fd, const ext4_inode& inode, uint32_t block_size);

    // Scan freed blocks in block bitmap
    void scan_freed_blocks(int fd, uint32_t group_index, const std::vector<uint8_t>& bitmap);

    // Scan allocated blocks for InnoDB INDEX pages (for DELETE recovery)
    void scan_allocated_blocks(int fd, uint32_t group_index, const std::vector<uint8_t>& bitmap);

    // Safe inode scanning with bounds checking
    void scan_deleted_inodes_safe(int fd, uint32_t group_index,
                                  const std::vector<uint8_t>& inode_bitmap);

    // Worker thread for parallel group scanning
    void group_worker_thread(uint32_t worker_id,
                             std::function<void(double, const std::string&)> progress);

    // Calculate disk offset from block number
    uint64_t block_to_disk_offset(uint64_t block) const;

    // Calculate disk offset for inode
    uint64_t inode_to_disk_offset(int fd, uint32_t group_index, uint32_t local_inode) const;

    int duplicate_source_fd() const;
};

} // namespace ext4
} // namespace recovery
} // namespace dbrecovery
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
 * Reference: testdisk ext2.c, ext2p.c implementations
 * Reference: ext4 filesystem structure documentation
 */

#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <map>
#include <optional>
#include <functional>
#include <mutex>
#include <atomic>

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

struct ext4_inode {
    uint16_t i_mode;                // File mode
    uint16_t i_uid;                 // Owner user ID (low 16 bits)
    uint32_t i_size_lo;             // File size (low 32 bits)
    uint32_t i_atime;               // Access time
    uint32_t i_ctime;               // Creation time
    uint32_t i_mtime;               // Modification time
    uint32_t i_dtime;               // Deletion time
    uint16_t i_gid;                 // Owner group ID (low 16 bits)
    uint16_t i_links_count;         // Links count
    uint32_t i_blocks_lo;           // Blocks count (low 32 bits)
    uint32_t i_flags;               // Inode flags
    uint32_t i_reserved1;
    uint16_t i_block[60];           // Block pointers or extent tree
    uint32_t i_generation;          // File version (for NFS)
    uint32_t i_file_acl_lo;         // Extended attribute block (low 32 bits)
    uint32_t i_size_hi;             // File size (high 32 bits)
    uint32_t i_obso_faddr;          // Obsolete fragment address
    uint16_t i_uid_high;            // Owner user ID (high 16 bits)
    uint16_t i_gid_high;            // Owner group ID (high 16 bits)
    uint16_t i_checksum_lo;         // Inode checksum (low 16 bits)
    uint16_t i_reserved2;
    uint16_t i_extra_isize;         // Extra inode size
    uint16_t i_checksum_hi;         // Inode checksum (high 16 bits)
    uint32_t i_ctime_extra;         // Creation time (extra bits)
    uint32_t i_mtime_extra;         // Modification time (extra bits)
    uint32_t i_atime_extra;         // Access time (extra bits)
    uint32_t i_crtime;              // Creation time (low 32 bits)
    uint32_t i_crtime_extra;        // Creation time (extra bits)
    uint32_t i_version_hi;          // File version (high 32 bits)
    uint32_t i_projid;              // Project ID
};

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
    bool verbose = false;
    uint64_t progress_interval_ms = 500;
    std::vector<uint64_t> target_index_ids;
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

    std::map<uint64_t, std::vector<Ext4BlockInfo>> pages_by_index_id;
    std::vector<Ext4InodeInfo> recovered_inodes;
    std::vector<std::string> errors;

    double elapsed_seconds = 0;
};

// ==================== Ext4 Recovery Class ====================

class Ext4Recovery {
public:
    Ext4Recovery();
    ~Ext4Recovery();

    void set_config(const Ext4Config& config);

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
    ext4_superblock superblock_;
    Ext4ScanResult last_result_;

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

    // Read block group descriptor
    bool read_group_descriptor(uint32_t group_index, ext4_group_desc& desc) const;

    // Read block bitmap for a group
    std::vector<uint8_t> read_block_bitmap(uint32_t group_index);

    // Read inode bitmap for a group
    std::vector<uint8_t> read_inode_bitmap(uint32_t group_index);

    // Read inode from inode table
    std::optional<Ext4InodeInfo> read_inode(uint32_t group_index, uint32_t local_inode);

    // Parse extents from inode
    std::vector<ext4_extent> parse_extents(const ext4_inode& inode);

    // Scan freed blocks in block bitmap
    void scan_freed_blocks(uint32_t group_index, const std::vector<uint8_t>& bitmap);

    // Worker thread for parallel group scanning
    void group_worker_thread(uint32_t worker_id,
                             std::function<void(double, const std::string&)> progress);

    // Calculate disk offset from block number
    uint64_t block_to_disk_offset(uint64_t block) const;

    // Calculate disk offset for inode
    uint64_t inode_to_disk_offset(uint32_t group_index, uint32_t local_inode) const;
};

} // namespace ext4
} // namespace recovery
} // namespace dbrecovery
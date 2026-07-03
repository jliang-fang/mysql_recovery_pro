/**
 * @file xfs_disk_carver.hpp
 * @brief XFS filesystem disk carving for MySQL/InnoDB data recovery
 *
 * This module implements XFS-specific disk carving techniques for recovering
 * deleted MySQL data files (.ibd, .frm) after rm -rf, DROP TABLE, TRUNCATE.
 *
 * XFS recovery strategies:
 * 1. AG (Allocation Group) scanning - XFS divides disk into multiple AGs
 * 2. Free inode B+tree traversal - find deleted inodes with recoverable extents
 * 3. Extent list reconstruction - read extent data from deleted inode memory
 * 4. Direct block scanning - fallback signature-based carving
 *
 * Reference: XFS Algorithms & Data Structures
 * https://mirrors.edge.kernel.org/pub/linux/utils/fs/xfs/docs/xfs_filesystem_structure.pdf
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
#include <thread>
#include <fstream>
#include <cstring>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <iostream>

#ifdef __linux__
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/fs.h>
#include <endian.h>
#endif

#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace disk {
namespace xfs {

// ==================== XFS Type Definitions ====================

// Basic XFS types (matching on-disk structures)
typedef uint64_t xfs_fsblock_t;   // blockno in filesystem (agno|agbno)
typedef uint64_t xfs_rfsblock_t;  // blockno in filesystem (raw)
typedef uint64_t xfs_fileoff_t;   // block number in a file
typedef uint64_t xfs_filblks_t;   // number of blocks in a file
typedef uint64_t xfs_ino_t;       // inode number
typedef uint32_t xfs_agblock_t;   // blockno in allocation group
typedef uint32_t xfs_agino_t;     // inode number within AG
typedef uint32_t xfs_extlen_t;    // extent length in blocks
typedef uint32_t xfs_agnumber_t;  // allocation group number
typedef uint64_t xfs_lsn_t;       // log sequence number

// UUID type
typedef unsigned char uuid_t[16];

// Timestamp structure
struct xfs_timestamp {
    uint32_t t_sec;   // seconds
    uint32_t t_nsec;  // nanoseconds
};

// ==================== XFS Magic Numbers ====================

constexpr uint32_t XFS_SB_MAGIC        = 0x58465342; // 'XFSB' - Superblock
constexpr uint32_t XFS_AGF_MAGIC       = 0x58414746; // 'XAGF' - AG Free Space
constexpr uint32_t XFS_AGI_MAGIC       = 0x58414749; // 'XAGI' - AG Inode Management
constexpr uint32_t XFS_AGFL_MAGIC      = 0x5841464C; // 'XAGL' - AG Free List
constexpr uint16_t XFS_DINODE_MAGIC    = 0x494E;     // 'IN'   - Inode

// B+tree magic numbers
constexpr uint32_t XFS_IBT_MAGIC       = 0x49414254; // 'IABT' - Inode Btree
constexpr uint32_t XFS_IBT_CRC_MAGIC   = 0x49414233; // 'IAB3' - Inode Btree (CRC)
constexpr uint32_t XFS_FIBT_MAGIC      = 0x46494254; // 'FIBT' - Free Inode Btree
constexpr uint32_t XFS_FIBT_CRC_MAGIC  = 0x46494233; // 'FIB3' - Free Inode Btree (CRC)
constexpr uint32_t XFS_ABTB_MAGIC      = 0x41425442; // 'ABTB' - Alloc Btree (by block)
constexpr uint32_t XFS_ABTB_CRC_MAGIC  = 0x41423342; // 'AB3B' - Alloc Btree (CRC)
constexpr uint32_t XFS_ABTC_MAGIC      = 0x41425443; // 'ABTC' - Alloc Btree (by count)
constexpr uint32_t XFS_ABTC_CRC_MAGIC  = 0x41423343; // 'AB3C' - Alloc Btree (CRC)

// ==================== XFS Superblock Structure ====================

struct xfs_superblock {
    uint32_t sb_magicnum;          // magic number XFS_SB_MAGIC
    uint32_t sb_blocksize;         // logical block size (bytes)
    uint64_t sb_dblocks;           // number of data blocks
    uint64_t sb_rblocks;           // number of realtime blocks
    uint64_t sb_rextents;          // number of realtime extents
    uuid_t   sb_uuid;              // filesystem UUID
    uint64_t sb_logstart;          // starting block of log
    uint64_t sb_rootino;           // root inode number
    uint64_t sb_rbmino;            // realtime bitmap inode
    uint64_t sb_rsumino;           // realtime summary inode
    uint32_t sb_rextsize;          // realtime extent size
    uint32_t sb_agblocks;          // size of an AG (blocks)
    uint32_t sb_agcount;           // number of AGs
    uint32_t sb_rbmblocks;         // realtime bitmap blocks
    uint32_t sb_logblocks;         // log blocks
    uint16_t sb_versionnum;        // version
    uint16_t sb_sectsize;          // sector size
    uint16_t sb_inodesize;         // inode size
    uint16_t sb_inopblock;         // inodes per block
    char     sb_fname[12];         // filesystem name
    uint8_t  sb_blocklog;          // log2(blocksize)
    uint8_t  sb_sectlog;           // log2(sectsize)
    uint8_t  sb_inodelog;          // log2(inodesize)
    uint8_t  sb_inopblog;          // log2(inopblock)
    uint8_t  sb_agblklog;          // log2(agblocks)
    uint8_t  sb_rextslog;          // log2(rextents)
    uint8_t  sb_inprogress;        // mkfs in progress
    uint8_t  sb_imax_pct;          // max inode space %
    uint64_t sb_icount;            // allocated inodes
    uint64_t sb_ifree;             // free inodes
    uint64_t sb_fdblocks;          // free data blocks
    uint64_t sb_frextents;         // free realtime extents
    uint64_t sb_uquotino;          // user quota inode
    uint64_t sb_gquotino;          // group quota inode
    uint16_t sb_qflags;            // quota flags
    uint8_t  sb_flags;             // misc flags
    uint8_t  sb_shared_vn;         // shared version
    uint32_t sb_inoalignmt;        // inode chunk alignment
    uint32_t sb_unit;              // stripe unit
    uint32_t sb_width;             // stripe width
    uint8_t  sb_dirblklog;         // log2(dir block size)
    uint8_t  sb_logsectlog;        // log2(log sector size)
    uint16_t sb_logsectsize;       // log sector size
    uint32_t sb_logsunit;          // log stripe unit
    uint32_t sb_features2;         // additional features
    uint32_t sb_bad_features2;     // bad features2
    // Version 5 fields
    uint32_t sb_features_compat;
    uint32_t sb_features_ro_compat;
    uint32_t sb_features_incompat;
    uint32_t sb_features_log_incompat;
    uint32_t sb_crc;               // superblock CRC
    uint32_t sb_spino_align;       // sparse inode alignment
    uint64_t sb_pquotino;          // project quota inode
    uint64_t sb_lsn;               // last write sequence
    uuid_t   sb_meta_uuid;         // metadata UUID
};

// ==================== XFS AGI (AG Inode Management) ====================

constexpr uint64_t XFS_AGI_UNLINKED_BUCKETS = 64;

struct xfs_agi {
    uint32_t agi_magicnum;         // XFS_AGI_MAGIC
    uint32_t agi_versionnum;       // version
    uint32_t agi_seqno;            // AG sequence number
    uint32_t agi_length;           // AG size (blocks)
    uint32_t agi_count;            // allocated inodes
    uint32_t agi_root;             // inode btree root
    uint32_t agi_level;            // inode btree levels
    uint32_t agi_freecount;        // free inode count
    uint32_t agi_newino;           // new inode
    uint32_t agi_dirino;           // last directory inode chunk
    uint32_t agi_unlinked[64];     // unlinked inode hash table
    uuid_t   agi_uuid;             // filesystem UUID
    uint32_t agi_crc;              // CRC
    uint32_t agi_pad32;
    uint64_t agi_lsn;              // last write sequence
    uint32_t agi_free_root;        // free inode btree root
    uint32_t agi_free_level;       // free inode btree levels
};

// ==================== XFS AGF (AG Free Space) ====================

enum xfs_btnum {
    XFS_BTNUM_BNOi = 0,  // free space by block number
    XFS_BTNUM_CNTi = 1,  // free space by count
    XFS_BTNUM_RMAPi = 2, // reverse map
    XFS_BTNUM_BMAPi = 3, // block map
    XFS_BTNUM_INOi  = 4, // inode btree
    XFS_BTNUM_FINOi = 5, // free inode btree
    XFS_BTNUM_REFCi = 6, // refcount
    XFS_BTNUM_MAX
};

struct xfs_agf {
    uint32_t agf_magicnum;         // XFS_AGF_MAGIC
    uint32_t agf_versionnum;       // version
    uint32_t agf_seqno;            // AG sequence number
    uint32_t agf_length;           // AG size (blocks)
    uint32_t agf_roots[3];         // btree roots (BNO, CNT, RMAP)
    uint32_t agf_levels[3];        // btree levels
    uint32_t agf_flfirst;          // freelist first index
    uint32_t agf_fllast;           // freelist last index
    uint32_t agf_flcount;          // freelist count
    uint32_t agf_freeblks;         // total free blocks
    uint32_t agf_longest;          // longest free space
    uint32_t agf_btreeblks;        // btree blocks
    uuid_t   agf_uuid;             // filesystem UUID
    uint32_t agf_rmap_blocks;
    uint32_t agf_refcount_blocks;
    uint32_t agf_refcount_root;
    uint32_t agf_refcount_level;
    uint64_t agf_spare64[14];
    uint64_t agf_lsn;              // last write sequence
    uint32_t agf_crc;              // CRC
    uint32_t agf_spare2;
};

// ==================== XFS Inode Structure ====================

enum xfs_dinode_fmt {
    XFS_DINODE_FMT_DEV     = 0,  // device
    XFS_DINODE_FMT_LOCAL   = 1,  // bulk data (symlinks)
    XFS_DINODE_FMT_EXTENTS = 2,  // extent list
    XFS_DINODE_FMT_BTREE   = 3,  // extent btree
    XFS_DINODE_FMT_UUID    = 4   // unused
};

enum xfs_exntst {
    XFS_EXT_NORM     = 0,  // normal extent
    XFS_EXT_UNWRITTEN = 1  // preallocated, not written
};

struct xfs_dinode_core {
    uint16_t di_magic;            // XFS_DINODE_MAGIC
    uint16_t di_mode;             // mode and file type
    uint8_t  di_version;          // inode version (3 for v5)
    uint8_t  di_format;           // data fork format
    uint16_t di_onlink;           // old link count
    uint32_t di_uid;              // owner uid
    uint32_t di_gid;              // owner gid
    uint32_t di_nlink;            // link count
    uint16_t di_projid_lo;        // project id (low)
    uint16_t di_projid_hi;        // project id (high)
    uint8_t  di_pad[6];           // padding
    uint16_t di_flushiter;        // flush iteration
    xfs_timestamp di_atime;       // access time
    xfs_timestamp di_mtime;       // modify time
    xfs_timestamp di_ctime;       // change time
    uint64_t di_size;             // file size
    uint64_t di_nblocks;          // block count
    uint32_t di_extsize;          // extent size hint
    uint32_t di_nextents;         // data extent count
    uint16_t di_anextents;        // attr extent count
    uint8_t  di_forkoff;          // attr fork offset
    uint8_t  di_aformat;          // attr fork format
    uint32_t di_dmevmask;         // DMIG event mask
    uint16_t di_dmstate;          // DMIG state
    uint16_t di_flags;            // flags
    uint32_t di_gen;              // generation
    uint32_t di_next_unlinked;    // next unlinked inode
    // Version 5 fields
    uint32_t di_crc;              // CRC
    uint64_t di_changecount;      // attribute change count
    uint64_t di_lsn;              // flush sequence
    uint64_t di_flags2;           // more flags
    uint32_t di_cowextsize;       // cow extent size
    uint8_t  di_pad2[12];         // padding
    xfs_timestamp di_crtime;      // creation time
    uint64_t di_ino;              // inode number
    uuid_t   di_uuid;             // filesystem UUID
};

// ==================== XFS B+tree Block Structure ====================

struct xfs_btree_block_shdr {
    uint32_t bb_leftsib;
    uint32_t bb_rightsib;
    uint64_t bb_blkno;
    uint64_t bb_lsn;
    uuid_t   bb_uuid;
    uint32_t bb_owner;
    uint32_t bb_crc;  // little-endian CRC
};

struct xfs_btree_block_lhdr {
    uint64_t bb_leftsib;
    uint64_t bb_rightsib;
    uint64_t bb_blkno;
    uint64_t bb_lsn;
    uuid_t   bb_uuid;
    uint64_t bb_owner;
    uint32_t bb_crc;  // little-endian CRC
    uint32_t bb_pad;
};

struct xfs_btree_block {
    uint32_t bb_magic;    // magic number
    uint16_t bb_level;    // 0 = leaf
    uint16_t bb_numrecs;  // record count
    // followed by short or long header
};

// ==================== XFS Inode Btree Record ====================

struct xfs_inobt_rec {
    uint32_t ir_startino;    // starting inode number
    uint16_t ir_holemask;    // hole mask for sparse chunks
    uint8_t  ir_count;       // total inode count
    uint8_t  ir_freecount;   // free inode count
    uint64_t ir_free;        // free inode mask (64 bits for 64 inodes)
};

// ==================== XFS Extent Structure ====================

// Bmap btree record (extent descriptor)
struct xfs_bmbt_rec {
    uint64_t l0;  // packed extent info part 1
    uint64_t l1;  // packed extent info part 2
};

// Parsed extent
struct xfs_extent {
    uint64_t file_offset;   // starting file offset (blocks)
    uint64_t block_offset;  // starting block (filesystem absolute)
    uint64_t block_count;   // number of blocks
    uint8_t  state;         // XFS_EXT_NORM or XFS_EXT_UNWRITTEN
};

// ==================== XFS Alloc Btree Record ====================

struct xfs_alloc_rec {
    uint32_t ar_startblock;  // starting block
    uint32_t ar_blockcount;  // block count
};

// ==================== XFS Disk Carver Configuration ====================

struct XfsConfig {
    std::string device_path;              // block device or image file
    std::string output_dir;               // output directory for recovered files
    uint32_t    page_size = 16384;        // InnoDB page size (16KB default)
    uint32_t    num_workers = 8;          // worker threads
    bool        scan_deleted_inodes = true;  // scan free inode btree
    bool        scan_free_space = true;      // scan free space btree
    bool        signature_scan = true;       // fallback signature scan
    bool        verbose = false;
    bool        recover_deleted = true;      // recover deleted records
    uint64_t    progress_interval_ms = 500;  // progress update interval
};

// ==================== XFS Disk Carver Result ====================

struct XfsExtentInfo {
    uint64_t inode_number;
    uint64_t file_size;
    std::vector<xfs_extent> extents;
    xfs_timestamp mtime;
    xfs_timestamp ctime;
    bool is_deleted;
};

struct XfsPageInfo {
    uint64_t disk_offset;        // absolute disk offset
    uint64_t inode_number;       // source inode (if known)
    uint32_t page_no;            // InnoDB page number
    uint64_t index_id;           // InnoDB index ID
    uint16_t page_type;          // InnoDB page type
    std::vector<uint8_t> data;   // page data
};

struct XfsScanResult {
    bool success = false;
    std::string filesystem_uuid;
    uint32_t ag_count = 0;
    uint32_t block_size = 0;
    uint32_t inode_size = 0;
    uint32_t ag_blocks = 0;

    uint64_t total_inodes_scanned = 0;
    uint64_t deleted_inodes_found = 0;
    uint64_t valid_extents_found = 0;
    uint64_t pages_recovered = 0;
    uint64_t total_records = 0;

    std::map<uint64_t, std::vector<XfsPageInfo>> pages_by_index_id;
    std::vector<XfsExtentInfo> recovered_extents;
    std::map<uint64_t, std::vector<std::map<std::string, std::string>>> records_by_index_id;
    std::vector<std::string> output_files;
    std::vector<std::string> errors;

    double elapsed_seconds = 0;
};

// ==================== XFS Disk Carver Class ====================

class XfsDiskCarver {
public:
    XfsDiskCarver();
    ~XfsDiskCarver();

    void set_config(const XfsConfig& config);

    // Set external schema for schema-aware recovery
    void set_external_schema(const catalog::TableSchema& schema);

    // Scan XFS filesystem for deleted MySQL data
    XfsScanResult scan(std::function<void(double, const std::string&)> progress = nullptr);

    // Recover data from scan result
    XfsScanResult recover(const XfsScanResult& scan_result,
                          std::function<void(double, const std::string&)> progress = nullptr);

    // Combined scan and recover
    XfsScanResult scan_and_recover(std::function<void(double, const std::string&)> progress = nullptr);

    // Check if device has valid XFS superblock
    static bool is_xfs_device(const std::string& device_path);

    // Read XFS superblock
    static bool read_superblock(const std::string& device_path, xfs_superblock& sb);

    // Parse extent from bmbt record
    static xfs_extent parse_extent(const xfs_bmbt_rec& rec);

    // Validate extent against free space btree
    bool validate_extent(const xfs_extent& ext, uint32_t ag_index);

private:
    XfsConfig config_;
    xfs_superblock superblock_;
    XfsScanResult last_result_;
    std::optional<catalog::TableSchema> external_schema_;

    std::atomic<uint64_t> current_ag_{0};
    std::atomic<uint64_t> inodes_scanned_{0};
    std::atomic<uint64_t> inodes_found_{0};
    std::atomic<bool> should_stop_{false};
    std::mutex result_mutex_;

    // Read AG headers (AGI, AGF)
    bool read_ag_headers(uint32_t ag_index, xfs_agi& agi, xfs_agf& agf);

    // Walk inode btree for deleted inodes
    void walk_inode_btree(uint32_t ag_index, const xfs_agi& agi, const xfs_agf& agf);

    // Read inode and extract extents
    std::optional<XfsExtentInfo> read_inode(uint32_t ag_index, uint32_t ag_inode);

    // Walk free space btree (for extent validation)
    void walk_free_space_btree(uint32_t ag_index, const xfs_agf& agf);

    // Direct signature scan fallback
    void signature_scan(std::function<void(double, const std::string&)> progress);

    // Read data from extent
    std::vector<uint8_t> read_extent_data(const xfs_extent& ext);

    // Calculate disk offset from filesystem block
    uint64_t block_to_disk_offset(uint64_t fsblock) const;

    // Worker thread for parallel AG scanning
    void ag_worker_thread(uint32_t worker_id,
                          std::function<void(double, const std::string&)> progress);

    // Parse InnoDB page from extent data
    std::vector<XfsPageInfo> parse_pages_from_extent(const std::vector<uint8_t>& data,
                                                      uint64_t inode_number);

    // Schema-less record parsing
    std::vector<std::map<std::string, std::string>> parse_records_schemaless_xfs(
        const std::vector<uint8_t>& page_data);

    // Schema-aware record parsing
    std::vector<std::map<std::string, std::string>> parse_records_with_schema_xfs(
        const std::vector<uint8_t>& page_data,
        const catalog::TableSchema& schema);
};

// ==================== Helper Functions ====================

namespace xfs_helper {

// Read big-endian values
inline uint16_t read_be16(const uint8_t* data) {
    return (static_cast<uint16_t>(data[0]) << 8) | data[1];
}

inline uint32_t read_be32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8)  |
           data[3];
}

inline uint64_t read_be64(const uint8_t* data) {
    return (static_cast<uint64_t>(data[0]) << 56) |
           (static_cast<uint64_t>(data[1]) << 48) |
           (static_cast<uint64_t>(data[2]) << 40) |
           (static_cast<uint64_t>(data[3]) << 32) |
           (static_cast<uint64_t>(data[4]) << 24) |
           (static_cast<uint64_t>(data[5]) << 16) |
           (static_cast<uint64_t>(data[6]) << 8)  |
           data[7];
}

// Convert AG-relative inode to disk offset
inline uint64_t inode_to_disk_offset(uint32_t ag_index, uint32_t ag_inode,
                                     const xfs_superblock& sb) {
    uint64_t ag_offset = static_cast<uint64_t>(ag_index) *
                         static_cast<uint64_t>(read_be32(reinterpret_cast<const uint8_t*>(&sb.sb_agblocks))) *
                         static_cast<uint64_t>(read_be32(reinterpret_cast<const uint8_t*>(&sb.sb_blocksize)));

    uint64_t inode_offset = static_cast<uint64_t>(ag_inode) *
                            static_cast<uint64_t>(read_be16(reinterpret_cast<const uint8_t*>(&sb.sb_inodesize)));

    return ag_offset + inode_offset;
}

// Convert filesystem block to disk offset
inline uint64_t fsblock_to_disk_offset(uint64_t fsblock, const xfs_superblock& sb) {
    uint32_t block_size = read_be32(reinterpret_cast<const uint8_t*>(&sb.sb_blocksize));
    return fsblock * block_size;
}

// Check if inode is deleted (mode == 0 for deleted inodes)
inline bool is_deleted_inode(const xfs_dinode_core& inode) {
    uint16_t magic = read_be16(reinterpret_cast<const uint8_t*>(&inode.di_magic));
    uint16_t mode = read_be16(reinterpret_cast<const uint8_t*>(&inode.di_mode));

    // Deleted inodes have:
    // - Valid magic (0x494E)
    // - Zero mode (file type)
    // - Zero nlink
    // - Version 3 (v5 XFS)
    return (magic == XFS_DINODE_MAGIC) &&
           (mode == 0) &&
           (inode.di_version == 3);
}

// Check if inode has recoverable data
inline bool has_recoverable_extents(const xfs_dinode_core& inode) {
    uint8_t format = inode.di_format;
    uint64_t size = read_be64(reinterpret_cast<const uint8_t*>(&inode.di_size));
    uint64_t nblocks = read_be64(reinterpret_cast<const uint8_t*>(&inode.di_nblocks));

    // Check for extent format with non-zero size
    return (format == XFS_DINODE_FMT_EXTENTS) &&
           (size > 0) &&
           (nblocks > 0);
}

} // namespace xfs_helper

} // namespace xfs
} // namespace disk
} // namespace dbrecovery
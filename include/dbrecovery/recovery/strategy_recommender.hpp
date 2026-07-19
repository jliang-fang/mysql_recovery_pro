#ifndef DBRECOVERY_RECOVERY_STRATEGY_RECOMMENDER_HPP
#define DBRECOVERY_RECOVERY_STRATEGY_RECOMMENDER_HPP

#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <filesystem>

namespace dbrecovery {
namespace recovery {

/**
 * @brief P4: Recovery strategy recommendation engine
 *
 * Analyzes the disaster scenario (file state, filesystem type, MySQL version,
 * available metadata) and recommends the best recovery strategy with rationale.
 *
 * Usage:
 *   StrategyRecommender recommender;
 *   recommender.set_mysql_version("8.0.35");
 *   recommender.set_filesystem("xfs");
 *   auto recs = recommender.recommend("DROP_TABLE");
 *   for (auto& r : recs) std::cout << r.to_string() << "\n";
 */
class StrategyRecommender {
public:
    enum class Scenario {
        DROP_TABLE,
        TRUNCATE,
        RM_RF,
        CORRUPTION,
        UNKNOWN
    };

    enum class Filesystem {
        EXT4,
        XFS,
        NTFS,
        RAW,
        UNKNOWN
    };

    struct Strategy {
        std::string name;               // e.g. "disk_carving_ext4_inode"
        std::string description;        // Human-readable
        int priority = 0;               // Lower = try first
        double expected_recovery_rate = 0.0;  // 0.0-1.0
        bool requires_schema = false;
        bool requires_ibdata = false;
        std::string rationale;
        std::string cli_flags;          // Suggested CLI flags
    };

    struct Recommendation {
        Scenario scenario;
        std::vector<Strategy> strategies;  // Ordered by priority
        std::string summary;
        std::vector<std::string> warnings;
        std::vector<std::string> prerequisites;

        std::string to_text() const {
            std::ostringstream oss;
            oss << "=== Recovery Strategy Recommendation ===\n\n";
            oss << "Scenario: " << scenario_name(scenario) << "\n";
            oss << "Summary: " << summary << "\n\n";

            oss << "--- Recommended Strategies (best first) ---\n";
            for (size_t i = 0; i < strategies.size(); i++) {
                const auto& s = strategies[i];
                oss << "\n[" << (i + 1) << "] " << s.name
                    << " (priority=" << s.priority
                    << ", expected_rate=" << std::fixed << std::setprecision(0)
                    << (s.expected_recovery_rate * 100) << "%)\n";
                oss << "    " << s.description << "\n";
                oss << "    Rationale: " << s.rationale << "\n";
                if (!s.cli_flags.empty()) {
                    oss << "    CLI: " << s.cli_flags << "\n";
                }
            }

            if (!warnings.empty()) {
                oss << "\n--- Warnings ---\n";
                for (const auto& w : warnings) oss << "  * " << w << "\n";
            }

            if (!prerequisites.empty()) {
                oss << "\n--- Prerequisites ---\n";
                for (const auto& p : prerequisites) oss << "  * " << p << "\n";
            }

            return oss.str();
        }
    };

    StrategyRecommender() = default;

    void set_mysql_version(const std::string& v) { mysql_version_ = v; }
    void set_filesystem(const std::string& fs) { filesystem_ = parse_fs(fs); }
    void set_has_schema(bool v) { has_schema_ = v; }
    void set_has_ibdata(bool v) { has_ibdata_ = v; }
    void set_has_undo_tablespace(bool v) { has_undo_ts_ = v; }
    void set_page_count(uint64_t n) { page_count_ = n; }
    void set_ibd_exists(bool v) { ibd_exists_ = v; }

    /**
     * @brief Generate recovery strategy recommendations for a scenario
     */
    Recommendation recommend(const std::string& scenario_str) {
        Scenario sc = parse_scenario(scenario_str);
        Recommendation rec;
        rec.scenario = sc;

        switch (sc) {
            case Scenario::DROP_TABLE:
                rec = recommend_drop_table();
                break;
            case Scenario::TRUNCATE:
                rec = recommend_truncate();
                break;
            case Scenario::RM_RF:
                rec = recommend_rm_rf();
                break;
            case Scenario::CORRUPTION:
                rec = recommend_corruption();
                break;
            default:
                rec = recommend_generic();
                break;
        }

        rec.summary = generate_summary(rec);
        return rec;
    }

private:
    std::string mysql_version_ = "8.0";
    Filesystem filesystem_ = Filesystem::UNKNOWN;
    bool has_schema_ = false;
    bool has_ibdata_ = false;
    bool has_undo_ts_ = false;
    uint64_t page_count_ = 0;
    bool ibd_exists_ = true;

    static Scenario parse_scenario(const std::string& s) {
        if (s == "DROP_TABLE" || s == "DROP TABLE" || s == "drop_table") return Scenario::DROP_TABLE;
        if (s == "TRUNCATE" || s == "TRUNCATE TABLE" || s == "truncate") return Scenario::TRUNCATE;
        if (s == "RM_RF" || s == "rm_rf" || s == "rm -rf") return Scenario::RM_RF;
        if (s == "CORRUPTION" || s == "corruption") return Scenario::CORRUPTION;
        return Scenario::UNKNOWN;
    }

    static Filesystem parse_fs(const std::string& s) {
        if (s == "ext4" || s == "EXT4") return Filesystem::EXT4;
        if (s == "xfs" || s == "XFS") return Filesystem::XFS;
        if (s == "ntfs" || s == "NTFS") return Filesystem::NTFS;
        if (s == "raw" || s == "RAW") return Filesystem::RAW;
        return Filesystem::UNKNOWN;
    }

    static std::string scenario_name(Scenario sc) {
        switch (sc) {
            case Scenario::DROP_TABLE: return "DROP TABLE";
            case Scenario::TRUNCATE:   return "TRUNCATE";
            case Scenario::RM_RF:      return "rm -rf";
            case Scenario::CORRUPTION: return "CORRUPTION";
            default:                   return "UNKNOWN";
        }
    }

    static std::string fs_name(Filesystem fs) {
        switch (fs) {
            case Filesystem::EXT4: return "ext4";
            case Filesystem::XFS:  return "xfs";
            case Filesystem::NTFS: return "ntfs";
            case Filesystem::RAW:  return "raw device";
            default:               return "unknown";
        }
    }

    bool is_mysql57_or_lower() const {
        return mysql_version_ >= "5.6" && mysql_version_ < "8.0";
    }

    bool is_mysql80_plus() const {
        return mysql_version_ >= "8.0";
    }

    Recommendation recommend_drop_table() {
        Recommendation rec;
        rec.scenario = Scenario::DROP_TABLE;
        rec.prerequisites = {
            "Stop all writes to the affected disk immediately",
            "Do NOT create new files on the same filesystem",
            "If possible, create a block-level backup of the disk first"
        };

        if (filesystem_ == Filesystem::EXT4) {
            // ext4: inode scanning is most effective for DROP TABLE
            rec.strategies.push_back({
                "disk_carving_ext4_inode",
                "Ext4 inode-based carving — scan freed inodes for InnoDB pages",
                0, 0.95, false, false,
                "ext4 preserves inode metadata for deleted files. Freed block "
                "scanning can recover both the .ibd file data and SDI metadata.",
                "--disk /dev/sdX --mode ext4 --output /recovery/"
            });

            if (has_schema_) {
                rec.strategies.push_back({
                    "schema_aware_ext4_parse",
                    "Schema-aware parsing of ext4-carved InnoDB pages",
                    1, 0.98, true, false,
                    "With schema metadata available, page parsing accuracy increases "
                    "significantly. B+Tree traversal recovers complete rows.",
                    "--disk /dev/sdX --mode ext4 --schema-file schema.json"
                });
            }
        } else if (filesystem_ == Filesystem::XFS) {
            rec.strategies.push_back({
                "disk_carving_xfs_extent",
                "XFS extent btree-based carving — traverse deleted inode extents",
                0, 0.95, false, false,
                "XFS uses extent btrees to track file blocks. Deleted inodes retain "
                "extent information through di_next_unlinked chains.",
                "--disk /dev/sdX --mode xfs --output /recovery/"
            });

            rec.strategies.push_back({
                "xfs_free_block_scan",
                "XFS free block scanning — scan freed AG blocks for residual data",
                1, 0.85, false, false,
                "Even after extent tree metadata is reclaimed, data blocks may still "
                "contain intact InnoDB pages. 512-byte step scanning catches unaligned pages.",
                "--disk /dev/sdX --mode xfs --xfs-scan-freed --output /recovery/"
            });
        } else {
            // Generic / RAW
            rec.strategies.push_back({
                "raw_disk_carving",
                "Raw disk carving — scan all sectors for InnoDB page signatures",
                0, 0.75, false, false,
                "Without filesystem metadata, scans every sector for FIL_PAGE_INDEX "
                "signatures. Effective but slower — 200GB disk takes ~2 minutes.",
                "--disk /dev/sdX --mode raw --output /recovery/"
            });
        }

        // If ibdata is available, add undo-assisted recovery
        if (has_ibdata_ || has_undo_ts_) {
            rec.strategies.push_back({
                "undo_log_flashback",
                "Undo log flashback — reverse DELETE operations from undo records",
                2, 0.70, false, true,
                "Scans undo logs (ibdata1 or standalone undo tablespaces) for "
                "TRX_UNDO_DEL_MARK records. Can reconstruct deleted row data.",
                "--system-tablespace ibdata1 --flashback-delete"
            });
        }

        rec.warnings = generate_drop_table_warnings();
        return rec;
    }

    Recommendation recommend_truncate() {
        Recommendation rec;
        rec.scenario = Scenario::TRUNCATE;
        rec.prerequisites = {
            "IMPORTANT: TRUNCATE recovery requires FLUSH TABLES before the incident",
            "MySQL 8.0+ TRUNCATE = DROP old tablespace + CREATE new one",
            "Check if MySQL was running with innodb_flush_method=O_DIRECT"
        };

        if (is_mysql80_plus()) {
            // MySQL 8.0+ TRUNCATE drops and recreates the tablespace
            rec.strategies.push_back({
                "disk_carving_truncate",
                "Disk carving for TRUNCATE — scan freed space for old tablespace pages",
                0, 0.60, false, false,
                "MySQL 8.0+ TRUNCATE performs DROP+CREATE internally. The old .ibd "
                "file space is freed. Recovery rate depends on whether data was flushed.",
                "--ibd old_table.ibd --mode truncate --output /recovery/"
            });

            if (has_ibdata_) {
                rec.strategies.push_back({
                    "undo_log_truncate_recovery",
                    "Undo log assisted TRUNCATE recovery — find pre-truncate data in undo",
                    1, 0.50, false, true,
                    "Undo logs may contain pre-TRUNCATE row data. This is limited by "
                    "undo retention — typically only recent data survives.",
                    "--ibd old_table.ibd --mode truncate --system-tablespace ibdata1"
                });
            }

            rec.warnings.push_back(
                "MySQL 8.0 TRUNCATE recovery is inherently limited — max ~60% rate "
                "without FLUSH TABLES. Use DROP TABLE recovery path if available.");

        } else {
            // MySQL 5.6/5.7 TRUNCATE reuses the same tablespace
            rec.strategies.push_back({
                "page_free_traversal",
                "PAGE_FREE list traversal — recover freed pages within the tablespace",
                0, 0.98, false, false,
                "MySQL 5.6/5.7 TRUNCATE marks pages as free but does not remove them. "
                "PAGE_FREE list traversal can recover nearly all rows.",
                "--ibd table.ibd --mode truncate --output /recovery/"
            });

            if (has_ibdata_ || has_undo_ts_) {
                rec.strategies.push_back({
                    "undo_log_page_recovery",
                    "Undo log page recovery — extract pre-TRUNCATE versions from undo",
                    1, 0.90, false, true,
                    "MySQL 5.6/5.7 stores TRUNCATE undo in the system rollback segment. "
                    "Full rollback can restore complete pre-TRUNCATE state.",
                    "--ibd table.ibd --mode truncate --system-tablespace ibdata1 --undo-recovery"
                });
            }
        }

        return rec;
    }

    Recommendation recommend_rm_rf() {
        Recommendation rec;
        rec.scenario = Scenario::RM_RF;
        rec.prerequisites = {
            "Unmount the filesystem immediately to prevent overwrites",
            "If unmounting isn't possible, stop mysqld and all other writes",
            "For SSDs, TRIM may have already invalidated freed blocks"
        };

        if (filesystem_ == Filesystem::EXT4) {
            rec.strategies.push_back({
                "ext4_inode_carving",
                "ext4 freed inode traversal — recover .ibd files from deleted inodes",
                0, 0.95, false, false,
                "ext4 journal and inode tables retain metadata about deleted files. "
                "Multi-level extent tree traversal recovers fragmented .ibd files.",
                "--data-dir /var/lib/mysql --mode rm-rf --filesystem ext4"
            });

            rec.strategies.push_back({
                "ext4_freed_block_scan",
                "ext4 freed block scan — scan all freed blocks for residual pages",
                1, 0.85, false, false,
                "When inode metadata is partially overwritten, freed block bitmap "
                "scanning is the fallback. Effective but may recover partial pages.",
                "--data-dir /var/lib/mysql --mode rm-rf --filesystem ext4 --scan-freed"
            });
        } else if (filesystem_ == Filesystem::XFS) {
            rec.strategies.push_back({
                "xfs_inode_carving",
                "XFS deleted inode chain traversal — follow di_next_unlinked",
                0, 0.95, false, false,
                "XFS uses linked lists for deleted inodes. Complete traversal recovers "
                "all deleted .ibd files. Tested at 200GB/100K rows scale.",
                "--data-dir /var/lib/mysql --mode rm-rf --filesystem xfs"
            });
        } else {
            rec.strategies.push_back({
                "raw_disk_carving",
                "Raw disk carving — filesystem-agnostic page signature scanning",
                0, 0.75, false, false,
                "Scans the entire disk for InnoDB page signatures (FIL_PAGE_INDEX). "
                "Works on any filesystem but slower and may miss fragmented files.",
                "--disk /dev/sdX --mode carving --output /recovery/"
            });
        }

        // Always recommend schemaless recovery as fallback
        if (!has_schema_) {
            rec.strategies.push_back({
                "schemaless_recovery",
                "Schemaless recovery — heuristic record parsing without schema",
                2, 0.70, false, false,
                "When no FRM/SDI metadata is available, physical layout analysis "
                "and statistical type inference can still recover most data.",
                "--ibd recovered.ibd --mode schemaless --infer-schema"
            });
        }

        return rec;
    }

    Recommendation recommend_corruption() {
        Recommendation rec;
        rec.scenario = Scenario::CORRUPTION;
        rec.prerequisites = {
            "Assess the extent of corruption (single page vs entire file)",
            "Backup the corrupted file before any repair attempt",
            "Determine corruption cause: disk failure, power loss, or software bug"
        };

        rec.strategies.push_back({
            "page_header_repair",
            "Page header repair — fix FIL header checksums and field values",
            0, 0.90, false, false,
            "Most corruption affects FIL header fields (checksum, LSN, page type). "
            "Header repair fixes these while preserving page data.",
            "--ibd corrupted.ibd --repair-header --output repaired.ibd"
        });

        rec.strategies.push_back({
            "salvage_engine_l2",
            "Salvage engine Level 2 — sliding window record detection",
            1, 0.70, false, false,
            "When page headers are damaged but record data is intact, the salvage "
            "engine uses COMPACT/DYNAMIC offset table patterns to find records.",
            "--ibd corrupted.ibd --salvage-mode --damage-threshold 0.5"
        });

        rec.strategies.push_back({
            "salvage_engine_l3",
            "Salvage engine Level 3 — regex pattern scanning for data fragments",
            2, 0.40, false, false,
            "For severely damaged pages, scans for recognizable data patterns "
            "(integers, dates, emails, etc.) and extracts fragments.",
            "--ibd corrupted.ibd --salvage-mode --damage-threshold 0.3"
        });

        if (has_schema_) {
            rec.strategies.push_back({
                "secondary_index_rescue",
                "Secondary index rescue — recover data from undamaged indexes",
                0, 0.85, true, false,
                "If the clustered index is damaged but secondary indexes are intact, "
                "extract PK values and indexed columns from secondary index leaves.",
                "--ibd corrupted_primary.ibd --secondary-index idx_*.ibd --secondary-rescue"
            });
        }

        rec.warnings.push_back(
            "Always try the least invasive strategy first (header repair). "
            "Escalate to salvage only if repair fails.");

        return rec;
    }

    Recommendation recommend_generic() {
        Recommendation rec;
        rec.scenario = Scenario::UNKNOWN;

        rec.strategies.push_back({
            "auto_detect",
            "Automatic scenario detection — analyze file state and choose strategy",
            0, 0.0, false, false,
            "The system will analyze the .ibd file (page count, zero-page ratio, "
            "free space pattern) to detect the likely disaster scenario.",
            "--ibd table.ibd --auto-detect"
        });

        rec.strategies.push_back({
            "disk_carving",
            "Full disk carving — comprehensive scan for all InnoDB data",
            1, 0.75, false, false,
            "When the scenario can't be determined, full disk carving is the safest "
            "option. Scans the entire device for InnoDB page signatures.",
            "--disk /dev/sdX --mode carving --output /recovery/"
        });

        return rec;
    }

    std::vector<std::string> generate_drop_table_warnings() {
        std::vector<std::string> warnings;
        warnings.push_back(
            "DROP TABLE recovery success depends on how quickly the freed space "
            "is overwritten. Recover immediately after the incident.");

        if (filesystem_ == Filesystem::XFS) {
            warnings.push_back(
                "XFS delayed allocation may mean recent data was never written to "
                "the .ibd file. Check MySQL's innodb_flush_method setting.");
        }

        if (is_mysql80_plus()) {
            warnings.push_back(
                "MySQL 8.0+ DROP TABLE performs final RENAME before deletion. "
                "If MySQL is still running, the file may be in the #sql-* state.");
        }

        return warnings;
    }

    std::string generate_summary(const Recommendation& rec) {
        std::ostringstream oss;
        oss << rec.strategies.size() << " strategy/strategies recommended for "
            << scenario_name(rec.scenario) << " on " << fs_name(filesystem_)
            << " (MySQL " << mysql_version_ << "). ";

        if (!rec.strategies.empty()) {
            const auto& best = rec.strategies.front();
            oss << "Best: " << best.name << " (expected "
                << std::fixed << std::setprecision(0)
                << (best.expected_recovery_rate * 100) << "% rate).";
        }

        return oss.str();
    }
};

} // namespace recovery
} // namespace dbrecovery

#endif // DBRECOVERY_RECOVERY_STRATEGY_RECOMMENDER_HPP

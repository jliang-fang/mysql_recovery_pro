/**
 * @file damage_level.hpp
 * @brief Unified damage classification and forensic evidence for salvage recovery
 *
 * Consolidates the previously duplicated DamageLevel enums (recovery::DamageLevel
 * and salvage::DamageLevel) into a single canonical enum in dbrecovery::common,
 * and defines the ForensicEvidence structure that records how a salvaged row was
 * recovered (strategy, source location, confidence, provenance tags).
 */

#ifndef DBRECOVERY_COMMON_DAMAGE_LEVEL_HPP
#define DBRECOVERY_COMMON_DAMAGE_LEVEL_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace dbrecovery {
namespace common {

// ==================== Unified Damage Level ====================

enum class DamageLevel : uint8_t {
    NONE = 0,          // Page intact; no damage
    L1_MINOR = 1,      // FIL header valid, checksum mismatch (repairable)
    L2_MODERATE = 2,   // Partial FIL header, record signatures present
    L3_SEVERE = 3,     // No reliable FIL header, recognizable data patterns
    L4_CRITICAL = 4    // Severely corrupted, only string/fragment extraction
};

inline std::string damage_level_to_string(DamageLevel level) {
    switch (level) {
        case DamageLevel::NONE:          return "NONE";
        case DamageLevel::L1_MINOR:      return "L1_MINOR";
        case DamageLevel::L2_MODERATE:   return "L2_MODERATE";
        case DamageLevel::L3_SEVERE:     return "L3_SEVERE";
        case DamageLevel::L4_CRITICAL:   return "L4_CRITICAL";
        default:                         return "UNKNOWN";
    }
}

// ==================== Forensic Evidence ====================

/**
 * @brief Provenance metadata attached to a salvaged row.
 *
 * Records how, where, and with what confidence a row fragment was recovered,
 * so that L3/L4 (heuristic) results are distinguishable from verified L1/L2
 * structure-derived results in downstream reporting.
 */
struct ForensicEvidence {
    DamageLevel level = DamageLevel::NONE;
    std::string strategy;         // e.g. "signature_scan", "pattern_match_email", "string_extract"
    uint32_t source_page_no = 0;  // page number the fragment came from
    uint64_t source_offset = 0;   // byte offset of the fragment within the source buffer
    float confidence = 0.0f;      // 0.0 to 1.0
    std::vector<std::string> tags; // e.g. "partial", "reconstructed", "heuristic", "verified"

    bool has_forensic_evidence() const {
        return !strategy.empty() || !tags.empty();
    }

    std::string evidence_summary() const {
        std::string out = damage_level_to_string(level) + ":" + strategy;
        if (!tags.empty()) {
            out += " [";
            for (size_t i = 0; i < tags.size(); i++) {
                if (i) out += ",";
                out += tags[i];
            }
            out += "]";
        }
        return out;
    }
};

// ==================== Convenience Aliases ====================

// Backward-compatible alias so existing recovery-code callers can keep using
// the short name within their own namespaces if they prefer.
using DamageLevelAlias = DamageLevel;

} // namespace common
} // namespace dbrecovery

#endif // DBRECOVERY_COMMON_DAMAGE_LEVEL_HPP
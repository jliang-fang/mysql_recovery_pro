#ifndef DBRECOVERY_SALVAGE_EVIDENCE_SERIALIZER_HPP
#define DBRECOVERY_SALVAGE_EVIDENCE_SERIALIZER_HPP

#include <string>
#include <vector>

#include "dbrecovery/recovery/salvage_engine.hpp"

namespace dbrecovery {
namespace salvage {

/**
 * @brief Serializes L3/L4 forensic evidence fragments to a non-importable JSON report.
 *
 * This serializer exists to ensure that heuristic (FRAGMENT/STRING_ONLY) salvage
 * output is never confused with structured recovery results. The output is clearly
 * marked as non-importable, includes only metadata, hashes, and bounded previews
 * (never full raw fragment values), and carries provenance information so engineers
 * can trace each fragment back to its source page and extraction strategy.
 */
class SalvageEvidenceSerializer {
public:
    SalvageEvidenceSerializer() = default;

    /**
     * @brief Compute SHA-256 hex digest of a string.
     */
    static std::string sha256_hex(const std::string& value);

    /**
     * @brief Write the evidence report to a JSON file.
     *
     * @param path       Output file path (.json)
     * @param fragments  The evidence_fragments collection from SalvageResult
     * @param metadata   Optional key-value metadata (e.g. source file, page size)
     * @return true on success, false on I/O error
     */
    bool write_evidence(
        const std::string& path,
        const std::vector<dbrecovery::recovery::SalvagedRow>& fragments,
        const std::map<std::string, std::string>& metadata = {});

    /**
     * @brief Generate the evidence JSON string in memory.
     */
    std::string to_json(
        const std::vector<dbrecovery::recovery::SalvagedRow>& fragments,
        const std::map<std::string, std::string>& metadata = {});

private:
    static constexpr size_t MAX_PREVIEW_LENGTH = 256;

    /**
     * @brief Produce a bounded, encoding-safe text preview of a fragment value.
     *
     * Replaces non-printable bytes with hex escapes and truncates at
     * MAX_PREVIEW_LENGTH with an explicit truncation marker.
     */
    static std::string bounded_preview(const std::string& value);
};

} // namespace salvage
} // namespace dbrecovery

#endif // DBRECOVERY_SALVAGE_EVIDENCE_SERIALIZER_HPP
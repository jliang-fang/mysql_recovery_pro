#include "dbrecovery/salvage/evidence_serializer.hpp"

#include <cstdint>
#include <iomanip>
#include <map>
#include <nlohmann/json.hpp>
#include <openssl/sha.h>
#include <sstream>
#include <vector>

namespace dbrecovery {
namespace salvage {

std::string SalvageEvidenceSerializer::sha256_hex(const std::string& value) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), hash);

    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(hash[i]);
    }
    return oss.str();
}

std::string SalvageEvidenceSerializer::bounded_preview(const std::string& value) {
    std::ostringstream oss;
    size_t end = std::min(value.size(), MAX_PREVIEW_LENGTH);

    for (size_t i = 0; i < end; ++i) {
        unsigned char c = static_cast<unsigned char>(value[i]);
        if (c >= 0x20 && c <= 0x7E && c != '\\' && c != '"') {
            oss << c;
        } else {
            oss << "\\x" << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<int>(c);
        }
    }

    if (value.size() > MAX_PREVIEW_LENGTH) {
        oss << "...[truncated " << (value.size() - MAX_PREVIEW_LENGTH) << " bytes]";
    }

    return oss.str();
}

std::string SalvageEvidenceSerializer::to_json(
    const std::vector<dbrecovery::recovery::SalvagedRow>& fragments,
    const std::map<std::string, std::string>& metadata) {

    nlohmann::json j;

    // Non-importable marker — this is forensic evidence, not structured data
    j["_metadata"]["format"] = "salvage_evidence";
    j["_metadata"]["version"] = 1;
    j["_metadata"]["non_importable_warning"] =
        "This file contains forensic evidence fragments (L3/L4 heuristic salvage "
        "output). These fragments are NOT verified structured rows and cannot be "
        "imported into a database. They are provided for forensic analysis only.";
    j["_metadata"]["total_fragments"] = fragments.size();

    // Optional user-provided metadata
    for (const auto& [key, val] : metadata) {
        j["_metadata"]["context"][key] = val;
    }

    // Count by salvage level
    size_t fragment_count = 0;
    size_t string_only_count = 0;
    for (const auto& frag : fragments) {
        if (frag.salvage_level == dbrecovery::recovery::SalvageLevel::FRAGMENT) {
            fragment_count++;
        } else if (frag.salvage_level == dbrecovery::recovery::SalvageLevel::STRING_ONLY) {
            string_only_count++;
        }
    }
    j["_metadata"]["fragment_count"] = fragment_count;
    j["_metadata"]["string_only_count"] = string_only_count;

    // Serialize each fragment
    j["fragments"] = nlohmann::json::array();
    for (const auto& frag : fragments) {
        nlohmann::json entry;

        // Provenance
        entry["salvage_level"] = static_cast<int>(frag.salvage_level);
        entry["recovery_method"] = frag.recovery_method;
        entry["confidence"] = frag.confidence;
        entry["source_page_no"] = frag.page_no;
        entry["source_page_offset"] = frag.page_offset;
        entry["source_record_offset"] = frag.record_offset;
        entry["content_sha256"] = frag.content_hash;

        // Forensic evidence provenance
        entry["evidence"]["level"] =
            dbrecovery::common::damage_level_to_string(frag.evidence.level);
        entry["evidence"]["strategy"] = frag.evidence.strategy;
        entry["evidence"]["source_page_no"] = frag.evidence.source_page_no;
        entry["evidence"]["source_offset"] = frag.evidence.source_offset;
        entry["evidence"]["confidence"] = frag.evidence.confidence;
        entry["evidence"]["tags"] = nlohmann::json::array();
        for (const auto& tag : frag.evidence.tags) {
            entry["evidence"]["tags"].push_back(tag);
        }

        // Fields: hash + bounded preview only (never full raw values)
        entry["fields"] = nlohmann::json::array();
        for (const auto& field : frag.fields) {
            nlohmann::json field_entry;
            field_entry["sha256"] = sha256_hex(field);
            field_entry["preview"] = bounded_preview(field);
            field_entry["preview_length"] = field.size();
            entry["fields"].push_back(std::move(field_entry));
        }

        j["fragments"].push_back(std::move(entry));
    }

    return j.dump(2);
}

bool SalvageEvidenceSerializer::write_evidence(
    const std::string& path,
    const std::vector<dbrecovery::recovery::SalvagedRow>& fragments,
    const std::map<std::string, std::string>& metadata) {

    std::string json_str = to_json(fragments, metadata);

    std::ofstream file(path);
    if (!file) return false;

    file << json_str << "\n";
    return file.good();
}

} // namespace salvage
} // namespace dbrecovery
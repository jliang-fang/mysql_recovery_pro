#ifndef DBRECOVERY_INNODB_SDI_MANAGER_HPP
#define DBRECOVERY_INNODB_SDI_MANAGER_HPP

#include <algorithm>
#include <string>
#include "mach_read.hpp"
#include <vector>
#include <optional>
#include <zlib.h>
#include <nlohmann/json.hpp>
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery::innodb {

// Shared SDI decompression safety limits.
// Single source of truth referenced by both SDIManager and SDIExtractor so the
// compression ratio policy cannot drift between the two extraction paths.
constexpr size_t SDI_MAX_UNCOMPRESSED_SIZE = 16 * 1024 * 1024;  // 16 MB
constexpr size_t SDI_MAX_COMPRESSION_RATIO = 20;                // max 20:1

class SDIManager {
public:
    struct SDIHeader {
        uint32_t type;
        uint64_t id;
        uint32_t uncompressed_len;
        uint32_t compressed_len;
    };

    enum class ExtractionOrigin {
        StrictStandardPayload,
        StrictZipRecordAdapter,
        HeuristicCorruptedZip,
    };

    struct ExtractionResult {
        ExtractionOrigin origin = ExtractionOrigin::StrictStandardPayload;
        std::string json;
        std::string failure_reason;
        uint32_t declared_uncompressed_length = 0;
        uint32_t declared_compressed_length = 0;
        size_t actual_uncompressed_length = 0;
        bool json_valid = false;

        bool success() const { return json_valid; }
    };

    static ExtractionResult extract_standard_payload(
        const uint8_t* payload, size_t payload_len,
        ExtractionOrigin origin = ExtractionOrigin::StrictStandardPayload) {
        ExtractionResult result;
        result.origin = origin;
        if (!payload || payload_len < 20) {
            result.failure_reason = "payload too small";
            return result;
        }

        SDIHeader header{};
        header.type = read_le<uint32_t>(payload);
        header.id = read_le<uint64_t>(payload + 4);
        header.uncompressed_len = read_le<uint32_t>(payload + 12);
        header.compressed_len = read_le<uint32_t>(payload + 16);
        result.declared_uncompressed_length = header.uncompressed_len;
        result.declared_compressed_length = header.compressed_len;

        if (header.type != 1 || header.id == 0 || header.uncompressed_len == 0 ||
            header.compressed_len == 0) {
            result.failure_reason = "invalid SDI header";
            return result;
        }
        if (header.uncompressed_len > SDI_MAX_UNCOMPRESSED_SIZE) {
            result.failure_reason = "declared output exceeds limit";
            return result;
        }
        if (header.uncompressed_len / header.compressed_len > SDI_MAX_COMPRESSION_RATIO) {
            result.failure_reason = "declared compression ratio exceeds limit";
            return result;
        }
        if (header.compressed_len > payload_len - 20) {
            result.failure_reason = "compressed data exceeds payload";
            return result;
        }

        std::vector<uint8_t> decompressed(header.uncompressed_len);
        z_stream stream{};
        stream.next_in = const_cast<Bytef*>(payload + 20);
        stream.avail_in = static_cast<uInt>(header.compressed_len);
        stream.next_out = decompressed.data();
        stream.avail_out = static_cast<uInt>(decompressed.size());
        if (inflateInit2(&stream, -15) != Z_OK) {
            result.failure_reason = "raw-deflate initialization failed";
            return result;
        }
        const int status = inflate(&stream, Z_FINISH);
        const size_t actual_length = stream.total_out;
        const size_t remaining_input = stream.avail_in;
        inflateEnd(&stream);
        result.actual_uncompressed_length = actual_length;

        if (status != Z_STREAM_END || remaining_input != 0) {
            result.failure_reason = "incomplete or trailing compressed stream";
            return result;
        }
        if (actual_length != header.uncompressed_len) {
            result.failure_reason = "decompressed length mismatch";
            return result;
        }

        result.json.assign(reinterpret_cast<const char*>(decompressed.data()), decompressed.size());
        try {
            const auto parsed_json = nlohmann::json::parse(result.json);
            (void)parsed_json;
        } catch (const nlohmann::json::exception&) {
            result.json.clear();
            result.failure_reason = "invalid JSON";
            return result;
        }
        result.json_valid = true;
        return result;
    }

    static ExtractionResult extract_zip_record(
        const uint8_t* page_data, size_t page_size, size_t record_offset) {
        ExtractionResult result;
        result.origin = ExtractionOrigin::StrictZipRecordAdapter;
        constexpr size_t record_header_size = 33;
        constexpr size_t uncompressed_length_offset = 25;
        constexpr size_t compressed_length_offset = 29;
        if (!page_data || record_offset > page_size || page_size - record_offset < record_header_size) {
            result.failure_reason = "ZIP record header exceeds page";
            return result;
        }
        const uint32_t uncompressed_length = read_be<uint32_t>(page_data + record_offset + uncompressed_length_offset);
        const uint32_t compressed_length = read_be<uint32_t>(page_data + record_offset + compressed_length_offset);
        result.declared_uncompressed_length = uncompressed_length;
        result.declared_compressed_length = compressed_length;
        if (compressed_length > page_size - record_offset - record_header_size) {
            result.failure_reason = "ZIP record data exceeds page";
            return result;
        }

        std::vector<uint8_t> payload(20 + compressed_length);
        write_le<uint32_t>(payload.data(), 0, 1);
        write_le<uint64_t>(payload.data(), 4, 1);
        write_le<uint32_t>(payload.data(), 12, uncompressed_length);
        write_le<uint32_t>(payload.data(), 16, compressed_length);
        std::copy_n(page_data + record_offset + record_header_size, compressed_length, payload.data() + 20);
        return extract_standard_payload(payload.data(), payload.size(), ExtractionOrigin::StrictZipRecordAdapter);
    }

    static std::optional<std::string> extract_sdi_json(const uint8_t* payload, size_t payload_len) {
        auto result = extract_standard_payload(payload, payload_len);
        if (!result.success()) return std::nullopt;
        return result.json;
    }

    /**
     * @brief Bounded heuristic fallback for corrupted SDI ZIP pages.
     *
     * Scans the page for zlib raw-deflate signatures (78 9c, 78 da, 78 01, 78 5e)
     * within a bounded range, tests up to @p max_candidates candidates, and
     * decompresses each through the same bounded limits (16 MiB / 20:1) as the
     * strict path.  Only accepts if the output is valid JSON.
     *
     * The result is tagged HeuristicCorruptedZip — callers must not treat it as
     * authoritative SDI for schema-critical decisions.
     */
    static ExtractionResult extract_heuristic_zip_page(
        const uint8_t* page_data, size_t page_size,
        size_t max_candidates = 5) {
        ExtractionResult result;
        result.origin = ExtractionOrigin::HeuristicCorruptedZip;
        if (!page_data || page_size < 100) {
            result.failure_reason = "page too small for heuristic scan";
            return result;
        }

        constexpr size_t search_start = 100;
        const size_t search_end = (page_size > 8) ? page_size - 8 : page_size;

        size_t candidates_tried = 0;
        for (size_t offset = search_start; offset + 2 < search_end; ++offset) {
            // Check for zlib raw-deflate magic bytes.
            if (page_data[offset] != 0x78) continue;
            const uint8_t b = page_data[offset + 1];
            if (b != 0x9c && b != 0xda && b != 0x01 && b != 0x5e) continue;

            if (++candidates_tried > max_candidates) break;

            const size_t max_input = search_end - offset;
            // Synthesize an SDI header so we can reuse extract_standard_payload.
            // Use a conservative uncompressed_len estimate capped at the safety limit.
            const uint32_t conservative_uncompressed =
                std::min<uint32_t>(static_cast<uint32_t>(max_input * SDI_MAX_COMPRESSION_RATIO),
                                   SDI_MAX_UNCOMPRESSED_SIZE);

            std::vector<uint8_t> candidate(20 + max_input);
            write_le<uint32_t>(candidate.data(), 0, 1);
            write_le<uint64_t>(candidate.data(), 4, 1);
            write_le<uint32_t>(candidate.data(), 12, conservative_uncompressed);
            write_le<uint32_t>(candidate.data(), 16, static_cast<uint32_t>(max_input));
            std::copy_n(page_data + offset, max_input, candidate.data() + 20);

            auto candidate_result = extract_standard_payload(
                candidate.data(), candidate.size(),
                ExtractionOrigin::HeuristicCorruptedZip);
            if (candidate_result.success()) {
                return candidate_result;
            }
        }

        result.failure_reason = "no valid heuristic ZIP candidate found";
        return result;
    }

    static catalog::TableSchema parse_sdi_record(const uint8_t* payload, size_t payload_len);
};

} // namespace dbrecovery::innodb

#endif // DBRECOVERY_INNODB_SDI_MANAGER_HPP

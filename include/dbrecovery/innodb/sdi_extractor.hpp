#ifndef DBRECOVERY_INNODB_SDI_EXTRACTOR_HPP
#define DBRECOVERY_INNODB_SDI_EXTRACTOR_HPP

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <limits>
#include <cstring>
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/sdi_manager.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/common/version_compat.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/salvage/evidence_serializer.hpp"
#include <nlohmann/json.hpp>

namespace dbrecovery {
namespace innodb {

// SDI decompression safety limits — shared from sdi_manager.hpp so the
// compression-ratio policy cannot drift between extraction paths.
// (Constants are in the same namespace, visible via the include above.)

// SDI 提取的结构体定义（在类之前声明）
struct ExtractedSDI {
    uint32_t page_no;              // 页号
    size_t offset;                 // SDI 在页中的偏移
    std::string sdi_json;          // SDI JSON 字符串
    bool is_valid;                 // 是否有效
    std::string error_message;     // 错误信息（如果无效）
    SDIManager::ExtractionOrigin origin = SDIManager::ExtractionOrigin::StrictStandardPayload;
    uint32_t declared_uncompressed_length = 0;
    uint32_t declared_compressed_length = 0;
    size_t actual_uncompressed_length = 0;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "ExtractedSDI{page_no=" << page_no
            << ", offset=" << offset
            << ", origin=" << static_cast<int>(origin)
            << ", valid=" << std::boolalpha << is_valid << std::noboolalpha
            << ", json_len=" << sdi_json.size()
            << ", decl_uncompressed=" << declared_uncompressed_length
            << ", decl_compressed=" << declared_compressed_length
            << ", actual_uncompressed=" << actual_uncompressed_length;
        if (!is_valid && !error_message.empty()) {
            oss << ", error=\"" << error_message << "\"";
        }
        oss << "}";
        return oss.str();
    }
};

// SDI 提取统计
struct SDIExtractStats {
    size_t total_pages = 0;
    size_t sdi_pages = 0;
    size_t sdi_extracted = 0;
    size_t sdi_failed = 0;
    size_t total_size = 0;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "SDIExtractStats{total_pages=" << total_pages
            << ", sdi_pages=" << sdi_pages
            << ", extracted=" << sdi_extracted
            << ", failed=" << sdi_failed << "}";
        return oss.str();
    }
};

/**
 * @brief SDI 提取器
 * @details 从损坏的页中提取 SDI (Space Descriptor Index) 元数据
 */
class SDIExtractor {
public:
    SDIExtractor(size_t page_size = 16384);

    /**
     * @brief 从文件中提取 SDI
     * @param file_path 文件路径
     * @return 提取的 SDI 数据
     */
    std::vector<ExtractedSDI> extract_from_file(const std::filesystem::path& file_path);

    std::vector<ExtractedSDI> extract_from_file_if_compatible(
        const std::filesystem::path& file_path, dbrecovery::MySQLVersion version);

    /**
     * @brief 从内存缓冲区中提取 SDI
     * @param data 缓冲区数据
     * @param size 缓冲区大小
     * @return 提取的 SDI 数据
     */
    std::vector<ExtractedSDI> extract_from_buffer(const uint8_t* data, size_t size);

    /**
     * @brief 获取提取统计
     */
    const SDIExtractStats& stats() const { return extract_stats_; }

    /**
     * @brief 解析 SDI JSON 字符串
     * @param sdi_json SDI JSON 字符串
     * @return 解析后的表结构
     */
    std::optional<catalog::TableSchema> parse_sdi_json(const std::string& sdi_json);

    /**
     * @brief Version-gated extraction from a buffer
     *
     * SDI pages only exist in MySQL 8.0+. If the resolved version does not
     * support SDI, extraction is skipped entirely so a 5.6/5.7 tablespace is
     * never mis-parsed as SDI. Pass an empty/unknown version to force-scan.
     */
    std::vector<ExtractedSDI> extract_from_buffer_if_compatible(
        const uint8_t* data, size_t size, dbrecovery::MySQLVersion version);

    /**
     * @brief 从损坏的页中提取 SDI（尝试多种偏移）
     * @param page_data 页数据
     * @param page_size 页大小
     * @return 提取的 SDI 数据
     */
    std::vector<ExtractedSDI> extract_from_page(const uint8_t* page_data, size_t page_size);

    /**
     * @brief Version-gated extraction from one page
     *
     * Known MySQL versions without SDI are skipped. UNKNOWN deliberately
     * force-scans for forensic workflows. This preserves cumulative statistics.
     */
    std::vector<ExtractedSDI> extract_from_page_if_compatible(
        const uint8_t* page_data, size_t page_size, dbrecovery::MySQLVersion version);

public:
    size_t page_size_;
    SDIExtractStats extract_stats_;

    // 从页中提取 SDI 负载数据
    std::optional<std::vector<uint8_t>> extract_sdi_payload(const uint8_t* page_data, size_t page_size);

    // 从页中提取 SDI（给定偏移）
    std::optional<ExtractedSDI> extract_sdi_at_offset(const uint8_t* page_data, size_t page_size, size_t offset);

    // 检查 SDI 页类型
    bool is_sdi_page(const uint8_t* page_data) const;
};

// ==================== 实现 ====================

inline SDIExtractor::SDIExtractor(size_t page_size)
    : page_size_(page_size) {
    // 验证页大小
    if (page_size_ != 4096 && page_size_ != 8192 &&
        page_size_ != 16384 && page_size_ != 32768 && page_size_ != 65536) {
        page_size_ = 16384;  // 默认 16KB
    }
}

inline bool SDIExtractor::is_sdi_page(const uint8_t* page_data) const {
    if (!page_data || page_size_ < 38) {
        return false;
    }

    // 读取页类型 (at offset 24, big-endian)
    uint16_t type = read_be<uint16_t>(page_data, FIL_PAGE_TYPE_ABS);

    // Uncompressed SDI (0x45BE) is unambiguous
    if (type == FIL_PAGE_SDI) return true;

    // FIL_PAGE_SDI_ZIP (0x45BD) is shared with FIL_PAGE_TYPE_BLOB.
    // Only the SDI signature at offset 64-71 establishes SDI identity.
    if (type == FIL_PAGE_SDI_ZIP) {
        if (page_size_ < 72) return false;
        constexpr uint8_t SDI_SIG[8] = {0x08, 'S', 'D', 'I', '0', '0', '0', '0'};
        return std::memcmp(page_data + 64, SDI_SIG, 8) == 0;
    }

    return false;
}

inline std::optional<std::vector<uint8_t>> SDIExtractor::extract_sdi_payload(const uint8_t* page_data, size_t page_size) {
    if (!page_data || page_size < 100 || !is_sdi_page(page_data)) return std::nullopt;

    if (read_be<uint16_t>(page_data, FIL_PAGE_TYPE_ABS) == FIL_PAGE_SDI_ZIP) {
        const int16_t slot_offset = static_cast<int16_t>(
            (static_cast<uint16_t>(page_data[97]) << 8) | static_cast<uint16_t>(page_data[98]));
        const int64_t record_offset = static_cast<int64_t>(slot_offset) + 99;
        if (record_offset < 0) return std::nullopt;
        const auto result = SDIManager::extract_zip_record(page_data, page_size, static_cast<size_t>(record_offset));
        if (!result.success()) return std::nullopt;
        return std::vector<uint8_t>(result.json.begin(), result.json.end());
    }

    constexpr size_t min_offset = 70;
    constexpr size_t max_offset = 200;
    for (size_t offset = min_offset; offset + 20 <= std::min(max_offset, page_size); ++offset) {
        const auto result = SDIManager::extract_standard_payload(page_data + offset, page_size - offset);
        if (result.success()) return std::vector<uint8_t>(result.json.begin(), result.json.end());
    }
    return std::nullopt;
}

inline std::optional<ExtractedSDI> SDIExtractor::extract_sdi_at_offset(
    const uint8_t* page_data, size_t page_size, size_t offset) {
    if (!page_data || offset > page_size || page_size - offset < 20) return std::nullopt;

    ExtractedSDI extracted{};
    extracted.page_no = read_be<uint32_t>(page_data, FIL_PAGE_OFFSET);
    extracted.offset = offset;
    const auto result = SDIManager::extract_standard_payload(page_data + offset, page_size - offset);
    extracted.origin = result.origin;
    extracted.is_valid = result.success();
    extracted.sdi_json = result.json;
    extracted.error_message = result.failure_reason;
    extracted.declared_uncompressed_length = result.declared_uncompressed_length;
    extracted.declared_compressed_length = result.declared_compressed_length;
    extracted.actual_uncompressed_length = result.actual_uncompressed_length;
    return extracted;
}

inline std::vector<ExtractedSDI> SDIExtractor::extract_from_page(const uint8_t* page_data, size_t page_size) {
    std::vector<ExtractedSDI> results;
    if (!page_data || page_size < 100 || !is_sdi_page(page_data)) return results;

    ++extract_stats_.sdi_pages;
    const uint16_t type = read_be<uint16_t>(page_data, FIL_PAGE_TYPE_ABS);
    if (type == FIL_PAGE_SDI_ZIP) {
        const int16_t slot_offset = static_cast<int16_t>(
            (static_cast<uint16_t>(page_data[97]) << 8) | static_cast<uint16_t>(page_data[98]));
        const int64_t record_offset = static_cast<int64_t>(slot_offset) + 99;
        if (record_offset >= 0) {
            const auto result = SDIManager::extract_zip_record(page_data, page_size, static_cast<size_t>(record_offset));
            if (result.success()) {
                results.push_back({read_be<uint32_t>(page_data, FIL_PAGE_OFFSET),
                                   static_cast<size_t>(record_offset), result.json, true, {}, result.origin,
                                   result.declared_uncompressed_length, result.declared_compressed_length,
                                   result.actual_uncompressed_length});
            }
        }
    } else {
        constexpr size_t min_offset = 70;
        constexpr size_t max_offset = 500;
        for (size_t offset = min_offset; offset + 20 <= std::min(max_offset, page_size); ++offset) {
            auto extracted = extract_sdi_at_offset(page_data, page_size, offset);
            if (extracted && extracted->is_valid) {
                results.push_back(std::move(*extracted));
                break;
            }
        }
    }

    if (results.empty()) ++extract_stats_.sdi_failed;
    else extract_stats_.sdi_extracted += results.size();
    return results;
}

inline std::vector<ExtractedSDI> SDIExtractor::extract_from_page_if_compatible(
    const uint8_t* page_data, size_t page_size, dbrecovery::MySQLVersion version) {
    if (version != dbrecovery::MySQLVersion::UNKNOWN &&
        !dbrecovery::version_supports_sdi(version)) {
        return {};
    }
    return extract_from_page(page_data, page_size);
}

inline std::vector<ExtractedSDI> SDIExtractor::extract_from_buffer(const uint8_t* data, size_t size) {
    std::vector<ExtractedSDI> results;
    extract_stats_ = SDIExtractStats{};

    if (!data || size < page_size_) {
        return results;
    }

    extract_stats_.total_size = size;

    // 扫描每个页
    for (size_t offset = 0; offset + page_size_ <= size; offset += page_size_) {
        auto page_results = extract_from_page(data + offset, page_size_);
        results.insert(results.end(), page_results.begin(), page_results.end());
        extract_stats_.total_pages++;
    }

    return results;
}

inline std::vector<ExtractedSDI> SDIExtractor::extract_from_buffer_if_compatible(
    const uint8_t* data, size_t size, dbrecovery::MySQLVersion version) {
    // SDI metadata only exists in MySQL 8.0+. Fail-closed: if the version is
    // known and doesn't support SDI, return nothing rather than scanning a
    // 5.6/5.7 tablespace for pages that cannot be SDI.
    if (version != dbrecovery::MySQLVersion::UNKNOWN &&
        !dbrecovery::version_supports_sdi(version)) {
        extract_stats_ = SDIExtractStats{};
        extract_stats_.total_size = size;
        return {};
    }
    return extract_from_buffer(data, size);
}

inline std::vector<ExtractedSDI> SDIExtractor::extract_from_file(
    const std::filesystem::path& file_path) {
    return extract_from_file_if_compatible(
        file_path, dbrecovery::MySQLVersion::UNKNOWN);
}

inline std::vector<ExtractedSDI> SDIExtractor::extract_from_file_if_compatible(
    const std::filesystem::path& file_path, dbrecovery::MySQLVersion version) {
    std::vector<ExtractedSDI> results;
    extract_stats_ = SDIExtractStats{};

    std::error_code size_error;
    const auto file_size = std::filesystem::file_size(file_path, size_error);
    if (size_error || file_size > std::numeric_limits<size_t>::max()) {
        return results;
    }
    extract_stats_.total_size = static_cast<size_t>(file_size);

    if (version != dbrecovery::MySQLVersion::UNKNOWN &&
        !dbrecovery::version_supports_sdi(version)) {
        return results;
    }

    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        return results;
    }

    constexpr size_t chunk_size = 1024 * 1024;
    std::vector<uint8_t> chunk(chunk_size);
    std::vector<uint8_t> pending;
    pending.reserve(chunk_size + page_size_);

    while (file.read(reinterpret_cast<char*>(chunk.data()), chunk.size()) || file.gcount() > 0) {
        const size_t bytes_read = static_cast<size_t>(file.gcount());
        pending.insert(pending.end(), chunk.begin(), chunk.begin() + bytes_read);

        const size_t complete_size = (pending.size() / page_size_) * page_size_;
        for (size_t offset = 0; offset < complete_size; offset += page_size_) {
            auto page_results = extract_from_page(pending.data() + offset, page_size_);
            results.insert(results.end(), page_results.begin(), page_results.end());
            ++extract_stats_.total_pages;
        }

        pending.erase(pending.begin(), pending.begin() + complete_size);
    }

    return results;
}

inline std::optional<catalog::TableSchema> SDIExtractor::parse_sdi_json(const std::string& sdi_json) {
    if (sdi_json.empty()) return std::nullopt;

    try {
        auto j = nlohmann::json::parse(sdi_json);

        // MySQL 8.0 SDI format: the dd_object contains table/column metadata
        catalog::TableSchema schema;
        if (!schema.from_json(j)) return std::nullopt;

        if (schema.table_name.empty()) return std::nullopt;
        schema.schema_hash = dbrecovery::salvage::SalvageEvidenceSerializer::sha256_hex(sdi_json);
        schema.schema_source = "SDI";
        schema.trust_level = "AUTHORITATIVE";
        return schema;
    } catch (const std::exception& e) {
        std::cerr << "[SDI] JSON parse error: " << e.what() << std::endl;
        return std::nullopt;
    }
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_SDI_EXTRACTOR_HPP

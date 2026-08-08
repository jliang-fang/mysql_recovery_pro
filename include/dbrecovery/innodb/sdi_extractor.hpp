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
#include <zlib.h>
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/sdi_manager.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include <nlohmann/json.hpp>

namespace dbrecovery {
namespace innodb {

// SDI decompression safety limits
constexpr size_t SDI_MAX_UNCOMPRESSED_SIZE = 16 * 1024 * 1024;  // 16MB
constexpr size_t SDI_MAX_COMPRESSION_RATIO = 256;  // max 256:1 compression ratio

// SDI 提取的结构体定义（在类之前声明）
struct ExtractedSDI {
    uint32_t page_no;              // 页号
    size_t offset;                 // SDI 在页中的偏移
    std::string sdi_json;          // SDI JSON 字符串
    bool is_valid;                 // 是否有效
    std::string error_message;     // 错误信息（如果无效）

    std::string to_string() const {
        std::ostringstream oss;
        oss << "ExtractedSDI{page_no=" << page_no
            << ", offset=" << offset
            << ", valid=" << std::boolalpha << is_valid << std::noboolalpha
            << ", json_len=" << sdi_json.size() << "}";
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
     * @brief 从损坏的页中提取 SDI（尝试多种偏移）
     * @param page_data 页数据
     * @param page_size 页大小
     * @return 提取的 SDI 数据
     */
    std::vector<ExtractedSDI> extract_from_page(const uint8_t* page_data, size_t page_size);

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
    // Support both uncompressed SDI (0x45BE) and compressed SDI (0x45BD)
    return type == FIL_PAGE_SDI || type == FIL_PAGE_SDI_ZIP;
}

inline std::optional<std::vector<uint8_t>> SDIExtractor::extract_sdi_payload(const uint8_t* page_data, size_t page_size) {
    if (!page_data || page_size < 100) {
        return std::nullopt;
    }

    // 读取页类型 (at offset 24, big-endian)
    uint16_t type = read_be<uint16_t>(page_data, FIL_PAGE_TYPE_ABS);
    if (type != FIL_PAGE_SDI && type != FIL_PAGE_SDI_ZIP) {
        return std::nullopt;
    }

    // For compressed SDI (0x45BD), extract the slot offset
    // offset = struct.unpack('>h',data[97:99])[0] + 99
    if (type == FIL_PAGE_SDI_ZIP) {
        int16_t slot_offset = static_cast<int16_t>(
            (static_cast<uint16_t>(page_data[97]) << 8) |
            static_cast<uint16_t>(page_data[98]));
        uint32_t record_offset = static_cast<uint32_t>(slot_offset + 99);

        if (record_offset > 0 && record_offset < page_size - 50) {
            // Read lengths at offset+25 and offset+29 (big-endian)
            uint32_t uncompressed_len = read_be<uint32_t>(page_data, record_offset + 25);
            uint32_t compressed_len = read_be<uint32_t>(page_data, record_offset + 29);

            if (compressed_len > 0 && compressed_len < page_size &&
                record_offset + 33 + compressed_len <= page_size) {

                // Safety: reject oversized uncompressed length
                if (uncompressed_len > SDI_MAX_UNCOMPRESSED_SIZE) {
                    return std::nullopt;
                }

                // Safety: reject extreme compression ratios
                if (compressed_len > 0 && uncompressed_len / compressed_len > SDI_MAX_COMPRESSION_RATIO) {
                    return std::nullopt;
                }

                // Decompress using raw deflate
                const uint8_t* compressed_data = page_data + record_offset + 33;

                z_stream strm = {};
                strm.next_in = const_cast<Bytef*>(compressed_data);
                strm.avail_in = compressed_len;

                std::vector<uint8_t> decompressed(uncompressed_len > 0 ? uncompressed_len : compressed_len * 4);
                strm.next_out = decompressed.data();
                strm.avail_out = decompressed.size();

                int ret = inflateInit2(&strm, -15);  // Raw deflate (no zlib header)
                if (ret == Z_OK) {
                    do {
                        ret = inflate(&strm, Z_NO_FLUSH);
                        if (ret == Z_BUF_ERROR && strm.avail_out == 0) {
                            decompressed.resize(decompressed.size() * 2);
                            strm.next_out = decompressed.data() + strm.total_out;
                            strm.avail_out = decompressed.size() - strm.total_out;
                        }
                    } while (ret == Z_OK);

                    if (ret == Z_STREAM_END) {
                        decompressed.resize(strm.total_out);
                        inflateEnd(&strm);
                        return decompressed;
                    }
                    inflateEnd(&strm);
                }
            }
        }
    }

    // For uncompressed SDI (0x45BE), try simple offset
    // SDI 数据从页头之后开始
    // Page Header 从偏移 38 开始，SDI 负载在 Page Header 之后
    // SDI 格式：[type:4] [id:8] [uncompressed_len:4] [compressed_len:4] [data...]

    // 尝试从不同偏移提取 SDI
    // SDI 负载通常在偏移 38 + 32 = 70 之后
    constexpr size_t MIN_SDI_OFFSET = 70;
    constexpr size_t MAX_SDI_OFFSET = 200;

    for (size_t offset = MIN_SDI_OFFSET; offset < std::min(MAX_SDI_OFFSET, page_size - 20); offset++) {
        if (offset + 20 > page_size) {
            break;
        }

        // 检查是否可能是 SDI 头部
        uint32_t sdi_type = read_le<uint32_t>(page_data, offset);
        uint64_t id = read_le<uint64_t>(page_data, offset + 4);
        uint32_t uncompressed_len = read_le<uint32_t>(page_data, offset + 12);
        uint32_t compressed_len = read_le<uint32_t>(page_data, offset + 16);

        // 验证 SDI 头部
        if (sdi_type <= 10 && id > 0 && uncompressed_len > 0 && compressed_len > 0 &&
            compressed_len <= uncompressed_len) {
            // 可能是有效的 SDI
            std::vector<uint8_t> payload(page_size - offset);
            std::copy(page_data + offset, page_data + page_size, payload.begin());
            return payload;
        }
    }

    return std::nullopt;
}

inline std::optional<ExtractedSDI> SDIExtractor::extract_sdi_at_offset(const uint8_t* page_data, size_t page_size, size_t offset) {
    if (!page_data || offset + 20 > page_size) {
        return std::nullopt;
    }

    ExtractedSDI extracted{};
    extracted.page_no = read_le<uint32_t>(page_data, 20);  // 页号
    extracted.offset = offset;
    extracted.is_valid = false;

    // 读取 SDI 头部
    uint32_t type = read_le<uint32_t>(page_data, offset);
    uint64_t id = read_le<uint64_t>(page_data, offset + 4);
    uint32_t uncompressed_len = read_le<uint32_t>(page_data, offset + 12);
    uint32_t compressed_len = read_le<uint32_t>(page_data, offset + 16);

    // 验证 SDI 头部
    if (type > 10 || id == 0 || uncompressed_len == 0 || compressed_len == 0 ||
        compressed_len > uncompressed_len) {
        extracted.error_message = "Invalid SDI header";
        return extracted;
    }

    // 提取 SDI 数据
    constexpr size_t MAX_SDI_SIZE = 16 * 1024 * 1024;  // 16 MB
    if (uncompressed_len > MAX_SDI_SIZE) {
        extracted.error_message = "SDI too large";
        return extracted;
    }

    const uint8_t* raw_data = page_data + offset + 20;
    size_t data_len = compressed_len > 0 ? compressed_len : uncompressed_len;

    if (offset + 20 + data_len > page_size) {
        extracted.error_message = "SDI data exceeds page boundary";
        return extracted;
    }

    // 解压缩 SDI
    std::vector<uint8_t> decompressed(uncompressed_len);

    z_stream strm = {};
    strm.next_in = const_cast<Bytef*>(raw_data);
    strm.avail_in = static_cast<uInt>(data_len);
    strm.next_out = decompressed.data();
    strm.avail_out = static_cast<uInt>(decompressed.size());

    int ret = inflateInit2(&strm, -15);  // -15 表示 raw deflate
    if (ret != Z_OK) {
        extracted.error_message = "Failed to initialize inflation";
        return extracted;
    }

    do {
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_BUF_ERROR) {
            size_t new_size = std::min(decompressed.size() * 2, MAX_SDI_SIZE);
            if (new_size <= decompressed.size()) break;
            decompressed.resize(new_size);
            strm.next_out = decompressed.data() + strm.total_out;
            strm.avail_out = static_cast<uInt>(decompressed.size() - strm.total_out);
        } else if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&strm);
            extracted.error_message = "Failed to decompress SDI";
            return extracted;
        }
    } while (ret != Z_STREAM_END);

    decompressed.resize(strm.total_out);
    inflateEnd(&strm);

    // 验证压缩比
    if (data_len > 0 && decompressed.size() / data_len > 20) {
        extracted.error_message = "Unreasonable compression ratio";
        return extracted;
    }

    extracted.sdi_json = std::string(reinterpret_cast<const char*>(decompressed.data()),
                                      decompressed.size());
    extracted.is_valid = true;

    return extracted;
}

inline std::vector<ExtractedSDI> SDIExtractor::extract_from_page(const uint8_t* page_data, size_t page_size) {
    std::vector<ExtractedSDI> results;

    if (!page_data || page_size < 100) {
        return results;
    }

    // 检查页类型 (at offset 24, big-endian)
    uint16_t type = read_be<uint16_t>(page_data, FIL_PAGE_TYPE_ABS);
    if (type != FIL_PAGE_SDI && type != FIL_PAGE_SDI_ZIP) {
        return results;
    }

    extract_stats_.sdi_pages++;

    // For compressed SDI (0x45BD), extract the slot offset
    if (type == FIL_PAGE_SDI_ZIP) {
        int16_t slot_offset = static_cast<int16_t>(
            (static_cast<uint16_t>(page_data[97]) << 8) |
            static_cast<uint16_t>(page_data[98]));
        uint32_t record_offset = static_cast<uint32_t>(slot_offset + 99);

        if (record_offset > 0 && record_offset < page_size - 50) {
            uint32_t uncompressed_len = read_be<uint32_t>(page_data, record_offset + 25);
            uint32_t compressed_len = read_be<uint32_t>(page_data, record_offset + 29);

            if (compressed_len > 0 && compressed_len < page_size &&
                record_offset + 33 + compressed_len <= page_size) {

                const uint8_t* compressed_data = page_data + record_offset + 33;

                z_stream strm = {};
                strm.next_in = const_cast<Bytef*>(compressed_data);
                strm.avail_in = compressed_len;

                std::vector<uint8_t> decompressed(uncompressed_len > 0 ? uncompressed_len : compressed_len * 4);
                strm.next_out = decompressed.data();
                strm.avail_out = decompressed.size();

                int ret = inflateInit2(&strm, -15);  // Raw deflate (no zlib header)
                if (ret == Z_OK) {
                    do {
                        ret = inflate(&strm, Z_NO_FLUSH);
                        if (ret == Z_BUF_ERROR && strm.avail_out == 0) {
                            decompressed.resize(decompressed.size() * 2);
                            strm.next_out = decompressed.data() + strm.total_out;
                            strm.avail_out = decompressed.size() - strm.total_out;
                        }
                    } while (ret == Z_OK);

                    if (ret == Z_STREAM_END) {
                        decompressed.resize(strm.total_out);

                        ExtractedSDI extracted{};
                        extracted.page_no = read_be<uint32_t>(page_data, FIL_PAGE_OFFSET);  // 页号
                        extracted.offset = record_offset;
                        extracted.sdi_json = std::string(reinterpret_cast<const char*>(decompressed.data()),
                                                         decompressed.size());
                        extracted.is_valid = true;

                        extract_stats_.sdi_extracted++;
                        results.push_back(extracted);
                    }
                    inflateEnd(&strm);
                }
            }
        }
    }

    // For uncompressed SDI or as fallback, try different offsets
    if (results.empty()) {
        constexpr size_t MIN_SDI_OFFSET = 70;
        constexpr size_t MAX_SDI_OFFSET = 500;

        for (size_t offset = MIN_SDI_OFFSET; offset < std::min(MAX_SDI_OFFSET, page_size - 20); offset++) {
            auto extracted_opt = extract_sdi_at_offset(page_data, page_size, offset);
            if (extracted_opt.has_value()) {
                auto& extracted = extracted_opt.value();
                if (extracted.is_valid) {
                    extract_stats_.sdi_extracted++;
                    results.push_back(extracted);
                    break;  // 找到一个有效的 SDI 就停止
                }
            }
        }
    }

    if (results.empty()) {
        extract_stats_.sdi_failed++;
    }

    return results;
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

inline std::vector<ExtractedSDI> SDIExtractor::extract_from_file(const std::filesystem::path& file_path) {
    std::vector<ExtractedSDI> results;

    // 打开文件
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file) {
        return results;
    }

    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    // 分配缓冲区
    std::vector<uint8_t> buffer(std::min(file_size, size_t(1024 * 1024)));  // 1MB 缓冲

    // 扫描
    size_t offset = 0;
    while (file.readsome(reinterpret_cast<char*>(buffer.data()), buffer.size())) {
        size_t read_size = file.gcount();
        auto file_results = extract_from_buffer(buffer.data(), read_size);

        // 调整页号
        for (auto& extracted : file_results) {
            // 页号已经是相对于文件的，不需要调整
        }

        results.insert(results.end(), file_results.begin(), file_results.end());
        offset += read_size;
    }

    extract_stats_.total_size = file_size;
    return results;
}

inline std::optional<catalog::TableSchema> SDIExtractor::parse_sdi_json(const std::string& sdi_json) {
    if (sdi_json.empty()) return std::nullopt;

    try {
        auto j = nlohmann::json::parse(sdi_json);

        // MySQL 8.0 SDI format: the dd_object contains table/column metadata
        catalog::TableSchema schema;
        schema.from_json(j);

        if (schema.table_name.empty()) return std::nullopt;
        return schema;
    } catch (const std::exception& e) {
        std::cerr << "[SDI] JSON parse error: " << e.what() << std::endl;
        return std::nullopt;
    }
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_SDI_EXTRACTOR_HPP

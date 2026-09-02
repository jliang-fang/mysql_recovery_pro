/**
 * @file file_signature.hpp
 * @brief File signature detection framework for data carving
 *
 * Provides extensible file signature detection for recovering various file types
 * from raw disk images during disaster recovery.
 */

#ifndef DBRECOVERY_DISK_FILE_SIGNATURE_HPP
#define DBRECOVERY_DISK_FILE_SIGNATURE_HPP

#include <cstdint>
#include <vector>
#include <string>
#include <functional>
#include <map>
#include <memory>
#include <cstring>
#include <algorithm>

namespace dbrecovery {
namespace disk {

// ==================== Constants ====================

/// Maximum signature offset (testdisk uses 65535)
constexpr uint32_t MAX_SIG_OFFSET = 65535;

/// Maximum signature size (testdisk uses 4095)
constexpr uint32_t MAX_SIG_SIZE = 4095;

/// Maximum reasonable file size (2TB)
constexpr uint64_t MAX_FILE_SIZE = 2ULL * 1024 * 1024 * 1024;

// ==================== Data Check Result ====================

/**
 * @brief Result of data check operation
 * Reference: testdisk data_check_t
 */
enum class DataCheckResult : uint8_t {
    SCAN = 0,       // Continue scanning
    CONTINUE = 1,   // Continue with this file
    STOP = 2,       // Stop - file found
    ERROR = 3       // Error occurred
};

// ==================== File Signature Hint ====================

/**
 * @brief File signature definition (similar to file_hint_t in testdisk)
 *
 * Usage:
 * 1. Define signature bytes and offset
 * 2. Optionally provide a header_check function for complex validation
 * 3. Optionally provide a data_check function for streaming validation
 */
struct FileSignatureHint {
    std::string name;               // Signature name (e.g., "JPEG", "PNG")
    std::string extension;          // File extension (e.g., "jpg", "png")
    std::string description;        // Human-readable description
    uint64_t max_filesize;          // Maximum file size (0 = unlimited)

    // Signature pattern
    uint32_t offset;                // Offset where signature appears
    std::vector<uint8_t> signature; // Binary signature bytes
    std::vector<uint8_t> mask;      // Mask for wildcard bytes (0xFF = exact match)

    // Optional: Custom header validation
    // Returns true if header is valid
    std::function<bool(const uint8_t* data, uint32_t size)> header_check;

    // Optional: Data check for streaming validation
    // Used to verify file continues correctly
    std::function<DataCheckResult(const uint8_t* data, uint32_t size,
                                   uint64_t current_offset)> data_check;

    // Optional: File size calculation from header
    std::function<uint64_t(const uint8_t* data, uint32_t size)> calculate_size;

    bool enabled_by_default;
    int priority;                   // Lower = check first

    FileSignatureHint()
        : max_filesize(0), offset(0), enabled_by_default(true), priority(100) {}
};

// ==================== File Detection Result ====================

struct FileDetectionResult {
    bool detected = false;
    std::string name;
    std::string extension;
    uint64_t start_offset = 0;
    uint64_t calculated_size = 0;
    uint64_t min_size = 0;
    const FileSignatureHint* hint = nullptr;

    static FileDetectionResult not_found() {
        return FileDetectionResult{};
    }

    static FileDetectionResult found(const std::string& name,
                                      const std::string& ext,
                                      uint64_t start,
                                      uint64_t size,
                                      const FileSignatureHint* h) {
        FileDetectionResult r;
        r.detected = true;
        r.name = name;
        r.extension = ext;
        r.start_offset = start;
        r.calculated_size = size;
        r.hint = h;
        return r;
    }
};

// ==================== Common File Signatures ====================

namespace signatures {

// JPEG signature
inline FileSignatureHint jpeg() {
    FileSignatureHint hint;
    hint.name = "JPEG";
    hint.extension = "jpg";
    hint.description = "JPEG image";
    hint.offset = 0;
    hint.signature = {0xFF, 0xD8, 0xFF};
    hint.max_filesize = 50 * 1024 * 1024;  // 50MB max
    hint.priority = 10;

    // JPEG end marker check
    hint.data_check = [](const uint8_t* data, uint32_t size, uint64_t offset) -> DataCheckResult {
        // Look for JPEG end marker 0xFF 0xD9
        for (uint32_t i = 0; i + 1 < size; i++) {
            if (data[i] == 0xFF && data[i + 1] == 0xD9) {
                return DataCheckResult::STOP;
            }
        }
        return DataCheckResult::CONTINUE;
    };

    return hint;
}

// PNG signature
inline FileSignatureHint png() {
    FileSignatureHint hint;
    hint.name = "PNG";
    hint.extension = "png";
    hint.description = "PNG image";
    hint.offset = 0;
    hint.signature = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    hint.max_filesize = 100 * 1024 * 1024;  // 100MB max
    hint.priority = 20;

    // PNG chunk parsing for size calculation
    hint.calculate_size = [](const uint8_t* data, uint32_t size) -> uint64_t {
        if (size < 16) return 0;
        // PNG has chunk structure, look for IEND chunk
        // This is a simplified check - real implementation would parse chunks
        return 0;  // Unknown, need to scan for IEND
    };

    return hint;
}

// GZIP signature
inline FileSignatureHint gzip() {
    FileSignatureHint hint;
    hint.name = "GZIP";
    hint.extension = "gz";
    hint.description = "GZIP compressed";
    hint.offset = 0;
    hint.signature = {0x1F, 0x8B};
    hint.max_filesize = MAX_FILE_SIZE;
    hint.priority = 30;
    return hint;
}

// ZIP signature
inline FileSignatureHint zip() {
    FileSignatureHint hint;
    hint.name = "ZIP";
    hint.extension = "zip";
    hint.description = "ZIP archive";
    hint.offset = 0;
    hint.signature = {0x50, 0x4B, 0x03, 0x04};
    hint.max_filesize = MAX_FILE_SIZE;
    hint.priority = 25;
    return hint;
}

// PDF signature
inline FileSignatureHint pdf() {
    FileSignatureHint hint;
    hint.name = "PDF";
    hint.extension = "pdf";
    hint.description = "PDF document";
    hint.offset = 0;
    hint.signature = {0x25, 0x50, 0x44, 0x46};  // %PDF
    hint.max_filesize = MAX_FILE_SIZE;
    hint.priority = 35;

    hint.data_check = [](const uint8_t* data, uint32_t size, uint64_t offset) -> DataCheckResult {
        // Look for %%EOF marker
        const char* eof_marker = "%%EOF";
        for (uint32_t i = 0; i + 4 < size; i++) {
            if (std::memcmp(data + i, eof_marker, 5) == 0) {
                return DataCheckResult::STOP;
            }
        }
        return DataCheckResult::CONTINUE;
    };

    return hint;
}

// SQLite/IBD signature (InnoDB uses similar structure)
inline FileSignatureHint sqlite() {
    FileSignatureHint hint;
    hint.name = "SQLite";
    hint.extension = "sqlite";
    hint.description = "SQLite database";
    hint.offset = 0;
    hint.signature = {0x53, 0x51, 0x4C, 0x69, 0x74, 0x65, 0x20, 0x66, 0x6F, 0x72, 0x6D, 0x61, 0x74, 0x20, 0x33, 0x00};
    hint.max_filesize = MAX_FILE_SIZE;
    hint.priority = 40;
    return hint;
}

// MySQL IBD page signature
inline FileSignatureHint ibd_index() {
    FileSignatureHint hint;
    hint.name = "IBD_INDEX";
    hint.extension = "ibd";
    hint.description = "InnoDB index page";
    hint.offset = 24;  // FIL_PAGE_TYPE offset
    hint.signature = {0x45, 0xBF};  // FIL_PAGE_INDEX
    hint.max_filesize = 64 * 1024;  // Single page max
    hint.priority = 5;  // High priority for MySQL recovery

    hint.header_check = [](const uint8_t* data, uint32_t size) -> bool {
        if (size < 64) return false;
        // Additional validation: check LSN and page directory
        return true;
    };

    return hint;
}

// MySQL SDI page signature
inline FileSignatureHint ibd_sdi() {
    FileSignatureHint hint;
    hint.name = "IBD_SDI";
    hint.extension = "ibd";
    hint.description = "InnoDB SDI page";
    hint.offset = 24;
    hint.signature = {0x45, 0xBE};  // FIL_PAGE_SDI
    hint.max_filesize = 64 * 1024;
    hint.priority = 6;
    return hint;
}

// Get all default signatures
inline std::vector<FileSignatureHint> get_default_signatures() {
    return {
        ibd_index(),
        ibd_sdi(),
        jpeg(),
        png(),
        zip(),
        gzip(),
        pdf(),
        sqlite()
    };
}

} // namespace signatures

// ==================== File Signature Registry ====================

class FileSignatureRegistry {
public:
    static FileSignatureRegistry& instance() {
        static FileSignatureRegistry registry;
        return registry;
    }

    void register_signature(const FileSignatureHint& hint) {
        signatures_[hint.name] = hint;
        update_sorted_list();
    }

    void register_defaults() {
        for (const auto& hint : signatures::get_default_signatures()) {
            register_signature(hint);
        }
    }

    FileDetectionResult check_data(const uint8_t* data, uint32_t size,
                                    uint64_t disk_offset = 0) const {
        if (!data || size == 0) {
            return FileDetectionResult::not_found();
        }

        for (const auto& name : sorted_signatures_) {
            const auto& hint = signatures_.at(name);
            if (!hint.enabled_by_default) continue;

            // Check if signature matches at specified offset
            if (hint.offset + hint.signature.size() > size) continue;

            bool match = true;
            for (size_t i = 0; i < hint.signature.size(); i++) {
                uint8_t data_byte = data[hint.offset + i];
                uint8_t sig_byte = hint.signature[i];
                uint8_t mask_byte = (i < hint.mask.size()) ? hint.mask[i] : 0xFF;

                if ((data_byte & mask_byte) != (sig_byte & mask_byte)) {
                    match = false;
                    break;
                }
            }

            if (match) {
                // If custom header check provided, use it
                if (hint.header_check && !hint.header_check(data, size)) {
                    continue;
                }

                // Calculate size if possible
                uint64_t calc_size = hint.max_filesize;
                if (hint.calculate_size) {
                    uint64_t sz = hint.calculate_size(data, size);
                    if (sz > 0) calc_size = sz;
                }

                return FileDetectionResult::found(
                    hint.name, hint.extension, disk_offset, calc_size, &hint);
            }
        }

        return FileDetectionResult::not_found();
    }

    const FileSignatureHint* get_signature(const std::string& name) const {
        auto it = signatures_.find(name);
        return (it != signatures_.end()) ? &it->second : nullptr;
    }

    std::vector<std::string> list_signatures() const {
        return sorted_signatures_;
    }

private:
    FileSignatureRegistry() {
        register_defaults();
    }

    void update_sorted_list() {
        sorted_signatures_.clear();
        for (const auto& pair : signatures_) {
            sorted_signatures_.push_back(pair.first);
        }
        std::sort(sorted_signatures_.begin(), sorted_signatures_.end(),
                  [this](const std::string& a, const std::string& b) {
                      return signatures_.at(a).priority < signatures_.at(b).priority;
                  });
    }

    std::map<std::string, FileSignatureHint> signatures_;
    std::vector<std::string> sorted_signatures_;
};

} // namespace disk
} // namespace dbrecovery

#endif // DBRECOVERY_DISK_FILE_SIGNATURE_HPP
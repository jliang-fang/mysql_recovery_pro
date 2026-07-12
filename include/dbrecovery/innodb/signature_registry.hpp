#ifndef DBRECOVERY_INNODB_SIGNATURE_REGISTRY_HPP
#define DBRECOVERY_INNODB_SIGNATURE_REGISTRY_HPP

#include <cstdint>
#include <vector>
#include <functional>
#include <string>
#include <map>
#include <memory>

namespace dbrecovery {
namespace innodb {

/**
 * @brief Signature registration framework for InnoDB page type detection
 *
 * This allows extensible page type detection through a plugin-like architecture.
 */

/// Maximum file/page size limits (prevent memory exhaustion)
constexpr uint64_t MAX_PAGE_SIZE = 64 * 1024;         // 64KB max page size
constexpr uint64_t_MAX_FILE_SIZE = 2ULL * 1024 * 1024 * 1024;  // 2TB max file size
constexpr uint64_t_MAX_BLOB_CHAIN_SIZE = 16ULL * 1024 * 1024;   // 16MB max BLOB chain

/**
 * @brief Result of a signature check
 */
struct SignatureCheckResult {
    bool matched = false;
    uint32_t page_type = 0;
    std::string type_name;
    size_t data_offset = 0;       // Where data starts after signature
    size_t expected_size = 0;     // Expected data size (if known)
};

/**
 * @brief Signature check function type
 */
using SignatureCheckFunc = std::function<SignatureCheckResult(const uint8_t* data, size_t size)>;

/**
 * @brief Page signature hint structure
 */
struct PageSignatureHint {
    std::string name;                    // Signature name (e.g., "INDEX", "BLOB")
    std::string extension;               // Related extension (e.g., "ibd")
    std::string description;             // Human-readable description
    uint64_t max_filesize = MAX_FILE_SIZE;  // Maximum file size for this type

    // Signature patterns
    uint32_t offset = 0;                 // Offset to check
    std::vector<uint8_t> signature;      // Binary signature pattern
    std::vector<uint8_t> mask;           // Mask for wildcard bytes (optional)

    // Custom check function (for complex signatures)
    SignatureCheckFunc custom_check;

    // Is this signature enabled by default?
    bool enabled_by_default = true;

    // Priority (lower = check first)
    int priority = 100;
};

/**
 * @brief Signature registry for managing all registered signatures
 */
class SignatureRegistry {
public:
    /// Get singleton instance
    static SignatureRegistry& instance() {
        static SignatureRegistry registry;
        return registry;
    }

    /// Register a signature
    void register_signature(const PageSignatureHint& hint) {
        std::string key = hint.name;
        signatures_[key] = hint;
        // Sort by priority
        sorted_signatures_.push_back(key);
        std::sort(sorted_signatures_.begin(), sorted_signatures_.end(),
                  [this](const std::string& a, const std::string& b) {
                      return signatures_[a].priority < signatures_[b].priority;
                  });
    }

    /// Check data against all registered signatures
    SignatureCheckResult check_signatures(const uint8_t* data, size_t size) {
        for (const auto& key : sorted_signatures_) {
            const auto& hint = signatures_[key];
            if (!hint.enabled_by_default) continue;

            // Check pattern-based signature
            if (!hint.signature.empty()) {
                if (size < hint.offset + hint.signature.size()) continue;

                bool match = true;
                for (size_t i = 0; i < hint.signature.size(); i++) {
                    uint8_t expected = hint.signature[i];
                    uint8_t actual = data[hint.offset + i];
                    uint8_t mask = hint.mask.empty() ? 0xFF : hint.mask[i];

                    if ((actual & mask) != (expected & mask)) {
                        match = false;
                        break;
                    }
                }

                if (match) {
                    SignatureCheckResult result;
                    result.matched = true;
                    result.type_name = hint.name;
                    result.data_offset = hint.offset + hint.signature.size();
                    return result;
                }
            }

            // Check custom function
            if (hint.custom_check) {
                auto result = hint.custom_check(data, size);
                if (result.matched) {
                    return result;
                }
            }
        }

        return SignatureCheckResult{};
    }

    /// Enable/disable a signature by name
    void set_signature_enabled(const std::string& name, bool enabled) {
        if (signatures_.count(name)) {
            signatures_[name].enabled_by_default = enabled;
        }
    }

    /// Get all registered signatures
    const std::map<std::string, PageSignatureHint>& get_signatures() const {
        return signatures_;
    }

private:
    SignatureRegistry() {
        // Register built-in InnoDB signatures
        register_builtin_signatures();
    }

    void register_builtin_signatures() {
        // INDEX page signature (FIL_PAGE_INDEX = 0x45BF)
        PageSignatureHint index_hint;
        index_hint.name = "INDEX";
        index_hint.extension = "ibd";
        index_hint.description = "InnoDB B+Tree index page";
        index_hint.offset = 24;  // FIL_PAGE_TYPE offset
        index_hint.signature = {0x45, 0xBF};  // Big-endian 0x45BF
        index_hint.priority = 1;
        register_signature(index_hint);

        // SDI page signature (FIL_PAGE_SDI = 0x45BE)
        PageSignatureHint sdi_hint;
        sdi_hint.name = "SDI";
        sdi_hint.extension = "ibd";
        sdi_hint.description = "InnoDB SDI metadata page";
        sdi_hint.offset = 24;
        sdi_hint.signature = {0x45, 0xBE};
        sdi_hint.priority = 2;
        register_signature(sdi_hint);

        // BLOB page signature (FIL_PAGE_TYPE_BLOB = 0x45BD)
        PageSignatureHint blob_hint;
        blob_hint.name = "BLOB";
        blob_hint.extension = "ibd";
        blob_hint.description = "InnoDB BLOB page";
        blob_hint.offset = 24;
        blob_hint.signature = {0x45, 0xBD};
        blob_hint.priority = 3;
        register_signature(blob_hint);

        // Compressed page signature (FIL_PAGE_COMPRESSED = 0x45B5)
        PageSignatureHint compressed_hint;
        compressed_hint.name = "COMPRESSED";
        compressed_hint.extension = "ibd";
        compressed_hint.description = "InnoDB compressed page";
        compressed_hint.offset = 24;
        compressed_hint.signature = {0x45, 0xB5};
        compressed_hint.priority = 4;
        register_signature(compressed_hint);

        // Undo log page signature (FIL_PAGE_UNDO_LOG = 0x45B2)
        PageSignatureHint undo_hint;
        undo_hint.name = "UNDO";
        undo_hint.extension = "ibd";
        undo_hint.description = "InnoDB undo log page";
        undo_hint.offset = 24;
        undo_hint.signature = {0x45, 0xB2};
        undo_hint.priority = 5;
        register_signature(undo_hint);

        // FSP header page (page 0) - custom check
        PageSignatureHint fsp_hint;
        fsp_hint.name = "FSP";
        fsp_hint.extension = "ibd";
        fsp_hint.description = "InnoDB file space header (page 0)";
        fsp_hint.priority = 0;  // Check first
        fsp_hint.custom_check = [](const uint8_t* data, size_t size) -> SignatureCheckResult {
            if (size < 38) return SignatureCheckResult{};

            // Page 0 should have page number = 0 at offset 4
            uint32_t page_no = read_be<uint32_t>(data, 4);
            if (page_no == 0) {
                SignatureCheckResult result;
                result.matched = true;
                result.type_name = "FSP";
                return result;
            }
            return SignatureCheckResult{};
        };
        register_signature(fsp_hint);
    }

    std::map<std::string, PageSignatureHint> signatures_;
    std::vector<std::string> sorted_signatures_;
};

/**
 * @brief Convenience function to register a custom signature
 */
inline void register_page_signature(const PageSignatureHint& hint) {
    SignatureRegistry::instance().register_signature(hint);
}

/**
 * @brief Convenience function to check page type using signatures
 */
inline SignatureCheckResult check_page_signature(const uint8_t* data, size_t size) {
    return SignatureRegistry::instance().check_signatures(data, size);
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_SIGNATURE_REGISTRY_HPP
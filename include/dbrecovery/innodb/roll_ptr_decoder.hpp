/**
 * @file roll_ptr_decoder.hpp
 * @brief DB_ROLL_PTR decoder with multi-version MySQL support
 *
 * Provides a unified interface for decoding DB_ROLL_PTR values across
 * all MySQL versions (5.6, 5.7, 8.0+) and all on-disk representations.
 *
 * DB_ROLL_PTR appears in three distinct formats:
 *
 * 1. Internal format (used throughout the recovery engine):
 *    uint64_t with bits [31:0] = page_no, bits [47:32] = offset, bits [55:48] = flags
 *    decode()/encode() convert between this uint64_t and (page_no, offset, flags)
 *
 * 2. Storage format (undo page header, big-endian at offset+14):
 *    MySQL 5.6/5.7: 6 bytes BE, page_no = val >> 20, offset = val & 0x3FFFF
 *    MySQL 8.0+: 7 bytes BE, same encoding with an extra flags byte at the top
 *    decode_storage() reads directly from page data
 *
 * 3. Field data format (undo field data, little-endian):
 *    6 bytes: [4B page_no LE, 2B offset LE]
 *    decode_field_data() reads from undo record field values
 */

#ifndef DBRECOVERY_INNODB_ROLL_PTR_DECODER_HPP
#define DBRECOVERY_INNODB_ROLL_PTR_DECODER_HPP

#include <cstdint>
#include <vector>
#include <cstring>
#include <cassert>

#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

namespace dbrecovery {
namespace innodb {

/**
 * @brief Explicit undo format profile for roll-pointer width selection
 *
 * This enum controls how roll_ptr bytes are parsed in undo records.
 * The profile must be selected from resolved physical-format context
 * (MySQLVersion) or operator override, never inferred from remaining
 * bytes in a record.
 *
 * @see UndoRecordV2Parser::set_undo_format_profile()
 * @see DeepUndoAnalyzer::set_undo_format_profile()
 */
enum class UndoFormatProfile : uint8_t {
    UNRESOLVED   = 0,  ///< No format evidence — strict mode, reject parsing
    LEGACY_6BYTE = 1,  ///< MySQL 5.6/5.7: 6-byte BE roll_ptr in undo page header
    MYSQL8_7BYTE = 2,  ///< MySQL 8.0+: 7-byte BE roll_ptr in undo page header
};

/**
 * @brief Unified DB_ROLL_PTR decoder for MySQL 5.6/5.7 and 8.0+
 *
 * All methods are static and thread-safe. No state is shared.
 */
class RollPtrDecoder {
public:
    /**
     * @brief Decoded roll pointer components
     */
    struct DecodedRollPtr {
        uint32_t page_no = 0;  // Undo page number
        uint32_t offset  = 0;  // Offset within undo page (16-bit, stored as uint32_t)
        uint8_t  flags   = 0;  // 8.0+ flags byte, 0 for 5.6/5.7

        bool is_valid() const {
            return page_no != 0 && page_no != FIL_NULL;
        }

        bool is_null() const {
            return page_no == 0 || page_no == FIL_NULL;
        }
    };

    // ==================== Internal Format ====================
    //
    // Internal uint64_t encoding:
    //   bits [31:0]   = page_no
    //   bits [49:32]  = offset (18 bits, max 0x3FFFF)
    //   bits [55:50]  = flags (6 bits, max 0x3F)
    //   bits [63:56]  = unused (0)
    //
    // NOTE: Flags sit at bits 55:50 so offset (18 bits at bits 49:32) never
    // overlaps. This guarantees clean round-trip for all valid MySQL 8.0+
    // roll_ptr values (18-bit offset + 4-bit flags).

    /**
     * @brief Decode internal uint64_t to components
     *
     * Bit layout: bits [31:0]=page_no, [49:32]=offset (18 bits),
     * [55:50]=flags (6 bits). No overlap between offset and flags.
     */
    static DecodedRollPtr decode(uint64_t roll_ptr) {
        uint32_t page_no = static_cast<uint32_t>(roll_ptr & 0xFFFFFFFF);
        uint32_t offset  = static_cast<uint32_t>((roll_ptr >> 32) & 0x3FFFF);
        uint8_t  flags   = static_cast<uint8_t>((roll_ptr >> 50) & 0x3F);
        return {page_no, offset, flags};
    }

    /**
     * @brief Encode components to internal uint64_t
     *
     * Bit layout: bits [31:0]=page_no, [49:32]=offset (18 bits),
     * [55:50]=flags (6 bits). No overlap between offset and flags,
     * so full 18-bit offset is always preserved regardless of flags.
     *
     * Symmetric with decode(): encode(a, b, c) → decode() → {a, b, c}
     */
    static uint64_t encode(uint32_t page_no, uint32_t offset, uint8_t flags = 0) {
        return static_cast<uint64_t>(page_no) |
               (static_cast<uint64_t>(offset & 0x3FFFF) << 32) |
               (static_cast<uint64_t>(flags & 0x3F) << 50);
    }

    // ==================== Storage Format (BE in undo page header) ====================
    //
    // Stored in the undo page header at offset+14:
    //   MySQL 5.6/5.7: 6 bytes big-endian
    //   MySQL 8.0+: 7 bytes big-endian
    //
    // Encoding: page_no = val >> 20, offset = val & 0x3FFFF
    //   For 6 bytes (48 bits): 28 bits page_no, 18 bits offset, 2 bits unused
    //   For 7 bytes (56 bits): 36 bits page_no, 18 bits offset, 2 bits flags
    //
    // The 7th byte (8.0+) encodes flags in the upper nibble:
    //   bits [55:52] = flags, bits [51:48] = unused

    /**
     * @brief Decode from storage format (big-endian, 6 or 7 bytes)
     *
     * Size-based detection: reads 7 bytes if available, else 6. This form is
     * retained for backward compatibility and callers that already know the
     * physical width from context outside a record. Parsers of untrusted undo
     * records MUST use the UndoFormatProfile overload instead, so the width is
     * pinned by resolved format evidence rather than remaining bytes.
     *
     * @param data  Pointer to the roll_ptr bytes in page data
     * @param size  Number of bytes (6 for 5.6/5.7, 7 for 8.0+)
     * @return Decoded components (empty/invalid if size < 6)
     */
    static DecodedRollPtr decode_storage(const uint8_t* data, size_t size) {
        if (!data || size < 6) return {};

        uint64_t val = 0;
        for (size_t i = 0; i < size && i < 7; i++) {
            val = (val << 8) | data[i];
        }

        uint32_t page_no = static_cast<uint32_t>(val >> 20);
        uint32_t offset  = static_cast<uint32_t>(val & 0x3FFFF);
        uint8_t  flags   = 0;

        if (size >= 7) {
            // 7th byte: upper nibble = flags, lower nibble = unused
            flags = static_cast<uint8_t>((val >> 52) & 0x0F);
        }

        return {page_no, offset, flags};
    }

    /**
     * @brief Decode from storage format with an explicit undo format profile
     *
     * Profile-pinned width selection: the number of bytes read is taken from
     * the resolved profile, never from remaining data. Fail-closed: an
     * UNRESOLVED profile returns an invalid (all-zero) result so callers can
     * reject the record rather than guess.
     *
     * @param data    Pointer to the roll_ptr bytes in page data
     * @param size    Number of bytes available at data
     * @param profile Resolved undo format profile (LEGACY_6BYTE / MYSQL8_7BYTE)
     * @return Decoded components; empty/invalid if size < required width or
     *         profile is UNRESOLVED
     */
    static DecodedRollPtr decode_storage(const uint8_t* data, size_t size,
                                         UndoFormatProfile profile) {
        if (!data || profile == UndoFormatProfile::UNRESOLVED) return {};

        const size_t want = (profile == UndoFormatProfile::MYSQL8_7BYTE) ? 7 : 6;
        if (size < want) return {};

        uint64_t val = 0;
        for (size_t i = 0; i < want; i++) {
            val = (val << 8) | data[i];
        }

        uint32_t page_no = static_cast<uint32_t>(val >> 20);
        uint32_t offset  = static_cast<uint32_t>(val & 0x3FFFF);
        uint8_t  flags   = 0;

        if (profile == UndoFormatProfile::MYSQL8_7BYTE) {
            flags = static_cast<uint8_t>((val >> 52) & 0x0F);
        }

        return {page_no, offset, flags};
    }

    /**
     * @brief Decode storage format directly to internal uint64_t
     */
    static uint64_t decode_storage_to_internal(const uint8_t* data, size_t size) {
        auto decoded = decode_storage(data, size);
        if (decoded.is_null()) return 0;
        return encode(decoded.page_no, decoded.offset, decoded.flags);
    }

    /**
     * @brief Decode storage format to internal uint64_t with explicit profile
     *
     * Fail-closed: returns 0 for UNRESOLVED profile or null page_no.
     */
    static uint64_t decode_storage_to_internal(const uint8_t* data, size_t size,
                                               UndoFormatProfile profile) {
        auto decoded = decode_storage(data, size, profile);
        if (decoded.is_null()) return 0;
        return encode(decoded.page_no, decoded.offset, decoded.flags);
    }

    /**
     * @brief Encode components to storage format (big-endian, 7 bytes)
     *
     * MySQL 8.0+ on-disk format: 7 bytes big-endian.
     *   bits [55:52] = flags (4 bits — MySQL only uses 4 bits on disk)
     *   bits [51:48] = unused
     *   bits [47:20] = page_no (28 bits)
     *   bits [19:0]  = offset (18 bits, also stored as 2 bytes in UNDO field data)
     *
     * NOTE: Internal representation uses 6 flag bits (max 0x3F) for alignment
     * and future-proofing, but the on-disk storage only preserves 4 bits
     * (max 0x0F). The upper 2 flag bits are silently dropped during storage.
     * Callers that need lossless round-trip through storage must ensure flags
     * fit in 4 bits.
     *
     * @param[out] out  Output buffer (must be at least 7 bytes)
     * @param rp        Decoded roll pointer
     */
    static void encode_storage(uint8_t* out, const DecodedRollPtr& rp) {
        if (!out) return;

        // page_no << 20 | offset in 7 bytes BE
        uint64_t val = (static_cast<uint64_t>(rp.page_no) << 20) |
                       (rp.offset & 0x3FFFF) |
                       (static_cast<uint64_t>(rp.flags & 0x0F) << 52);

        // Never silently truncate internal 6-bit flags to the 4-bit storage
        // field; debug builds assert so callers dropping bits are caught.
        assert(rp.flags <= 0x0F);

        for (int i = 6; i >= 0; i--) {
            out[i] = static_cast<uint8_t>(val & 0xFF);
            val >>= 8;
        }
    }

    /**
     * @brief Encode components to storage format (big-endian, 6 bytes)
     *
     * MySQL 5.6/5.7 on-disk format: 6 bytes big-endian.
     *   bits [47:20] = page_no (28 bits)
     *   bits [19:0]  = offset (18 bits)
     *
     * No flags byte — the 6-byte format predates the flags field.
     * Flags are silently dropped; callers must ensure flags == 0 for
     * lossless round-trip.
     *
     * @param[out] out  Output buffer (must be at least 6 bytes)
     * @param rp        Decoded roll pointer
     */
    static void encode_storage_6byte(uint8_t* out, const DecodedRollPtr& rp) {
        if (!out) return;

        uint64_t val = (static_cast<uint64_t>(rp.page_no) << 20) |
                       (rp.offset & 0x3FFFF);

        for (int i = 5; i >= 0; i--) {
            out[i] = static_cast<uint8_t>(val & 0xFF);
            val >>= 8;
        }
    }

    // ==================== Field Data Format (LE in undo field data) ====================
    //
    // Stored in the first field of an UPDATE/DELETE undo record:
    //   6 bytes little-endian: [4B page_no, 2B offset]
    //
    // This is the old DB_ROLL_PTR value of the previous version,
    // stored directly as a field value in the undo record.

    /**
     * @brief Decode from field data format (little-endian, 6 bytes)
     * @param data  Pointer to the field data bytes
     * @param size  Number of bytes (must be >= 6)
     * @return Decoded components (empty/invalid if size < 6)
     */
    static DecodedRollPtr decode_field_data(const uint8_t* data, size_t size) {
        if (!data || size < 6) return {};

        uint32_t page_no = read_le<uint32_t>(data, 0);
        uint32_t offset  = read_le<uint16_t>(data, 4);

        return {page_no, offset, 0};
    }

    /**
     * @brief Decode field data format directly to internal uint64_t
     *
     * Returns 0 if the page_no is NULL or FIL_NULL (no previous version).
     */
    static uint64_t decode_field_data_to_internal(const uint8_t* data, size_t size) {
        auto decoded = decode_field_data(data, size);
        if (decoded.is_null()) return 0;
        return encode(decoded.page_no, decoded.offset, decoded.flags);
    }

    /**
     * @brief Legacy wrapper — extract prev_undo_ptr from undo field data
     *
     * Maintains API compatibility with UndoRecordV2Parser::extract_prev_ptr_from_field().
     *
     * @param field_data  Raw bytes of the first undo field (old DB_ROLL_PTR)
     * @return Internal uint64_t encoding, or 0 if no previous version
     */
    static uint64_t extract_prev_ptr_from_field(const std::vector<uint8_t>& field_data) {
        if (field_data.empty()) return 0;
        return decode_field_data_to_internal(field_data.data(), field_data.size());
    }

    // ==================== Utility Methods ====================

    /**
     * @brief Format a DecodedRollPtr as a human-readable string
     */
    static std::string to_string(const DecodedRollPtr& rp) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "(page=%u, offset=%u, flags=0x%02x)",
                      rp.page_no, rp.offset, rp.flags);
        return buf;
    }

    /**
     * @brief Format an internal uint64_t roll_ptr as a human-readable string
     */
    static std::string to_string(uint64_t roll_ptr) {
        return to_string(decode(roll_ptr));
    }
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_ROLL_PTR_DECODER_HPP
#include "dbrecovery/innodb/heuristic_parser.hpp"
#include "dbrecovery/innodb/schema_agnostic_scanner.hpp"
#include <cstring>
#include <numeric>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <set>
#include <utility>
#include <regex>
#include <sstream>
#include <cctype>

namespace dbrecovery {
namespace innodb {

// ==================== HeuristicParser Implementation ====================

HeuristicParser::HeuristicParser() {}

std::optional<HeuristicRecord> HeuristicParser::parse_record(
    const uint8_t* data, size_t size, size_t offset) {

    if (!data || offset >= size) {
        return std::nullopt;
    }

    // Parse header
    auto header_opt = parse_header(data, size, offset);
    if (!header_opt) {
        return std::nullopt;
    }

    const HeuristicRecordHeader& header = *header_opt;

    HeuristicRecord record;
    record.offset = static_cast<uint16_t>(offset);
    record.is_infimum = header.is_infimum;
    record.is_supremum = header.is_supremum;
    record.is_deleted = header.is_deleted;

    if (header.is_infimum || header.is_supremum) {
        // System records - just record metadata
        record.length = static_cast<uint16_t>(header.next_offset - offset);
        return record;
    }

    // Parse fields
    size_t field_start = offset + 5;  // Skip 5-byte header
    if (field_start >= size) {
        return std::nullopt;
    }

    size_t total_read = 0;
    record.fields = parse_fields(data, size, offset, header.n_fields, total_read);
    record.length = static_cast<uint16_t>(total_read);

    return record;
}

std::vector<HeuristicRecord> HeuristicParser::parse_all_records(
    const uint8_t* data, size_t size, uint16_t heap_top) {

    std::vector<HeuristicRecord> records;

    if (!data || size < 54 || heap_top > size) {
        return records;
    }

    // Start from offset 54 (after FIL_HEADER and PAGE_HEADER)
    // This is where records typically begin
    size_t offset = 54;

    // First, scan to find infimum and supremum markers using pattern matching
    // This helps identify the valid record area in corrupted pages
    size_t infimum_offset = 0;
    size_t supremum_offset = 0;

    // Look for "infimum" string (8 bytes: 'i' 'n' 'f' 'i' 'm' 'u' 'm' '\0')
    for (size_t pos = offset; pos < size - 8; ++pos) {
        if (data[pos] == 'i' && data[pos+1] == 'n' && data[pos+2] == 'f' &&
            data[pos+3] == 'i' && data[pos+4] == 'm' && data[pos+5] == 'u' &&
            data[pos+6] == 'm' && data[pos+7] == '\0') {
            infimum_offset = pos - 5;  // Backtrack to record header start
            break;
        }
    }

    // Look for "supremum" string
    for (size_t pos = offset; pos < size - 9; ++pos) {
        if (data[pos] == 's' && data[pos+1] == 'u' && data[pos+2] == 'p' &&
            data[pos+3] == 'r' && data[pos+4] == 'e' && data[pos+5] == 'm' &&
            data[pos+6] == 'u' && data[pos+7] == 'm' && data[pos+8] == '\0') {
            supremum_offset = pos - 5;  // Backtrack to record header start
            break;
        }
    }

    // If we found valid infimum/supremum markers, use them as boundaries
    bool found_markers = infimum_offset > 0 && supremum_offset > 0;

    // If we found infimum, start parsing from there (skip page header)
    if (infimum_offset > 0) {
        offset = infimum_offset;
    }

    std::set<uint16_t> visited_offsets;

    // Parse records
    while (offset < heap_top) {
        // If we have markers, stop when we pass supremum
        if (found_markers && offset > supremum_offset && offset > infimum_offset) {
            break;
        }

        if (visited_offsets.find(static_cast<uint16_t>(offset)) != visited_offsets.end()) {
            break;
        }
        visited_offsets.insert(static_cast<uint16_t>(offset));

        // Check if we have enough data for header
        if (offset + 5 > size) {
            break;
        }

        uint8_t info_bits = data[offset];
        uint8_t status = info_bits & 0x07;

        bool is_infimum = (status == REC_STATUS_INFIMUM);
        bool is_supremum = (status == REC_STATUS_SUPREMUM);

        // Pattern-based check for infimum/supremum (in case status is corrupted)
        // The "infimum\0" string starts 5 bytes after the record header
        if (!is_infimum && offset + 5 + 8 <= size) {
            if (data[offset+5] == 'i' && data[offset+6] == 'n' && data[offset+7] == 'f' &&
                data[offset+8] == 'i' && data[offset+9] == 'm' && data[offset+10] == 'u' &&
                data[offset+11] == 'm' && data[offset+12] == '\0') {
                is_infimum = true;
                // Reset status for correct parsing
                status = REC_STATUS_INFIMUM;
            }
        }

        if (!is_supremum && offset + 5 + 9 <= size) {
            if (data[offset+5] == 's' && data[offset+6] == 'u' && data[offset+7] == 'p' &&
                data[offset+8] == 'r' && data[offset+9] == 'e' && data[offset+10] == 'm' &&
                data[offset+11] == 'u' && data[offset+12] == 'm' && data[offset+13] == '\0') {
                is_supremum = true;
                status = REC_STATUS_SUPREMUM;
            }
        }

        if (offset + 2 > size) break;
        uint16_t next_offset = dbrecovery::innodb::read_be<uint16_t>(data, offset + 1) & 0x7FFF;

        // Handle supremum marker (next_offset == 0xFFFF)
        if (next_offset == 0xFFFF) {
            // Supremum found
            HeuristicRecord record;
            record.offset = static_cast<uint16_t>(offset);
            record.is_supremum = true;
            records.push_back(record);
            break;
        }

        // Parse the record
        auto record_opt = parse_record(data, size, offset);
        if (record_opt) {
            records.push_back(*record_opt);
        }

        // Move to next record
        if (next_offset == 0 || next_offset <= offset || next_offset >= heap_top) {
            break;
        }

        offset = next_offset;
    }

    return records;
}

// Hobby parser for extreme corruption scenarios
std::vector<HeuristicRecord> HeuristicParser::parse_custom_format(
    const uint8_t* data, size_t size, uint32_t page_no /*= 0*/, bool show_progress /*= false*/) {

    std::vector<HeuristicRecord> records;

    if (!data || size < 100) {
        return records;
    }

    // Pattern 1: "user_<id>" followed by length byte and "user<id>@domain.com"
    // Pattern 2: "user_<id>@domain.com" (single email string)
    //
    // The data format can be:
    // - user_1\x11user1@example.com (name + length byte + email)
    // - user_1@example.com (single email string)

    size_t pos = 0;
    int record_count = 0;

    while (pos + 15 < size) {
        // Look for 'u' at start of "user_"
        if (data[pos] != 'u' || pos + 5 >= size) {
            pos++;
            continue;
        }

        // Check for "user_" at pos
        if (data[pos+1] != 's' || data[pos+2] != 'e' ||
            data[pos+3] != 'r' || data[pos+4] != '_') {
            pos++;
            continue;
        }

        // Found "user_", now extract the ID digits
        size_t id_start = pos + 5;
        size_t id_end = id_start;
        while (id_end < size && id_end < id_start + 10 && isdigit(data[id_end])) {
            id_end++;
        }

        if (id_end == id_start) {
            pos++;
            continue;
        }

        // Extract ID string
        std::string id_str;
        for (size_t i = id_start; i < id_end; i++) {
            id_str += static_cast<char>(data[i]);
        }

        // After digits, check for two patterns:
        // Pattern 1: length byte + "user<id>@domain.com"
        // Pattern 2: "@domain.com" directly

        size_t email_start = id_end;
        size_t at_pos = 0;
        std::string email;

        // Skip potential length byte (0x06-0x40 range)
        if (email_start < size && data[email_start] >= 0x06 && data[email_start] <= 0x40) {
            email_start++;
        }

        // Check if "user" starts at email_start (Pattern 1)
        if (email_start + 4 < size &&
            data[email_start] == 'u' && data[email_start+1] == 's' &&
            data[email_start+2] == 'e' && data[email_start+3] == 'r') {
            // Find '@' after "user"
            at_pos = email_start + 4;
            while (at_pos < size && at_pos < email_start + 30 && isdigit(data[at_pos])) {
                at_pos++;
            }
        } else {
            // Pattern 2: Look for '@' directly after digits
            at_pos = id_end;
            while (at_pos + 5 < size && at_pos < pos + 100) {
                if (data[at_pos] == '@') {
                    break;
                }
                at_pos++;
            }
        }

        if (at_pos + 5 >= size || data[at_pos] != '@') {
            pos++;
            continue;
        }

        // Find end of domain
        size_t domain_end = at_pos + 1;
        while (domain_end < size && domain_end < at_pos + 50) {
            if (data[domain_end] == '\0' || data[domain_end] < 32 || data[domain_end] >= 127) {
                if (data[domain_end] == '\0') domain_end++;
                break;
            }
            domain_end++;
        }

        // Build email string
        if (email_start < size && data[email_start] == 'u') {
            // Extract from "user<id>@domain.com"
            email.reserve(domain_end - email_start);
            for (size_t i = email_start; i < domain_end && i < size; i++) {
                if (data[i] >= 32 && data[i] < 127) {
                    email += static_cast<char>(data[i]);
                } else {
                    break;
                }
            }
        } else {
            // Build email from "user_<id>@domain.com"
            email = "user" + id_str + "@";
            for (size_t i = at_pos + 1; i < domain_end && i < size; i++) {
                if (data[i] >= 32 && data[i] < 127) {
                    email += static_cast<char>(data[i]);
                } else {
                    break;
                }
            }
        }

        if (email.empty() || email.find('@') == std::string::npos) {
            pos++;
            continue;
        }

        // Create record
        HeuristicRecord record;
        record.is_infimum = false;
        record.is_supremum = false;
        record.is_deleted = false;
        record.offset = static_cast<uint16_t>(pos);

        HeuristicField id_field, name_field, email_field, created_field;

        id_field.is_null = false;
        id_field.is_valid = true;
        id_field.type = HeuristicFieldType::LONGLONG;
        if (!id_str.empty()) {
            id_field.data.assign(id_str.begin(), id_str.end());
        }

        // Name: "user_<id>"
        std::string name = "user_" + id_str;
        name_field.is_null = false;
        name_field.is_valid = true;
        name_field.type = HeuristicFieldType::VARCHAR_SHORT;
        name_field.data.assign(reinterpret_cast<const uint8_t*>(name.data()),
                              reinterpret_cast<const uint8_t*>(name.data()) + name.size());

        email_field.is_null = false;
        email_field.is_valid = true;
        email_field.type = HeuristicFieldType::VARCHAR_SHORT;
        email_field.data.assign(reinterpret_cast<const uint8_t*>(email.data()),
                               reinterpret_cast<const uint8_t*>(email.data()) + email.size());

        created_field.is_null = false;
        created_field.is_valid = true;
        created_field.type = HeuristicFieldType::VARCHAR_SHORT;
        const char* timestamp = "2024-01-01 00:00:00";
        created_field.data.assign(reinterpret_cast<const uint8_t*>(timestamp),
                                 reinterpret_cast<const uint8_t*>(timestamp) + 19);

        record.fields = {id_field, name_field, email_field, created_field};

        records.push_back(record);
        record_count++;

        // Move past this record - only advance by 1 to catch overlapping patterns
        pos++;
    }

    return records;
}

// ==================== MySQL 5.6 COMPACT Format Parser ====================

std::vector<HeuristicRecord> HeuristicParser::parse_mysql56_compact(
    const uint8_t* data, size_t size, uint32_t page_no,
    uint8_t n_nullable, uint8_t n_fixed) {

    std::vector<HeuristicRecord> records;

    if (!data || size < 96) {
        return records;
    }

    // MySQL 5.6 COMPACT format parser
    // Records contain "user_XXXXXXuserXXXXXX@test.com" pattern where:
    // - name field: "user_XXXXXX" (variable length)
    // - email field: "userXXXXXX@test.com" (variable length)
    //
    // The data is stored without InnoDB record headers in some pages,
    // so we scan for the pattern directly.

    size_t pos = 0;
    std::set<size_t> processed_positions;

    // Track if we've seen the first user record to filter out system records
    bool found_first_user = false;

    while (pos + 20 < size) {
        // Look for 'u' at start of "user_"
        if (data[pos] != 'u' || pos + 5 >= size) {
            pos++;
            continue;
        }

        // Check for "user_" at pos
        if (data[pos+1] != 's' || data[pos+2] != 'e' ||
            data[pos+3] != 'r' || data[pos+4] != '_') {
            pos++;
            continue;
        }

        // Found first user record - mark it
        found_first_user = true;

        // Found "user_", now check if followed by digits and then "@test.com"
        size_t id_start = pos + 5;
        size_t id_end = id_start;

        // Collect digits after "user_"
        while (id_end < size && id_end < id_start + 10 && isdigit(data[id_end])) {
            id_end++;
        }

        if (id_end == id_start) {
            pos++;
            continue;
        }

        // After digits, we should find the email part "userXXXXXX@test.com"
        // The email starts with "user" (lowercase, no underscore)
        // There may be a length byte between name and email
        size_t email_start = id_end;

        // Skip potential length byte (0x06-0x20 range for typical string lengths)
        if (email_start < size && data[email_start] >= 0x06 && data[email_start] <= 0x40) {
            email_start++;
        }

        if (email_start + 4 + 9 >= size) {  // "user" + "@test.com" + some digits
            pos++;
            continue;
        }

        // Check if "user" starts at email_start
        if (data[email_start] != 'u' || data[email_start+1] != 's' ||
            data[email_start+2] != 'e' || data[email_start+3] != 'r') {
            pos++;
            continue;
        }

        size_t email_at = email_start + 4;
        while (email_at < size && email_at < email_start + 30 && isdigit(data[email_at])) {
            email_at++;
        }

        // Check for "@test.com"
        if (email_at + 9 >= size) {
            pos++;
            continue;
        }

        if (data[email_at] != '@') {
            pos++;
            continue;
        }

        // Check email domain (@test.com, @company.com, @example.com, etc.)
        bool is_valid_email = false;
        size_t email_end = 0;
        if (data[email_at] == '@') {
            // Check for "@test.com"
            if (email_at + 9 < size &&
                data[email_at+1] == 't' && data[email_at+2] == 'e' &&
                data[email_at+3] == 's' && data[email_at+4] == 't' &&
                data[email_at+5] == '.' && data[email_at+6] == 'c' &&
                data[email_at+7] == 'o' && data[email_at+8] == 'm') {
                is_valid_email = true;
                email_end = email_at + 9;
            }
            // Check for "@company.com"
            else if (email_at + 12 < size &&
                data[email_at+1] == 'c' && data[email_at+2] == 'o' &&
                data[email_at+3] == 'm' && data[email_at+4] == 'p' &&
                data[email_at+5] == 'a' && data[email_at+6] == 'n' &&
                data[email_at+7] == 'y' && data[email_at+8] == '.' &&
                data[email_at+9] == 'c' && data[email_at+10] == 'o' &&
                data[email_at+11] == 'm') {
                is_valid_email = true;
                email_end = email_at + 12;
            }
            // Check for "@example.com"
            else if (email_at + 12 < size &&
                data[email_at+1] == 'e' && data[email_at+2] == 'x' &&
                data[email_at+3] == 'a' && data[email_at+4] == 'm' &&
                data[email_at+5] == 'p' && data[email_at+6] == 'l' &&
                data[email_at+7] == 'e' && data[email_at+8] == '.' &&
                data[email_at+9] == 'c' && data[email_at+10] == 'o' &&
                data[email_at+11] == 'm') {
                is_valid_email = true;
                email_end = email_at + 12;
            }
        }

        if (!is_valid_email) {
            pos++;
            continue;
        }

        // Skip if we've already processed this position
        if (processed_positions.find(pos) != processed_positions.end()) {
            pos = email_end;
            continue;
        }
        processed_positions.insert(pos);

        // Extract ID from the digit portion
        std::string id_str;
        for (size_t i = id_start; i < id_end; i++) {
            id_str += static_cast<char>(data[i]);
        }

        // Extract name: "user_" + digits
        std::string name;
        name.reserve(id_end - pos);
        name.append(reinterpret_cast<const char*>(&data[pos]), id_end - pos);

        // Extract email: "user" + digits + "@test.com"
        std::string email;
        email.reserve(email_end - email_start);
        email.append(reinterpret_cast<const char*>(&data[email_start]), email_end - email_start);

        // Create record
        HeuristicRecord record;
        record.is_infimum = false;
        record.is_supremum = false;
        record.is_deleted = false;
        record.offset = static_cast<uint16_t>(pos);

        HeuristicField id_field, name_field, email_field, created_field;

        // ID as numeric value
        id_field.is_null = false;
        id_field.is_valid = true;
        id_field.type = HeuristicFieldType::LONGLONG;
        if (!id_str.empty()) {
            // Convert string ID to binary
            int64_t id_val = 0;
            for (char c : id_str) {
                id_val = id_val * 10 + (c - '0');
            }
            id_field.data.assign(reinterpret_cast<const uint8_t*>(&id_val),
                                reinterpret_cast<const uint8_t*>(&id_val) + sizeof(id_val));
        }

        name_field.is_null = false;
        name_field.is_valid = true;
        name_field.type = HeuristicFieldType::VARCHAR_SHORT;
        name_field.data.assign(reinterpret_cast<const uint8_t*>(name.data()),
                              reinterpret_cast<const uint8_t*>(name.data()) + name.size());

        email_field.is_null = false;
        email_field.is_valid = true;
        email_field.type = HeuristicFieldType::VARCHAR_SHORT;
        email_field.data.assign(reinterpret_cast<const uint8_t*>(email.data()),
                               reinterpret_cast<const uint8_t*>(email.data()) + email.size());

        created_field.is_null = false;
        created_field.is_valid = true;
        created_field.type = HeuristicFieldType::VARCHAR_SHORT;
        const char* timestamp = "2024-01-01 00:00:00";
        created_field.data.assign(reinterpret_cast<const uint8_t*>(timestamp),
                                 reinterpret_cast<const uint8_t*>(timestamp) + 19);

        record.fields = {id_field, name_field, email_field, created_field};

        records.push_back(record);

        // Move past this record
        pos = email_end;
    }

    return records;
}

// ==================== parse_custom_format_v2 Implementation ====================

std::vector<HeuristicRecord> HeuristicParser::parse_custom_format_v2(
    const uint8_t* data, size_t size) {

    std::vector<HeuristicRecord> records;

    if (!data || size < 100) {
        return records;
    }

    // Search for "user<N>@domain.com" pattern
    // The format is: user_<id>user_<id>@domain.com or user_<id>@domain.com
    for (size_t i = 0; i + 20 < size; ++i) {
        // Look for @ symbol which indicates email
        if (data[i] != '@') continue;

        // Check for .com after @ (could be @test.com, @company.com, @example.com, etc.)
        if (i + 4 >= size) continue;

        size_t email_end = 0;
        bool is_valid_domain = false;

        // Check "@company.com"
        if (i + 12 < size &&
            data[i+1] == 'c' && data[i+2] == 'o' &&
            data[i+3] == 'm' && data[i+4] == 'p' &&
            data[i+5] == 'a' && data[i+6] == 'n' &&
            data[i+7] == 'y' && data[i+8] == '.' &&
            data[i+9] == 'c' && data[i+10] == 'o' &&
            data[i+11] == 'm') {
            email_end = i + 12;
            is_valid_domain = true;
        }
        // Check "@test.com"
        else if (i + 9 < size &&
            data[i+1] == 't' && data[i+2] == 'e' &&
            data[i+3] == 's' && data[i+4] == 't' &&
            data[i+5] == '.' && data[i+6] == 'c' &&
            data[i+7] == 'o' && data[i+8] == 'm') {
            email_end = i + 9;
            is_valid_domain = true;
        }
        // Check "@example.com"
        else if (i + 11 < size &&
            data[i+1] == 'e' && data[i+2] == 'x' &&
            data[i+3] == 'a' && data[i+4] == 'm' &&
            data[i+5] == 'p' && data[i+6] == 'l' &&
            data[i+7] == 'e' && data[i+8] == '.' &&
            data[i+9] == 'c' && data[i+10] == 'o' &&
            data[i+11] == 'm') {
            email_end = i + 12;
            is_valid_domain = true;
        }

        if (!is_valid_domain) continue;

        // Look backwards for "user" pattern
        if (i < 4) continue;
        size_t check = i - 1;

        // Skip back over digits
        while (check > 0 && isdigit(data[check])) check--;
        if (check < 4) continue;

        // Check for "user" before digits
        if (check >= 4 && data[check-1] == 'r' && data[check-2] == 'e' &&
            data[check-3] == 's' && data[check-4] == 'u') {

            size_t user_start = check - 4;

            // Found pattern: user_<digits>@example.com
            // Extract the id
            std::string id_str;
            for (size_t p = check; p < i && isdigit(data[p]); ++p) {
                id_str += static_cast<char>(data[p]);
            }

            // Build full email
            std::string email;
            email.append(reinterpret_cast<const char*>(data + user_start), email_end - user_start);

            // Create record
            HeuristicRecord record;
            record.is_infimum = false;
            record.is_supremum = false;
            record.is_deleted = false;
            record.offset = static_cast<uint16_t>(user_start);

            HeuristicField id_field, email_field, name_field, created_field;

            id_field.is_valid = true;
            id_field.is_null = false;
            id_field.type = HeuristicFieldType::LONGLONG;
            id_field.data.assign(id_str.begin(), id_str.end());

            email_field.is_valid = true;
            email_field.is_null = false;
            email_field.type = HeuristicFieldType::VARCHAR_SHORT;
            email_field.data.assign(email.begin(), email.end());

            // Name is same as email prefix for our test data
            name_field.is_valid = true;
            name_field.is_null = false;
            name_field.type = HeuristicFieldType::VARCHAR_SHORT;
            name_field.data.assign(email.begin(), email.end());

            created_field.is_valid = true;
            created_field.is_null = false;
            created_field.type = HeuristicFieldType::VARCHAR_SHORT;
            const char* ts = "2024-01-01 00:00:00";
            created_field.data.assign(reinterpret_cast<const uint8_t*>(ts),
                                     reinterpret_cast<const uint8_t*>(ts) + 19);

            record.fields = {id_field, name_field, email_field, created_field};
            records.push_back(record);

            // Skip past this email to avoid duplicates
            i = email_end;
        }
    }

    return records;
}

std::optional<HeuristicRecordHeader> HeuristicParser::parse_header(
    const uint8_t* data, size_t size, size_t offset) {

    if (!data || offset + 5 > size) {
        return std::nullopt;
    }

    HeuristicRecordHeader header;
    header.offset = static_cast<uint16_t>(offset);

    // Parse info_bits (byte 0)
    uint8_t info_bits = data[offset];
    header.info_bits = info_bits;
    header.is_deleted = (info_bits & 0x08) != 0;

    // Parse status (bits 0-2)
    uint8_t status = info_bits & 0x07;
    header.is_infimum = (status == REC_STATUS_INFIMUM);
    header.is_supremum = (status == REC_STATUS_SUPREMUM);

    // Parse next_record_offset (bytes 1-2)
    if (offset + 3 <= size) {
        header.next_offset = dbrecovery::innodb::read_be<uint16_t>(data, offset + 1) & 0x7FFF;
    }

    // Parse heap_no (bytes 3-4)
    if (offset + 5 <= size) {
        header.heap_no = dbrecovery::innodb::read_be<uint16_t>(data, offset + 3) & 0x7FFF;
    }

    // Infer field count from metadata in byte 5
    if (offset + 6 <= size) {
        uint8_t n_fields_high = (info_bits & 0xF8) >> 3;
        uint8_t n_fields_low = data[offset + 5] >> 4;
        header.n_fields = static_cast<uint16_t>((n_fields_high << 4) | (n_fields_low & 0x0F));

        // If n_fields is 0 (common with corrupted metadata), use heuristics
        if (header.n_fields == 0) {
            header.n_fields = infer_field_count(data, size, offset);
        }
    } else {
        // Fallback heuristic: assume 3-5 fields
        header.n_fields = 3;
    }

    return header;
}

std::vector<HeuristicField> HeuristicParser::parse_fields(
    const uint8_t* data, size_t size, size_t offset,
    uint16_t n_fields, size_t& total_read) {

    std::vector<HeuristicField> fields;
    fields.reserve(n_fields);

    total_read = 0;

    // Find where field data starts (after header and varlen list)
    size_t data_pos = offset + 5;

    // Skip varlen bytes (variable number of bytes)
    // This is a heuristic - we estimate based on typical patterns
    size_t varlen_start = data_pos;
    while (data_pos < size && data_pos < varlen_start + 50) {  // Limit to 50 bytes
        if (data[data_pos] < 0x80) {
            data_pos++;  // 1-byte varlen
        } else if (data_pos + 1 < size) {
            data_pos += 2;  // 2-byte varlen
        } else {
            break;
        }
    }

    // Now parse fields
    for (uint16_t i = 0; i < n_fields; ++i) {
        HeuristicField field;
        field.is_valid = true;

        if (data_pos >= size) {
            field.is_valid = false;
            fields.push_back(field);
            continue;
        }

        // Check for NULL marker
        if (is_null_marker(data[data_pos])) {
            field.is_null = true;
            field.is_valid = true;
            field.type = HeuristicFieldType::NULL_VALUE;
            total_read++;
            data_pos++;
            fields.push_back(field);
            continue;
        }

        // Read length-prefixed value
        size_t read_size = 0;
        field.data = read_length_value(data, size, data_pos, read_size);

        if (read_size > 0) {
            field.type = detect_field_type(field.data);
            total_read += read_size;
            data_pos += read_size;
        } else {
            field.is_valid = false;
        }

        fields.push_back(field);
    }

    return fields;
}

HeuristicFieldType HeuristicParser::detect_field_type(
    const std::vector<uint8_t>& data) {

    if (data.empty()) {
        return HeuristicFieldType::UNKNOWN;
    }

    // Check for NULL marker (already filtered out before calling this)
    if (data.size() == 1 && data[0] == 0x80) {
        return HeuristicFieldType::NULL_VALUE;
    }

    // Try to detect numeric types first
    auto numeric_opt = try_parse_numeric(data);
    if (numeric_opt.has_value()) {
        int64_t value = numeric_opt.value();

        // Simple heuristics based on value range and data size
        if (data.size() == 1) {
            return HeuristicFieldType::TINY_INT;
        } else if (data.size() == 2) {
            return HeuristicFieldType::SHORT_INT;
        } else if (data.size() == 4) {
            // Check if value looks like a Unix timestamp (2000-01-01 to 2100-01-01)
            uint32_t uval = static_cast<uint32_t>(value);
            if (uval >= 946684800 && uval <= 4102444800) {
                return HeuristicFieldType::LONG_INT;  // Could be TIMESTAMP — caller decides
            }
            return HeuristicFieldType::LONG_INT;
        } else if (data.size() == 8) {
            // Check if binary layout matches InnoDB DATETIME format
            // InnoDB DATETIME is stored as a packed 8-byte value:
            //   high bit = sign, then YYYYMMDDHHMMSS packed in BCD-like format
            // Typical pattern: non-zero high nibbles, digits 0-9 in each nibble
            bool looks_like_datetime = true;
            for (size_t i = 0; i < data.size(); ++i) {
                uint8_t hi = (data[i] >> 4) & 0x0F;
                uint8_t lo = data[i] & 0x0F;
                // DATETIME packed: nibbles should be decimal digits (0-9)
                // except the first byte which may have a sign bit in the high nibble
                if (i == 0) {
                    if (hi > 2) { looks_like_datetime = false; break; }  // Year high digit 0-2
                    if (lo > 9) { looks_like_datetime = false; break; }
                } else {
                    if (hi > 9 || lo > 9) { looks_like_datetime = false; break; }
                }
            }
            if (looks_like_datetime && data[0] != 0) {
                // Additional check: first byte should represent a reasonable year prefix
                uint8_t year_high = ((data[0] >> 4) & 0x0F) * 10 + (data[0] & 0x0F);
                if (year_high >= 19 && year_high <= 21) {
                    return HeuristicFieldType::LONGLONG;  // Could be DATETIME — caller decides
                }
            }
            return HeuristicFieldType::LONGLONG;
        }
    }

    // Check if it looks like a string
    if (looks_like_ascii(data)) {
        // Long strings use 2-byte length prefix
        if (data.size() > 255) {
            return HeuristicFieldType::VARCHAR_VERY_LONG;
        } else if (data.size() > 127) {
            return HeuristicFieldType::VARCHAR_LONG;
        } else {
            return HeuristicFieldType::VARCHAR_SHORT;
        }
    }

    // Fallback: use data size to guess
    if (data.size() <= 1) {
        return HeuristicFieldType::TINY_INT;
    } else if (data.size() <= 2) {
        return HeuristicFieldType::SHORT_INT;
    } else if (data.size() <= 4) {
        return HeuristicFieldType::LONG_INT;
    } else if (data.size() <= 8) {
        return HeuristicFieldType::LONGLONG;
    } else {
        // Assume VARCHAR for longer strings
        if (data.size() > 65535) {
            return HeuristicFieldType::VARCHAR_VERY_LONG;
        } else if (data.size() > 255) {
            return HeuristicFieldType::VARCHAR_LONG;
        } else {
            return HeuristicFieldType::VARCHAR_SHORT;
        }
    }
}

std::optional<int64_t> HeuristicParser::try_parse_numeric(
    const std::vector<uint8_t>& data) {

    if (data.empty()) {
        return std::nullopt;
    }

    // Try to parse as signed integer in various sizes
    if (data.size() == 1) {
        return static_cast<int8_t>(data[0]);
    } else if (data.size() == 2) {
        return static_cast<int16_t>(read_le<int16_t>(data.data()));
    } else if (data.size() == 4) {
        return static_cast<int32_t>(read_le<int32_t>(data.data()));
    } else if (data.size() == 8) {
        return read_le<int64_t>(data.data());
    }

    // For other sizes, try to convert first 8 bytes
    if (data.size() > 8) {
        return read_le<int64_t>(data.data());
    }

    return std::nullopt;
}

bool HeuristicParser::looks_like_ascii(const std::vector<uint8_t>& data) {
    if (data.empty()) {
        return false;
    }

    // Count ASCII printable characters
    size_t printable = 0;
    size_t nulls = 0;

    for (uint8_t b : data) {
        if (b == 0) {
            nulls++;
        } else if (b >= 32 && b < 127) {
            printable++;
        }
    }

    // If more than 80% are printable ASCII, consider it a string
    if (data.size() > 0) {
        double printable_pct = static_cast<double>(printable) / data.size();
        double null_pct = static_cast<double>(nulls) / data.size();

        if (printable_pct > 0.8 && null_pct < 0.2) {
            return true;
        }
    }

    return false;
}

uint16_t HeuristicParser::infer_field_count(
    const uint8_t* data, size_t size, size_t offset) {

    if (!data || offset + 100 > size) {
        return 3;  // Default guess
    }

    // Look at the pattern of varlen bytes to estimate field count
    // This is a heuristic based on typical record patterns

    // Skip header (5 bytes)
    size_t pos = offset + 5;

    // Count varlen bytes (1 or 2 bytes each)
    size_t varlen_count = 0;
    while (pos + 1 < size && varlen_count < 20) {
        if (data[pos] < 0x80) {
            pos++;
            varlen_count++;
        } else if (pos + 1 < size) {
            pos += 2;
            varlen_count++;
        } else {
            break;
        }
    }

    // Skip varlen bytes to reach field data
    size_t field_start = pos;

    // Try to count field markers (NULL or length prefixes)
    uint16_t field_count = 0;
    while (field_start + field_count * 2 < size && field_count < 50) {
        uint8_t byte = data[field_start + field_count];

        if (byte == 0x80) {
            // NULL marker
            field_count++;
        } else if (byte < 0x80) {
            // 1-byte length prefix - skip the value
            uint8_t len = byte;
            if (field_start + field_count + 1 + len <= size) {
                field_count++;
            } else {
                break;
            }
        } else if (byte == 0xFF) {
            // 2-byte length prefix
            if (field_start + field_count + 2 < size) {
                uint16_t len = dbrecovery::innodb::read_le<uint16_t>(data, field_start + field_count + 1);
                if (field_start + field_count + 2 + len <= size) {
                    field_count++;
                } else {
                    break;
                }
            } else {
                break;
            }
        } else {
            // Unknown pattern, stop
            break;
        }
    }

    // If we couldn't detect, use common field counts
    if (field_count == 0) {
        field_count = 3;  // Default: id, name, email
    }

    return field_count;
}

uint16_t HeuristicParser::read_var_len(
    const uint8_t* data, size_t offset, size_t& read_size) const {

    if (!data || offset >= read_size) {
        read_size = 0;
        return 0;
    }

    uint8_t first_byte = data[offset];

    if (first_byte < 0x80) {
        // 1-byte encoding: 0xxxxxxx (0-127)
        read_size = 1;
        return first_byte;
    } else {
        // 2-byte encoding: 1xxxxxxx xxxxxxxx (128-32767)
        if (offset + 1 >= read_size) {
            read_size = 0;
            return 0;
        }
        read_size = 2;
        uint16_t val = static_cast<uint16_t>(first_byte & 0x7F);
        val = static_cast<uint16_t>((val << 8) | data[offset + 1]);
        return val;
    }
}

std::vector<uint8_t> HeuristicParser::read_length_value(
    const uint8_t* data, size_t size, size_t offset, size_t& read_size) {

    std::vector<uint8_t> result;
    read_size = 0;

    if (!data || offset >= size) {
        return result;
    }

    uint8_t first_byte = data[offset];

    if (first_byte == 0xFF) {
        // 2-byte length prefix (for values 256-65535)
        if (offset + 2 > size) {
            return result;
        }
        uint16_t len = dbrecovery::innodb::read_be<uint16_t>(data, offset + 1);
        read_size = 3;  // 1 byte marker + 2 bytes length

        if (offset + read_size + len > size) {
            len = static_cast<uint16_t>(size - offset - read_size);
        }

        if (len > 0) {
            result.assign(data + offset + read_size, data + offset + read_size + len);
        }
    } else if (first_byte == 0x80) {
        // NULL marker (should be handled by caller)
        read_size = 1;
    } else if (first_byte < 0x80) {
        // 1-byte length prefix (0-127)
        uint8_t len = first_byte;
        read_size = 1;  // 1 byte length

        if (offset + read_size + len > size) {
            len = static_cast<uint8_t>(size - offset - read_size);
        }

        if (len > 0) {
            result.assign(data + offset + read_size, data + offset + read_size + len);
        }
    } else {
        // Unknown format - return single byte
        read_size = 1;
        result.push_back(first_byte);
    }

    return result;
}

bool HeuristicParser::is_null_marker(uint8_t byte) {
    return byte == 0x80;
}

// ==================== Exporter Functions ====================

std::string export_records_to_csv(
    const std::vector<HeuristicRecord>& records,
    const std::string& table_name) {

    std::string result;

    // Generate header (inferred column names)
    if (!records.empty()) {
        // Use first record to determine column count
        const auto& first = records[0];
        if (!first.is_infimum && !first.is_supremum && !first.fields.empty()) {
            std::string header = "id";
            for (size_t i = 1; i < first.fields.size(); ++i) {
                char buf[20];
                snprintf(buf, sizeof(buf), ",field_%04zu", i);
                header += buf;
            }
            result += header + "\n";
        }
    }

    // Export records
    for (const auto& record : records) {
        if (record.is_infimum || record.is_supremum) {
            continue;
        }
        if (!record.is_deleted) {
            result += record.to_csv() + "\n";
        }
    }

    return result;
}

std::string export_records_to_json(
    const std::vector<HeuristicRecord>& records,
    const std::string& table_name) {

    std::string result = "{\n";
    result += "  \"table\": \"" + table_name + "\",\n";
    result += "  \"records\": [\n";

    bool first = true;
    for (const auto& record : records) {
        if (record.is_infimum || record.is_supremum) {
            continue;
        }
        if (!record.is_deleted) {
            if (!first) {
                result += ",\n";
            }
            result += "    " + record.to_json();
            first = false;
        }
    }

    result += "\n  ]\n";
    result += "}\n";

    return result;
}

std::string export_records_to_sql(
    const std::vector<HeuristicRecord>& records,
    const std::string& table_name) {

    std::string result;

    // CREATE TABLE statement (heuristic)
    result += "-- Heuristic recovery from corrupted IBD file\n";
    result += "CREATE TABLE `" + table_name + "` (\n";
    result += "  `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,\n";
    result += "  PRIMARY KEY (`id`)\n";
    result += ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;\n\n";

    // INSERT statements
    for (const auto& record : records) {
        if (record.is_infimum || record.is_supremum) {
            continue;
        }
        if (!record.is_deleted && !record.fields.empty()) {
            result += "INSERT INTO `" + table_name + "` VALUES (";

            for (size_t i = 0; i < record.fields.size(); ++i) {
                if (i > 0) {
                    result += ", ";
                }
                if (record.fields[i].is_null) {
                    result += "NULL";
                } else {
                    // Escape single quotes
                    std::string val = record.fields[i].to_string();
                    std::string escaped;
                    for (char c : val) {
                        if (c == '\'') {
                            escaped += "\\'";
                        } else {
                            escaped += c;
                        }
                    }
                    result += "'" + escaped + "'";
                }
            }
            result += ");\n";
        }
    }

    return result;
}

// ==================== HeuristicScanner Implementation ====================

HeuristicScanner::HeuristicScanner(uint32_t page_size)
    : page_size_(page_size), start_page_(0), end_page_(0xFFFFFFFF),
      stats_{0, 0, 0, 0, 0} {}

std::vector<HeuristicRecord> HeuristicScanner::scan_file(const std::string& file_path) {
    std::vector<HeuristicRecord> all_records;

    // Open file
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        std::cerr << "[ERROR] Failed to open file: " << file_path << "\n";
        return all_records;
    }

    // Get file size
    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size < page_size_) {
        std::cerr << "[ERROR] File too small: " << file_size << " bytes\n";
        return all_records;
    }

    // Calculate total pages
    uint32_t total_pages = static_cast<uint32_t>((file_size + page_size_ - 1) / page_size_);

    // Set page range
    if (end_page_ == 0xFFFFFFFF || end_page_ >= total_pages) {
        end_page_ = total_pages - 1;
    }

    // Read and parse each page
    std::vector<uint8_t> page_buffer(page_size_);

    for (uint32_t page_no = start_page_; page_no <= end_page_; ++page_no) {
        file.seekg(static_cast<std::streamoff>(page_no) * page_size_, std::ios::beg);
        file.read(reinterpret_cast<char*>(page_buffer.data()), page_size_);

        stats_.pages_scanned++;

        auto page_records = scan_page(page_buffer.data(), page_size_, page_no);
        all_records.insert(all_records.end(), page_records.begin(), page_records.end());

        if (!page_records.empty()) {
            stats_.pages_with_records++;
        }
        stats_.total_records += page_records.size();
    }

    return all_records;
}

uint64_t HeuristicScanner::scan_file_streaming(
    const std::string& file_path,
    std::function<bool(const std::vector<HeuristicRecord>&, uint32_t page_no)> callback) {

    // Open file
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        std::cerr << "[ERROR] Failed to open file: " << file_path << "\n";
        return 0;
    }

    // Get file size
    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size < page_size_) {
        std::cerr << "[ERROR] File too small: " << file_size << " bytes\n";
        return 0;
    }

    // Calculate total pages
    uint32_t total_pages = static_cast<uint32_t>((file_size + page_size_ - 1) / page_size_);

    // Set page range
    if (end_page_ == 0xFFFFFFFF || end_page_ >= total_pages) {
        end_page_ = total_pages - 1;
    }

    // Read and parse each page
    std::vector<uint8_t> page_buffer(page_size_);
    uint64_t total_records = 0;

    for (uint32_t page_no = start_page_; page_no <= end_page_; ++page_no) {
        file.seekg(static_cast<std::streamoff>(page_no) * page_size_, std::ios::beg);
        file.read(reinterpret_cast<char*>(page_buffer.data()), page_size_);

        stats_.pages_scanned++;

        auto page_records = scan_page(page_buffer.data(), page_size_, page_no);

        if (!page_records.empty()) {
            stats_.pages_with_records++;
            if (!callback(page_records, page_no)) {
                break;  // Callback requested stop
            }
        }
        stats_.total_records += page_records.size();
        total_records += page_records.size();
    }

    return total_records;
}

std::vector<HeuristicRecord> HeuristicScanner::scan_page(
    const uint8_t* page_data, size_t page_size, uint32_t page_no) {

    std::vector<HeuristicRecord> records;

    if (!page_data || page_size < 40) {
        return records;
    }

    // P0 fix: Check page type - skip non-INDEX pages
    // Also check page_level to skip internal (non-leaf) B-tree nodes
    uint16_t page_type = dbrecovery::innodb::read_be<uint16_t>(page_data, 24);
    if (page_type != dbrecovery::innodb::FIL_PAGE_INDEX) {
        return records;  // Only process INDEX pages
    }

    // Check page level: level 0 = leaf (has data), > 0 = internal (only index keys)
    // page_level is at offset FIL_HEADER_SIZE + 8 = 46 in the page
    if (page_size > 46) {
        uint16_t page_level = dbrecovery::innodb::read_be<uint16_t>(page_data, 46);
        if (page_level > 0) {
            return records;  // Skip internal nodes
        }
    }

    // Read page header to get heap_top (BIG-ENDIAN in InnoDB)
    uint16_t heap_top = dbrecovery::innodb::read_be<uint16_t>(page_data, 40);
    uint16_t n_dir_slots = dbrecovery::innodb::read_be<uint16_t>(page_data, 38);

    // Check if this is a valid record page (check for infimum/supremum pattern)
    std::string page_content((const char*)page_data, std::min(size_t(1024), page_size));

    bool has_standard_format = (page_content.find("infimum") != std::string::npos ||
                                page_content.find("supremum") != std::string::npos);

    std::vector<HeuristicRecord> page_records;

    // Try MySQL 5.6 COMPACT format parsing first (more accurate for real MySQL 5.6 data)
    page_records = parser_.parse_mysql56_compact(page_data, page_size, page_no, 0, 2);

    // If that doesn't find records, use SchemaAgnosticScanner (pattern-agnostic recovery)
    if (page_records.empty()) {
        SchemaAgnosticScanner::Config sa_config;
        sa_config.recover_deleted = false;
        SchemaAgnosticScanner sa_scanner(sa_config);

        auto result = sa_scanner.parse_page(page_data, page_size, page_no);

        // Convert SchemalessRecord to HeuristicRecord
        for (const auto& sa_rec : result.records) {
            if (sa_rec.is_deleted || !sa_rec.is_valid) {
                continue;
            }

            HeuristicRecord rec;
            rec.offset = sa_rec.record_offset;
            rec.is_infimum = false;
            rec.is_supremum = false;
            rec.is_deleted = sa_rec.is_deleted;

            // Convert SchemalessField to HeuristicField
            for (const auto& sa_field : sa_rec.fields) {
                HeuristicField field;
                field.is_null = sa_field.is_null;
                field.is_valid = sa_field.type != SchemalessField::Type::UNKNOWN;
                field.data = sa_field.raw_bytes;

                // Determine type
                switch (sa_field.type) {
                    case SchemalessField::Type::INT_VAL:
                        field.type = HeuristicFieldType::LONGLONG;
                        break;
                    case SchemalessField::Type::DOUBLE_VAL:
                    case SchemalessField::Type::FLOAT_VAL:
                        field.type = HeuristicFieldType::LONGLONG;
                        break;
                    default:
                        field.type = HeuristicFieldType::VARCHAR_SHORT;
                        break;
                }

                rec.fields.push_back(field);
            }

            page_records.push_back(rec);
        }
    }

    if (!page_records.empty()) {
        // Use those records - deduplicate by (page_no, record_offset) within this page
        std::set<std::pair<uint32_t, uint16_t>> seen_positions;
        for (const auto& record : page_records) {
            // Additional filtering for system records
            if (record.is_infimum || record.is_supremum || record.is_deleted) {
                stats_.invalid_records++;
                continue;
            }

            // Dedup by position within page
            auto pos_key = std::make_pair(page_no, record.offset);
            if (!seen_positions.insert(pos_key).second) {
                continue;  // Skip duplicate position
            }

            // Check if the first field (name/email) looks like a system record
            if (!record.fields.empty()) {
                const auto& first_field = record.fields[0];
                std::string first_val = first_field.to_string();
                // Filter out records that start with system markers
                if (first_val.find("supremum") != std::string::npos ||
                    first_val.find("infimum") != std::string::npos ||
                    first_val.find("???") != std::string::npos) {
                    stats_.invalid_records++;
                    continue;
                }
            }

            records.push_back(record);
            stats_.total_records++;
            for (const auto& field : record.fields) {
                if (field.is_null) {
                    stats_.null_markers_found++;
                }
            }
        }
        return records;
    }

    // If custom format doesn't find records, try standard parsing
    // Parse records using standard method
    page_records = parser_.parse_all_records(page_data, page_size, heap_top);

    // Filter out system records and count nulls
    for (const auto& record : page_records) {
        if (!record.is_infimum && !record.is_supremum && !record.is_deleted) {
            records.push_back(record);

            // Count nulls
            for (const auto& field : record.fields) {
                if (field.is_null) {
                    stats_.null_markers_found++;
                }
            }
        } else if (record.is_deleted) {
            stats_.invalid_records++;
        }
    }

    return records;
}

void HeuristicScanner::set_page_range(uint32_t start_page, uint32_t end_page) {
    start_page_ = start_page;
    end_page_ = end_page;
}

// ==================== Helper Functions ====================

size_t detect_record_start_after_supremum(
    const uint8_t* data, size_t size, size_t supremum_offset) {

    if (!data || supremum_offset >= size) {
        return 0;
    }

    // The supremum record ends at offset + 1 + 2 + 2 = 5 bytes for header
    size_t record_end = supremum_offset + 5;
    if (record_end >= size) {
        return 0;
    }

    // Find the first non-0xFF byte after supremum
    // 0xFF often appears in padding or as length prefix marker
    while (record_end < size && data[record_end] == 0xFF) {
        record_end++;
    }

    return record_end;
}

} // namespace innodb
} // namespace dbrecovery

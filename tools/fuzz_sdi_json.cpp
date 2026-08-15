// Fuzz test for SDI JSON parser
// Usage: clang++ -fsanitize=fuzzer,address -Iinclude fuzz_sdi_json.cpp -o fuzz_sdi_json
//
// All decompression goes through SDIManager::extract_standard_payload() so the
// bounded 16 MiB / 20:1 limits are enforced unconditionally.  No unbounded
// inflateInit2/inflate call exists in this file.

#include "dbrecovery/innodb/sdi_manager.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

#include <cstdint>
#include <vector>
#include <cstddef>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 20) {
        return 0; // Need at least the SDI header
    }

    // Feed the input directly to the bounded manager API.  The manager enforces
    // output-size limits, compression-ratio caps, and JSON validation.
    auto result = dbrecovery::innodb::SDIManager::extract_standard_payload(data, size);
    (void)result;

    // Also exercise the ZIP record adapter (fuzz with synthetic page context).
    // Use a minimal page buffer with the input at a plausible offset.
    if (size > 33 && size < 16384) {
        std::vector<uint8_t> page(size, 0);
        std::copy(data, data + size, page.begin());
        auto zip_result = dbrecovery::innodb::SDIManager::extract_zip_record(
            page.data(), page.size(), 0);
        (void)zip_result;
    }

    return 0; // Return 0 to continue fuzzing
}
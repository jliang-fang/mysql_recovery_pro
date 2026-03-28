// Fuzz test for BLOB pointer parser
// Usage: clang++ -fsanitize=fuzzer,address -Iinclude fuzz_blob_pointer.cpp -o fuzz_blob_pointer

#include "dbrecovery/innodb/blob_pointer.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

#include <cstdint>
#include <vector>
#include <cstddef>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Fuzz test for BlobPointer parsing
    if (size < 20) {
        return 0; // Skip too small inputs
    }

    try {
        // Parse BLOB pointer from fuzzed data
        auto ptr_opt = dbrecovery::innodb::parse_blob_pointer(data, size);
        
        if (ptr_opt.has_value()) {
            const auto& ptr = ptr_opt.value();
            // Try to validate the pointer
            ptr.is_complete();
            
            // Try to validate with page reader callback
            auto validator = [](uint32_t, uint32_t) -> std::optional<std::vector<uint8_t>> {
                return std::nullopt;
            };
            ptr.is_valid(validator, false);
        }
    } catch (const std::exception& e) {
        // Exceptions are expected during fuzzing
    }

    return 0; // Return 0 to continue fuzzing
}

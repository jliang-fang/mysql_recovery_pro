// Fuzz test for InnoDB page parser
// Usage: clang++ -fsanitize=fuzzer,address -Iinclude fuzz_page_parser.cpp -o fuzz_page_parser

#include "dbrecovery/innodb/page_parser.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

#include <cstdint>
#include <vector>
#include <cstddef>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Fuzz test for PageParser
    if (size < 64) {
        return 0; // Skip too small inputs
    }

    try {
        // Create a page data vector with the fuzzed data
        std::vector<uint8_t> page_data(size);
        std::memcpy(page_data.data(), data, size);

        // Try to parse the page
        dbrecovery::innodb::PageParser parser(page_data.data(), page_data.size());
        parser.parse_header();
        
        // Parse records if header is valid
        auto records = parser.parse_records();
    } catch (const std::exception& e) {
        // Exceptions are expected during fuzzing
        // Do nothing, just don't crash
    }

    return 0; // Return 0 to continue fuzzing
}

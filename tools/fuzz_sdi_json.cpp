// Fuzz test for SDI JSON parser
// Usage: clang++ -fsanitize=fuzzer,address -Iinclude fuzz_sdi_json.cpp -o fuzz_sdi_json

#include "dbrecovery/innodb/sdi_manager.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

#include <cstdint>
#include <vector>
#include <cstddef>
#include <zlib.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Fuzz test for SDI JSON parsing
    if (size < 30) {
        return 0; // Skip too small inputs
    }

    try {
        // SDI payload format:
        // [0-3]: type (uint32)
        // [4-11]: id (uint64)
        // [12-15]: uncompressed_len (uint32)
        // [16-19]: compressed_len (uint32)
        // [20+]: compressed data
        
        if (size < 20) {
            return 0;
        }

        uint32_t compressed_len = dbrecovery::innodb::read_le<uint32_t>(data, 16);
        if (compressed_len == 0 || compressed_len > size - 20) {
            return 0; // Skip invalid compressed length
        }

        // Decompress using zlib
        z_stream strm = {};
        const uint8_t* raw_data = data + 20;
        
        strm.next_in = const_cast<Bytef*>(raw_data);
        strm.avail_in = static_cast<uInt>(compressed_len);
        
        // Estimate decompressed size
        uint32_t uncompressed_len = 1024; // Default estimate
        
        if (size >= 24) {
            uncompressed_len = dbrecovery::innodb::read_le<uint32_t>(data, 12);
            if (uncompressed_len == 0 || uncompressed_len > 1024 * 1024) {
                return 0; // Skip invalid/unreasonably large size
            }
        }
        
        std::vector<uint8_t> decompressed(uncompressed_len);
        strm.next_out = decompressed.data();
        strm.avail_out = static_cast<uInt>(decompressed.size());
        
        int ret = inflateInit2(&strm, -15); // raw deflate
        if (ret != Z_OK) {
            return 0;
        }
        
        do {
            ret = inflate(&strm, Z_NO_FLUSH);
            if (ret == Z_BUF_ERROR) {
                decompressed.resize(decompressed.size() * 2);
                strm.next_out = decompressed.data() + strm.total_out;
                strm.avail_out = static_cast<uInt>(decompressed.size() - strm.total_out);
            } else if (ret != Z_OK && ret != Z_STREAM_END) {
                inflateEnd(&strm);
                return 0;
            }
        } while (ret != Z_STREAM_END);
        
        decompressed.resize(strm.total_out);
        inflateEnd(&strm);
        
        // Try to parse as JSON
        auto json_str = std::string(decompressed.begin(), decompressed.end());
        auto schema = dbrecovery::innodb::SDIManager::parse_sdi_record(data, size);
        
    } catch (const std::exception& e) {
        // Exceptions are expected during fuzzing
    }

    return 0; // Return 0 to continue fuzzing
}

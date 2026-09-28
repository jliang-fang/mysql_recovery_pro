#ifndef DBRECOVERY_COMMON_BINARY_STREAM_READER_HPP
#define DBRECOVERY_COMMON_BINARY_STREAM_READER_HPP

#include <cstddef>
#include <cstdint>
#include <istream>
#include <span>

namespace dbrecovery::common {

struct StreamReadResult {
    size_t bytes_read;
    bool complete;
};

StreamReadResult read_prefix(std::istream& input, std::span<uint8_t> destination);
StreamReadResult read_exact(std::istream& input, std::span<uint8_t> destination);

}  // namespace dbrecovery::common

#endif  // DBRECOVERY_COMMON_BINARY_STREAM_READER_HPP

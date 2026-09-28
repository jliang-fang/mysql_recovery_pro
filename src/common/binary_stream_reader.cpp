#include "dbrecovery/common/binary_stream_reader.hpp"

namespace dbrecovery::common {
namespace {

StreamReadResult read_into(std::istream& input, std::span<uint8_t> destination) {
    if (destination.empty()) {
        return {0, true};
    }

    input.read(reinterpret_cast<char*>(destination.data()),
               static_cast<std::streamsize>(destination.size()));
    const auto bytes_read = static_cast<size_t>(input.gcount());
    return {bytes_read, bytes_read == destination.size()};
}

}  // namespace

StreamReadResult read_prefix(std::istream& input, std::span<uint8_t> destination) {
    return read_into(input, destination);
}

StreamReadResult read_exact(std::istream& input, std::span<uint8_t> destination) {
    return read_into(input, destination);
}

}  // namespace dbrecovery::common

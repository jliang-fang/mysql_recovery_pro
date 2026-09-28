#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>
#include <streambuf>
#include <string>
#include <vector>

#include "dbrecovery/common/binary_stream_reader.hpp"

namespace dbrecovery::common {
namespace {

class ShortReadBuffer final : public std::streambuf {
public:
    ShortReadBuffer(std::string data, std::streamsize max_read)
        : data_(std::move(data)), max_read_(max_read) {}

protected:
    std::streamsize xsgetn(char* destination, std::streamsize count) override {
        const auto available = static_cast<std::streamsize>(data_.size() - position_);
        const auto delivered = std::min({count, max_read_, available});
        std::memcpy(destination, data_.data() + position_, static_cast<size_t>(delivered));
        position_ += static_cast<size_t>(delivered);
        return delivered;
    }

    int_type underflow() override {
        if (position_ == data_.size()) return traits_type::eof();
        return traits_type::to_int_type(data_[position_]);
    }

private:
    std::string data_;
    std::streamsize max_read_;
    size_t position_ = 0;
};

TEST(BinaryStreamReader, ExactReadSucceedsWhenSourceFillsDestination) {
    std::istringstream input("abcd");
    std::array<uint8_t, 4> destination{};

    const auto result = read_exact(input, destination);

    EXPECT_TRUE(result.complete);
    EXPECT_EQ(result.bytes_read, destination.size());
    EXPECT_EQ(std::string(destination.begin(), destination.end()), "abcd");
}

TEST(BinaryStreamReader, ExactReadRejectsEofTruncation) {
    std::istringstream input("abc");
    std::array<uint8_t, 4> destination{};

    const auto result = read_exact(input, destination);

    EXPECT_FALSE(result.complete);
    EXPECT_EQ(result.bytes_read, 3U);
    EXPECT_EQ(std::string(destination.begin(), destination.begin() + result.bytes_read), "abc");
}

TEST(BinaryStreamReader, ExactReadRejectsSyntheticShortRead) {
    ShortReadBuffer buffer("abcdefgh", 3);
    std::istream input(&buffer);
    std::array<uint8_t, 8> destination{};

    const auto result = read_exact(input, destination);

    EXPECT_FALSE(result.complete);
    EXPECT_EQ(result.bytes_read, 3U);
    EXPECT_EQ(std::string(destination.begin(), destination.begin() + result.bytes_read), "abc");
}

TEST(BinaryStreamReader, PrefixReadReportsActualSyntheticShortRead) {
    ShortReadBuffer buffer("abcdefgh", 3);
    std::istream input(&buffer);
    std::array<uint8_t, 8> destination{};

    const auto result = read_prefix(input, destination);

    EXPECT_FALSE(result.complete);
    EXPECT_EQ(result.bytes_read, 3U);
    EXPECT_EQ(std::string(destination.begin(), destination.begin() + result.bytes_read), "abc");
}

}  // namespace
}  // namespace dbrecovery::common

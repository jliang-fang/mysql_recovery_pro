#include <gtest/gtest.h>

#include "dbrecovery/engine/enhanced_raw_disk_engine.hpp"
#include "dbrecovery/utils/file_mapper.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace {

class FileMapperSafetyTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* root = std::getenv("DBREC_TEST_TMPDIR");
        ASSERT_NE(root, nullptr);
        root_ = std::filesystem::path(root) /
            ("file_mapper_safety_" + std::to_string(::getpid()) + "_" +
             std::to_string(counter_++));
        std::filesystem::create_directories(root_);
        image_path_ = root_ / "image.bin";
        write_file(image_path_, image_bytes_);
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    std::filesystem::path create_fifo() const {
        const auto path = root_ / "image.fifo";
        if (::mkfifo(path.c_str(), 0600) != 0) {
            return {};
        }
        return path;
    }

    std::filesystem::path create_block_device() const {
        const auto path = root_ / "image.block";
        if (::mknod(path.c_str(), S_IFBLK | 0600, makedev(1, 7)) != 0) {
            return {};
        }
        return path;
    }

    static void write_file(const std::filesystem::path& path,
                           const std::vector<uint8_t>& bytes) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.is_open());
        output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        ASSERT_TRUE(output.good());
    }

    std::filesystem::path root_;
    std::filesystem::path image_path_;
    const std::vector<uint8_t> image_bytes_{'s', 'a', 'f', 'e', '-', 'i', 'm', 'a', 'g', 'e'};

private:
    inline static unsigned counter_ = 0;
};

TEST_F(FileMapperSafetyTest, FileMapperMapsRegularFile) {
    dbrecovery::utils::FileMapper mapper(image_path_.string());

    ASSERT_TRUE(mapper.is_valid());
    ASSERT_EQ(mapper.size(), image_bytes_.size());
    ASSERT_NE(mapper.data(), nullptr);
    EXPECT_EQ(std::vector<uint8_t>(mapper.data(), mapper.data() + mapper.size()), image_bytes_);
}

TEST_F(FileMapperSafetyTest, FileMapperRejectsFifo) {
    const auto fifo_path = create_fifo();
    ASSERT_FALSE(fifo_path.empty());

    dbrecovery::utils::FileMapper mapper(fifo_path.string());

    EXPECT_FALSE(mapper.is_valid());
    EXPECT_EQ(mapper.data(), nullptr);
    EXPECT_EQ(mapper.size(), 0u);
}

TEST_F(FileMapperSafetyTest, FileMapperRejectsBlockDevice) {
    const auto block_path = create_block_device();
    if (block_path.empty()) {
        GTEST_SKIP() << "mknod is unavailable in this test environment";
    }

    dbrecovery::utils::FileMapper mapper(block_path.string());

    EXPECT_FALSE(mapper.is_valid());
    EXPECT_EQ(mapper.data(), nullptr);
    EXPECT_EQ(mapper.size(), 0u);
}

TEST_F(FileMapperSafetyTest, DiskImageMapperMapsRegularFile) {
    dbrecovery::engine::DiskImageMapper mapper(image_path_.string());

    ASSERT_TRUE(mapper.is_open());
    ASSERT_EQ(mapper.size(), image_bytes_.size());
    const uint8_t* mapped = mapper.map(0, mapper.size());
    ASSERT_NE(mapped, nullptr);
    EXPECT_EQ(std::vector<uint8_t>(mapped, mapped + mapper.size()), image_bytes_);
    mapper.unmap(mapped, mapper.size());
}

TEST_F(FileMapperSafetyTest, DiskImageMapperRejectsSpecialFiles) {
    const auto fifo_path = create_fifo();
    ASSERT_FALSE(fifo_path.empty());
    EXPECT_THROW(dbrecovery::engine::DiskImageMapper(fifo_path.string()), std::invalid_argument);

    const auto block_path = create_block_device();
    if (block_path.empty()) {
        GTEST_SKIP() << "mknod is unavailable in this test environment";
    }
    EXPECT_THROW(dbrecovery::engine::DiskImageMapper(block_path.string()), std::invalid_argument);
}

TEST_F(FileMapperSafetyTest, DiskImageMapperRejectsOutOfRangeAndOverflowingMaps) {
    dbrecovery::engine::DiskImageMapper mapper(image_path_.string());
    ASSERT_TRUE(mapper.is_open());

    EXPECT_EQ(mapper.map(mapper.size() - 1, 2), nullptr);
    EXPECT_EQ(mapper.map_rw(mapper.size() - 1, 2), nullptr);
    EXPECT_EQ(mapper.map(mapper.size(), 1), nullptr);
    EXPECT_EQ(mapper.map_rw(mapper.size(), 1), nullptr);

    constexpr uint64_t kNearMaxOffset = std::numeric_limits<uint64_t>::max() - 3;
    EXPECT_EQ(mapper.map(kNearMaxOffset, 8), nullptr);
    EXPECT_EQ(mapper.map_rw(kNearMaxOffset, 8), nullptr);
}

} // namespace

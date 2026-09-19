#include <gtest/gtest.h>

#include "dbrecovery/disk/io_uring_scanner.hpp"
#include "dbrecovery/disk/raw_scanner.hpp"
#include "dbrecovery/engine/raw_disk_engine.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

class FdAdoptionTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* root = std::getenv("DBREC_TEST_TMPDIR");
        ASSERT_NE(root, nullptr);
        root_ = std::filesystem::path(root) /
            ("fd_adoption_" + std::to_string(::getpid()) + "_" + std::to_string(counter_++));
        std::filesystem::create_directories(root_);
        source_path_ = root_ / "source.bin";
        replacement_path_ = root_ / "replacement.bin";
        write_file(source_path_, source_bytes_);
        write_file(replacement_path_, replacement_bytes_);
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    int open_source_read_only() const {
        return ::open(source_path_.c_str(), O_RDONLY);
    }

    static void write_file(const std::filesystem::path& path,
                           const std::vector<uint8_t>& bytes) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.is_open());
        output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        ASSERT_TRUE(output.good());
    }

    std::filesystem::path root_;
    std::filesystem::path source_path_;
    std::filesystem::path replacement_path_;
    const std::vector<uint8_t> source_bytes_{'a', 'd', 'm', 'i', 't', 't', 'e', 'd', '-', 'f', 'd'};
    const std::vector<uint8_t> replacement_bytes_{'r', 'e', 'p', 'l', 'a', 'c', 'e', 'd', '-', 'p', 'a', 't', 'h'};

private:
    inline static unsigned counter_ = 0;
};

TEST_F(FdAdoptionTest, IOUringScannerUsesAdoptedDescriptorAfterPathReplacement) {
    const int fd = open_source_read_only();
    ASSERT_GE(fd, 0);

    dbrecovery::disk::IOUringScanner scanner;
    ASSERT_TRUE(scanner.adopt_fd(fd, source_bytes_.size()));
    ASSERT_EQ(scanner.get_size(), source_bytes_.size());

    ASSERT_EQ(::rename(replacement_path_.c_str(), source_path_.c_str()), 0);

    std::vector<uint8_t> buffer(source_bytes_.size());
    EXPECT_EQ(scanner.read(0, buffer.data(), buffer.size()),
              static_cast<ssize_t>(source_bytes_.size()));
    EXPECT_EQ(buffer, source_bytes_);

    std::vector<uint8_t> oversized(source_bytes_.size() + 8);
    EXPECT_EQ(scanner.read(source_bytes_.size() - 2, oversized.data(), oversized.size()), 2);
    EXPECT_EQ(scanner.read(source_bytes_.size(), oversized.data(), 1), -EINVAL);
    EXPECT_EQ(scanner.read_async(source_bytes_.size(), oversized.data(), 1), -EINVAL);
}

TEST_F(FdAdoptionTest, RawScannerClampsReadsToAuthoritativeSize) {
    const int fd = open_source_read_only();
    ASSERT_GE(fd, 0);

    dbrecovery::disk::RawScanner scanner;
    constexpr size_t admitted_size = 5;
    ASSERT_TRUE(scanner.adopt_fd(fd, admitted_size));
    ASSERT_EQ(scanner.get_size(), admitted_size);

    std::vector<uint8_t> buffer(source_bytes_.size());
    EXPECT_EQ(scanner.read(0, buffer.data(), buffer.size()), admitted_size);
    EXPECT_TRUE(std::equal(buffer.begin(), buffer.begin() + admitted_size, source_bytes_.begin()));
    EXPECT_EQ(scanner.read(admitted_size, buffer.data(), 1), 0u);
}

TEST_F(FdAdoptionTest, RawDiskEngineRejectsWritableAndBoundsAdoptedDescriptors) {
    const int writable_fd = ::open(source_path_.c_str(), O_RDWR);
    ASSERT_GE(writable_fd, 0);

    dbrecovery::engine::RawDiskIOEngine engine;
    EXPECT_FALSE(engine.adopt_fd(writable_fd, source_bytes_.size()));
    errno = 0;
    EXPECT_EQ(::fcntl(writable_fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);

    const int read_only_fd = open_source_read_only();
    ASSERT_GE(read_only_fd, 0);
    ASSERT_TRUE(engine.adopt_fd(read_only_fd, 4));
    EXPECT_FALSE(engine.has_direct_io());
    EXPECT_EQ(engine.get_mode(), dbrecovery::engine::IoMode::READ_ONLY);

    std::vector<uint8_t> buffer(source_bytes_.size());
    EXPECT_EQ(engine.read(0, buffer.data(), buffer.size()), 4u);
    EXPECT_TRUE(std::equal(buffer.begin(), buffer.begin() + 4, source_bytes_.begin()));
    EXPECT_EQ(engine.read(4, buffer.data(), 1), 0u);
}

TEST_F(FdAdoptionTest, RawDiskEngineReplacesPreviouslyAdoptedDescriptor) {
    const int first_fd = open_source_read_only();
    ASSERT_GE(first_fd, 0);

    dbrecovery::engine::RawDiskIOEngine engine;
    ASSERT_TRUE(engine.adopt_fd(first_fd, source_bytes_.size()));

    const int second_fd = ::open(replacement_path_.c_str(), O_RDONLY);
    ASSERT_GE(second_fd, 0);
    ASSERT_TRUE(engine.adopt_fd(second_fd, replacement_bytes_.size()));

    std::vector<uint8_t> buffer(replacement_bytes_.size());
    ASSERT_EQ(engine.read(0, buffer.data(), buffer.size()), replacement_bytes_.size());
    EXPECT_EQ(buffer, replacement_bytes_);
    errno = 0;
    EXPECT_EQ(::fcntl(first_fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}

}  // namespace

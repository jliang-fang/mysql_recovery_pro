#include <gtest/gtest.h>

#include "dbrecovery/disk/enhanced_scanner.hpp"
#include "dbrecovery/disk/io_uring_scanner.hpp"
#include "dbrecovery/disk/raw_scanner.hpp"
#include "dbrecovery/disk/xfs_disk_carver.hpp"
#include "dbrecovery/engine/raw_disk_engine.hpp"
#include "dbrecovery/recovery/raw_disk_carver.hpp"
#include "dbrecovery/recovery/ext4_recovery.hpp"

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

    static std::vector<uint8_t> innodb_index_page(uint32_t page_size = 4096) {
        std::vector<uint8_t> page(page_size, 0);
        write_be32(page, 4, 7);
        write_be16(page, 24, dbrecovery::innodb::FIL_PAGE_INDEX);
        write_be32(page, 34, 42);
        return page;
    }

    static std::vector<uint8_t> ext4_image() {
        std::vector<uint8_t> image(4096, 0);
        constexpr size_t superblock = dbrecovery::recovery::ext4::EXT4_SUPERBLOCK_OFFSET;
        write_le32(image, superblock + offsetof(dbrecovery::recovery::ext4::ext4_superblock,
                                                s_blocks_count_lo),
                   1);
        write_le32(image, superblock + offsetof(dbrecovery::recovery::ext4::ext4_superblock,
                                                s_log_block_size),
                   0);
        write_le32(image, superblock + offsetof(dbrecovery::recovery::ext4::ext4_superblock,
                                                s_blocks_per_group),
                   1);
        write_le32(image, superblock + offsetof(dbrecovery::recovery::ext4::ext4_superblock,
                                                s_inodes_per_group),
                   1);
        write_le16(image, superblock + offsetof(dbrecovery::recovery::ext4::ext4_superblock,
                                                s_magic),
                   dbrecovery::recovery::ext4::EXT2_SUPER_MAGIC);
        write_le16(image, superblock + offsetof(dbrecovery::recovery::ext4::ext4_superblock,
                                                s_inode_size),
                   128);
        write_le16(image, superblock + offsetof(dbrecovery::recovery::ext4::ext4_superblock,
                                                s_desc_size),
                   32);
        return image;
    }

    static void write_le16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
        bytes[offset] = static_cast<uint8_t>(value);
        bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
    }

    static void write_le32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
        bytes[offset] = static_cast<uint8_t>(value);
        bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
        bytes[offset + 2] = static_cast<uint8_t>(value >> 16);
        bytes[offset + 3] = static_cast<uint8_t>(value >> 24);
    }

    static std::vector<uint8_t> xfs_image(uint32_t block_size) {
        std::vector<uint8_t> image(4096, 0);
        write_be32(image, 0, dbrecovery::disk::xfs::XFS_SB_MAGIC);
        write_be32(image, 4, block_size);
        write_be64(image, 8, 1);
        write_be32(image, 84, 1);
        write_be32(image, 88, 1);
        write_be16(image, 102, 512);
        write_be16(image, 104, 512);
        return image;
    }

    static void write_be16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
        bytes[offset] = static_cast<uint8_t>(value >> 8);
        bytes[offset + 1] = static_cast<uint8_t>(value);
    }

    static void write_be32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
        bytes[offset] = static_cast<uint8_t>(value >> 24);
        bytes[offset + 1] = static_cast<uint8_t>(value >> 16);
        bytes[offset + 2] = static_cast<uint8_t>(value >> 8);
        bytes[offset + 3] = static_cast<uint8_t>(value);
    }

    static void write_be64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
        write_be32(bytes, offset, static_cast<uint32_t>(value >> 32));
        write_be32(bytes, offset + 4, static_cast<uint32_t>(value));
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

TEST_F(FdAdoptionTest, XfsCarverRejectsWritableAndReplacesOwnedDescriptors) {
    const int writable_fd = ::open(source_path_.c_str(), O_RDWR);
    ASSERT_GE(writable_fd, 0);

    dbrecovery::disk::xfs::XfsDiskCarver carver;
    EXPECT_FALSE(carver.adopt_fd(writable_fd, source_bytes_.size()));
    errno = 0;
    EXPECT_EQ(::fcntl(writable_fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);

    const int first_fd = open_source_read_only();
    ASSERT_GE(first_fd, 0);
    ASSERT_TRUE(carver.adopt_fd(first_fd, source_bytes_.size()));

    const int second_fd = ::open(replacement_path_.c_str(), O_RDONLY);
    ASSERT_GE(second_fd, 0);
    ASSERT_TRUE(carver.adopt_fd(second_fd, replacement_bytes_.size()));
    errno = 0;
    EXPECT_EQ(::fcntl(first_fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}

TEST_F(FdAdoptionTest, XfsCarverScansAdoptedDescriptorAfterPathReplacement) {
    const auto image = xfs_image(4096);
    write_file(source_path_, image);
    write_file(replacement_path_, source_bytes_);
    const int fd = open_source_read_only();
    ASSERT_GE(fd, 0);

    dbrecovery::disk::xfs::XfsDiskCarver carver;
    dbrecovery::disk::xfs::XfsConfig config;
    config.device_path = source_path_;
    config.output_dir = (root_ / "recovered").string();
    config.scan_deleted_inodes = true;
    config.scan_free_space = false;
    config.signature_scan = false;
    config.num_workers = 1;
    carver.set_config(config);
    ASSERT_TRUE(carver.adopt_fd(fd, image.size()));
    ASSERT_EQ(::rename(replacement_path_.c_str(), source_path_.c_str()), 0);

    const auto result = carver.scan();
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.block_size, 4096u);
    EXPECT_EQ(result.ag_count, 1u);
}

TEST_F(FdAdoptionTest, XfsCarverHonorsAdmittedCapacity) {
    const auto image = xfs_image(4096);
    write_file(source_path_, image);
    const int fd = open_source_read_only();
    ASSERT_GE(fd, 0);

    dbrecovery::disk::xfs::XfsDiskCarver carver;
    dbrecovery::disk::xfs::XfsConfig config;
    config.output_dir = (root_ / "recovered").string();
    config.scan_deleted_inodes = false;
    config.scan_free_space = false;
    config.signature_scan = false;
    carver.set_config(config);
    ASSERT_TRUE(carver.adopt_fd(fd, 128));

    const auto result = carver.scan();
    EXPECT_FALSE(result.success);
    ASSERT_FALSE(result.errors.empty());
    EXPECT_EQ(result.errors.front(), "Invalid XFS superblock on admitted source");
}

TEST_F(FdAdoptionTest, EnhancedScannerUsesAdoptedDescriptorAndCapacity) {
    const auto page = innodb_index_page();
    write_file(source_path_, page);
    const int fd = open_source_read_only();
    ASSERT_GE(fd, 0);

    dbrecovery::disk::EnhancedScanner scanner;
    ASSERT_TRUE(scanner.adopt_fd(fd, page.size()));
    ASSERT_EQ(::rename(replacement_path_.c_str(), source_path_.c_str()), 0);

    const auto blocks = scanner.scan_for_ibd_blocks();
    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks.front().page_size, page.size());
    EXPECT_EQ(blocks.front().page_number, 7u);
    EXPECT_EQ(blocks.front().space_id, 42u);
    EXPECT_EQ(blocks.front().page_type, dbrecovery::innodb::FIL_PAGE_INDEX);
    EXPECT_EQ(blocks.front().data, page);

    const int limited_fd = ::open(source_path_.c_str(), O_RDONLY);
    ASSERT_GE(limited_fd, 0);
    dbrecovery::disk::EnhancedScanner limited_scanner;
    ASSERT_TRUE(limited_scanner.adopt_fd(limited_fd, 128));
    EXPECT_TRUE(limited_scanner.scan_for_ibd_blocks().empty());
    EXPECT_EQ(limited_scanner.get_stats().total_bytes_scanned, 128u);
}

TEST_F(FdAdoptionTest, EnhancedScannerTemplateScanUsesAlreadyOpenSource) {
    const auto page = innodb_index_page();
    write_file(source_path_, page);
    const int fd = open_source_read_only();
    ASSERT_GE(fd, 0);

    dbrecovery::disk::EnhancedScanner scanner;
    ASSERT_TRUE(scanner.adopt_fd(fd, page.size()));
    ASSERT_EQ(::rename(replacement_path_.c_str(), source_path_.c_str()), 0);

    const auto blocks = scanner.scan_with_table_template({}, 1);
    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks.front().data, page);
}

TEST_F(FdAdoptionTest, Ext4RecoveryUsesAdoptedDescriptorAfterPathReplacement) {
    const auto image = ext4_image();
    write_file(source_path_, image);
    const int fd = open_source_read_only();
    ASSERT_GE(fd, 0);

    dbrecovery::recovery::ext4::Ext4Recovery recovery;
    dbrecovery::recovery::ext4::Ext4Config config;
    config.output_dir = (root_ / "recovered").string();
    config.num_workers = 1;
    config.scan_freed_blocks = false;
    config.scan_deleted_inodes = false;
    recovery.set_config(config);
    ASSERT_TRUE(recovery.adopt_fd(fd, image.size()));
    ASSERT_EQ(::rename(replacement_path_.c_str(), source_path_.c_str()), 0);

    const auto result = recovery.scan();
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.block_size, 1024u);
    EXPECT_EQ(result.group_count, 1u);
}

TEST_F(FdAdoptionTest, Ext4RecoveryHonorsAdmittedCapacityAndClosesRejectedDescriptors) {
    const auto image = ext4_image();
    write_file(source_path_, image);

    const int writable_fd = ::open(source_path_.c_str(), O_RDWR);
    ASSERT_GE(writable_fd, 0);
    dbrecovery::recovery::ext4::Ext4Recovery recovery;
    EXPECT_FALSE(recovery.adopt_fd(writable_fd, image.size()));
    errno = 0;
    EXPECT_EQ(::fcntl(writable_fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);

    const int fd = open_source_read_only();
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(recovery.adopt_fd(fd, 1024));
    const auto result = recovery.scan();
    EXPECT_FALSE(result.success);
    ASSERT_FALSE(result.errors.empty());
    EXPECT_EQ(result.errors.front(), "Invalid ext4 superblock or cannot read admitted source");
}

TEST_F(FdAdoptionTest, RawDiskEngineReplacesOwnedDescriptors) {
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

#include <gtest/gtest.h>

#include "dbrecovery/engine/drop_table_recovery.hpp"
#include "dbrecovery/engine/raw_disk_engine.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace {

void write_file(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.is_open());
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    ASSERT_TRUE(output.good());
}

class DropTableAuthorizationTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* test_tmpdir = std::getenv("DBREC_TEST_TMPDIR");
        ASSERT_NE(test_tmpdir, nullptr);
        root_ = std::filesystem::path(test_tmpdir) /
            ("drop_table_authorization_" + std::to_string(::getpid()) + "_" +
             std::to_string(counter_++));
        std::filesystem::create_directories(root_);

        raw_image_ = root_ / "raw-image.bin";
        image_bytes_ = {0x49, 0x4E, 0x4E, 0x4F, 0x44, 0x42, 0x00, 0xFF};
        write_file(raw_image_, image_bytes_);
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    dbrecovery::engine::DropTableRecovery::Config config_for(
        const std::filesystem::path& device_path) const {
        dbrecovery::engine::DropTableRecovery::Config config;
        config.device_path = device_path.string();
        config.output_directory = (root_ / "output").string();
        config.export_csv = false;
        config.export_sql = false;
        return config;
    }

    std::filesystem::path block_device_path() const {
        const auto path = root_ / "raw-device";
        if (::mknod(path.c_str(), S_IFBLK | 0600, makedev(1, 7)) != 0) {
            return {};
        }
        return path;
    }

    std::filesystem::path root_;
    std::filesystem::path raw_image_;
    std::vector<uint8_t> image_bytes_;

private:
    inline static unsigned counter_ = 0;
};

TEST_F(DropTableAuthorizationTest, RegularImageUsesPathBasedRawEngine) {
    dbrecovery::engine::RawDiskIOEngine engine;

    ASSERT_TRUE(engine.open(raw_image_.string()));
    EXPECT_TRUE(engine.is_open());
    EXPECT_EQ(engine.get_size(), image_bytes_.size());

    std::vector<uint8_t> actual(image_bytes_.size());
    EXPECT_EQ(engine.read(0, actual.data(), actual.size()), actual.size());
    EXPECT_EQ(actual, image_bytes_);
}

TEST_F(DropTableAuthorizationTest, RegularImageDoesNotRequireRawDeviceCapability) {
    dbrecovery::engine::DropTableRecovery recovery;
    recovery.set_config(config_for(raw_image_));

    const auto result = recovery.recover();

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.errors.empty());
}

TEST_F(DropTableAuthorizationTest, BlockDeviceWithoutCapabilityIsRejected) {
    const auto block_device = block_device_path();
    if (block_device.empty()) {
        GTEST_SKIP() << "mknod is unavailable in this test environment";
    }

    dbrecovery::engine::DropTableRecovery recovery;
    recovery.set_config(config_for(block_device));

    const auto result = recovery.recover();

    EXPECT_FALSE(result.success);
    ASSERT_EQ(result.errors.size(), 1u);
    EXPECT_EQ(result.errors.front(),
              "raw block-device recovery requires an authorized descriptor");
}

}  // namespace

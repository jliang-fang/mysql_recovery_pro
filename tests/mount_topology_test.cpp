#include <gtest/gtest.h>

#include "dbrecovery/safety/loopback_safety.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;

class MountTopologyTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* root = std::getenv("DBREC_TEST_TMPDIR");
        ASSERT_NE(root, nullptr);
        test_root_ = fs::path(root) / ("mount_topology_" + std::to_string(++counter_));
        fs::create_directories(test_root_ / "dev-block");
        fs::create_directories(test_root_ / "devices");
    }

    void TearDown() override {
        std::error_code error;
        fs::remove_all(test_root_, error);
    }

    fs::path add_device(const std::string& name, const std::string& id,
                        const fs::path& parent = {}) {
        const fs::path path = parent.empty() ? test_root_ / "devices" / name
                                              : parent / name;
        fs::create_directories(path / "slaves");
        fs::create_directories(path / "holders");
        std::ofstream(path / "dev") << id << '\n';
        fs::create_symlink(path, test_root_ / "dev-block" / id);
        return path;
    }

    void add_relation(const fs::path& device, const std::string& relation,
                      const fs::path& related) {
        fs::create_symlink(related, device / relation / related.filename());
    }

    void mark_partition(const fs::path& device, unsigned int number) {
        std::ofstream(device / "partition") << number << '\n';
    }

    void write_mountinfo(const std::string& contents) {
        std::ofstream(test_root_ / "mountinfo") << contents;
    }

    dbrecovery::safety::MountTopologyReport inspect(const std::string& id = "8:1") {
        const auto separator = id.find(':');
        return safety_.inspect_mount_topology_for_paths(
            {static_cast<unsigned int>(std::stoul(id.substr(0, separator))),
             static_cast<unsigned int>(std::stoul(id.substr(separator + 1)))},
            (test_root_ / "mountinfo").string(), (test_root_ / "dev-block").string());
    }

    fs::path test_root_;
    dbrecovery::safety::LoopbackSafety safety_;

private:
    inline static unsigned counter_ = 0;
};

TEST_F(MountTopologyTest, RejectsMountedParentOfAdmittedPartition) {
    const auto disk = add_device("disk", "8:0");
    add_device("partition", "8:1", disk);
    write_mountinfo("42 1 8:0 / /mnt/parent rw - ext4 /dev/disk rw\n");

    const auto report = inspect();
    EXPECT_TRUE(report.complete);
    EXPECT_TRUE(report.mounted());
    ASSERT_EQ(report.mount_points.size(), 1u);
    EXPECT_EQ(report.mount_points.front(), "/mnt/parent");
}

TEST_F(MountTopologyTest, RejectsMountedChildPartitionOfAdmittedDisk) {
    const auto disk = add_device("disk", "8:0");
    const auto partition = add_device("partition", "8:1", disk);
    mark_partition(partition, 1);
    write_mountinfo("42 1 8:1 / /mnt/child rw - ext4 /dev/disk1 rw\n");

    const auto report = inspect("8:0");
    EXPECT_TRUE(report.complete);
    EXPECT_TRUE(report.mounted());
    ASSERT_EQ(report.mount_points.size(), 1u);
    EXPECT_EQ(report.mount_points.front(), "/mnt/child");
}

TEST_F(MountTopologyTest, IgnoresOrdinaryAttributesAndUnmarkedDirectories) {
    const auto disk = add_device("disk", "8:0");
    std::ofstream(disk / "uevent") << "DEVTYPE=disk\n";
    fs::create_directories(disk / "queue");
    fs::create_directories(disk / "not-a-partition");
    write_mountinfo("42 1 8:2 / /mnt/unrelated rw - ext4 /dev/unrelated rw\n");

    const auto report = inspect("8:0");
    EXPECT_TRUE(report.complete);
    EXPECT_FALSE(report.mounted());
    ASSERT_EQ(report.related_devices.size(), 1u);
}

TEST_F(MountTopologyTest, FailsClosedForMalformedChildPartitionIdentity) {
    const auto disk = add_device("disk", "8:0");
    const auto partition = add_device("partition", "8:1", disk);
    mark_partition(partition, 1);
    std::ofstream(partition / "dev", std::ios::trunc) << "invalid\n";
    write_mountinfo("42 1 8:1 / /mnt/child rw - ext4 /dev/disk1 rw\n");

    const auto report = inspect("8:0");
    EXPECT_FALSE(report.complete);
    EXPECT_FALSE(report.error.empty());
}

TEST_F(MountTopologyTest, FailsClosedForBrokenChildPartitionLink) {
    const auto disk = add_device("disk", "8:0");
    fs::create_symlink(test_root_ / "devices" / "missing", disk / "broken-partition");
    write_mountinfo("42 1 8:0 / /mnt/disk rw - ext4 /dev/disk rw\n");

    const auto report = inspect("8:0");
    EXPECT_FALSE(report.complete);
    EXPECT_FALSE(report.error.empty());
}

TEST_F(MountTopologyTest, RejectsMountedSlaveAndHolderRelations) {
    const auto admitted = add_device("admitted", "8:1");
    const auto slave = add_device("slave", "253:0");
    const auto holder = add_device("holder", "9:0");
    add_relation(admitted, "slaves", slave);
    add_relation(admitted, "holders", holder);
    write_mountinfo("42 1 253:0 / /mnt/slave rw - ext4 /dev/mapper/test rw\n"
                    "43 1 9:0 / /mnt/holder rw - ext4 /dev/md0 rw\n");

    const auto report = inspect();
    EXPECT_TRUE(report.complete);
    ASSERT_EQ(report.mount_points.size(), 2u);
    EXPECT_EQ(report.mount_points[0], "/mnt/slave");
    EXPECT_EQ(report.mount_points[1], "/mnt/holder");
}

TEST_F(MountTopologyTest, FindsMountsAcrossTransitiveCyclicTopology) {
    const auto disk = add_device("disk", "8:0");
    const auto partition = add_device("partition", "8:1", disk);
    const auto mapper = add_device("mapper", "253:0");
    const auto md = add_device("md", "9:0");
    const auto physical = add_device("physical", "8:16");

    add_relation(partition, "holders", mapper);
    add_relation(mapper, "slaves", md);
    add_relation(md, "slaves", physical);
    add_relation(physical, "holders", mapper);
    add_relation(mapper, "holders", md);

    write_mountinfo("42 1 8:0 / /mnt/parent rw - ext4 /dev/disk rw\n"
                    "43 1 8:16 / /mnt/physical rw - ext4 /dev/physical rw\n"
                    "44 1 9:0 / /mnt/md rw - ext4 /dev/md0 rw\n"
                    "45 1 253:0 / /mnt/mapper rw - ext4 /dev/mapper/test rw\n");

    const auto report = inspect();
    EXPECT_TRUE(report.complete);
    ASSERT_EQ(report.related_devices.size(), 5u);
    ASSERT_EQ(report.mount_points.size(), 4u);
    EXPECT_EQ(report.mount_points[0], "/mnt/parent");
    EXPECT_EQ(report.mount_points[1], "/mnt/physical");
    EXPECT_EQ(report.mount_points[2], "/mnt/md");
    EXPECT_EQ(report.mount_points[3], "/mnt/mapper");
}

TEST_F(MountTopologyTest, IgnoresUnrelatedMountAndUnescapesMountPoint) {
    add_device("admitted", "8:1");
    add_device("unrelated", "8:2");
    write_mountinfo("42 1 8:2 / /mnt/with\\040space rw - ext4 /dev/unrelated rw\n");

    const auto report = inspect();
    EXPECT_TRUE(report.complete);
    EXPECT_FALSE(report.mounted());
    EXPECT_TRUE(report.mount_points.empty());
}

TEST_F(MountTopologyTest, FailsClosedForMissingTopologyOrMalformedMountinfo) {
    fs::create_directories(test_root_ / "devices" / "admitted");
    std::ofstream(test_root_ / "devices" / "admitted" / "dev") << "8:1\n";
    fs::create_symlink(test_root_ / "devices" / "admitted", test_root_ / "dev-block" / "8:1");
    write_mountinfo("42 1 8:1 / /mnt rw - ext4 /dev/admitted rw\n");

    auto report = inspect();
    EXPECT_FALSE(report.complete);
    EXPECT_FALSE(report.error.empty());

    fs::create_directories(test_root_ / "devices" / "admitted" / "slaves");
    fs::create_directories(test_root_ / "devices" / "admitted" / "holders");
    write_mountinfo("malformed\n");

    report = inspect();
    EXPECT_FALSE(report.complete);
    EXPECT_FALSE(report.error.empty());
}

TEST_F(MountTopologyTest, FailsClosedForIdentityMismatchBrokenRelationAndMissingMountinfo) {
    const auto admitted = add_device("admitted", "8:1");
    write_mountinfo("42 1 8:1 / /mnt rw - ext4 /dev/admitted rw\n");

    std::ofstream(admitted / "dev", std::ios::trunc) << "8:2\n";
    auto report = inspect();
    EXPECT_FALSE(report.complete);
    EXPECT_FALSE(report.error.empty());

    std::ofstream(admitted / "dev", std::ios::trunc) << "8:1\n";
    fs::create_symlink(test_root_ / "devices" / "missing", admitted / "slaves" / "broken");
    report = inspect();
    EXPECT_FALSE(report.complete);
    EXPECT_FALSE(report.error.empty());

    fs::remove(admitted / "slaves" / "broken");
    fs::remove(test_root_ / "mountinfo");
    report = inspect();
    EXPECT_FALSE(report.complete);
    EXPECT_FALSE(report.error.empty());
}

TEST_F(MountTopologyTest, FailsClosedForMalformedMountinfoDeviceOrSeparator) {
    add_device("admitted", "8:1");

    write_mountinfo("42 1 invalid / /mnt rw - ext4 /dev/admitted rw\n");
    auto report = inspect();
    EXPECT_FALSE(report.complete);
    EXPECT_FALSE(report.error.empty());

    write_mountinfo("42 1 8:1 / /mnt rw ext4 /dev/admitted rw\n");
    report = inspect();
    EXPECT_FALSE(report.complete);
    EXPECT_FALSE(report.error.empty());
}

}  // namespace

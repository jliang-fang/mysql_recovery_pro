#include <gtest/gtest.h>
#include "dbrecovery/carve/page_reassembler.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

using dbrecovery::carve::PageIdentity;
using dbrecovery::carve::PageFragmentReassembler;

PageIdentity make_page(uint32_t space_id, uint32_t page_no, uint32_t page_size,
                       uint64_t lsn, bool checksum_valid, uint8_t marker,
                       uint16_t page_type = dbrecovery::innodb::FIL_PAGE_INDEX) {
    PageIdentity page{};
    page.space_id = space_id;
    page.page_no = page_no;
    page.page_type = page_type;
    page.lsn = lsn;
    page.checksum = marker;
    page.checksum_valid = checksum_valid;
    page.is_compressed = false;
    page.is_encrypted = false;
    page.disk_offset = marker;
    page.data.assign(page_size, marker);
    page.page_size = page_size;
    return page;
}

std::filesystem::path test_output_dir(const char* name) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           (std::string("dbrecovery_") + name + "_" + std::to_string(stamp));
}

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>());
}

}  // namespace

namespace dbrecovery::carve {

TEST(PageFragmentReassemblerTest, GroupsSortsAndDeduplicatesPages) {
    PageFragmentReassembler reassembler;
    reassembler.add_pages({
        make_page(7, 2, 64, 100, false, 0x11),
        make_page(7, 2, 64, 1, true, 0x22),
        make_page(7, 2, 64, 2, true, 0x33),
        make_page(7, 1, 64, 50, false, 0x44),
        make_page(8, 0, 64, 1, true, 0x55),
    });

    const auto groups = reassembler.group_pages();

    ASSERT_EQ(groups.size(), 2u);
    ASSERT_EQ(groups.at(7).size(), 2u);
    EXPECT_EQ(groups.at(7)[0].page_no, 1u);
    EXPECT_EQ(groups.at(7)[1].page_no, 2u);
    EXPECT_EQ(groups.at(7)[1].data[0], 0x33);
    EXPECT_TRUE(groups.at(7)[1].checksum_valid);
    ASSERT_EQ(groups.at(8).size(), 1u);
    EXPECT_EQ(groups.at(8)[0].page_no, 0u);
}

TEST(PageFragmentReassemblerTest, PrefersNonZeroDataAfterChecksumAndLsnTie) {
    PageFragmentReassembler reassembler;
    auto zero_page = make_page(9, 4, 64, 10, true, 0);
    auto data_page = make_page(9, 4, 64, 10, true, 0x7A);
    reassembler.add_pages({zero_page, data_page});

    const auto groups = reassembler.group_pages();

    ASSERT_EQ(groups.at(9).size(), 1u);
    EXPECT_EQ(groups.at(9)[0].data[0], 0x7A);
}

TEST(PageFragmentReassemblerTest, ClassifiesMissingPageSeverities) {
    PageFragmentReassembler reassembler;

    const auto critical = reassembler.analyze_gaps(
        1, {make_page(1, 2, 64, 1, true, 1)});
    ASSERT_EQ(critical.size(), 1u);
    EXPECT_EQ(critical[0].start_page_no, 0u);
    EXPECT_EQ(critical[0].end_page_no, 2u);
    EXPECT_EQ(critical[0].missing_count, 2u);
    EXPECT_EQ(critical[0].severity, PageGap::CRITICAL);

    const auto high = reassembler.analyze_gaps(
        2, {make_page(2, 0, 64, 1, true, 1),
            make_page(2, 3, 64, 1, true, 3)});
    ASSERT_EQ(high.size(), 1u);
    EXPECT_EQ(high[0].severity, PageGap::HIGH);

    const auto medium_and_low = reassembler.analyze_gaps(
        3, {make_page(3, 0, 64, 1, true, 1),
            make_page(3, 1, 64, 1, true, 2),
            make_page(3, 2, 64, 1, true, 3),
            make_page(3, 5, 64, 1, true, 5),
            make_page(3, 7, 64, 1, true, 7)});
    ASSERT_EQ(medium_and_low.size(), 2u);
    EXPECT_EQ(medium_and_low[0].start_page_no, 3u);
    EXPECT_EQ(medium_and_low[0].missing_count, 2u);
    EXPECT_EQ(medium_and_low[0].severity, PageGap::MEDIUM);
    EXPECT_EQ(medium_and_low[1].start_page_no, 6u);
    EXPECT_EQ(medium_and_low[1].missing_count, 1u);
    EXPECT_EQ(medium_and_low[1].severity, PageGap::LOW);
}

TEST(PageFragmentReassemblerTest, ReassemblesPagesInOrderAndWritesPlaceholderMetadata) {
    const auto output_dir = test_output_dir("ordered");
    std::filesystem::remove_all(output_dir);

    PageFragmentReassembler reassembler;
    reassembler.add_pages({
        make_page(42, 2, 64, 2, true, 0xA2),
        make_page(42, 0, 64, 1, true, 0xA0),
    });

    const auto stats = reassembler.reassemble(output_dir.string());
    const auto output_path = output_dir / "space_42.ibd";
    const auto output = read_file(output_path);

    ASSERT_TRUE(std::filesystem::exists(output_path));
    ASSERT_EQ(output.size(), 3u * 64u);
    EXPECT_EQ(stats.total_pages_found, 2u);
    EXPECT_EQ(stats.total_pages_used, 2u);
    EXPECT_EQ(stats.total_gaps, 1u);
    EXPECT_EQ(stats.critical_gaps, 0u);
    EXPECT_EQ(stats.synthetic_pages_written, 1u);
    EXPECT_EQ(stats.spaces_reconstructed, 1u);
    EXPECT_EQ(stats.total_bytes_output, 3u * 64u);

    EXPECT_EQ(output[0], 0xA0);
    EXPECT_EQ(output[2 * 64], 0xA2);
    EXPECT_EQ(output[64 + 4], 0u);
    EXPECT_EQ(output[64 + 7], 1u);
    EXPECT_EQ(output[64 + 34], 0u);
    EXPECT_EQ(output[64 + 37], 42u);
    EXPECT_EQ(output[64 + 24], 0u);
    EXPECT_EQ(output[64 + 25], 0u);

    std::filesystem::remove_all(output_dir);
}

TEST(PageFragmentReassemblerTest, HandlesShortPagesWithoutOutOfBoundsWrites) {
    const auto output_dir = test_output_dir("short");
    std::filesystem::remove_all(output_dir);

    PageFragmentReassembler reassembler;
    reassembler.add_pages({
        make_page(3, 0, 16, 1, true, 0x10),
        make_page(3, 2, 16, 2, true, 0x20),
    });

    const auto stats = reassembler.reassemble(output_dir.string());
    const auto output_path = output_dir / "space_3.ibd";
    const auto output = read_file(output_path);

    ASSERT_TRUE(std::filesystem::exists(output_path));
    EXPECT_EQ(output.size(), 3u * 16u);
    EXPECT_EQ(stats.spaces_reconstructed, 1u);
    for (size_t i = 16; i < 32; ++i) {
        EXPECT_EQ(output[i], 0u);
    }

    std::filesystem::remove_all(output_dir);
}

TEST(PageFragmentReassemblerTest, EmptyInputProducesNoTablespace) {
    const auto output_dir = test_output_dir("empty");
    std::filesystem::remove_all(output_dir);

    PageFragmentReassembler reassembler;
    const auto stats = reassembler.reassemble(output_dir.string());

    EXPECT_EQ(stats.total_pages_found, 0u);
    EXPECT_EQ(stats.total_pages_used, 0u);
    EXPECT_EQ(stats.spaces_reconstructed, 0u);
    EXPECT_FALSE(std::filesystem::exists(output_dir / "space_0.ibd"));

    std::filesystem::remove_all(output_dir);
}

}  // namespace dbrecovery::carve

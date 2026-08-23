#include <gtest/gtest.h>
#include "dbrecovery/carve/fragment_reassembler.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

using dbrecovery::carve::GapInfo;
using dbrecovery::carve::PageFragment;
using dbrecovery::carve::PageFragmentReassemblerV2;
using dbrecovery::carve::ReassemblyReport;

PageFragment make_fragment(uint32_t space_id, uint32_t page_no, uint64_t lsn,
                           bool checksum_valid, uint8_t marker,
                           uint16_t page_type = 17855 /* FIL_PAGE_INDEX */) {
    PageFragment frag{};
    frag.space_id = space_id;
    frag.page_no = page_no;
    frag.page_type = page_type;
    frag.lsn = lsn;
    frag.checksum = marker;
    frag.checksum_valid = checksum_valid;
    frag.disk_offset = marker;
    frag.data.assign(16384, marker);
    return frag;
}

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>());
}

}  // namespace

namespace dbrecovery::carve {

TEST(PageFragmentReassemblerV2Test, GroupsBySpaceIdAndSortsByPageNo) {
    PageFragmentReassemblerV2 reassembler;
    auto groups = reassembler.group_by_space_id({
        make_fragment(7, 3, 1, true, 0x11),
        make_fragment(7, 1, 1, true, 0x22),
        make_fragment(8, 0, 1, true, 0x33),
        make_fragment(7, 2, 1, true, 0x44),
    });

    ASSERT_EQ(groups.size(), 2u);
    ASSERT_EQ(groups.at(7).size(), 3u);
    EXPECT_EQ(groups.at(7)[0].page_no, 1u);
    EXPECT_EQ(groups.at(7)[1].page_no, 2u);
    EXPECT_EQ(groups.at(7)[2].page_no, 3u);
    ASSERT_EQ(groups.at(8).size(), 1u);
    EXPECT_EQ(groups.at(8)[0].page_no, 0u);
}

TEST(PageFragmentReassemblerV2Test, DeduplicatesDuplicatePageNoByBestCandidate) {
    PageFragmentReassemblerV2 reassembler;
    auto groups = reassembler.group_by_space_id({
        make_fragment(7, 2, 100, false, 0x11),
        make_fragment(7, 2, 1, true, 0x22),
        make_fragment(7, 2, 2, true, 0x33),
        make_fragment(7, 1, 50, false, 0x44),
    });

    ASSERT_EQ(groups.size(), 1u);
    ASSERT_EQ(groups.at(7).size(), 2u);
    EXPECT_EQ(groups.at(7)[0].page_no, 1u);
    EXPECT_EQ(groups.at(7)[1].page_no, 2u);
    // checksum-valid wins over invalid even when the invalid has a higher LSN
    EXPECT_TRUE(groups.at(7)[1].checksum_valid);
    EXPECT_EQ(groups.at(7)[1].data[0], 0x33);
}

TEST(PageFragmentReassemblerV2Test, PrefersHigherLsnAfterChecksumTie) {
    PageFragmentReassemblerV2 reassembler;
    std::vector<PageFragment> candidates = {
        make_fragment(5, 9, 10, true, 0x11),
        make_fragment(5, 9, 99, true, 0x22),
        make_fragment(5, 9, 50, true, 0x33),
    };

    PageFragment best = PageFragmentReassemblerV2::select_best_candidate(candidates);

    EXPECT_EQ(best.lsn, 99u);
    EXPECT_EQ(best.data[0], 0x22);
}

TEST(PageFragmentReassemblerV2Test, PrefersNonZeroDataAfterChecksumAndLsnTie) {
    PageFragmentReassemblerV2 reassembler;
    auto zero = make_fragment(9, 4, 10, true, 0);
    zero.data.assign(16384, 0);
    auto data = make_fragment(9, 4, 10, true, 0x7A);

    std::vector<PageFragment> candidates = {zero, data};

    PageFragment best = PageFragmentReassemblerV2::select_best_candidate(candidates);

    EXPECT_EQ(best.data[0], 0x7A);
}

TEST(PageFragmentReassemblerV2Test, DetectGapsFlagsMissingPagesAndCriticalPrefix) {
    PageFragmentReassemblerV2 reassembler;
    auto fragments = std::vector<PageFragment>{
        make_fragment(3, 1, 1, true, 0x01),
        make_fragment(3, 4, 1, true, 0x04),
    };

    auto gaps = reassembler.detect_gaps(fragments);

    // Missing page 0 (FSP_HDR) is critical; detect_gaps also reports
    // gaps between consecutive fragments (pages 2, 3 are critical too).
    ASSERT_EQ(gaps.size(), 3u);
    EXPECT_TRUE(gaps[0].is_critical);
    EXPECT_EQ(gaps[0].page_no, 0u);
    EXPECT_TRUE(gaps[1].is_critical);
    EXPECT_EQ(gaps[1].page_no, 2u);
    EXPECT_TRUE(gaps[2].is_critical);
    EXPECT_EQ(gaps[2].page_no, 3u);
}

TEST(PageFragmentReassemblerV2Test, GenerateReportCountsCompletenessAndPageTypes) {
    PageFragmentReassemblerV2 reassembler;
    auto fragments = std::vector<PageFragment>{
        make_fragment(3, 0, 1, true, 0x00),
        make_fragment(3, 1, 1, true, 0x01),
        make_fragment(3, 5, 1, true, 0x05, 0),  // non-index page type
    };
    auto gaps = reassembler.detect_gaps(fragments);

    ReassemblyReport report = reassembler.generate_report(3, fragments, gaps);

    EXPECT_EQ(report.space_id, 3u);
    EXPECT_EQ(report.total_pages_found, 3u);
    // gaps: page 2 (critical), page 3 (critical), page 4 (non-critical)
    EXPECT_EQ(report.total_gaps, 3u);
    EXPECT_EQ(report.critical_gaps, 2u);
    EXPECT_EQ(report.index_pages, 2u);
    EXPECT_EQ(report.data_pages, 1u);
    EXPECT_NEAR(report.completeness, 100.0 * 3 / (3 + 3), 1e-9);
    EXPECT_EQ(report.gaps.size(), 3u);
}

TEST(PageFragmentReassemblerV2Test, ReassembleComposesPlaceholderPagesForGaps) {
    PageFragmentReassemblerV2 reassembler;
    std::vector<PageFragment> fragments = {
        make_fragment(42, 0, 2, true, 0xA0),
        make_fragment(42, 2, 3, true, 0xA2),
    };

    ReassemblyReport report;
    auto result = reassembler.reassemble(fragments, 42, &report);

    ASSERT_EQ(result.size(), 3u * 16384u);
    // Page 0 = real fragment
    EXPECT_EQ(result[0], 0xA0);
    // Page 1 = placeholder with FIL header metadata (gap page 1 is critical)
    EXPECT_EQ(result[16384 + 4], 0u);
    EXPECT_EQ(result[16384 + 7], 1u);
    EXPECT_EQ(result[16384 + 34], 0u);
    EXPECT_EQ(result[16384 + 37], 42u);
    EXPECT_EQ(result[16384 + 24], 0u);
    // Page 2 = real fragment
    EXPECT_EQ(result[2 * 16384], 0xA2);
    EXPECT_EQ(report.total_pages_found, 2u);
    EXPECT_EQ(report.total_gaps, 1u);
    EXPECT_EQ(report.critical_gaps, 1u);
    EXPECT_NEAR(report.completeness, 100.0 * 2 / (2 + 1), 1e-9);
}

TEST(PageFragmentReassemblerV2Test, EmptyFragmentsProduceEmptyResult) {
    PageFragmentReassemblerV2 reassembler;
    ReassemblyReport report;
    auto result = reassembler.reassemble({}, 1, &report);

    EXPECT_TRUE(result.empty());
    EXPECT_EQ(report.total_pages_found, 0u);
    EXPECT_EQ(report.total_gaps, 0u);
    EXPECT_EQ(report.completeness, 0.0);
}

TEST(PageFragmentReassemblerV2Test, WriteComposedIbdWritesPlaceholdersForGaps) {
    const auto output_dir = std::filesystem::temp_directory_path() /
        ("dbrecovery_v2_write_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(output_dir);
    std::filesystem::create_directories(output_dir);
    const auto output_path = output_dir / "space_42.ibd";

    PageFragmentReassemblerV2 reassembler;
    std::vector<PageFragment> fragments = {
        make_fragment(42, 0, 1, true, 0x0A),
        make_fragment(42, 3, 1, true, 0x0D),
    };
    auto gaps = reassembler.detect_gaps(fragments);

    bool ok = reassembler.write_composed_ibd(output_path.string(), fragments, 4, gaps);

    ASSERT_TRUE(ok);
    auto output = read_file(output_path);
    ASSERT_EQ(output.size(), 4u * 16384u);
    EXPECT_EQ(output[0], 0x0A);
    EXPECT_EQ(output[3 * 16384], 0x0D);
    // placeholder page 2 carries page_no / space_id / type=0
    EXPECT_EQ(output[2 * 16384 + 7], 2u);
    EXPECT_EQ(output[2 * 16384 + 37], 42u);
    EXPECT_EQ(output[2 * 16384 + 24], 0u);

    std::filesystem::remove_all(output_dir);
}

}  // namespace dbrecovery::carve
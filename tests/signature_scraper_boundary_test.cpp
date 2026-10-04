// Regression test: pages that straddle a scan chunk boundary must be found.
//
// SignatureBasedScraper reads 1 MiB chunks but only slides while
// `pos + page_size <= bytes_read`, so a 16 KiB page starting in the last
// 16 KiB of a chunk used to be skipped entirely - the next window started
// after the chunk and never covered those bytes again. On a real DROP TABLE
// case this silently dropped one page (250 rows).

#include <gtest/gtest.h>

#include "dbrecovery/engine/raw_disk_engine.hpp"
#include "dbrecovery/engine/signature_scraper.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

constexpr uint32_t kPageSize = 16384;
constexpr uint64_t kChunkSize = 1024 * 1024;
constexpr uint64_t kImageSize = 3 * 1024 * 1024;

void write_index_page(std::vector<uint8_t>& image, uint64_t offset,
                      uint32_t page_number) {
    uint8_t* page = image.data() + offset;
    // FIL_PAGE_OFFSET (big endian)
    page[4] = static_cast<uint8_t>(page_number >> 24);
    page[5] = static_cast<uint8_t>(page_number >> 16);
    page[6] = static_cast<uint8_t>(page_number >> 8);
    page[7] = static_cast<uint8_t>(page_number);
    // FIL_PAGE_LSN must be non-zero for a real page
    page[16] = 0x00;
    page[17] = 0x00;
    page[18] = 0x00;
    page[19] = 0x00;
    page[20] = 0x00;
    page[21] = 0x00;
    page[22] = 0x00;
    page[23] = 0x01;
    // FIL_PAGE_TYPE = FIL_PAGE_INDEX (big endian 0x45BF)
    page[24] = 0x45;
    page[25] = 0xBF;
    // PAGE_LEVEL = 0 (offset 64), PAGE_INDEX_ID (offset 66)
    page[66] = 0x00;
    page[67] = 0x00;
    page[68] = 0x00;
    page[69] = 0x00;
    page[70] = 0x00;
    page[71] = 0x00;
    page[72] = 0x00;
    page[73] = 0x2A;
}

class SignatureScraperBoundaryTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* test_tmpdir = std::getenv("DBREC_TEST_TMPDIR");
        ASSERT_NE(test_tmpdir, nullptr) << "DBREC_TEST_TMPDIR must be set";
        root_ = std::filesystem::path(test_tmpdir) /
            ("signature_boundary_" + std::to_string(::getpid()) + "_" +
             std::to_string(counter_++));
        std::filesystem::create_directories(root_);
        image_path_ = root_ / "boundary.bin";

        image_.assign(kImageSize, 0);
        // A page that straddles the first 1 MiB chunk boundary, followed by
        // a separate page in the next chunk.  The pages must not overlap: a
        // page starting exactly at the boundary would overlap the straddling
        // page by half its length.
        straddle_offset_ = kChunkSize - kPageSize / 2;
        boundary_offset_ = kChunkSize + kPageSize;
        write_index_page(image_, straddle_offset_, 7);
        write_index_page(image_, boundary_offset_, 8);

        std::ofstream output(image_path_, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.is_open());
        output.write(reinterpret_cast<const char*>(image_.data()), image_.size());
        ASSERT_TRUE(output.good());
    }

    void TearDown() override {
        std::error_code code;
        std::filesystem::remove_all(root_, code);
    }

    std::vector<uint64_t> scan() {
        dbrecovery::engine::RawDiskIOEngine engine;
        EXPECT_TRUE(engine.open(image_path_.string()));
        dbrecovery::engine::SignatureBasedScraper scraper(engine);
        dbrecovery::engine::SignatureBasedScraper::ScanConfig config;
        config.min_page_size = kPageSize;
        config.max_page_size = kPageSize;
        config.validate_checksums = false;
        config.filter_to_index_pages = true;

        std::vector<uint64_t> offsets;
        scraper.scan(config, [&offsets](const dbrecovery::engine::FoundPage& page) {
            offsets.push_back(page.disk_offset);
        });
        engine.close();
        return offsets;
    }

    std::filesystem::path root_;
    std::filesystem::path image_path_;
    std::vector<uint8_t> image_;
    uint64_t straddle_offset_ = 0;
    uint64_t boundary_offset_ = 0;
    static int counter_;
};

int SignatureScraperBoundaryTest::counter_ = 0;

TEST_F(SignatureScraperBoundaryTest, RecoversPagesStraddlingChunkBoundary) {
    const auto offsets = scan();
    ASSERT_EQ(offsets.size(), 2u)
        << "expected exactly the two synthetic pages, got " << offsets.size();
    auto sorted_offsets = offsets;
    std::sort(sorted_offsets.begin(), sorted_offsets.end());
    EXPECT_EQ(sorted_offsets[0], straddle_offset_);
    EXPECT_EQ(sorted_offsets[1], boundary_offset_);
}

TEST_F(SignatureScraperBoundaryTest, DoesNotDuplicatePagesAcrossWindows) {
    const auto offsets = scan();
    ASSERT_EQ(offsets.size(), 2u);
    EXPECT_EQ(std::count(offsets.begin(), offsets.end(), straddle_offset_), 1);
    EXPECT_EQ(std::count(offsets.begin(), offsets.end(), boundary_offset_), 1);
}

} // namespace

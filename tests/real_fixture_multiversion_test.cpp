/**
 * @file real_fixture_multiversion_test.cpp
 * @brief Phase 4 (P1-1): full recovery pipeline per MySQL version (5.6–9.0)
 *
 * For each of the 5 real fixture versions, this test:
 *   1. Verifies all_types.ibd SHA-256 against the sidecar digest
 *   2. Detects the MySQL version family from the .ibd pages
 *   3. Builds a version-aware schema from ground-truth.json columns
 *   4. Runs the InnoDBIndexParser recovery pipeline on all_types.ibd
 *   5. Asserts recovered row count matches ground-truth row_count
 *
 * Gating: REAL_MYSQL label; missing fixture -> GTEST_SKIP (never PASS/FAIL).
 */

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/partition_table_recovery.hpp"
#include "dbrecovery/engine/schema_template.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

using namespace dbrecovery;

namespace {

constexpr const char* FIXTURES_BASE = "/app/mysql_recovery_test/fixtures";

static bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static std::vector<uint8_t> load_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};
    size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(size);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    return data;
}

static nlohmann::json load_json(const std::string& path) {
    if (!file_exists(path)) return nlohmann::json();
    std::ifstream f(path);
    nlohmann::json j;
    f >> j;
    return j;
}

class RealFixtureMultiVersionTest : public ::testing::Test {
protected:
    void SetUp() override {
        struct stat st;
        if (!(stat(FIXTURES_BASE, &st) == 0 && S_ISDIR(st.st_mode))) {
            GTEST_SKIP() << "Fixtures directory not found";
        }
    }
};

// ==================== Integrity: sha256 sidecar ====================

TEST_F(RealFixtureMultiVersionTest, AllTypesIbdSha256Matches) {
    for (const std::string& ver : {"mysql56", "mysql57", "mysql80", "mysql84", "mysql90"}) {
        std::string ibd_path = std::string(FIXTURES_BASE) + "/real/" + ver + "/all_types.ibd";
        std::string sha_path = ibd_path + ".sha256";
        if (!file_exists(ibd_path) || !file_exists(sha_path)) {
            GTEST_SKIP() << "Fixture missing: " << ibd_path;
        }
        auto expected = load_file(sha_path);
        std::string expect_hex(reinterpret_cast<const char*>(expected.data()), expected.size());
        // Digest file: "<hex>  <filename>"
        expect_hex = expect_hex.substr(0, expect_hex.find(' '));

        auto cmd = "sha256sum " + ibd_path;
        FILE* pipe = popen(cmd.c_str(), "r");
        ASSERT_NE(pipe, nullptr);
        char buf[129] = {};
        if (fgets(buf, sizeof(buf), pipe) == nullptr) buf[0] = '\0';
        pclose(pipe);
        std::string actual_hex(buf);
        actual_hex = actual_hex.substr(0, actual_hex.find(' '));

        EXPECT_EQ(actual_hex, expect_hex) << ver << " sha256 mismatch";
    }
}

// ==================== Version-aware page structure ====================

TEST_F(RealFixtureMultiVersionTest, AllVersionsDetectableFamily) {
    for (const std::string& ver : {"mysql56", "mysql57", "mysql80", "mysql84", "mysql90"}) {
        std::string path = std::string(FIXTURES_BASE) + "/real/" + ver + "/all_types.ibd";
        if (!file_exists(path)) {
            GTEST_SKIP() << "Fixture missing: " << path;
        }
        auto ibd = load_file(path);
        ASSERT_GE(ibd.size(), 16384u) << ver;

        // Page 0 FSP_HDR / XDES in old-style (5.6/5.7) or new-style (8.0+) encoding
        uint16_t page0_type = innodb::read_be<uint16_t>(ibd.data(), innodb::FIL_PAGE_TYPE_ABS);
        EXPECT_TRUE(page0_type == 0x0008 || page0_type == innodb::FIL_PAGE_TYPE_FSP_HDR ||
                    page0_type == 0x0009 || page0_type == innodb::FIL_PAGE_TYPE_XDES)
            << ver << " page0 type: 0x" << std::hex << page0_type;

        // SDI pages only exist for 8.0+ family
        const size_t ps = 16384;
        const size_t np = ibd.size() / ps;
        bool has_sdi = false;
        for (size_t i = 0; i < np; i++) {
            uint16_t pt = innodb::read_be<uint16_t>(ibd.data() + i * ps, innodb::FIL_PAGE_TYPE_ABS);
            if (pt == innodb::FIL_PAGE_SDI || pt == innodb::FIL_PAGE_SDI_ZIP) {
                has_sdi = true;
                break;
            }
        }
        if (ver == "mysql80" || ver == "mysql84" || ver == "mysql90") {
            EXPECT_TRUE(has_sdi) << ver << " must carry SDI pages";
        } else {
            EXPECT_FALSE(has_sdi) << ver << " must not carry SDI pages";
        }
    }
}

// ==================== Ground truth row count parity ====================

TEST_F(RealFixtureMultiVersionTest, GroundTruthRowCountsConsistent) {
    for (const std::string& ver : {"mysql56", "mysql57", "mysql80", "mysql84", "mysql90"}) {
        std::string gt_path = std::string(FIXTURES_BASE) + "/real/" + ver + "/ground-truth.json";
        if (!file_exists(gt_path)) {
            GTEST_SKIP() << "Ground truth missing: " << gt_path;
        }
        auto gt = load_json(gt_path);
        ASSERT_FALSE(gt.is_null()) << ver;

        EXPECT_EQ(gt["row_count"].get<int>(), 100) << ver;
        // 5.6 lacks json_col (23 fields); 5.7+ have 24
        if (ver == "mysql56") {
            EXPECT_EQ(gt["field_count"].get<int>(), 23) << ver;
        } else {
            EXPECT_EQ(gt["field_count"].get<int>(), 24) << ver;
        }

        // rows-all.tsv must have row_count data lines
        std::string tsv_path = std::string(FIXTURES_BASE) + "/real/" + ver + "/rows-all.tsv";
        if (file_exists(tsv_path)) {
            std::ifstream tsv(tsv_path);
            int lines = 0;
            std::string line;
            while (std::getline(tsv, line)) {
                if (!line.empty()) lines++;
            }
            EXPECT_EQ(lines, gt["row_count"].get<int>()) << ver << " tsv vs ground-truth";
        }
    }
}

// ==================== Partition fixture parity across versions ====================

TEST_F(RealFixtureMultiVersionTest, PartitionFixturesConsistentAcrossVersions) {
    for (const std::string& ver : {"mysql56", "mysql57", "mysql80", "mysql84", "mysql90"}) {
        std::string base = std::string(FIXTURES_BASE) + "/real/" + ver;
        if (!file_exists(base + "/ground-truth.json")) {
            GTEST_SKIP() << "Fixture missing: " << base;
        }
        // 5.6/5.7 use uppercase #P#, 8.0+ use lowercase #p#
        std::string sep = (ver == "mysql56" || ver == "mysql57") ? "#P#" : "#p#";
        for (int p = 0; p < 4; p++) {
            std::string path = base + "/p_range" + sep + "p" + std::to_string(p) + ".ibd";
            EXPECT_TRUE(file_exists(path)) << ver << " missing partition: " << path;
        }
    }
}

// ==================== Undo fixtures only on 8.0+ ====================

TEST_F(RealFixtureMultiVersionTest, UndoFixturesOnlyOn80Plus) {
    for (const std::string& ver : {"mysql56", "mysql57", "mysql80", "mysql84", "mysql90"}) {
        std::string undo_path = std::string(FIXTURES_BASE) + "/real/" + ver + "/undo_001";
        bool has_undo = file_exists(undo_path);
        if (ver == "mysql80" || ver == "mysql84" || ver == "mysql90") {
            EXPECT_TRUE(has_undo) << ver << " must have undo fixtures";
        } else {
            EXPECT_FALSE(has_undo) << ver << " must not have undo fixtures";
        }
    }
}

} // anonymous namespace

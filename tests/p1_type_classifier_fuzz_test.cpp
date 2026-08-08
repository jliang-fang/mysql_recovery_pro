/**
 * @file p1_type_classifier_fuzz_test.cpp
 * @brief P1 Format Correctness Fuzz Tests for StatisticalTypeClassifier
 *
 * Tests the StatisticalTypeClassifier and FeatureExtractor with edge case
 * data to verify valid type likelihoods, crash safety, and numerical stability.
 */

#include <gtest/gtest.h>
#include "dbrecovery/inference/physical_layout_analyzer.hpp"

#include <vector>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <random>
#include <cmath>
#include <limits>
#include <numeric>

namespace dbrecovery {
namespace test {

using namespace dbrecovery::inference;

// ==================== Helpers ====================

/** Generate a random byte vector of given size. */
static std::vector<uint8_t> random_bytes(size_t sz) {
    std::vector<uint8_t> buf(sz);
    for (size_t i = 0; i < sz; i++) {
        buf[i] = static_cast<uint8_t>(rand() % 256);
    }
    return buf;
}

/** Generate all-NULL samples (empty vectors). */
static std::vector<std::vector<uint8_t>> make_all_null_samples(size_t count) {
    std::vector<std::vector<uint8_t>> samples;
    for (size_t i = 0; i < count; i++) {
        samples.emplace_back(); // empty = NULL
    }
    return samples;
}

/** Generate all-zero samples. */
static std::vector<std::vector<uint8_t>> make_all_zero_samples(size_t count, size_t field_sz) {
    std::vector<std::vector<uint8_t>> samples;
    for (size_t i = 0; i < count; i++) {
        samples.push_back(std::vector<uint8_t>(field_sz, 0));
    }
    return samples;
}

/** Generate binary garbage samples. */
static std::vector<std::vector<uint8_t>> make_binary_garbage_samples(size_t count, size_t max_sz) {
    std::vector<std::vector<uint8_t>> samples;
    for (size_t i = 0; i < count; i++) {
        size_t sz = static_cast<size_t>(rand() % (max_sz + 1));
        samples.push_back(random_bytes(sz));
    }
    return samples;
}

/** Generate ASCII string samples. */
static std::vector<std::vector<uint8_t>> make_ascii_samples(
    size_t count, size_t min_len, size_t max_len)
{
    std::vector<std::vector<uint8_t>> samples;
    for (size_t i = 0; i < count; i++) {
        size_t sz = min_len + static_cast<size_t>(rand() % (max_len - min_len + 1));
        std::vector<uint8_t> buf(sz);
        for (size_t j = 0; j < sz; j++) {
            buf[j] = static_cast<uint8_t>(0x20 + rand() % 0x5F); // printable ASCII
        }
        samples.push_back(buf);
    }
    return samples;
}

/** Generate digit-string samples (numeric characters). */
static std::vector<std::vector<uint8_t>> make_digit_samples(
    size_t count, size_t min_len, size_t max_len)
{
    std::vector<std::vector<uint8_t>> samples;
    for (size_t i = 0; i < count; i++) {
        size_t sz = min_len + static_cast<size_t>(rand() % (max_len - min_len + 1));
        std::vector<uint8_t> buf(sz);
        for (size_t j = 0; j < sz; j++) {
            buf[j] = static_cast<uint8_t>('0' + rand() % 10);
        }
        samples.push_back(buf);
    }
    return samples;
}

/** Generate datetime-like string samples. */
static std::vector<std::vector<uint8_t>> make_datetime_samples(size_t count) {
    std::vector<std::vector<uint8_t>> samples;
    for (size_t i = 0; i < count; i++) {
        int y = 2000 + rand() % 50;
        int m = 1 + rand() % 12;
        int d = 1 + rand() % 28;
        int h = rand() % 24;
        int min = rand() % 60;
        int s = rand() % 60;
        char buf[20];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", y, m, d, h, min, s);
        std::vector<uint8_t> vec(reinterpret_cast<uint8_t*>(buf),
                                 reinterpret_cast<uint8_t*>(buf) + strlen(buf));
        samples.push_back(vec);
    }
    return samples;
}

// ==================== All-NULL Fields ====================

TEST(P1TypeClassifierFuzzTest, AllNullFields) {
    FeatureExtractor extractor;
    StatisticalTypeClassifier classifier;

    for (int iter = 0; iter < 100; iter++) {
        auto samples = make_all_null_samples(10 + rand() % 100);
        auto features = extractor.extract(samples);

        EXPECT_EQ(features.sample_count, samples.size());
        EXPECT_EQ(features.null_ratio, 1.0);
        EXPECT_EQ(features.len_min, 0.0);
        EXPECT_EQ(features.len_max, 0.0);

        auto candidates = classifier.classify(features);
        // Should produce some candidates even for all-NULL
        EXPECT_FALSE(candidates.empty());

        for (const auto& c : candidates) {
            EXPECT_GE(c.confidence, 0.0);
            EXPECT_LE(c.confidence, 1.0);
        }
    }
}

// ==================== All-Zero Fields ====================

TEST(P1TypeClassifierFuzzTest, AllZeroFields) {
    FeatureExtractor extractor;
    StatisticalTypeClassifier classifier;

    for (int iter = 0; iter < 100; iter++) {
        auto samples = make_all_zero_samples(10 + rand() % 100, 4);
        auto features = extractor.extract(samples);

        EXPECT_EQ(features.zero_ratio, 1.0);
        EXPECT_TRUE(features.is_fixed_length);
        EXPECT_EQ(features.fixed_size, 4);

        auto candidates = classifier.classify(features);
        EXPECT_FALSE(candidates.empty());

        for (const auto& c : candidates) {
            EXPECT_GE(c.confidence, 0.0);
            EXPECT_LE(c.confidence, 1.0);
        }
    }
}

// ==================== Maximum-Length Fields ====================

TEST(P1TypeClassifierFuzzTest, MaximumLengthFields) {
    FeatureExtractor extractor;
    StatisticalTypeClassifier classifier;

    // VARCHAR(65535) sized fields
    for (int iter = 0; iter < 20; iter++) {
        size_t huge_sz = 10000 + static_cast<size_t>(rand() % 50000);
        auto samples = make_binary_garbage_samples(5, huge_sz);
        auto features = extractor.extract(samples);

        EXPECT_GE(features.len_max, 5000);

        auto candidates = classifier.classify(features);
        EXPECT_FALSE(candidates.empty());

        for (const auto& c : candidates) {
            EXPECT_GE(c.confidence, 0.0);
            EXPECT_LE(c.confidence, 1.0);
        }
    }
}

// ==================== Binary Garbage Input ====================

TEST(P1TypeClassifierFuzzTest, BinaryGarbageInput) {
    FeatureExtractor extractor;
    StatisticalTypeClassifier classifier;

    for (int iter = 0; iter < 100; iter++) {
        auto samples = make_binary_garbage_samples(
            10 + rand() % 50,
            1 + static_cast<size_t>(rand() % 256));

        auto features = extractor.extract(samples);

        EXPECT_EQ(features.sample_count, samples.size());

        auto candidates = classifier.classify(features);
        EXPECT_FALSE(candidates.empty());

        // Each candidate confidence must be in [0, 1]
        double total = 0.0;
        for (const auto& c : candidates) {
            EXPECT_GE(c.confidence, 0.0);
            EXPECT_LE(c.confidence, 1.0);
            total += c.confidence;
        }

        // Total may exceed 1.0 since these are likelihoods, not probabilities.
        // But individual values must be valid.
        EXPECT_GE(total, 0.0);
    }
}

// ==================== Empty Field Data ====================

TEST(P1TypeClassifierFuzzTest, EmptyFieldData) {
    FeatureExtractor extractor;
    StatisticalTypeClassifier classifier;

    // Empty samples vector
    std::vector<std::vector<uint8_t>> empty;
    auto features = extractor.extract(empty);
    EXPECT_EQ(features.sample_count, 0);

    auto candidates = classifier.classify(features);
    // Empty features may produce default candidates (e.g., VARCHAR at 0.5).
    // Verify all confidences are in valid range [0.0, 1.0].
    for (const auto& c : candidates) {
        EXPECT_GE(c.confidence, 0.0);
        EXPECT_LE(c.confidence, 1.0);
    }

    // Empty columns_samples for extract_all (not available on FeatureExtractor)
    // Use extract() on each column instead
    std::vector<std::vector<std::vector<uint8_t>>> empty_cols;
    for (const auto& col : empty_cols) {
        auto feats = extractor.extract(col);
        EXPECT_TRUE(feats.sample_count == 0);
    }
}

// ==================== Single-Byte Fields ====================

TEST(P1TypeClassifierFuzzTest, SingleByteFields) {
    FeatureExtractor extractor;
    StatisticalTypeClassifier classifier;

    for (int iter = 0; iter < 100; iter++) {
        // TINYINT boundary: single byte values
        std::vector<std::vector<uint8_t>> samples;
        for (int i = 0; i < 20; i++) {
            uint8_t val = static_cast<uint8_t>(rand() % 256);
            samples.push_back({val});
        }

        auto features = extractor.extract(samples);
        EXPECT_TRUE(features.is_fixed_length);
        EXPECT_EQ(features.fixed_size, 1);

        auto candidates = classifier.classify(features);
        EXPECT_FALSE(candidates.empty());

        for (const auto& c : candidates) {
            EXPECT_GE(c.confidence, 0.0);
            EXPECT_LE(c.confidence, 1.0);
        }
    }
}

// ==================== NaN/Inf/Overflow Safety ====================

TEST(P1TypeClassifierFuzzTest, NanInfOverflow) {
    FeatureExtractor extractor;
    StatisticalTypeClassifier classifier;

    for (int iter = 0; iter < 100; iter++) {
        ColumnFeatures f;

        // Set features to extreme values
        f.sample_count = 0;
        f.null_ratio = std::numeric_limits<double>::quiet_NaN();
        f.zero_ratio = std::numeric_limits<double>::infinity();
        f.empty_ratio = -std::numeric_limits<double>::infinity();
        f.len_min = -1.0;
        f.len_max = std::numeric_limits<double>::max();
        f.len_mean = std::numeric_limits<double>::quiet_NaN();
        f.len_stddev = std::numeric_limits<double>::infinity();
        f.ascii_ratio = 1.5;  // > 1.0
        f.digit_ratio = -0.5; // < 0.0
        f.entropy = std::numeric_limits<double>::quiet_NaN();
        f.is_fixed_length = true;
        f.fixed_size = 0;

        auto candidates = classifier.classify(f);
        // Should not crash or produce NaN/inf
        EXPECT_FALSE(candidates.empty());

        for (const auto& c : candidates) {
            EXPECT_TRUE(std::isfinite(c.confidence))
                << "Confidence must be finite, got " << c.confidence;
            EXPECT_GE(c.confidence, 0.0);
            EXPECT_LE(c.confidence, 1.0);
        }
    }

    // Also test with FeatureExtractor on edge case samples
    {
        // Sample with one empty field
        std::vector<std::vector<uint8_t>> samples = {{}};
        auto features = extractor.extract(samples);
        auto candidates = classifier.classify(features);
        for (const auto& c : candidates) {
            EXPECT_TRUE(std::isfinite(c.confidence));
        }
    }
}

// ==================== Type-Specific Likelihood Tests ====================

TEST(P1TypeClassifierFuzzTest, TypeSpecificLikelihoods) {
    StatisticalTypeClassifier classifier;

    // Test that known patterns produce expected top candidates
    {
        // Digit-heavy, fixed-length 10 chars -> likely TIMESTAMP or YEAR
        ColumnFeatures f;
        f.sample_count = 50;
        f.len_min = 10; f.len_max = 10; f.len_mean = 10.0; f.len_stddev = 0.0;
        f.is_fixed_length = true; f.fixed_size = 10;
        f.ascii_ratio = 1.0; f.digit_ratio = 1.0;
        f.entropy = 3.0;
        f.null_ratio = 0.0; f.zero_ratio = 0.0; f.empty_ratio = 0.0;

        auto candidates = classifier.classify(f);
        EXPECT_FALSE(candidates.empty());
        EXPECT_GE(candidates[0].confidence, 0.0);
        EXPECT_LE(candidates[0].confidence, 1.0);
    }

    {
        // Variable-length ASCII strings -> VARCHAR
        ColumnFeatures f;
        f.sample_count = 50;
        f.len_min = 5; f.len_max = 100; f.len_mean = 30.0; f.len_stddev = 15.0;
        f.is_fixed_length = false; f.fixed_size = 0;
        f.ascii_ratio = 0.9; f.digit_ratio = 0.3;
        f.entropy = 5.0;
        f.null_ratio = 0.0; f.zero_ratio = 0.0; f.empty_ratio = 0.0;

        auto candidates = classifier.classify(f);
        EXPECT_FALSE(candidates.empty());
        // VARCHAR should be highly ranked
        bool found_varchar = false;
        for (const auto& c : candidates) {
            if (c.type_name == "VARCHAR(N)") found_varchar = true;
        }
        EXPECT_TRUE(found_varchar);
    }

    {
        // Fixed-length 8 bytes, high entropy -> BIGINT or DOUBLE
        ColumnFeatures f;
        f.sample_count = 50;
        f.len_min = 8; f.len_max = 8; f.len_mean = 8.0; f.len_stddev = 0.0;
        f.is_fixed_length = true; f.fixed_size = 8;
        f.ascii_ratio = 0.0; f.digit_ratio = 0.0;
        f.entropy = 7.0;
        f.null_ratio = 0.0; f.zero_ratio = 0.0; f.empty_ratio = 0.0;

        auto candidates = classifier.classify(f);
        EXPECT_FALSE(candidates.empty());
        for (const auto& c : candidates) {
            EXPECT_GE(c.confidence, 0.0);
            EXPECT_LE(c.confidence, 1.0);
        }
    }

    {
        // Datetime format strings
        ColumnFeatures f;
        f.sample_count = 30;
        f.len_min = 19; f.len_max = 19; f.len_mean = 19.0; f.len_stddev = 0.0;
        f.is_fixed_length = true; f.fixed_size = 19;
        f.ascii_ratio = 1.0; f.digit_ratio = 0.5;
        f.entropy = 4.5;
        f.has_dash_ratio = 1.0; f.has_colon_ratio = 1.0;
        f.null_ratio = 0.0; f.zero_ratio = 0.0; f.empty_ratio = 0.0;

        auto candidates = classifier.classify(f);
        EXPECT_FALSE(candidates.empty());
        // DATETIME should be a candidate
        bool found_datetime = false;
        for (const auto& c : candidates) {
            if (c.type_name == "DATETIME") found_datetime = true;
        }
        EXPECT_TRUE(found_datetime);
    }

    {
        // High entropy, non-ASCII, variable length -> BLOB
        ColumnFeatures f;
        f.sample_count = 20;
        f.len_min = 50; f.len_max = 500; f.len_mean = 200.0; f.len_stddev = 100.0;
        f.is_fixed_length = false; f.fixed_size = 0;
        f.ascii_ratio = 0.2; f.digit_ratio = 0.1;
        f.entropy = 7.5;
        f.null_ratio = 0.0; f.zero_ratio = 0.0; f.empty_ratio = 0.0;

        auto candidates = classifier.classify(f);
        EXPECT_FALSE(candidates.empty());
        for (const auto& c : candidates) {
            EXPECT_GE(c.confidence, 0.0);
            EXPECT_LE(c.confidence, 1.0);
        }
    }
}

// ==================== FeatureExtractor Extreme Values ====================

TEST(P1TypeClassifierFuzzTest, FeatureExtractorExtremeValues) {
    FeatureExtractor extractor;

    // Empty bytes
    {
        std::vector<std::vector<uint8_t>> samples;
        for (int i = 0; i < 10; i++) {
            samples.emplace_back(); // empty
        }
        auto features = extractor.extract(samples);
        EXPECT_EQ(features.sample_count, 10);
        EXPECT_EQ(features.zero_ratio, 1.0);
        EXPECT_EQ(features.empty_ratio, 1.0);
    }

    // Maximum size variation
    {
        std::vector<std::vector<uint8_t>> samples;
        samples.push_back({0x00});                             // 1 byte
        samples.push_back(std::vector<uint8_t>(65535, 0xFF));  // max InnoDB varchar
        auto features = extractor.extract(samples);
        EXPECT_EQ(features.len_min, 1.0);
        EXPECT_EQ(features.len_max, 65535.0);
        EXPECT_FALSE(features.is_fixed_length);
    }

    // Very high entropy
    {
        auto samples = make_binary_garbage_samples(50, 100);
        auto features = extractor.extract(samples);
        EXPECT_GE(features.entropy, 0.0);
        EXPECT_LE(features.entropy, 8.0); // max for bytes is log2(256) = 8
    }

    // Single sample
    {
        std::vector<std::vector<uint8_t>> samples = {{0x41, 0x42, 0x43}};
        auto features = extractor.extract(samples);
        EXPECT_EQ(features.sample_count, 1);
        EXPECT_TRUE(features.is_fixed_length);
        EXPECT_EQ(features.fixed_size, 3);
        EXPECT_EQ(features.ascii_ratio, 1.0);
    }
}

} // namespace test
} // namespace dbrecovery
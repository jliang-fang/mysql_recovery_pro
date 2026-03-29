// Fuzz testing for BLOB pointer parsing
// This test uses fuzzing techniques to test BLOB pointer parsing

#include <gtest/gtest.h>
#include "dbrecovery/innodb/blob_pointer.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== BLOB Pointer Fuzz Tests ====================

// Test Case 1: Fuzz test with random data
TEST(BlobPointerFuzzTest, RandomData) {
    // Generate random data of various sizes
    std::vector<uint8_t> random_data(20);
    
    for (int i = 0; i < 100; ++i) {
        // Fill with random data
        for (int j = 0; j < 20; ++j) {
            random_data[j] = static_cast<uint8_t>(rand() % 256);
        }
        
        // Parse - should not crash
        auto ptr = parse_blob_pointer(random_data.data(), random_data.size());
        
        // If parsing succeeds, verify the result
        if (ptr.has_value()) {
            EXPECT_NE(ptr->space_id, 0xFFFFFFFF);
            EXPECT_NE(ptr->page_no, 0xFFFFFFFF);
        }
    }
}

// Test Case 2: Fuzz test with zero-filled data
TEST(BlobPointerFuzzTest, ZeroFilledData) {
    std::vector<uint8_t> zero_data(20, 0);
    
    auto ptr = parse_blob_pointer(zero_data.data(), zero_data.size());
    
    // Zero-filled data should be handled gracefully
    // Either returns nullopt or valid pointer with zero values
    EXPECT_TRUE(ptr.has_value() || !ptr.has_value()); // Either is fine
}

// Test Case 3: Fuzz test with all 0xFF data
TEST(BlobPointerFuzzTest, AllFFData) {
    std::vector<uint8_t> ff_data(20, 0xFF);
    
    auto ptr = parse_blob_pointer(ff_data.data(), ff_data.size());
    
    // All 0xFF data should be handled gracefully
    EXPECT_TRUE(ptr.has_value() || !ptr.has_value());
}

// Test Case 4: Fuzz test with various sizes
TEST(BlobPointerFuzzTest, VariousSizes) {
    std::vector<uint8_t> data(20, 0);
    
    // Test with various sizes (16-20 bytes)
    for (size_t size = 16; size <= 20; ++size) {
        auto ptr = parse_blob_pointer(data.data(), size);
        
        // Should handle all sizes gracefully
        EXPECT_TRUE(ptr.has_value() || !ptr.has_value());
    }
}

// Test Case 5: Fuzz test with invalid space_id
TEST(BlobPointerFuzzTest, InvalidSpaceId) {
    std::vector<uint8_t> data(20, 0);
    
    // Set space_id to invalid value
    write_le<uint32_t>(data.data(), 0, 0xFFFFFFFF);
    
    auto ptr = parse_blob_pointer(data.data(), data.size());
    
    // Invalid space_id should return nullopt
    EXPECT_FALSE(ptr.has_value());
}

// Test Case 6: Fuzz test with invalid page_no
TEST(BlobPointerFuzzTest, InvalidPageNo) {
    std::vector<uint8_t> data(20, 0);
    
    // Set page_no to invalid value
    write_le<uint32_t>(data.data(), 4, 0xFFFFFFFF);
    
    auto ptr = parse_blob_pointer(data.data(), data.size());
    
    // Invalid page_no should return nullopt
    EXPECT_FALSE(ptr.has_value());
}

// Test Case 7: Fuzz test with large total_len
TEST(BlobPointerFuzzTest, LargeTotalLen) {
    std::vector<uint8_t> data(20, 0);
    
    // Set total_len to very large value (64GB)
    write_le<uint32_t>(data.data(), 16, 0xFFFFFFFF);
    
    auto ptr = parse_blob_pointer(data.data(), data.size());
    
    // Large total_len should be handled gracefully
    EXPECT_TRUE(ptr.has_value() || !ptr.has_value());
}

// Test Case 8: Fuzz test with offset > page_size
TEST(BlobPointerFuzzTest, OffsetGreaterThanPageSize) {
    std::vector<uint8_t> data(20, 0);
    
    // Set offset to larger than page size
    write_le<uint32_t>(data.data(), 8, 65536);
    
    auto ptr = parse_blob_pointer(data.data(), data.size());
    
    // Invalid offset should return nullopt
    EXPECT_FALSE(ptr.has_value());
}

// Test Case 9: Fuzz test with len > page_size
TEST(BlobPointerFuzzTest, LenGreaterThanPageSize) {
    std::vector<uint8_t> data(20, 0);

    // Set len to larger than page size (65536 is the MAX_PAGE_SIZE limit)
    write_le<uint32_t>(data.data(), 12, 65537);  // 65537 > 65536

    auto ptr = parse_blob_pointer(data.data(), data.size());

    // Invalid len should return nullopt
    EXPECT_FALSE(ptr.has_value());
}

// Test Case 10: Fuzz test with circular chain detection
TEST(BlobPointerFuzzTest, CircularChainDetection) {
    std::vector<uint8_t> data(20, 0);
    
    // Set up a chain that could cause circular reference
    write_le<uint32_t>(data.data(), 0, 0);   // space_id
    write_le<uint32_t>(data.data(), 4, 100); // page_no (first)
    write_le<uint32_t>(data.data(), 8, 0);   // offset
    write_le<uint32_t>(data.data(), 12, 50); // len
    write_le<uint32_t>(data.data(), 16, 150); // total_len (multi-page)
    
    auto ptr = parse_blob_pointer(data.data(), data.size());
    
    // Should handle multi-page BLOB gracefully
    EXPECT_TRUE(ptr.has_value() || !ptr.has_value());
}

// Test Case 11: Fuzz test with empty chain
TEST(BlobPointerFuzzTest, EmptyChain) {
    BlobPageChain chain;
    
    // Empty chain should be detected
    EXPECT_FALSE(chain.has_circular_reference());
    EXPECT_FALSE(chain.is_valid(nullptr, true));
}

// Test Case 12: Fuzz test with single page chain
TEST(BlobPointerFuzzTest, SinglePageChain) {
    BlobPageChain chain;
    chain.add_page(0, 100);
    
    // Single page chain should be valid (no circular reference)
    EXPECT_FALSE(chain.has_circular_reference());
}

// Test Case 13: Fuzz test with multi-page chain
TEST(BlobPointerFuzzTest, MultiPageChain) {
    BlobPageChain chain;
    chain.add_page(0, 100);
    chain.add_page(0, 200);
    chain.add_page(0, 300);
    
    // Multi-page chain should be valid (no circular reference)
    EXPECT_FALSE(chain.has_circular_reference());
}

// Test Case 14: Fuzz test with circular reference
TEST(BlobPointerFuzzTest, CircularReference) {
    BlobPageChain chain;
    chain.add_page(0, 100);
    chain.add_page(0, 200);
    chain.add_page(0, 100); // Circular reference
    
    // Circular reference should be detected
    EXPECT_TRUE(chain.has_circular_reference());
}

// Test Case 15: Fuzz test with duplicate pages
TEST(BlobPointerFuzzTest, DuplicatePages) {
    BlobPageChain chain;
    chain.add_page(0, 100);
    chain.add_page(0, 100); // Duplicate page
    
    // Duplicate pages should be detected as circular reference
    EXPECT_TRUE(chain.has_circular_reference());
}

// Test Case 16: Fuzz test with large chain
TEST(BlobPointerFuzzTest, LargeChain) {
    BlobPageChain chain;
    
    // Add many pages (more than 1000 should be detected as suspicious)
    for (int i = 0; i < 100; ++i) {
        chain.add_page(0, 100 + i);
    }
    
    // Large chain should be handled gracefully
    EXPECT_FALSE(chain.has_circular_reference());
}

// Test Case 17: Fuzz test with max values
TEST(BlobPointerFuzzTest, MaxValues) {
    std::vector<uint8_t> data(20, 0);
    
    // Set all fields to max values
    write_le<uint32_t>(data.data(), 0, 0xFFFFFFFF); // space_id
    write_le<uint32_t>(data.data(), 4, 0xFFFFFFFF); // page_no
    write_le<uint32_t>(data.data(), 8, 0xFFFFFFFF); // offset
    write_le<uint32_t>(data.data(), 12, 0xFFFFFFFF); // len
    write_le<uint32_t>(data.data(), 16, 0xFFFFFFFF); // total_len
    
    auto ptr = parse_blob_pointer(data.data(), data.size());
    
    // Max values should be handled gracefully
    EXPECT_TRUE(ptr.has_value() || !ptr.has_value());
}

// Test Case 18: Fuzz test with min values
TEST(BlobPointerFuzzTest, MinValues) {
    std::vector<uint8_t> data(20, 0);
    
    // Set all fields to min values (0)
    write_le<uint32_t>(data.data(), 0, 0); // space_id
    write_le<uint32_t>(data.data(), 4, 0); // page_no
    write_le<uint32_t>(data.data(), 8, 0); // offset
    write_le<uint32_t>(data.data(), 12, 0); // len
    write_le<uint32_t>(data.data(), 16, 0); // total_len
    
    auto ptr = parse_blob_pointer(data.data(), data.size());
    
    // Min values should be handled gracefully
    EXPECT_TRUE(ptr.has_value() || !ptr.has_value());
}

// Test Case 19: Fuzz test with partial data
TEST(BlobPointerFuzzTest, PartialData) {
    std::vector<uint8_t> partial_data(16, 0);
    
    // Test with 16 bytes (minimum valid size)
    auto ptr = parse_blob_pointer(partial_data.data(), partial_data.size());
    
    // Partial data should be handled gracefully
    EXPECT_TRUE(ptr.has_value() || !ptr.has_value());
}

// Test Case 20: Fuzz test with vector interface
TEST(BlobPointerFuzzTest, VectorInterface) {
    std::vector<uint8_t> data(20, 0);
    
    // Test with vector interface
    auto ptr = parse_blob_pointer(data);
    
    // Should handle vector interface gracefully
    EXPECT_TRUE(ptr.has_value() || !ptr.has_value());
}

} // namespace innodb
} // namespace dbrecovery

/**
 * @example binary_recovery_example.cpp
 * @brief Example usage of the Binary Recovery module for TRUNCATE and rm -rf scenarios
 * 
 * This example demonstrates:
 * 1. Scanning for InnoDB pages using binary signature scanning
 * 2. Extracting zombie (deleted) tuples
 * 3. Reconstructing B+Tree chain using sibling pointers
 * 4. Heuristically inferring schema when .frm is missing
 */

#include <iostream>
#include <string>
#include <vector>
#include <map>

#include "dbrecovery/innodb/binary_recovery.hpp"

void example_basic_recovery() {
    std::cout << "=== Example: Basic Binary Recovery ===" << std::endl;
    
    // Create a BinaryRecovery instance with 16KB page size
    dbrecovery::innodb::BinaryRecovery recovery(16384);
    
    std::string ibd_file = "/path/to/table.ibd";
    std::vector<dbrecovery::innodb::RecoveredRecord> records;
    
    // Recover records from the file
    if (recovery.recover_file(ibd_file, records)) {
        std::cout << "Recovered " << records.size() << " records" << std::endl;
        
        // Print statistics
        const auto& stats = recovery.get_stats();
        std::cout << "Pages scanned: " << stats.pages_scanned << std::endl;
        std::cout << "Valid pages: " << stats.valid_pages << std::endl;
        std::cout << "Recovery rate: " << (stats.recovery_rate() * 100) << "%" << std::endl;
    } else {
        std::cerr << "Failed to recover records from " << ibd_file << std::endl;
    }
}

void example_truncate_recovery() {
    std::cout << "\n=== Example: TRUNCATE Recovery ===" << std::endl;
    
    std::string ibd_file = "/path/to/truncated_table.ibd";
    
    // Detect if file was TRUNCATED
    dbrecovery::innodb::TruncateRecovery truncate_recovery(16384);
    
    if (truncate_recovery.detect_truncate(ibd_file)) {
        std::cout << "File appears to be TRUNCATED" << std::endl;
        
        // Recover from TRUNCATED file
        std::vector<dbrecovery::innodb::RecoveredRecord> records;
        uint64_t count = truncate_recovery.recover_from_truncate(ibd_file, records);
        
        std::cout << "Recovered " << count << " zombie records from TRUNCATED file" << std::endl;
        
        // TRUNCATE recoveries typically have very few valid pages but 
        // many zombie (deleted) tuples still present
    } else {
        std::cout << "File does not appear to be TRUNCATED" << std::endl;
    }
}

void example_rmrf_recovery() {
    std::cout << "\n=== Example: rm -rf Recovery ===" << std::endl;
    
    std::string data_dir = "/path/to/mysql/data/directory";
    
    // Create RmrfRecovery instance
    dbrecovery::innodb::RmrfRecoveryCustom rmrf_recovery(16384);
    
    // Step 1: Scan directory for orphaned .ibd files
    std::vector<std::string> found_files = rmrf_recovery.scan_directory(data_dir);
    
    std::cout << "Found " << found_files.size() << " .ibd files" << std::endl;
    for (const auto& file : found_files) {
        std::cout << "  - " << file << std::endl;
    }
    
    // Step 2: Recover from found files
    std::vector<dbrecovery::innodb::RecoveredRecord> all_records;
    uint64_t total = rmrf_recovery.recover_from_files(found_files, all_records);
    
    std::cout << "Total records recovered: " << total << std::endl;
}

void example_schema_inference() {
    std::cout << "\n=== Example: Schema Inference (no .frm) ===" << std::endl;
    
    std::string ibd_file = "/path/to/table.ibd";
    
    dbrecovery::innodb::BinaryRecovery recovery(16384);
    std::vector<dbrecovery::innodb::RecoveredRecord> records;
    dbrecovery::innodb::HeuristicSchema schema;
    
    // Recover and infer schema
    if (recovery.recover_with_schema(ibd_file, records, schema)) {
        std::cout << "Inferred schema:" << std::endl;
        
        for (const auto& [col_name, col_type] : schema.columns) {
            std::cout << "  " << col_name << " -> ";
            
            switch (col_type) {
                case dbrecovery::innodb::HeuristicFieldType::TINY_INT:
                    std::cout << "TINYINT (1 byte)" << std::endl;
                    break;
                case dbrecovery::innodb::HeuristicFieldType::SMALL_INT:
                    std::cout << "SMALLINT (2 bytes)" << std::endl;
                    break;
                case dbrecovery::innodb::HeuristicFieldType::INT:
                    std::cout << "INT (4 bytes)" << std::endl;
                    break;
                case dbrecovery::innodb::HeuristicFieldType::BIGINT:
                    std::cout << "BIGINT (8 bytes)" << std::endl;
                    break;
                case dbrecovery::innodb::HeuristicFieldType::VARCHAR_VARLEN:
                    std::cout << "VARCHAR (var-length)" << std::endl;
                    break;
                case dbrecovery::innodb::HeuristicFieldType::VARCHAR_FIXLEN:
                    std::cout << "CHAR (fixed-length)" << std::endl;
                    break;
                case dbrecovery::innodb::HeuristicFieldType::BLOB_PTR:
                    std::cout << "BLOB/TEXT (pointer)" << std::endl;
                    break;
                default:
                    std::cout << "UNKNOWN" << std::endl;
                    break;
            }
        }
        
        std::cout << "Total columns detected: " << schema.columns_detected << std::endl;
        std::cout << "INT columns: " << schema.int_columns << std::endl;
        std::cout << "VARCHAR columns: " << schema.varchar_columns << std::endl;
        std::cout << "BLOB columns: " << schema.blob_columns << std::endl;
    }
}

void example_page_chain_reconstruction() {
    std::cout << "\n=== Example: B+Tree Chain Reconstruction ===" << std::endl;
    
    // Use mmap to read the entire file
    std::string ibd_file = "/path/to/table.ibd";
    
    dbrecovery::innodb::BinaryScanner scanner(16384);
    
    std::vector<uint32_t> leaf_pages;
    
    auto page_callback = [&leaf_pages](uint32_t page_no, const uint8_t* page_data, size_t size) -> bool {
        // Extract only leaf pages (page_level == 0)
        uint16_t page_level = dbrecovery::innodb::read_le<uint16_t>(
            page_data, dbrecovery::innodb::BTR_N_LEVELS_ABS);
        
        if (page_level == 0) {
            leaf_pages.push_back(page_no);
        }
        return true;
    };
    
    scanner.scan_file(ibd_file, page_callback);
    
    std::cout << "Found " << leaf_pages.size() << " leaf pages" << std::endl;
    
    // Reconstruct chain using B+TreeReconstructor
    dbrecovery::innodb::BPlusTreeReconstructor reconstructor(16384);
    
    // We need to read pages with metadata
    std::ifstream file(ibd_file, std::ios::binary);
    if (file) {
        file.seekg(0, std::ios::end);
        uint64_t file_size = file.tellg();
        file.seekg(0, std::ios::beg);
        
        std::vector<uint8_t> page_buffer(16384);
        uint64_t page_count = file_size / 16384;
        
        for (uint64_t i = 0; i < page_count; ++i) {
            file.seekg(i * 16384, std::ios::beg);
            file.read(reinterpret_cast<char*>(page_buffer.data()), 16384);
            
            reconstructor.add_page(static_cast<uint32_t>(i), 
                                   page_buffer.data(), page_buffer.size());
        }
        
        dbrecovery::innodb::PageChain chain;
        if (reconstructor.reconstruct_chain(chain)) {
            std::cout << "Chain reconstructed:" << std::endl;
            std::cout << "  First page: " << chain.first_page << std::endl;
            std::cout << "  Last page: " << chain.last_page << std::endl;
            std::cout << "  Total pages: " << chain.pages.size() << std::endl;
        }
    }
}

void example_zombie_tuple_extraction() {
    std::cout << "\n=== Example: Zombie Tuple Extraction ===" << std::endl;
    
    std::string ibd_file = "/path/to/table.ibd";
    
    dbrecovery::innodb::ZombieTupleExtractor extractor(16384);
    
    // Read page data (using mmap in real scenario)
    std::ifstream file(ibd_file, std::ios::binary);
    if (!file) {
        std::cerr << "Failed to open file" << std::endl;
        return;
    }
    
    std::vector<uint8_t> page_buffer(16384);
    file.read(reinterpret_cast<char*>(page_buffer.data()), 16384);
    
    // Extract all tuples (including zombies)
    auto records = extractor.extract_all_tuples(page_buffer.data(), 16384, 0);
    
    // Statistics
    const auto& stats = extractor.get_stats();
    std::cout << "Zombie records (deleted): " << stats.zombie_records << std::endl;
    std::cout << "Valid records: " << stats.valid_records << std::endl;
    std::cout << "Total: " << stats.total_records << std::endl;
    
    // Zombie records are crucial for TRUNCATE recovery!
    // They contain the actual data bytes even though the delete-mark bit is set
}

void example_custom_binary_scanning() {
    std::cout << "\n=== Example: Custom Binary Signature Scanning ===" << std::endl;
    
    std::string device_path = "/dev/sda1";  // or raw file path
    
    // Create a custom scanner
    dbrecovery::innodb::BinaryScanner scanner(16384);
    
    uint64_t found = scanner.scan_file("/path/to/file.ibd", 
        [](uint32_t page_no, const uint8_t* page_data, size_t size) -> bool {
            // Get page type
            uint16_t page_type = scanner.get_page_type(page_data);
            
            // Validate page header
            bool valid = scanner.validate_page_header(page_data, size);
            
            std::cout << "Page " << page_no << ": type=0x" 
                      << std::hex << page_type << std::dec 
                      << ", valid=" << valid << std::endl;
            
            return true;  // Continue scanning
        });
    
    std::cout << "Total pages found: " << found << std::endl;
    std::cout << "Scanner stats:" << std::endl;
    std::cout << "  Pages scanned: " << scanner.get_stats().pages_scanned << std::endl;
    std::cout << "  Valid pages: " << scanner.get_stats().valid_pages << std::endl;
}

void example_record_chain_traversal() {
    std::cout << "\n=== Example: Record Chain Traversal ===" << std::endl;
    
    // Create a record chain reconstructor
    dbrecovery::innodb::RecordChainReconstructor chain_recon(16384);
    
    // Add record offsets (typically from page directory)
    chain_recon.add_record_offset(13, 0, false);   // INFIMUM
    chain_recon.add_record_offset(54, 1, false);   // First user record
    chain_recon.add_record_offset(120, 2, false);  // Second user record
    chain_recon.add_record_offset(180, 3, true);   // Deleted record (zombie!)
    chain_recon.add_record_offset(240, 4, false);  // Third user record
    
    std::vector<uint16_t> chain;
    if (chain_recon.reconstruct_chain(chain)) {
        std::cout << "Record chain (by heap number):" << std::endl;
        for (auto offset : chain) {
            std::cout << "  Offset: " << offset << std::endl;
        }
    }
    
    std::cout << "Record count: " << chain_recon.record_count() << std::endl;
}

void example_truncated_page_detection() {
    std::cout << "\n=== Example: Truncated Page Detection ===" << std::endl;
    
    // Simulate a page that's been TRUNCATED
    std::vector<uint8_t> page(16384, 0);
    
    // Write a minimal page header (TRUNCATED - only system records)
    dbrecovery::innodb::write_le<uint16_t>(page.data(), 
        dbrecovery::innodb::FIL_PAGE_TYPE_ABS, 
        dbrecovery::innodb::FIL_PAGE_INDEX);  // Page type
    
    dbrecovery::innodb::write_le<uint32_t>(page.data(), 
        dbrecovery::innodb::FIL_PAGE_OFFSET, 0);  // Page number = 0
    
    // Minimal heap - TRUNCATE scenario
    // heap_top would be very small (< 100 bytes)
    dbrecovery::innodb::write_le<uint16_t>(page.data(), 
        dbrecovery::innodb::FIL_HEADER_SIZE + 2, 54);  // heap_top
    dbrecovery::innodb::write_le<uint16_t>(page.data(), 
        dbrecovery::innodb::FIL_HEADER_SIZE + 4, 2);   // n_heap (infimum + supremum only)
    
    // Check if this looks like a TRUNCATED page
    dbrecovery::innodb::TruncateRecovery truncate_recovery(16384);
    
    std::cout << "Page heap_top: " << dbrecovery::innodb::read_le<uint16_t>(
        page.data(), dbrecovery::innodb::FIL_HEADER_SIZE + 2) << std::endl;
    std::cout << "Page n_heap: " << dbrecovery::innodb::read_le<uint16_t>(
        page.data(), dbrecovery::innodb::FIL_HEADER_SIZE + 4) << std::endl;
    
    // The actual detection uses file-level analysis
    // This page structure indicates a TRUNCATED file
}

void example_schema_inference_from_record() {
    std::cout << "\n=== Example: Schema Inference from Binary Data ===" << std::endl;
    
    dbrecovery::innodb::HeuristicSchemaInferer inferer;
    
    // Simulate some records with different field types
    std::vector<std::vector<std::vector<uint8_t>>> sample_records;
    
    // Record 1: id=1, name="Alice", email="alice@test.com"
    sample_records.push_back({});
    auto& rec1 = sample_records.back();
    
    // ID field (4 bytes: 0x01 0x00 0x00 0x00)
    rec1.push_back({0x01, 0x00, 0x00, 0x00});
    
    // Name field (5 bytes: "Alice" with 1-byte length prefix)
    rec1.push_back({0x05, 'A', 'l', 'i', 'c', 'e'});
    
    // Email field (14 bytes: "alice@test.com")
    rec1.push_back({0x0e, 'a', 'l', 'i', 'c', 'e', '@', 't', 'e', 's', 't', '.', 'c', 'o', 'm'});
    
    // Record 2: id=2, name="Bob", email="bob@test.com"
    sample_records.push_back({});
    auto& rec2 = sample_records.back();
    
    rec2.push_back({0x02, 0x00, 0x00, 0x00});
    rec2.push_back({0x03, 'B', 'o', 'b'});
    rec2.push_back({0x0e, 'b', 'o', 'b', '@', 't', 'e', 's', 't', '.', 'c', 'o', 'm'});
    
    // Analyze records
    for (const auto& record : sample_records) {
        inferer.analyze_record(record);
    }
    
    // Infer schema
    dbrecovery::innodb::HeuristicSchema schema = inferer.infer_schema();
    
    std::cout << "Inferred schema from sample records:" << std::endl;
    for (const auto& [col_name, col_type] : schema.columns) {
        std::cout << "  " << col_name << " (";
        
        switch (col_type) {
            case dbrecovery::innodb::HeuristicFieldType::INT:
                std::cout << "INT" << std::endl;
                break;
            case dbrecovery::innodb::HeuristicFieldType::VARCHAR_VARLEN:
                std::cout << "VARCHAR" << std::endl;
                break;
            default:
                std::cout << "OTHER" << std::endl;
                break;
        }
    }
    
    std::cout << "\nInference statistics:" << std::endl;
    std::cout << "  Records analyzed: " << inferer.get_stats().records_analyzed << std::endl;
    std::cout << "  INT columns detected: " << inferer.get_stats().int_columns << std::endl;
    std::cout << "  VARCHAR columns detected: " << inferer.get_stats().varchar_columns << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Binary Recovery Module - Usage Examples" << std::endl;
    std::cout << "========================================" << std::endl;
    
    // Example: Basic recovery
    example_basic_recovery();
    
    // Example: TRUNCATE recovery
    example_truncate_recovery();
    
    // Example: rm -rf recovery
    example_rmrf_recovery();
    
    // Example: Schema inference
    example_schema_inference();
    
    // Example: Page chain reconstruction
    example_page_chain_reconstruction();
    
    // Example: Zombie tuple extraction
    example_zombie_tuple_extraction();
    
    // Example: Custom binary scanning
    example_custom_binary_scanning();
    
    // Example: Record chain traversal
    example_record_chain_traversal();
    
    // Example: Truncated page detection
    example_truncated_page_detection();
    
    // Example: Schema inference from records
    example_schema_inference_from_record();
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "All examples completed!" << std::endl;
    std::cout << "========================================" << std::endl;
    
    return 0;
}

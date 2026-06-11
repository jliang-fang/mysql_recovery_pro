// =============================================================================
// Recovery Tester - Direct testing of mysql_recovery_pro recovery engine
// =============================================================================

#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include <chrono>
#include <filesystem>
#include <cstdlib>

#include "dbrecovery/innodb.h"
#include "dbrecovery/innodb/recovery_engine.hpp"
#include "dbrecovery/innodb/sdi_manager.hpp"
#include "dbrecovery/innodb/page_scanner.hpp"
#include "dbrecovery/innodb/btree_node_parser.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/recovery/engine.hpp"

namespace dbrecovery {
namespace catalog {
    // Forward declaration if needed
}
}

using namespace dbrecovery;

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " <action> <ibd_file> [options]\n";
    std::cout << "Actions:\n";
    std::cout << "  scan    - Scan and display page info\n";
    std::cout << "  recover - Recover records from IBD file\n";
    std::cout << "  speed   - Test recovery speed (100 pages)\n";
    std::cout << "  page    - Parse specific page (requires --page=<n>)\n";
}

// Helper to create test schema
catalog::TableSchema create_test_schema() {
    catalog::TableSchema schema;
    schema.table_name = "test_users";
    schema.row_format = 2;  // DYNAMIC
    schema.page_size = 16384;

    // id BIGINT PRIMARY KEY
    catalog::ColumnInfo id_col;
    id_col.name = "id";
    id_col.mysql_type_id = 8;  // LONGLONG
    id_col.is_nullable = false;
    id_col.column_length = 8;
    schema.columns[1] = id_col;

    // username VARCHAR(50)
    catalog::ColumnInfo username_col;
    username_col.name = "username";
    username_col.mysql_type_id = 15;  // VARCHAR
    username_col.is_nullable = false;
    username_col.column_length = 50;
    schema.columns[2] = username_col;

    // email VARCHAR(100)
    catalog::ColumnInfo email_col;
    email_col.name = "email";
    email_col.mysql_type_id = 15;  // VARCHAR
    email_col.is_nullable = false;
    email_col.column_length = 100;
    schema.columns[3] = email_col;

    // full_name VARCHAR(100)
    catalog::ColumnInfo name_col;
    name_col.name = "full_name";
    name_col.mysql_type_id = 15;  // VARCHAR
    name_col.is_nullable = true;
    name_col.column_length = 100;
    schema.columns[4] = name_col;

    return schema;
}

int scan_pages(const std::string& ibd_path) {
    std::cout << "Scanning pages: " << ibd_path << "\n";
    
    auto start = std::chrono::high_resolution_clock::now();
    
    innodb::PageScanner scanner(16384);
    auto pages = scanner.scan_file(ibd_path);
    
    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration<double>(end - start).count();
    
    std::cout << "Scan completed in " << elapsed << "s\n";
    std::cout << "Total pages: " << pages.size() << "\n";
    
    // Show first 10 pages
    for (size_t i = 0; i < std::min(size_t(10), pages.size()); ++i) {
        std::cout << "  Page " << i << ": " << pages[i].to_string() << "\n";
    }
    
    // Show stats
    const auto& s = scanner.stats();
    std::cout << "\nScanner Statistics:\n";
    std::cout << "  Total: " << s.total_pages << "\n";
    std::cout << "  Valid: " << s.valid_pages << "\n";
    std::cout << "  Index: " << s.index_pages << "\n";
    std::cout << "  SDI: " << s.sdi_pages << "\n";
    
    return 0;
}

int do_recover(const std::string& ibd_path, const std::string& output_path) {
    std::cout << "Recovering: " << ibd_path << "\n";
    
    auto start = std::chrono::high_resolution_clock::now();
    
    // Create recovery engine
    recovery::RecoveryEngine engine;
    
    if (!engine.open(ibd_path)) {
        std::cerr << "Failed to open file\n";
        return 1;
    }
    
    // Create schema
    auto schema = create_test_schema();
    
    // Perform truncate recovery
    auto records = engine.recover_truncate(schema);
    
    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration<double>(end - start).count();
    
    // Export results
    if (!records.empty()) {
        if (!engine.export_to_sql(records, "test_users", output_path)) {
            std::cerr << "Failed to export to SQL\n";
            return 1;
        }
    }
    
    engine.close();
    
    std::cout << "\nRecovery completed in " << elapsed << "s\n";
    std::cout << "Records recovered: " << records.size() << "\n";
    
    // Show sample records
    size_t sample_count = std::min(records.size(), size_t(5));
    std::cout << "\nFirst " << sample_count << " records:\n";
    for (size_t i = 0; i < sample_count; ++i) {
        std::cout << "  [";
        bool first = true;
        for (const auto& kv : records[i]) {
            if (!first) std::cout << ", ";
            std::cout << kv.first << "=" << kv.second;
            first = false;
        }
        std::cout << "]\n";
    }
    
    return 0;
}

int test_speed(const std::string& ibd_path, int num_pages = 100) {
    std::cout << "Testing speed for " << num_pages << " pages\n";
    
    innodb::InnoDBFileReader reader(ibd_path);
    if (!reader.is_valid()) {
        std::cerr << "Failed to open file\n";
        return 1;
    }
    
    std::cout << "File size: " << reader.get_file_size() << " bytes\n";
    std::cout << "Page size: " << reader.get_page_size() << " bytes\n";
    std::cout << "Total pages: " << reader.get_total_pages() << "\n";
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < num_pages && i < reader.get_total_pages(); ++i) {
        auto page = reader.read_page_raw(i);
        if (page.empty()) {
            std::cerr << "Failed to read page " << i << "\n";
            return 1;
        }
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration<double>(end - start).count();
    
    std::cout << "\nRead " << num_pages << " pages in " << elapsed << "s\n";
    std::cout << "Speed: " << (num_pages / elapsed) << " pages/s\n";
    
    return 0;
}

int parse_specific_page(const std::string& ibd_path, int page_no) {
    std::cout << "Parsing page " << page_no << " from " << ibd_path << "\n";
    
    innodb::InnoDBFileReader reader(ibd_path);
    if (!reader.is_valid()) {
        std::cerr << "Failed to open file\n";
        return 1;
    }
    
    auto page_data = reader.read_page_raw(page_no);
    if (page_data.empty()) {
        std::cerr << "Failed to read page " << page_no << "\n";
        return 1;
    }
    
    std::cout << "Page data size: " << page_data.size() << " bytes\n";
    
    // Read page header fields
    uint16_t page_type = innodb::read_le<uint16_t>(page_data.data(), 16);
    uint32_t page_no_read = innodb::read_le<uint32_t>(page_data.data(), 4);
    uint64_t lsn = innodb::read_le<uint64_t>(page_data.data(), 16 + 8);
    
    std::cout << "\nPage Header:\n";
    std::cout << "  Type: 0x" << std::hex << page_type << std::dec << "\n";
    std::cout << "  Page No: " << page_no_read << "\n";
    std::cout << "  LSN: " << lsn << "\n";
    
    // Parse with BPlusTreeParser
    innodb::BPlusTreeParser parser(page_data);
    if (parser.parse()) {
        std::cout << "\nB+Tree Parse: SUCCESS\n";
        std::cout << "  Node type: " << (int)parser.node_type() << "\n";
        std::cout << "  Page level: " << parser.get_page_level() << "\n";
        std::cout << "  Records: " << parser.records().size() << "\n";
    } else {
        std::cout << "\nB+Tree Parse: FAILED - " << parser.error_message() << "\n";
    }
    
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }
    
    std::string action = argv[1];
    std::string ibd_path = argv[2];
    
    // Get output path
    std::string output_path = "/tmp/recovery_output.sql";
    if (argc > 3) {
        output_path = argv[3];
    }
    
    // Check if --page= option provided
    int page_no = -1;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--page=", 0) == 0) {
            page_no = std::stoi(arg.substr(7));
        }
    }
    
    if (action == "scan") {
        return scan_pages(ibd_path);
    } else if (action == "recover") {
        return do_recover(ibd_path, output_path);
    } else if (action == "speed") {
        return test_speed(ibd_path);
    } else if (action == "page" && page_no >= 0) {
        return parse_specific_page(ibd_path, page_no);
    } else {
        std::cerr << "Unknown action: " << action << "\n";
        print_usage(argv[0]);
        return 1;
    }
    
    return 0;
}

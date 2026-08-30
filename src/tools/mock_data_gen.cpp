#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <random>
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

/**
 * @brief MockDataGenerator creates a binary file that mimics MySQL 5.6 InnoDB pages.
 * This is used to validate the recovery rate without requiring a live MySQL instance.
 */
class MockDataGenerator {
public:
    static void generate_ibd_mock(const std::string& path, size_t num_records) {
        std::ofstream ofs(path, std::ios::binary);
        if (!ofs) throw std::runtime_error("Cannot open file for writing");

        size_t page_size = dbrecovery::innodb::UNIV_PAGE_SIZE;
        size_t records_per_page = 100; // Simplified for mock
        size_t total_pages = (num_records + records_per_page - 1) / records_per_page;

        std::cout << "[GEN] Generating " << num_records << " records across " << total_pages << " pages...\n";

        for (size_t p = 0; p < total_pages; ++p) {
            std::vector<uint8_t> page(page_size, 0);
            
            // 1. FIL Header
            // Offset 24: Page Type (FIL_PAGE_INDEX / 0x45BC for 5.6)
            constexpr uint16_t type = 17852;
            dbrecovery::innodb::write_be<uint16_t>(
                page.data(), dbrecovery::innodb::FIL_PAGE_TYPE_ABS, type);

            // Offset 4: Page Number
            const uint32_t pno = static_cast<uint32_t>(p);
            dbrecovery::innodb::write_be<uint32_t>(
                page.data(), dbrecovery::innodb::FIL_PAGE_OFFSET, pno);

            // 2. Page Header
            // Offset 38: n_dir_slots, 40: heap_top
            uint16_t heap_top = 54 + (records_per_page * 64); // Rough estimate
            page[38] = 0; page[39] = 0; // n_dir_slots
            page[40] = heap_top & 0xFF;
            page[41] = (heap_top >> 8) & 0xFF;

            // 3. Records
            size_t current_offset = 54; // INFIMUM start
            for (size_t r = 0; r < records_per_page && (p * records_per_page + r) < num_records; ++r) {
                // Record Header (5 bytes)
                page[current_offset] = 0x00; // info_bits: Ordinary record, not deleted
                uint16_t next_off = static_cast<uint16_t>(current_offset + 64);
                page[current_offset+1] = next_off & 0xFF;
                page[current_offset+2] = (next_off >> 8) & 0xFF;

                // Mock Data: user_ID@email.com
                std::string email = "user_" + std::to_string(p * records_per_page + r) + "@test.com";
                for (size_t i = 0; i < email.size() && (current_offset + 5 + i) < page_size; ++i) {
                    page[current_offset + 5 + i] = email[i];
                }

                current_offset = next_off;
                if (current_offset >= page_size - 64) break;
            }

            ofs.write(reinterpret_cast<const char*>(page.data()), page_size);
        }
        ofs.close();
        std::cout << "[GEN] Mock file created successfully: " << path << "\n";
    }
};

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <output_path> <num_records>\n";
        return 1;
    }
    try {
        MockDataGenerator::generate_ibd_mock(argv[1], std::stoull(argv[2]));
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

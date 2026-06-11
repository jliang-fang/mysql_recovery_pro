// =============================================================================
// Direct test of InnoDB page recovery from live database
// =============================================================================

#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include <chrono>
#include <string>

// Minimal InnoDB constants
const uint16_t FIL_PAGE_INDEX = 9969;  // 0x26F1
const uint16_t FIL_PAGE_SDI = 9971;    // 0x26F3
const uint16_t FIL_PAGE_TYPE_ALLOCATED = 0;

namespace dbrecovery {
namespace innodb {

// Simple LE read helpers
inline uint16_t read_le16(const uint8_t* data, size_t offset) {
    return data[offset] | (data[offset + 1] << 8);
}

inline uint32_t read_le32(const uint8_t* data, size_t offset) {
    return data[offset] | (data[offset + 1] << 8) | 
           (data[offset + 2] << 16) | (data[offset + 3] << 24);
}

inline uint64_t read_le64(const uint8_t* data, size_t offset) {
    return (uint64_t)read_le32(data, offset) | ((uint64_t)read_le32(data, offset + 4) << 32);
}

// Page info structure
struct PageInfo {
    uint32_t page_no;
    uint16_t type;
    uint32_t prev_page;
    uint32_t next_page;
    uint64_t lsn;
    uint32_t space_id;
    uint16_t n_dir_slots;
    uint16_t heap_top;
};

// Read page info
bool read_page_info(const uint8_t* data, PageInfo& info) {
    if (!data) return false;
    
    info.page_no = read_le32(data, 4);
    info.prev_page = read_le32(data, 22);
    info.next_page = read_le32(data, 26);
    info.lsn = read_le64(data, 8);
    info.type = read_le16(data, 16);
    info.space_id = read_le32(data, 24);
    info.n_dir_slots = read_le16(data, 38);
    info.heap_top = read_le16(data, 40);
    
    return true;
}

} // namespace innodb
} // namespace dbrecovery

void show_page(const std::string& filename, uint32_t page_no) {
    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        std::cerr << "Cannot open file: " << filename << std::endl;
        return;
    }
    
    const size_t PAGE_SIZE = 16384;
    file.seekg(page_no * PAGE_SIZE);
    
    std::vector<uint8_t> page_data(PAGE_SIZE);
    file.read(reinterpret_cast<char*>(page_data.data()), PAGE_SIZE);
    
    auto& page = page_data;
    
    // Print first 64 bytes
    std::cout << "Page " << page_no << " header (first 64 bytes):\n";
    for (size_t i = 0; i < 64; i += 16) {
        printf("%04zx: ", i);
        for (size_t j = 0; j < 16; ++j) {
            printf("%02x ", page[i + j]);
        }
        std::cout << " |";
        for (size_t j = 0; j < 16; ++j) {
            char c = page[i + j];
            printf("%c", (c >= 32 && c < 127) ? c : '.');
        }
        std::cout << "|\n";
    }
    
    // Parse header
    dbrecovery::innodb::PageInfo info;
    if (dbrecovery::innodb::read_page_info(page.data(), info)) {
        std::cout << "\nParsed header:\n";
        std::cout << "  Page No: " << info.page_no << "\n";
        std::cout << "  Type: 0x" << std::hex << info.type << std::dec;
        std::cout << " (" << (info.type == FIL_PAGE_INDEX ? "INDEX" :
                             info.type == FIL_PAGE_SDI ? "SDI" :
                             info.type == 0 ? "ALLOCATED" : "UNKNOWN") << ")\n";
        std::cout << "  Prev/Next: " << info.prev_page << " / " << info.next_page << "\n";
        std::cout << "  LSN: " << info.lsn << "\n";
        std::cout << "  Space ID: " << info.space_id << "\n";
        std::cout << "  Dir Slots: " << info.n_dir_slots << "\n";
        std::cout << "  Heap Top: " << info.heap_top << "\n";
    }
}

bool check_all_pages(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        std::cerr << "Cannot open file: " << filename << std::endl;
        return false;
    }
    
    // Get file size
    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    const size_t PAGE_SIZE = 16384;
    size_t num_pages = file_size / PAGE_SIZE;
    
    std::cout << "File: " << filename << "\n";
    std::cout << "Size: " << file_size << " bytes (" << num_pages << " pages)\n\n";
    
    size_t index_pages = 0;
    size_t allocated_pages = 0;
    
    // Scan first 100 pages for analysis
    for (size_t i = 0; i < std::min(size_t(100), num_pages); ++i) {
        file.seekg(i * PAGE_SIZE);
        std::vector<uint8_t> header(64);
        file.read(reinterpret_cast<char*>(header.data()), 64);
        
        uint16_t type = dbrecovery::innodb::read_le16(header.data(), 16);
        uint32_t page_no = dbrecovery::innodb::read_le32(header.data(), 4);
        
        if (type == FIL_PAGE_INDEX) {
            index_pages++;
            if (index_pages <= 5) {
                std::cout << "Page " << i << ": INDEX (page_no=" << page_no << ")\n";
            }
        } else if (type == FIL_PAGE_TYPE_ALLOCATED) {
            allocated_pages++;
        }
    }
    
    std::cout << "\nFirst 100 pages summary:\n";
    std::cout << "  INDEX pages: " << index_pages << "\n";
    std::cout << "  ALLOCATED pages: " << allocated_pages << "\n";
    
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <ibd_file> [page_no]\n";
        return 1;
    }
    
    std::string filename = argv[1];
    
    if (argc > 2) {
        // Show specific page
        show_page(filename, std::stoul(argv[2]));
    } else {
        // Scan all pages
        check_all_pages(filename);
    }
    
    return 0;
}

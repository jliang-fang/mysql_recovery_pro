#include "dbrecovery/innodb/forensic_manager.hpp"
#include <iostream>
#include <fstream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace dbrecovery {
namespace innodb {

ForensicRecoveryManager::ForensicRecoveryManager() 
    : extractor_(std::make_unique<TupleExtractor>()),
      heuristic_parser_(std::make_unique<HeuristicParser>()) {}

ForensicRecoveryManager::~ForensicRecoveryManager() = default;

bool ForensicRecoveryManager::perform_deep_recovery(const std::string& target_path, const std::string& output_csv) {
    std::cout << "[FORENSIC] Starting deep recovery for: " << target_path << "\n";
    
    try {
        scanner_ = std::make_unique<InodeScanner>(target_path);
        auto page_offsets = scanner_->scan_for_pages();
        std::cout << "[FORENSIC] Found " << page_offsets.size() << " potential InnoDB pages\n";

        // Open file for mmap access to extract tuples
        int fd = open(target_path.c_str(), O_RDONLY);
        if (fd < 0) return false;
        struct stat st;
        fstat(fd, &st);
        void* mapped = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        const uint8_t* data = static_cast<const uint8_t*>(mapped);

        std::vector<HeuristicRecord> all_recovered_records;

        for (size_t offset : page_offsets) {
            uint32_t page_no = static_cast<uint32_t>(offset / UNIV_PAGE_SIZE);
            auto tuples = extractor_->extract_tuples(data + offset, page_no);
            
            for (const auto& tuple : tuples) {
                // Use HeuristicParser to turn raw binary data into a record
                // Note: we pass the tuple's raw_data and its internal offset
                auto rec = heuristic_parser_->parse_record(tuple.raw_data.data(), tuple.raw_data.size(), 0);
                if (rec) {
                    all_recovered_records.push_back(*rec);
                }
            }
        }

        std::cout << "[FORENSIC] Extracted " << all_recovered_records.size() << " total tuples\n";
        save_to_csv(output_csv, all_recovered_records);

        munmap(mapped, st.st_size);
        close(fd);
    } catch (const std::exception& e) {
        std::cerr << "[FORENSIC] Error during recovery: " << e.what() << "\n";
        return false;
    }

    return true;
}

void ForensicRecoveryManager::save_to_csv(const std::string& path, const std::vector<HeuristicRecord>& records) {
    std::ofstream out(path);
    out << "page_no,offset,is_deleted,fields\n";
    for (const auto& rec : records) {
        out << rec.offset << "," << rec.is_deleted << ",";
        for (size_t i = 0; i < rec.fields.size(); ++i) {
            out << "f" << i << "=" << (rec.fields[i].is_null ? "NULL" : "DATA") << (i == rec.fields.size() - 1 ? "" : "|");
        }
        out << "\n";
    }
    std::cout << "[FORENSIC] Results saved to " << path << "\n";
}

} // namespace innodb
} // namespace dbrecovery

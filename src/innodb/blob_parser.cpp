#include "dbrecovery/innodb/blob_pointer.hpp"
#include "dbrecovery/innodb/page_reader.hpp"
#include <iostream>
#include <vector>

namespace dbrecovery {
namespace innodb {

// ✅ 新增：BLOB 页链验证与校验和检查
bool validate_blob_page_chain(const BlobPageChain& chain,
                              const std::function<std::optional<std::vector<uint8_t>>(uint32_t, uint32_t)>& page_reader) {
    if (chain.pages.empty()) {
        return false;
    }

    for (size_t i = 0; i < chain.pages.size(); ++i) {
        const auto& [space_id, page_no] = chain.pages[i];

        auto page_data = page_reader(space_id, page_no);
        if (!page_data || page_data->size() < 62) {
            std::cerr << "[ERROR] BLOB page (" << space_id << ", " << page_no
                      << ") too small for validation" << std::endl;
            return false;
        }

        // 校验和验证
        uint32_t stored_checksum = read_le<uint32_t>(page_data->data(), 24);
        if (stored_checksum != 0 && stored_checksum != 0xFFFFFFFF) {
            std::vector<uint8_t> page_copy = *page_data;
            for (int j = 0; j < 4; ++j) {
                page_copy[24 + j] = 0;
            }

            uint32_t computed_crc32c = PageReader::compute_crc32c(page_copy);
            if (computed_crc32c != stored_checksum) {
                std::cerr << "[WARN] BLOB page checksum mismatch at ("
                          << space_id << ", " << page_no << ")" << std::endl;
                return false;
            }
        }
    }

    return true;
}

} // namespace innodb
} // namespace dbrecovery

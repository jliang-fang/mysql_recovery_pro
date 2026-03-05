#ifndef DBRECOVERY_REPAIR_REPAIR_HPP
#define DBRECOVERY_REPAIR_REPAIR_HPP

#include <cstdint>
#include <vector>
#include <optional>
#include <memory>

namespace DBRecovery {
namespace InnoDB {
    struct BlobPointer;
}
}

namespace DBRecovery {
namespace Repair {

class Repairer {
public:
    static std::optional<InnoDB::BlobPointer> get_blob_info_from_sdi(
        const uint8_t* page,
        size_t page_size,
        std::shared_ptr<class SDIManager> sdi_manager
    );

    static bool fix_checksum(uint8_t* page, size_t page_size);
};

} // namespace Repair
} // namespace DBRecovery

#endif // DBRECOVERY_REPAIR_REPAIR_HPP

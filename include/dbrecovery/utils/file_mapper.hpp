#ifndef DBRECOVERY_UTILS_FILE_MAPPER_HPP
#define DBRECOVERY_UTILS_FILE_MAPPER_HPP
#include <string>
#include <cstdint>

namespace dbrecovery {
namespace utils {

class FileMapper {
public:
    explicit FileMapper(const std::string& path);
};

} // namespace utils
} // namespace dbrecovery

#endif // DBRECOVERY_UTILS_FILE_MAPPER_HPP

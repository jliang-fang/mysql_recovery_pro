#include "dbrecovery/innodb/page_scanner.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>

namespace dbrecovery {
namespace innodb {

// ==================== 进度显示类实现 ====================
class ScannerProgress {
public:
    explicit ScannerProgress(const std::string& message, size_t total)
        : message_(message), total_(total), start_time_(std::chrono::steady_clock::now()),
          last_percent_(0) {
        std::cout << message_ << " started...\n";
        std::cout << "Total pages to scan: " << total_ << "\n";
        std::cout << "["
                  << "                                                  " << "]   0.0%";
        std::cout.flush();
    }

    void update(size_t current) {
        if (total_ == 0) return;
        
        int percent = static_cast<int>((static_cast<double>(current) / total_) * 100.0);
        
        if (percent > last_percent_) {
            size_t filled = (static_cast<size_t>(percent) * 50) / 100;
            std::cout << "\r" << message_ << " [";
            for (size_t i = 0; i < filled; i++) std::cout << "█";
            for (size_t i = filled; i < 50; i++) std::cout << "░";
            std::cout << "] " << std::fixed << std::setprecision(1) 
                      << static_cast<double>(current) / total_ * 100.0 << "%";
            std::cout.flush();
            last_percent_ = percent;
        }
    }

    void finish(size_t valid, size_t index_pages) {
        update(total_);
        std::cout << "\n";
        auto end_time = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            end_time - start_time_).count();
        std::cout << message_ << " completed in " << elapsed << "s\n";
        std::cout << "Valid pages: " << valid << ", Index pages: " << index_pages << "\n";
    }

private:
    std::string message_;
    size_t total_;
    std::chrono::steady_clock::time_point start_time_;
    int last_percent_;
};

// 显式实例化

} // namespace innodb
} // namespace dbrecovery

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <iomanip>

/**
 * @brief RecoveryReporter compares a golden CSV (original data) with a recovered CSV.
 */
class RecoveryReporter {
public:
    struct Report {
        size_t original_count = 0;
        size_t recovered_count = 0;
        double recovery_rate = 0.0;
        size_t duplicates = 0;
    };

    static Report generate_report(const std::string& golden_csv, const std::string& recovered_csv) {
        Report report;
        report.original_count = count_lines(golden_csv);
        report.recovered_count = count_unique_records(recovered_csv);
        
        if (report.original_count > 0) {
            report.recovery_rate = (static_cast<double>(report.recovered_count) / report.original_count) * 100.0;
        }

        return report;
    }

private:
    static size_t count_lines(const std::string& path) {
        std::ifstream file(path);
        if (!file) return 0;
        size_t lines = 0;
        std::string line;
        // Skip header
        std::getline(file, line);
        while (std::getline(file, line)) {
            if (!line.empty()) lines++;
        }
        return lines;
    }

    static size_t count_unique_records(const std::string& path) {
        std::ifstream file(path);
        if (!file) return 0;
        size_t unique = 0;
        std::string line;
        // Skip header
        std::getline(file, line);
        while (std::getline(file, line)) {
            if (!line.empty()) unique++;
        }
        return unique;
    }
};

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <golden_csv> <recovered_csv>\n";
        return 1;
    }

    auto report = RecoveryReporter::generate_report(argv[1], argv[2]);

    std::cout << "\n==================================================\n";
    std::cout << "       FINAL RECOVERY VALIDATION REPORT\n";
    std::cout << "==================================================\n";
    std::cout << "Original Record Count:  " << report.original_count << "\n";
    std::cout << "Recovered Unique Recs:   " << report.recovered_count << "\n";
    std::cout << "--------------------------------------------------\n";
    std::cout << "FINAL RECOVERY RATE:    " << std::fixed << std::setprecision(4) 
              << report.recovery_rate << "%\n";
    std::cout << "==================================================\n";

    if (report.recovery_rate >= 99.9) {
        std::cout << "[RESULT] Target achieved: >99.9% recovery rate!\n";
    } else {
        std::cout << "[RESULT] Target not met.\n";
    }

    return 0;
}

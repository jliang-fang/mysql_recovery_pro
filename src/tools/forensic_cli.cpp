#include <iostream>
#include <string>
#include <vector>
#include "dbrecovery/innodb/recovery_engine.hpp"
#include "dbrecovery/innodb/forensic_manager.hpp"

void print_usage() {
    std::cout << "Usage: forensic_recover [options] <target_path> <output_csv>\n"
              << "Options:\n"
              << "  --help            Show this help message\n"
              << "  --deep            Perform deep binary scanning (default)\n"
              << "\nExample:\n"
              << "  ./forensic_recover /dev/sdb1 recovered.csv\n"
              << "  ./forensic_recover corrupted.ibd recovered.csv\n";
}

int main(int argc, char** argv) {
    if (argc < 3) {
        print_usage();
        return 1;
    }

    std::string target_path = argv[argc - 2];
    std::string output_csv = argv[argc - 1];

    std::cout << "==================================================\n";
    std::cout << "   MySQL 5.6 InnoDB Forensic Recovery Tool\n";
    std::cout << "==================================================\n";
    std::cout << "[*] Target: " << target_path << "\n";
    std::cout << "[*] Output: " << output_csv << "\n";

    try {
        dbrecovery::innodb::RecoveryEngine engine;
        if (engine.deep_recover(target_path, output_csv)) {
            std::cout << "[SUCCESS] Deep recovery completed successfully.\n";
        } else {
            std::cerr << "[ERROR] Deep recovery failed.\n";
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] Unexpected error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

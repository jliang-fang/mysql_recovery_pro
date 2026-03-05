#include <iostream>
#include "dbrecovery/innodb.h"
#include "dbrecovery/innodb/v2/types.hpp"

int main() {
    std::cout << "[TEST] RecoveryEngine Basic Functionality\n";
    
    // 1. 测试 LSN 连续性校验
    std::vector<dbrecovery::innodb::v2::RedoRecord> records = {
        {100, 24, {}},
        {150, 25, {}},
        {120, 26, {}}  // LSN 回退，应触发警告
    };
    
    dbrecovery::innodb::RecoveryEngine engine;
    bool valid = engine.validate_lsn_chain(records);
    if (!valid) {
        std::cout << "[PASS] LSN gap detected correctly\n";
    } else {
        std::cerr << "[FAIL] LSN gap not detected\n";
        return 1;
    }
    
    // 2. 测试空记录校验
    std::vector<dbrecovery::innodb::v2::RedoRecord> empty_records;
    if (engine.validate_lsn_chain(empty_records)) {
        std::cout << "[PASS] Empty records handled correctly\n";
    } else {
        std::cerr << "[FAIL] Empty records validation failed\n";
        return 1;
    }
    
    std::cout << "[OK] All tests passed.\n";
    return 0;
}

#include <iostream>
#include <fstream>
#include <vector>

int main() {
    std::ifstream file("/app/mysql_recovery_test/mysql56/data/recovery_test/mixed_types.frm", std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const uint8_t* raw = data.data();
    size_t size = data.size();
    
    // pos after screen transition = 8836
    std::cout << "Bytes at pos 8836-8860:\n";
    for (size_t i = 8836; i < 8860 && i < size; i++) {
        printf("%3zu: 0x%02X ", i, raw[i]);
        if (raw[i] >= 32 && raw[i] < 127) printf("(%c)", raw[i]);
        else printf("   ");
        if ((i - 8836 + 1) % 4 == 0) printf("\n");
    }
    std::cout << "\n\n";
    
    // Compare with transaction_records (which works)
    std::ifstream file2("/app/mysql_recovery_test/mysql56/data/recovery_test/transaction_records.frm", std::ios::binary);
    std::vector<uint8_t> data2((std::istreambuf_iterator<char>(file2)), std::istreambuf_iterator<char>());
    const uint8_t* raw2 = data2.data();
    
    // transaction_records has only 13 fields, so no screen transition
    // Let's check what happens for a table with screen transition
    // The key difference: mixed_types has 25 fields, fields_per_screen = 20
    // So fields 0-18 are in first screen, fields 19-24 need second screen
    
    // After field 18 (col_time), we should have:
    // [8 bytes padding] [terminator] [spaces...] [field 19 data]
    
    // Let me check if there's another terminator before field 19
    std::cout << "Looking for field data structure...\n";
    std::cout << "Expected field 19 (col_year) should have:\n";
    std::cout << "  ordinal position (2 bytes)\n";
    std::cout << "  namesize (1 byte)\n";
    std::cout << "  name string\n";
    
    // From debug_frm5 output, field 18 ends at some position
    // Let's trace from field 18:
    // col_time has namesize=9, so: 2 (ordinal) + 1 (size) + 9 (name) = 12 bytes
    
    // Find where col_time ends
    // Field 18: ordinal=22 name='col_time'
    // Let me search for "col_year"
    std::cout << "\nSearching for 'col_year' pattern...\n";
    for (size_t i = 8500; i < size - 10; i++) {
        if (raw[i] == 'c' && raw[i+1] == 'o' && raw[i+2] == 'l' && raw[i+3] == '_') {
            if (raw[i+4] == 'y') {
                printf("Found 'col_y' at pos %zu\n", i);
                printf("Preceding bytes: ");
                for (int j = -5; j <= 0; j++) printf("%02X ", raw[i+j]);
                printf("\n");
            }
        }
    }
    
    return 0;
}

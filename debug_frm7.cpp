#include <iostream>
#include <fstream>
#include <vector>

int main() {
    std::ifstream file("/app/mysql_recovery_test/mysql56/data/recovery_test/mixed_types.frm", std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const uint8_t* raw = data.data();
    
    // Screen transition analysis
    // pos 8789 is screen transition point (after field 18)
    std::cout << "Screen transition analysis:\n";
    
    // Print from 8789 to 8860
    std::cout << "\nBytes from 8789 to 8860:\n";
    size_t start = 8789;
    for (size_t i = start; i < 8860; i++) {
        if (i == start + 8) printf(" | ");  // After 8 bytes
        if (i == 8836) printf(" >>> ");     // Our current position
        
        printf("%02X", raw[i]);
        if (raw[i] >= 32 && raw[i] < 127) printf("(%c)", raw[i]);
        else printf("   ");
        
        if ((i - start + 1) % 8 == 0 && i < start + 8) printf("\n");
        else if ((i - start + 1) % 10 == 0) printf("\n");
    }
    std::cout << "\n";
    
    // ibd2sql logic:
    // self.data.read(8) -> skip 8 bytes
    // _terminator = self.data.read(1) -> read terminator
    // while _terminator == b' ': _terminator = self.data.read(1)
    
    // So after 8 bytes (8797), we read terminator = 0x20 (space)
    // Then continue reading: 8798, 8799, ..., until non-space
    // Last terminator read is at 8835 (0x20 space)
    // But 8836 is NOT a terminator - it's the next field data!
    
    // Wait, let's check: 8836 is 0x00, which could be:
    // 1. Part of ordinal position (big endian)
    // 2. A terminator for something else
    
    std::cout << "\nAnalysis:\n";
    std::cout << "After 8 bytes skip (pos 8797): terminator loop starts\n";
    std::cout << "Terminator loop reads: 8797(0x20), 8798(0x20), ..., 8835(0x20)\n";
    std::cout << "Then reads one more byte 8836(0x00) - this is the FINAL terminator\n";
    std::cout << "Now pos should be 8837\n";
    
    std::cout << "\nAt pos 8837:\n";
    printf("ordinal: %02X %02X = %d (little endian)\n", raw[8837], raw[8838], 
           raw[8837] | (raw[8838] << 8));
    printf("namesize: %02X = %d\n", raw[8839], raw[8839]);
    printf("name: ");
    for (int i = 0; i < raw[8839]; i++) printf("%c", raw[8840 + i]);
    printf("\n");
    
    return 0;
}

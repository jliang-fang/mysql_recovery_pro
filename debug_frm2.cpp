#include <iostream>
#include <fstream>
#include <vector>

int main() {
    std::ifstream file("/app/mysql_recovery_test/mysql56/data/recovery_test/mixed_types.frm", std::ios::binary | std::ios::ate);
    size_t size = file.tellg();
    file.seekg(0);
    
    std::vector<uint8_t> data(size);
    file.read(reinterpret_cast<char*>(data.data()), size);
    const uint8_t* raw = data.data();
    
    // Print bytes from 8789 to 8900
    std::cout << "Bytes from pos 8789 (screen transition point):\n";
    for (size_t i = 8789; i < 8900 && i < size; i++) {
        printf("%3zu: 0x%02X ", i, raw[i]);
        if (raw[i] >= 32 && raw[i] < 127) printf("(%c)", raw[i]);
        else printf("   ");
        if ((i - 8789 + 1) % 5 == 0) printf("\n");
    }
    std::cout << "\n";
    
    // Check: after 8 bytes + terminator + spaces
    // pos = 8789 + 8 = 8797
    // terminator at 8797 = 0x20 (space)
    // skip spaces until non-space at 8838
    // But 8838 is 0x00 (namesize = 0)
    
    // Maybe the skip logic is wrong - let's trace manually
    std::cout << "\nManual trace after pos 8789:\n";
    std::cout << "8789+8=8797: 0x" << std::hex << (int)raw[8797] << std::dec;
    if (raw[8797] == 0x20) std::cout << " (space)\n";
    
    // Find next non-space after 8797
    size_t p = 8798;
    while (p < size && raw[p] == 0x20) p++;
    std::cout << "Next non-space at pos " << p << ": 0x" << std::hex << (int)raw[p] << std::dec << "\n";
    
    // Actually let's look at the structure
    // ibd2sql reads terminator after 8 bytes, then if terminator is space, continues reading
    // The structure should be:
    // [8 bytes padding] [terminator] [next field: ordinal(2) namesize(1) name...]
    
    // Let's check what's at 8797+1
    std::cout << "\nBytes at 8798-8805:\n";
    for (size_t i = 8798; i < 8805; i++) {
        printf("%zu: 0x%02X ", i, raw[i]);
        if (raw[i] >= 32 && raw[i] < 127) printf("(%c)", raw[i]);
        printf("\n");
    }
    
    return 0;
}

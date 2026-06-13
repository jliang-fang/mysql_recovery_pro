#include <iostream>
#include <fstream>
#include <vector>

int main() {
    std::ifstream file("/app/mysql_recovery_test/mysql56/data/recovery_test/transaction_records.frm", std::ios::binary | std::ios::ate);
    size_t size = file.tellg();
    file.seekg(0);
    
    std::vector<uint8_t> data(size);
    file.read(reinterpret_cast<char*>(data.data()), size);
    const uint8_t* raw = data.data();
    
    // Parse header
    uint16_t io_size = raw[6] | (raw[7] << 8);
    uint16_t tmp_key_length = raw[14] | (raw[15] << 8);
    uint16_t rec_length = raw[16] | (raw[17] << 8);
    uint32_t record_offset = io_size + tmp_key_length + rec_length;
    record_offset = ((record_offset / io_size) + 1) * io_size;
    
    size_t pos = record_offset + 258 + 30 + 42;  // Skip to field names
    
    std::cout << "transaction_records field names at pos " << pos << ":\n";
    
    // Read first 3 field names
    for (int i = 0; i < 3; i++) {
        uint16_t ordinal = raw[pos] | (raw[pos+1] << 8);
        pos += 2;
        uint8_t namesize = raw[pos++];
        std::string name(reinterpret_cast<const char*>(raw + pos), namesize - 1);
        pos += namesize;
        
        std::cout << "Field " << i << ": ordinal=" << ordinal << " namesize=" << (int)namesize << " name='" << name << "'\n";
        
        // Print raw bytes
        printf("  Raw: ");
        for (int j = -3; j < (int)namesize + 2; j++) {
            printf("%02X ", raw[pos - namesize - 2 + j]);
        }
        printf("\n");
    }
    
    return 0;
}

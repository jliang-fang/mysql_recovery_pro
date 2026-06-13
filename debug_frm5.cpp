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
    
    uint16_t io_size = raw[6] | (raw[7] << 8);
    uint16_t tmp_key_length = raw[14] | (raw[15] << 8);
    uint16_t rec_length = raw[16] | (raw[17] << 8);
    uint32_t record_offset = io_size + tmp_key_length + rec_length;
    record_offset = ((record_offset / io_size) + 1) * io_size;
    
    std::cout << "mixed_types:\n";
    std::cout << "io_size: " << io_size << "\n";
    std::cout << "record_offset: " << record_offset << "\n";
    
    size_t pos = record_offset + 258;
    
    uint16_t fields = raw[pos] | (raw[pos+1] << 8);
    pos += 2 + 2 + 6;  // fields, field_pos, _0
    
    uint16_t n_length = raw[pos] | (raw[pos+1] << 8);
    pos += 2;
    uint16_t interval_count = raw[pos] | (raw[pos+1] << 8);
    pos += 2 + 2 + 2;  // interval_count, interval_parts, int_length
    pos += 6;  // _1
    
    uint16_t null_fields = raw[pos] | (raw[pos+1] << 8);
    pos += 2 + 2 + 2;  // null_fields, comment_length, gcol_screen_length
    pos += 5;  // _2
    
    uint8_t fields_per_screen = raw[pos];
    pos += 1;
    
    std::cout << "fields: " << fields << "\n";
    std::cout << "n_length: " << n_length << "\n";
    std::cout << "null_fields: " << null_fields << "\n";
    std::cout << "fields_per_screen: " << (int)fields_per_screen << "\n";
    std::cout << "pos after header: " << pos << "\n";
    
    pos += 42;
    std::cout << "pos after skip 42: " << pos << "\n";
    
    // Print first 50 bytes
    std::cout << "\nFirst 50 bytes at pos " << pos << ":\n";
    for (size_t i = 0; i < 50 && pos + i < size; i++) {
        printf("%02X ", raw[pos + i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    std::cout << "\n";
    
    // Read field names
    std::cout << "\nReading field names:\n";
    int col_in_screen = 1;
    for (int i = 0; i < fields && pos + 3 < size; i++) {
        // Check screen transition BEFORE reading field
        if (col_in_screen == fields_per_screen) {
            std::cout << "  [Screen transition at field " << i << "]\n";
            pos += 8;  // Skip 8 bytes
            if (pos < size) {
                uint8_t terminator = raw[pos++];
                std::cout << "  terminator: 0x" << std::hex << (int)terminator << std::dec;
                while (pos < size && raw[pos] == 0x20) pos++;
                std::cout << " pos after: " << pos << "\n";
            }
            col_in_screen = 2;
        } else {
            col_in_screen++;
        }
        
        uint16_t ordinal = raw[pos] | (raw[pos+1] << 8);
        pos += 2;
        uint8_t namesize = raw[pos++];
        
        if (namesize == 0 || namesize > 100) {
            std::cout << "  ERROR: Invalid namesize " << (int)namesize << " at pos " << pos-1 << "\n";
            break;
        }
        
        std::string name(reinterpret_cast<const char*>(raw + pos), namesize - 1);
        pos += namesize;
        
        std::cout << "  Field " << i << ": ordinal=" << ordinal << " name='" << name << "'\n";
    }
    
    return 0;
}

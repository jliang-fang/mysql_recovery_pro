#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>

uint16_t read_uint16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

int main() {
    // Parse mixed_types.frm
    std::ifstream file("/app/mysql_recovery_test/mysql56/data/recovery_test/mixed_types.frm", std::ios::binary | std::ios::ate);
    size_t size = file.tellg();
    file.seekg(0);
    
    std::vector<uint8_t> data(size);
    file.read(reinterpret_cast<char*>(data.data()), size);
    const uint8_t* raw = data.data();
    
    // Parse header
    uint16_t io_size = read_uint16(raw + 6);
    uint16_t tmp_key_length = read_uint16(raw + 14);
    uint16_t rec_length = read_uint16(raw + 16);
    uint32_t record_offset = io_size + tmp_key_length + rec_length;
    record_offset = ((record_offset / io_size) + 1) * io_size;
    
    std::cout << "io_size: " << io_size << "\n";
    std::cout << "record_offset: " << record_offset << "\n";
    
    // Column section at record_offset + 258
    size_t pos = record_offset + 258;
    
    uint16_t fields = read_uint16(raw + pos);
    uint16_t field_pos = read_uint16(raw + pos + 2);
    pos += 4 + 6;  // fields, pos, _0
    
    uint16_t n_length = read_uint16(raw + pos);
    uint16_t interval_count = read_uint16(raw + pos + 2);
    uint16_t interval_parts = read_uint16(raw + pos + 4);
    uint16_t int_length = read_uint16(raw + pos + 6);
    pos += 8 + 6;  // n_length, intervals, int_length, _1
    
    uint16_t null_fields = read_uint16(raw + pos);
    uint16_t comment_length = read_uint16(raw + pos + 2);
    uint16_t gcol_screen_length = read_uint16(raw + pos + 4);
    pos += 6 + 5;  // null_fields, comments, gcol, _2
    
    uint8_t fields_per_screen = raw[pos++];
    
    std::cout << "fields: " << fields << "\n";
    std::cout << "n_length: " << n_length << "\n";
    std::cout << "interval_count: " << interval_count << "\n";
    std::cout << "null_fields: " << null_fields << "\n";
    std::cout << "fields_per_screen: " << (int)fields_per_screen << "\n";
    std::cout << "pos after header: " << pos << "\n";
    
    // Skip 42 bytes
    pos += 42;
    std::cout << "pos after skip 42: " << pos << "\n";
    
    // Read field names with detailed tracking
    std::cout << "\nReading field names:\n";
    int col_in_screen = 1;
    size_t NAMESIZE = 0;
    
    for (int i = 0; i < fields && pos < size; i++) {
        // Check screen transition
        if (col_in_screen == fields_per_screen) {
            std::cout << "  [Screen transition before field " << i << " at pos " << pos << "]\n";
            pos += 8;  // Skip 8 bytes
            
            // Read terminator and skip spaces
            if (pos < size) {
                uint8_t terminator = raw[pos++];
                std::cout << "  terminator byte: 0x" << std::hex << (int)terminator << std::dec << "\n";
                while (pos < size && raw[pos] == ' ') pos++;
            }
            col_in_screen = 2;
        } else {
            col_in_screen++;
        }
        
        // Read ordinal position
        uint16_t ordinal = read_uint16(raw + pos);
        pos += 2;
        
        // Read name size
        uint8_t namesize = raw[pos++];
        
        if (namesize == 0 || namesize > 100) {
            std::cout << "  ERROR: Invalid namesize " << (int)namesize << " at field " << i << " pos " << pos-1 << "\n";
            break;
        }
        
        // Read name (skip last byte which is usually null/space)
        std::string name(reinterpret_cast<const char*>(raw + pos), namesize - 1);
        pos += namesize;
        NAMESIZE += namesize;
        
        std::cout << "  Field " << i << ": ordinal=" << ordinal << " name='" << name << "' size=" << (int)namesize << "\n";
    }
    
    std::cout << "\nAfter field names: pos=" << pos << " NAMESIZE=" << NAMESIZE << "\n";
    
    // Read field metadata
    std::cout << "\nReading field metadata (first 10):\n";
    for (int i = 0; i < 10 && pos + 17 <= size; i++) {
        pos += 3;  // _0
        
        uint16_t field_length = read_uint16(raw + pos);
        pos += 2;
        
        // recpos (3 bytes, little endian)
        uint32_t recpos = raw[pos] | (raw[pos+1] << 8) | (raw[pos+2] << 16);
        pos += 3;
        
        uint16_t pack_flag = read_uint16(raw + pos);
        pos += 2;
        
        pos += 3;  // unireg_type, charset_low, interval_nr
        
        uint8_t field_type = raw[pos++];
        uint8_t collation_id = raw[pos++];
        
        uint16_t comment_len = read_uint16(raw + pos);
        pos += 2;
        
        std::cout << "  Field " << i << ": len=" << field_length << " type=" << (int)field_type 
                  << " recpos=" << recpos << " collation=" << (int)collation_id << "\n";
    }
    
    return 0;
}

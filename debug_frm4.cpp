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
    
    // Parse header manually, step by step
    uint16_t io_size = raw[6] | (raw[7] << 8);
    uint16_t tmp_key_length = raw[14] | (raw[15] << 8);
    uint16_t rec_length = raw[16] | (raw[17] << 8);
    uint32_t record_offset = io_size + tmp_key_length + rec_length;
    record_offset = ((record_offset / io_size) + 1) * io_size;
    
    std::cout << "io_size: " << io_size << "\n";
    std::cout << "tmp_key_length: " << tmp_key_length << "\n";
    std::cout << "rec_length: " << rec_length << "\n";
    std::cout << "record_offset: " << record_offset << "\n";
    
    size_t pos = record_offset + 258;
    std::cout << "Column section starts at: " << pos << "\n";
    
    // Read column section header
    uint16_t fields = raw[pos] | (raw[pos+1] << 8);
    std::cout << "fields: " << fields << "\n";
    pos += 2;
    
    uint16_t field_pos = raw[pos] | (raw[pos+1] << 8);
    std::cout << "field_pos: " << field_pos << "\n";
    pos += 2;
    
    // Skip _0 (6 bytes)
    pos += 6;
    
    uint16_t n_length = raw[pos] | (raw[pos+1] << 8);
    std::cout << "n_length: " << n_length << "\n";
    pos += 2;
    
    uint16_t interval_count = raw[pos] | (raw[pos+1] << 8);
    std::cout << "interval_count: " << interval_count << "\n";
    pos += 2;
    
    uint16_t interval_parts = raw[pos] | (raw[pos+1] << 8);
    pos += 2;
    
    uint16_t int_length = raw[pos] | (raw[pos+1] << 8);
    std::cout << "int_length: " << int_length << "\n";
    pos += 2;
    
    // Skip _1 (6 bytes)
    pos += 6;
    
    uint16_t null_fields = raw[pos] | (raw[pos+1] << 8);
    std::cout << "null_fields: " << null_fields << "\n";
    pos += 2;
    
    uint16_t comment_length = raw[pos] | (raw[pos+1] << 8);
    std::cout << "comment_length: " << comment_length << "\n";
    pos += 2;
    
    uint16_t gcol_screen_length = raw[pos] | (raw[pos+1] << 8);
    std::cout << "gcol_screen_length: " << gcol_screen_length << "\n";
    pos += 2;
    
    // Skip _2 (5 bytes)
    pos += 5;
    
    uint8_t fields_per_screen = raw[pos];
    std::cout << "fields_per_screen: " << (int)fields_per_screen << "\n";
    pos += 1;
    
    std::cout << "\nAfter header: pos=" << pos << "\n";
    
    // Now we need to skip to field names
    // ibd2sql: _ = self.read(42)
    pos += 42;
    std::cout << "After skip 42: pos=" << pos << "\n";
    
    // Print next 50 bytes
    std::cout << "\nNext 50 bytes:\n";
    for (size_t i = 0; i < 50 && pos + i < size; i++) {
        printf("%02X ", raw[pos + i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    std::cout << "\n";
    
    // Now read first field
    // But wait - should we skip some padding?
    // The first bytes are spaces, then we have the actual data
    
    return 0;
}

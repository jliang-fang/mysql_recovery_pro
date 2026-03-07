#include "dbrecovery/innodb/page_reader.hpp"
#include "dbrecovery/utils/file_mapper.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <stdexcept>

namespace dbrecovery {
namespace innodb {

PageReader::PageReader(const std::string& file_path)
    : fd_(-1), file_size_(0), page_size_(16384), total_pages_(0), is_valid_(false) {
    
    utils::FileMapper mapper(file_path);
    if (!mapper.is_valid()) {
        return;
    }

    file_data_ = std::make_unique<std::vector<uint8_t>>(mapper.data(), mapper.data() + mapper.size());
    file_size_ = file_data_->size();
    
    // 尝试从第一页解析实际页大小（InnoDB 支持 4KB/8KB/16KB/32KB/64KB）
    if (file_size_ >= 16384) {
        uint32_t page_type = *reinterpret_cast<const uint32_t*>(file_data_->data() + 0);
        
        // 检查是否为有效页类型
        if (page_type == FIL_PAGE_INDEX || 
            page_type == FIL_PAGE_SDI ||
            page_type == FIL_PAGE_TYPE_ALLOCATED) {
            
            // 尝试从 FIL_HEADER 中读取实际页大小（偏移 16-19）
            uint32_t actual_page_size = *reinterpret_cast<const uint32_t*>(file_data_->data() + 16);
            if (actual_page_size >= 4096 && actual_page_size <= 65536 &&
                (actual_page_size & (actual_page_size - 1)) == 0) {
                page_size_ = actual_page_size;
            }
        }
    }
    
    total_pages_ = static_cast<uint32_t>(file_size_ / page_size_);
    is_valid_ = true;
}

PageReader::~PageReader() {}

std::vector<uint8_t> PageReader::read_page(uint32_t page_no) {
    if (!is_valid_ || page_no >= total_pages_) return {};
    
    std::vector<uint8_t> buffer(page_size_);
    off_t offset = static_cast<off_t>(page_no) * page_size_;
    
    // 从内存映射中复制数据
    if (offset + page_size_ > file_data_->size()) {
        return {};
    }
    
    std::copy(file_data_->begin() + offset, 
              file_data_->begin() + offset + page_size_, 
              buffer.begin());
    
    // 校验页校验和
    if (!validate_checksum(buffer)) {
        return {}; // 校验失败，返回空
    }
    
    return buffer;
}

bool PageReader::validate_checksum(const std::vector<uint8_t>& page_data) {
    if (page_data.size() < 38) return false;

    uint32_t stored_csum = DBRecovery::InnoDB::MachRead::read_le<uint32_t>(
        page_data.data(), 24
    );
    uint32_t new_csum = compute_crc32c(page_data);

    return stored_csum == new_csum;
}

uint32_t PageReader::compute_crc32c(const std::vector<uint8_t>& page_data) {
    static const uint32_t crc32c_table[256] = {
        0x00000000, 0xF26B8303, 0xE13B70F7, 0x1350F3F4,
        0xC79A971F, 0x35F1141C, 0x26A1E7E8, 0xD4CA64EB,
        0x8AD958CF, 0x78B2DBCC, 0x6BE22838, 0x9989AB3B,
        0x4D43CFD0, 0xBF284CD3, 0xAC78BF27, 0x5E133C24,
        0x105EC76F, 0xE235446C, 0xF165B798, 0x030E349B,
        0xD7C45070, 0x25AFD373, 0x36FF2087, 0xC494A384,
        0x9A879FA0, 0x68EC1CA3, 0x7BBCEF57, 0x89D76C54,
        0x5D1D08BF, 0xAF768BBC, 0xBC267848, 0x4E4DFB4B,
        0x20BD8EDE, 0xD2D60DDF, 0xC186FE2B, 0x33ED7D28,
        0xE72719C3, 0x154C9AC0, 0x061C6934, 0xF477EA37,
        0xAA64D613, 0x580F5510, 0x4B5FA6E4, 0xB93425E7,
        0x6DFE410C, 0x9F95C20F, 0x8CC531FB, 0x7EAEB2F8,
        0x30E349B3, 0xC288CAB0, 0xD1D83944, 0x23B3BA47,
        0xF779DEAC, 0x05125DAC, 0x1642AE58, 0xE4292D5B,
        0xBA3A117F, 0x4851927C, 0x5B016188, 0xA96AE28B,
        0x7DA08660, 0x8FCB0563, 0x9C9BF697, 0x6EF07594,
        0x417B1DBC, 0xB3109EBF, 0xA0406D4B, 0x522BEE48,
        0x86E18AA3, 0x748A09A0, 0x67DAFA54, 0x95B17957,
        0xCBA24573, 0x39C9C670, 0x2A993584, 0xD8F2B687,
        0x0C38D26C, 0xFE53516F, 0xED03A29B, 0x1F682198,
        0x5125DAC3, 0xA34E59C0, 0xB01EEA34, 0x42756937,
        0x96BF0DDC, 0x64D48EDF, 0x77847D2B, 0x85EFFE28,
        0xDBFC C20C, 0x2997410F, 0x3AC7B2FB, 0xC8AC31F8,
        0x1C665513, 0xEE0DD610, 0xFD5D25E4, 0x0F36A6E7,
        0x61C6937C, 0x93AD107F, 0x80FDE38B, 0x72966088,
        0xA65C0463, 0x54378760, 0x47677494, 0xB50CF797,
        0xEB1FCBB3, 0x197448B0, 0x0A24BB44, 0xF84F3847,
        0x2C855CAC, 0xDEEEDFAF, 0xCDBE2C5B, 0x3FD5AF58,
        0x71985413, 0x83F3D710, 0x90A324E4, 0x62C8A7E7,
        0xB602C30C, 0x4469400F, 0x5739B3FB, 0xA55230F8,
        0xFB410CDC, 0x092A8FDF, 0x1A7A7C2B, 0xE811FF28,
        0x3CDB9BC3, 0xCEB018C0, 0xDDE0EB34, 0x2F8B6837,
        0x82F63B78, 0x709DB87B, 0x63CD4B8F, 0x91A6C88C,
        0x456CAC67, 0xB7072F64, 0xA457DC90, 0x563C5F93,
        0x082F63B7, 0xFA44E0B4, 0xE9141340, 0x1B7F9043,
        0xCFB5F4A8, 0x3DDE77AB, 0x2E8E845F, 0xDCE5075C,
        0x92A8FC17, 0x60C37F14, 0x73938CE0, 0x81F80FE3,
        0x55326B08, 0xA759E80B, 0xB4091BFF, 0x466298FC,
        0x1871A4D8, 0xEA1A27DB, 0xF94AD42F, 0x0B21572C,
        0xDFEB33C7, 0x2D80B0C4, 0x3ED04330, 0xCCBBC033,
        0xA24BB5A6, 0x502036A5, 0x4370C551, 0xB11B4652,
        0x65D122B9, 0x97BAA1BA, 0x84EA524E, 0x7681D14D,
        0x2892ED69, 0xDAF96E6A, 0xC9A99D9E, 0x3BC21E9D,
        0xEF087A76, 0x1D63F975, 0x0E330A81, 0xFC588982,
        0xB21572C9, 0x407EF1CA, 0x532E023E, 0xA145813D,
        0x758FE5D6, 0x87E466D5, 0x94B49521, 0x66DF1622,
        0x38CC2A06, 0xCAA7A905, 0xD9F75AF1, 0x2B9CD9F2,
        0xFF56BD19, 0x0D3D3E1A, 0x1E6DCDEE, 0xEC064EED,
        0xC38D26C4, 0x31E6A5C7, 0x22B65633, 0xD0DDD530,
        0x0417B1DB, 0xF67C32D8, 0xE52CC12C, 0x1747422F,
        0x49547E0B, 0xBB3FFD08, 0xA86F0EFC, 0x5A048DFF,
        0x8ECEE914, 0x7CA56A17, 0x6FF599E3, 0x9D9E1AE0,
        0xD3D3E1AB, 0x21B862A8, 0x32E8915C, 0xC083125F,
        0x144976B4, 0xE622F5B7, 0xF5720643, 0x07198540,
        0x590AB964, 0xAB613A67, 0xB831C993, 0x4A5A4A90,
        0x9E902E7B, 0x6CFBAD78, 0x7FAB5E8C, 0x8DC0DD8F,
        0xE330A814, 0x115B2B17, 0x020BD8E3, 0xF0605BE0,
        0x24AA3F0B, 0xD6C1BC08, 0xC5914FFC, 0x37FACCFF,
        0x69E9F0DB, 0x9B8273D8, 0x88D2802C, 0x7AB9032F,
        0xAE7367C4, 0x5C18E4C7, 0x4F481733, 0xBD239430,
        0xF36E6F7B, 0x0105EC78, 0x12551F8C, 0xE03E9C8F,
        0x34F4F864, 0xC69F7B67, 0xD5CF8893, 0x27A40B90,
        0x79B737B4, 0x8BDCB4B7, 0x988C4743, 0x6AE7C440,
        0xBE2DA0AB, 0x4C4623A8, 0x5F16D05C, 0xAD7D535F,
        0xC9E2A6E6, 0x3B8925E5, 0x28D9D611, 0xDAB25512,
        0x0E7831F9, 0xFC13B2FA, 0xEF43410E, 0x1D28C20D,
        0x433BF E29, 0xB1507D2A, 0xA2008EDF, 0x506B0DDC,
        0x84A16937, 0x76CAF A34, 0x659A59C0, 0x97F1DA C3
    };

    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < page_data.size(); ++i) {
        if (i >= 24 && i < 28) continue; // Skip checksum field
        crc = (crc >> 8) ^ crc32c_table[(crc ^ page_data[i]) & 0xFF];
    }
    return ~crc;
}

} // namespace innodb
} // namespace dbrecovery

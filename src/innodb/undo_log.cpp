// ✅ 删除：这些函数已在 mach_read.hpp 中实现，此处无需重复定义

UndoLogParser::UndoLogParser() {}

UndoLogParser::~UndoLogParser() = default;

std::optional<UndoLogRecord> UndoLogParser::parse_record(const uint8_t* data, size_t len) {
    if (!data || len < 20) {
        return std::nullopt;
    }

    UndoLogRecord record;
    size_t pos = 0;

    // 解析事务ID和回滚指针
    record.u_trx_id = read_uint64_le(data, pos);
    record.u_roll_ptr = read_uint64_le(data, pos);

    // 解析类型
    record.u_type = read_uint16_le(data, pos);

    // 剩余数据作为记录数据
    if (pos < len) {
        record.u_data.assign(data + pos, data + len);
    }

    return record;
}

std::vector<UndoLogRecord> UndoLogParser::parse_file(const std::string& path) {
    std::vector<UndoLogRecord> records;

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[ERROR] Failed to open undo log file: " << path << std::endl;
        return records;
    }

    // 读取文件大小
    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size < 20) {
        std::cerr << "[ERROR] Undo log file too small" << std::endl;
        return records;
    }

    // 读取整个文件
    std::vector<uint8_t> buffer(file_size);
    file.read(reinterpret_cast<char*>(buffer.data()), file_size);

    // 解析所有记录
    size_t pos = 0;
    while (pos + 20 <= buffer.size()) {
        auto record = parse_record(buffer.data() + pos, buffer.size() - pos);
        if (!record) {
            // 尝试同步到下一个可能的记录起点
            pos++;
            continue;
        }

        records.push_back(*record);
        // 计算下一条记录的起始位置（考虑记录长度字段）
        pos += 20 + record->u_data.size();
    }

    return records;
}

} // namespace innodb
} // namespace dbrecovery


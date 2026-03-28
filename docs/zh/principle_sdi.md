# SDI (Space Descriptor Index) 原理与实现

## 1. 概述

SDI (Space Descriptor Index) 是 MySQL 8.0 引入的元数据存储机制，用于存储表空间的元数据信息。SDI 数据存储在专门的 SDI 页中，页类型为 `FIL_PAGE_SDI (9971)`。

## 2. SDI 结构

### 2.1 SDI 页结构

```
┌─────────────────────────────────────────────────────┐
│  FIL Header (38 bytes)                              │
├─────────────────────────────────────────────────────┤
│  SDI Header (20 bytes)                              │
│  - Type (4 bytes)                                   │
│  - ID (8 bytes)                                     │
│  - Uncompressed Length (4 bytes)                    │
│  - Compressed Length (4 bytes)                      │
├─────────────────────────────────────────────────────┤
│  Compressed JSON Data...                            │
└─────────────────────────────────────────────────────┘
```

### 2.2 SDI Header

| 偏移 | 大小 | 字段 | 说明 |
|------|------|------|------|
| 0-3 | 4 bytes | Type | 对象类型（1=Table, 2=Index） |
| 4-11 | 8 bytes | ID | 对象ID（表ID/索引ID） |
| 12-15 | 4 bytes | Uncompressed Length | 解压后长度 |
| 16-19 | 4 bytes | Compressed Length | 压缩后长度 |

### 2.3 SDI 对象类型

| 类型值 | 类型名称 | 说明 |
|--------|----------|------|
| 1 | DD_OBJECT_TABLE | 表定义 |
| 2 | DD_OBJECT_INDEX | 索引定义 |
| 3 | DD_OBJECT_COLUMN | 列定义 |
| 4 | DD_OBJECT_SCHEMA | Schema定义 |

## 3. SDI 解析

### 3.1 SDI 解析流程

```cpp
class SDIManager {
public:
    static catalog::TableSchema parse_sdi_record(
        const uint8_t* payload, size_t payload_len) {
        
        if (payload_len < 20) {
            throw std::runtime_error("SDI payload too small");
        }
        
        // 1. 读取SDI Header
        uint32_t type = read_le<uint32_t>(payload);
        uint64_t id = read_le<uint64_t>(payload + 4);
        uint32_t uncompressed_len = read_le<uint32_t>(payload + 12);
        uint32_t compressed_len = read_le<uint32_t>(payload + 16);
        
        if (compressed_len == 0 || compressed_len > payload_len - 20) {
            throw std::runtime_error("Invalid SDI compressed length");
        }
        
        // 2. 解压缩
        const uint8_t* raw_data = payload + 20;
        std::vector<uint8_t> decompressed = decompress(raw_data, compressed_len);
        
        // 3. 解析JSON
        auto json_obj = nlohmann::json::parse(decompressed.begin(), decompressed.end());
        
        // 4. 解析表结构
        catalog::TableSchema schema;
        parse_table_schema(json_obj, schema);
        
        return schema;
    }
    
private:
    static std::vector<uint8_t> decompress(const uint8_t* data, size_t compressed_len) {
        z_stream strm = {};
        strm.next_in = const_cast<Bytef*>(data);
        strm.avail_in = static_cast<uInt>(compressed_len);
        
        // 分配解压缓冲区
        std::vector<uint8_t> decompressed(1024 * 1024); // 1MB
        strm.next_out = decompressed.data();
        strm.avail_out = static_cast<uInt>(decompressed.size());
        
        int ret = inflateInit2(&strm, -15); // raw deflate
        if (ret != Z_OK) {
            throw std::runtime_error("Failed to init zlib");
        }
        
        do {
            ret = inflate(&strm, Z_NO_FLUSH);
            if (ret == Z_BUF_ERROR) {
                decompressed.resize(decompressed.size() * 2);
                strm.next_out = decompressed.data() + strm.total_out;
                strm.avail_out = static_cast<uInt>(decompressed.size() - strm.total_out);
            } else if (ret != Z_OK && ret != Z_STREAM_END) {
                inflateEnd(&strm);
                throw std::runtime_error("Decompression failed");
            }
        } while (ret != Z_STREAM_END);
        
        decompressed.resize(strm.total_out);
        inflateEnd(&strm);
        
        return decompressed;
    }
};
```

### 3.2 表结构解析

```cpp
void SDIManager::parse_table_schema(const nlohmann::json& json_obj, 
                                     catalog::TableSchema& schema) {
    // 解析表名
    if (json_obj.contains("schema")) {
        schema.table_name = json_obj["schema"].value("name", "");
    }
    if (json_obj.contains("table")) {
        auto table_json = json_obj["table"];
        schema.table_name = table_json.value("name", schema.table_name);
        
        // 解析行格式
        if (table_json.contains("row_format")) {
            schema.row_format = table_json["row_format"].get<uint32_t>();
        }
        
        // 解析页大小
        if (table_json.contains("page_size")) {
            schema.page_size = table_json["page_size"].get<uint32_t>();
        }
        
        // 解析表ID
        if (table_json.contains("id")) {
            schema.table_id = table_json["id"].get<uint64_t>();
        }
        
        // 解析引擎
        if (table_json.contains("engine")) {
            schema.engine = table_json["engine"].get<std::string>();
        }
        
        // 解析列
        if (table_json.contains("columns") && table_json["columns"].is_array()) {
            uint32_t ordinal = 1;
            for (const auto& col : table_json["columns"]) {
                catalog::ColumnInfo column_info;
                
                if (col.contains("name")) {
                    column_info.name = col["name"].get<std::string>();
                }
                if (col.contains("type")) {
                    column_info.mysql_type_id = col["type"].get<uint32_t>();
                }
                if (col.contains("is_nullable")) {
                    column_info.is_nullable = !col["is_nullable"].get<bool>();
                }
                if (col.contains("length")) {
                    column_info.column_length = col["length"].get<uint32_t>();
                }
                if (col.contains("charset_id")) {
                    column_info.charset_id = col["charset_id"].get<uint32_t>();
                }
                if (col.contains("collation_id")) {
                    column_info.collation_id = col["collation_id"].get<uint32_t>();
                }
                if (col.contains("default_value")) {
                    column_info.default_value = col["default_value"].get<std::string>();
                }
                if (col.contains("is_auto_increment")) {
                    column_info.is_auto_increment = col["is_auto_increment"].get<bool>();
                }
                
                schema.columns[ordinal++] = column_info;
            }
        }
        
        // 解析索引
        if (table_json.contains("indexes") && table_json["indexes"].is_array()) {
            for (const auto& idx : table_json["indexes"]) {
                catalog::IndexInfo index;
                
                if (idx.contains("name")) {
                    index.name = idx["name"].get<std::string>();
                }
                if (idx.contains("is_primary")) {
                    index.is_primary = idx["is_primary"].get<bool>();
                }
                if (idx.contains("is_unique")) {
                    index.is_unique = idx["is_unique"].get<bool>();
                }
                if (idx.contains("columns") && idx["columns"].is_array()) {
                    for (const auto& col : idx["columns"]) {
                        index.columns.push_back(col["name"].get<std::string>());
                    }
                }
                
                schema.indexes.push_back(index);
            }
        }
    }
}
```

### 3.3 列定义解析

```cpp
catalog::ColumnInfo SDIManager::parse_column(const nlohmann::json& col_json) {
    catalog::ColumnInfo col;
    
    if (col_json.contains("name")) {
        col.name = col_json["name"].get<std::string>();
    }
    if (col_json.contains("type")) {
        col.mysql_type_id = col_json["type"].get<uint32_t>();
    }
    if (col_json.contains("is_nullable")) {
        col.is_nullable = !col_json["is_nullable"].get<bool>();
    }
    if (col_json.contains("length")) {
        col.column_length = col_json["length"].get<uint32_t>();
    }
    if (col_json.contains("charset_id")) {
        col.charset_id = col_json["charset_id"].get<uint32_t>();
    }
    if (col_json.contains("collation_id")) {
        col.collation_id = col_json["collation_id"].get<uint32_t>();
    }
    if (col_json.contains("default_value")) {
        col.default_value = col_json["default_value"].get<std::string>();
    }
    if (col_json.contains("is_auto_increment")) {
        col.is_auto_increment = col_json["is_auto_increment"].get<bool>();
    }
    if (col_json.contains("datetime_precision")) {
        col.datetime_precision = col_json["datetime_precision"].get<int>();
    }
    if (col_json.contains("numeric_scale")) {
        col.numeric_scale = col_json["numeric_scale"].get<int>();
    }
    if (col_json.contains("numeric_precision")) {
        col.numeric_precision = col_json["numeric_precision"].get<int>();
    }
    
    return col;
}
```

## 4. SDI 缓存

### 4.1 SDI 缓存结构

```cpp
class SDICache {
public:
    using SDIPtr = std::shared_ptr<const catalog::TableSchema>;
    
    explicit SDICache(size_t max_size = 1000) : max_size_(max_size) {}
    
    // 获取SDI
    std::optional<SDIPtr> get(uint32_t space_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(space_id);
        if (it != cache_.end()) {
            return it->second;
        }
        return std::nullopt;
    }
    
    // 添加SDI
    void put(uint32_t space_id, SDIPtr sdi) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // LRU替换策略：当缓存满时，移除最旧的条目
        if (cache_.size() >= max_size_) {
            auto oldest_it = cache_.begin();
            cache_.erase(oldest_it);
        }
        
        cache_[space_id] = std::move(sdi);
    }
    
    // 获取缓存大小
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return cache_.size();
    }
    
private:
    mutable std::mutex mutex_;
    std::map<uint32_t, SDIPtr> cache_;
    size_t max_size_;
};
```

### 4.2 SDI 缓存使用

```cpp
class SDIManager {
public:
    catalog::TableSchema get_table_schema(uint32_t space_id) {
        // 1. 检查缓存
        auto cached = cache_.get(space_id);
        if (cached) {
            return **cached;
        }
        
        // 2. 读取SDI页
        auto page_data = read_page(space_id, 0); // SDI通常在第一个页
        if (page_data.empty()) {
            throw std::runtime_error("Failed to read SDI page");
        }
        
        // 3. 解析SDI
        auto schema = parse_sdi_page(page_data.data(), page_data.size());
        
        // 4. 缓存结果
        cache_.put(space_id, std::make_shared<catalog::TableSchema>(schema));
        
        return schema;
    }
    
private:
    SDICache cache_;
};
```

## 5. 实际应用

### 5.1 表结构恢复

```cpp
bool RecoveryEngine::recover_table_schema(const std::string& ibd_path) {
    // 1. 打开文件
    InnoDBFileReader reader(ibd_path);
    if (!reader.is_valid()) {
        return false;
    }
    
    // 2. 读取SDI页
    auto sdi_page = reader.read_page(0); // SDI通常在第一个页
    if (sdi_page.empty()) {
        std::cerr << "Failed to read SDI page" << std::endl;
        return false;
    }
    
    // 3. 解析SDI
    try {
        SDIManager manager;
        table_schema_ = manager.parse_sdi_page(sdi_page.data(), sdi_page.size());
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to parse SDI: " << e.what() << std::endl;
        return false;
    }
}
```

### 5.2 元数据导出

```cpp
bool RecoveryEngine::export_metadata(const std::string& output_path) {
    // 1. 解析SDI
    if (!recover_table_schema(input_path_)) {
        return false;
    }
    
    // 2. 导出为JSON
    nlohmann::json json_obj = table_schema_.to_json();
    
    // 3. 写入文件
    std::ofstream out(output_path);
    if (!out) {
        return false;
    }
    
    out << json_obj.dump(2);
    out.close();
    
    return true;
}
```

### 5.3 表结构验证

```cpp
bool RecoveryEngine::validate_table_schema() {
    // 1. 解析SDI
    if (!recover_table_schema(input_path_)) {
        return false;
    }
    
    // 2. 验证表结构
    if (!table_schema_.validate()) {
        std::cerr << "Table schema validation failed" << std::endl;
        return false;
    }
    
    // 3. 验证列定义
    for (const auto& pair : table_schema_.columns) {
        const auto& col = pair.second;
        
        // 验证列名
        if (col.name.empty()) {
            std::cerr << "Column has empty name" << std::endl;
            return false;
        }
        
        // 验证MySQL类型
        if (col.mysql_type_id > 255) {
            std::cerr << "Invalid MySQL type ID: " << col.mysql_type_id << std::endl;
            return false;
        }
    }
    
    return true;
}
```

## 6. 参考资料

- MySQL 8.0 源码：`include/dd/types/table.h`, `include/dd/types/column.h`
- SDI 机制：https://dev.mysql.com/doc/refman/8.0/en/dd-tables.html
- JSON 元数据：https://dev.mysql.com/doc/refman/8.0/en/json-descriptors.html

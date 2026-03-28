# B+Tree 原理与实现

## 1. 概述

B+Tree 是 InnoDB 存储引擎的核心数据结构，用于实现索引。与 B-Tree 相比，B+Tree 具有以下特点：
- 所有数据存储在叶子节点
- 内部节点只存储索引键
- 叶子节点通过双向链表连接，支持快速范围查询

## 2. B+Tree 结构

### 2.1 节点类型

```
                    [Root Node]
                         │
         ┌───────────────┼───────────────┐
         │               │               │
    [Internal]      [Internal]      [Internal]
         │               │               │
    ┌────┴────┐     ┌────┴────┐     ┌────┴────┐
    │    │    │     │    │    │     │    │    │
 [Leaf][Leaf][Leaf][Leaf][Leaf][Leaf][Leaf][Leaf][Leaf]
```

### 2.2 节点结构

#### 内部节点（Internal Node）

```
┌─────────────────────────────────────────────────────┐
│  FIL Header (38 bytes)                              │
├─────────────────────────────────────────────────────┤
│  BTR Header (8 bytes)                               │
│  - BTR_SEG_ID (8 bytes)                             │
│  - BTR_LEVEL (2 bytes)                              │
│  - BTR_INDEX_ID (8 bytes)                           │
├─────────────────────────────────────────────────────┤
│  Page Header (14+ bytes)                            │
├─────────────────────────────────────────────────────┤
│  Record Area                                        │
│  - INFIMUM (system record)                          │
│  - Node Pointer Records                             │
│  - SUPREMUM (system record)                         │
└─────────────────────────────────────────────────────┘
```

#### 叶子节点（Leaf Node）

```
┌─────────────────────────────────────────────────────┐
│  FIL Header (38 bytes)                              │
├─────────────────────────────────────────────────────┤
│  BTR Header (8 bytes)                               │
│  - BTR_SEG_ID (8 bytes)                             │
│  - BTR_LEVEL (2 bytes) = 0 (leaf)                   │
│  - BTR_INDEX_ID (8 bytes)                           │
├─────────────────────────────────────────────────────┤
│  Page Header (14+ bytes)                            │
├─────────────────────────────────────────────────────┤
│  Record Area                                        │
│  - INFIMUM (system record)                          │
│  - User Records (actual data)                       │
│  - SUPREMUM (system record)                         │
└─────────────────────────────────────────────────────┘
```

## 3. 节点指针（Node Pointer）

内部节点的记录包含：
- 索引键值
- 子节点页号（6字节）
  - 4字节：页号
  - 2字节：上层页号（用于回溯）

## 4. B+Tree 操作

### 4.1 查找（Search）

```cpp
std::optional<Record> BPlusTreeParser::find_record(uint64_t key) {
    // 1. 从根节点开始
    uint32_t current_page = root_page_no_;
    
    while (true) {
        // 2. 读取当前页
        auto page_data = read_page(current_page);
        
        // 3. 解析页
        BPlusTreeParser parser(page_data);
        parser.parse();
        
        // 4. 在页中查找
        auto record = binary_search(parser.records(), key);
        
        if (parser.page_level() == 0) {
            // 叶子节点，返回记录
            return record;
        }
        
        // 内部节点，继续向下
        current_page = record.node_pointer();
    }
}
```

### 4.2 插入（Insert）

插入操作可能导致节点分裂：
1. 找到插入位置
2. 插入新记录
3. 如果节点满，分裂节点
4. 更新父节点

### 4.3 删除（Delete）

删除操作可能导致节点合并：
1. 找到删除位置
2. 删除记录
3. 如果节点空，合并节点
4. 更新父节点

## 5. 页链（Page Chain）

叶子节点通过双向链表连接：
- `FIL_PAGE_PREV`: 前一个叶子节点
- `FIL_PAGE_NEXT`: 后一个叶子节点

```
[Leaf 100] <-> [Leaf 101] <-> [Leaf 102] <-> [Leaf 103]
```

## 6. B+Tree 解析实现

### 6.1 页解析

```cpp
class BPlusTreeParser {
public:
    bool parse() noexcept {
        // 1. 解析页头
        if (!parse_header()) {
            return false;
        }
        
        // 2. 验证页类型
        if (!is_valid_page_type()) {
            return false;
        }
        
        // 3. 解析目录槽
        if (!parse_directory()) {
            return false;
        }
        
        // 4. 解析记录
        if (!parse_user_records()) {
            return false;
        }
        
        return true;
    }
    
private:
    bool parse_header() noexcept {
        // 解析 FIL Header
        page_type_ = read_le<uint16_t>(buffer_ + 12);
        page_level_ = read_le<uint16_t>(buffer_ + 54) >> 12;
        
        // 解析 Page Header
        header_.n_dir_slots = read_le<uint16_t>(buffer_ + 38);
        header_.heap_top = read_le<uint16_t>(buffer_ + 40);
        header_.n_heap = read_le<uint16_t>(buffer_ + 42) & 0x7FFF;
        
        return true;
    }
    
    bool parse_directory() {
        // 解析页目录槽
        for (uint16_t i = 0; i < header_.n_dir_slots; ++i) {
            uint16_t offset = 38 + 16 + i * 2;
            directory_slots_.push_back(read_le<uint16_t>(buffer_ + offset));
        }
        return true;
    }
    
    bool parse_user_records() {
        // 从 INFIMUM 开始遍历记录
        uint16_t current_offset = PAGE_INFIMUM_REC_OFFSET_ABS;
        
        while (true) {
            auto record = parse_record(current_offset);
            if (!record) break;
            
            records_.push_back(record.value());
            
            if (record->is_supremum()) break;
            current_offset = record->next_offset();
        }
        
        return true;
    }
};
```

### 6.2 记录解析

```cpp
struct Record {
    uint16_t offset;
    uint16_t next_offset;
    bool is_deleted;
    uint16_t heap_no;
    uint8_t info_bits;
    uint16_t n_fields;
    
    // COMPACT 格式记录头
    uint8_t info_bits;      // 1 byte
    uint16_t next_offset;   // 2 bytes
    uint16_t heap_no;       // 2 bytes
    uint8_t n_fields;       // 1 byte (compact only)
    
    // 字段数据
    std::vector<Field> fields;
};

std::optional<Record> parse_record(uint16_t offset) {
    const uint8_t* ptr = buffer_ + offset;
    
    // 解析记录头
    RecordHeader header;
    header.info_bits = ptr[0];
    header.next_offset = read_le<uint16_t>(ptr + 1) & 0x7FFF;
    header.heap_no = read_le<uint16_t>(ptr + 3) & 0x7FFF;
    
    // 解析字段
    std::vector<Field> fields;
    size_t data_pos = 5; // 跳过记录头
    
    for (uint8_t i = 0; i < header.n_fields; ++i) {
        Field field;
        
        // 解析字段长度
        if (is_compact_format()) {
            // COMPACT 格式：1-2字节长度前缀
            if (ptr[data_pos] < 0x80) {
                field.length = ptr[data_pos];
                data_pos += 1;
            } else {
                field.length = ((ptr[data_pos] & 0x7F) << 8) | ptr[data_pos + 1];
                data_pos += 2;
            }
        } else {
            // REDUNDANT 格式：2字节长度前缀
            field.length = read_le<uint16_t>(ptr + data_pos);
            data_pos += 2;
        }
        
        // 解析字段数据
        field.data = std::vector<uint8_t>(ptr + data_pos, ptr + data_pos + field.length);
        data_pos += field.length;
        
        fields.push_back(field);
    }
    
    return Record{
        .offset = offset,
        .next_offset = header.next_offset,
        .is_deleted = (header.info_bits & 0x08) != 0,
        .heap_no = header.heap_no,
        .info_bits = header.info_bits,
        .n_fields = header.n_fields,
        .fields = fields
    };
}
```

## 7. 行格式支持

### 7.1 REDUNDANT 格式

- 记录头：6字节
- 字段长度：2字节前缀
- NULL位图：可选

### 7.2 COMPACT 格式

- 记录头：5字节
- 字段长度：1-2字节前缀
- NULL位图：可选

### 7.3 DYNAMIC 格式

- 记录头：5字节
- 变长字段偏移：动态计算
- BLOB指针：20字节

### 7.4 COMPRESSED 格式

- 压缩页格式
- 需要解压后解析

## 8. 实际应用

### 8.1 数据恢复

```cpp
std::vector<Record> recover_records(uint32_t root_page) {
    std::vector<Record> records;
    
    // 1. 解析根节点
    auto root_data = read_page(root_page);
    BPlusTreeParser root_parser(root_data);
    root_parser.parse();
    
    // 2. 遍历所有叶子节点
    uint32_t current_page = root_page;
    while (current_page != 0xFFFFFFFF) {
        auto page_data = read_page(current_page);
        BPlusTreeParser parser(page_data);
        parser.parse();
        
        // 收集记录
        for (const auto& record : parser.records()) {
            if (!record.is_deleted()) {
                records.push_back(record);
            }
        }
        
        // 移动到下一个叶子节点
        current_page = parser.next_page();
    }
    
    return records;
}
```

### 8.2 索引重建

```cpp
bool rebuild_index(const std::vector<Record>& records) {
    // 1. 排序记录
    std::sort(records.begin(), records.end(), 
              [](const auto& a, const auto& b) {
                  return compare_keys(a, b) < 0;
              });
    
    // 2. 构建B+树
    BPlusTreeBuilder builder;
    for (const auto& record : records) {
        builder.insert(record);
    }
    
    // 3. 生成新页
    auto new_pages = builder.build();
    
    // 4. 写入新文件
    write_pages(new_pages);
    
    return true;
}
```

## 9. 参考资料

- MySQL 8.0 源码：`include/btr0btr.h`, `include/page0page.h`
- B+Tree 算法：https://dev.mysql.com/doc/refman/8.0/en/innodb-index-types.html

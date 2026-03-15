# 架构设计

## 核心模块

### 1. FileMapper
- 跨平台文件映射 (mmap/Win32)
- RAII 资源管理
- 移动语义支持

### 2. PageReader
- CRC32C 校验和验证 (MySQL 8.0 默认)
- INNODB/OLD 校验兼容性
- 性能优化的查表法实现

### 3. BPlusTreeParser
- 目录槽解析
- 记录偏移提取
- 健康状态检查

## 数据流

```
.ibd 文件 → FileMapper → PageReader → BPlusTreeParser → 记录
                                              ↓
                                         BlobPointer → BLOB 数据
```

## 扩展点设计

### 添加新的 MySQL 版本

```cpp
// 在 page_defs.hpp 中添加版本特定常量
constexpr uint16_t MYSQL_8_1_FEATURE_FLAG = 0x8000;
```

### 添加新的行格式

```cpp
enum class RowFormat {
    COMPACT,
    DYNAMIC,
    REDUNDANT,
    COMPRESSED  // 未来支持
};
```

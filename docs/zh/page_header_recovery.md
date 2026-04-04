# Page Header 完全损坏恢复技术文档

## 1. 概述

本文档详细描述了 MySQL Recovery Pro 项目中 **Page Header 完全损坏场景**的恢复技术实现。

### 1.1 场景描述

当 InnoDB 表空间文件的 **Page 0 (FSP Header)** 完全损坏时，传统的基于元数据的恢复方法失效。需要通过 **启发式反向推导算法**，利用页链信息（FIL_PAGE_PREV/FIL_PAGE_NEXT）重建页头结构。

### 1.2 损坏特征

| 特征 | 正常页 | 损坏页 |
|------|--------|--------|
| FIL_PAGE_TYPE | 有效值 (0x0000, 0x26f1, 0x26f2, 0x26f3) | 0xFFFF 或随机值 |
| FIL_PAGE_OFFSET | 0 (Page 0) | 0 或随机值 |
| FIL_PAGE_PREV | 0xFFFFFFFF (无前页) | 0xFFFFFFFF |
| FIL_PAGE_NEXT | 有效页号 | 0xFFFFFFFF |
| 校验和 | CRC32C 匹配 | 不匹配 |

---

## 2. 技术原理

### 2.1 反向推导算法

```
┌─────────────────────────────────────────────────────────────┐
│                    反向推导流程                              │
├─────────────────────────────────────────────────────────────┤
│  1. 从数据页开始扫描 (Page N)                                │
│  2. 读取 FIL_PAGE_PREV 指针 (偏移 0-3)                      │
│  3. 跳转到 Page N-1                                         │
│  4. 重复步骤 2-3，直到遇到 Page 0 (prev = 0xFFFFFFFF)       │
│  5. 通过特征码识别页类型 (type at offset 16)                │
│  6. 重建 FSP Header 结构                                    │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 页链验证

```cpp
// 页链闭环验证
bool validate_page_chain(uint32_t prev, uint32_t next, uint32_t current) {
    // 合法页号范围
    if (current >= 0xFFFFFFF0) return false;
    
    // 前后页关系验证
    if (prev != 0xFFFFFFFF && prev >= current) return false;
    if (next != 0xFFFFFFFF && next <= current) return false;
    
    return true;
}
```

### 2.3 页类型识别

| 页类型值 | 名称 | 说明 |
|---------|------|------|
| 0x0000 | FIL_PAGE_TYPE_ALLOCATED | 新分配页 |
| 0x26f1 (9969) | FIL_PAGE_INDEX | B+Tree 索引页 |
| 0x26f2 (9970) | FIL_PAGE_UNDO_LOG | Undo Log 页 |
| 0x26f3 (9971) | FIL_PAGE_SDI | SDI 元数据页 |
| 0x26f4 (9972) | FIL_PAGE_TYPE_BLOB | BLOB 页 |
| 0x26f5 (9973) | FIL_PAGE_COMPRESSED | 压缩页 |

---

## 3. 核心实现

### 3.1 页头结构定义

```cpp
// include/dbrecovery/innodb/page_header.hpp
struct alignas(1) PageHeader {
    // FIL Header (20 bytes)
    uint32_t prev_page;     // 0-3: 前一个页号
    uint32_t next_page;     // 4-7: 下一个页号
    uint64_t lsn;           // 8-15: LSN
    uint16_t page_type;     // 16-17: 页类型
    uint32_t page_offset;   // 18-21: 页号
    uint32_t space_id;      // 22-25: 空间ID
    
    // Page Header (变长)
    uint16_t n_dir_slots;   // 26-27: 目录槽数量
    uint16_t heap_top;      // 28-29: 堆顶偏移
    uint16_t n_heap;        // 30-31: 记录数量
    // ... 更多字段
};
```

### 3.2 反向推导算法实现

```cpp
// src/recovery/page_header_recovery.cpp
#include "dbrecovery/innodb/page_header.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

namespace dbrecovery {
namespace recovery {

/**
 * @brief 从数据页反向推导 Page 0
 * @param data 页数据指针
 * @param page_size 页大小 (默认 16KB)
 * @return 重建的 PageHeader 或空值
 */
std::optional<PageHeader> recover_page0_from_data(
    const uint8_t* data, 
    size_t page_size = 16384) {
    
    // Step 1: 读取当前页的 FIL_PAGE_PREV
    uint32_t current_offset = read_le<uint32_t>(data, 18);
    uint32_t prev_page = read_le<uint32_t>(data, 0);
    
    // Step 2: 向前遍历直到 Page 0
    std::vector<uint32_t> page_chain;
    uint32_t search_offset = current_offset;
    
    while (prev_page != 0xFFFFFFFF && prev_page < search_offset) {
        page_chain.push_back(prev_page);
        search_offset = prev_page;
        
        // 读取前一页的 FIL_PAGE_PREV
        // (实际实现中需要从文件读取)
        prev_page = read_le<uint32_t>(data, 0); // 简化示例
    }
    
    // Step 3: 重建 Page 0
    PageHeader recovered{};
    recovered.page_offset = 0;
    recovered.prev_page = 0xFFFFFFFF;  // Page 0 没有前页
    recovered.next_page = page_chain.empty() ? 0xFFFFFFFF : page_chain[0];
    
    return recovered;
}

/**
 * @brief 通过特征码识别页类型
 */
uint16_t detect_page_type(const uint8_t* data) {
    uint16_t type = read_le<uint16_t>(data, 16);
    
    // 验证页类型有效性
    constexpr uint16_t VALID_TYPES[] = {
        0x0000, 0x26f1, 0x26f2, 0x26f3, 0x26f4, 0x26f5
    };
    
    for (auto valid : VALID_TYPES) {
        if (type == valid) return type;
    }
    
    return 0xFFFF;  // 无效页类型
}

} // namespace recovery
} // namespace dbrecovery
```

### 3.3 校验和验证

```cpp
/**
 * @brief 校验和验证 (CRC32C)
 */
bool verify_checksum(const uint8_t* data, size_t size) {
    // 读取存储的校验和
    uint32_t stored = read_le<uint32_t>(data, 24);
    
    // 计算校验和
    uint32_t computed = compute_crc32c(data, size);
    
    return stored == computed;
}
```

---

## 4. 恢复流程

### 4.1 完整恢复流程

```
┌─────────────────────────────────────────────────────────────┐
│                    恢复流程图                                │
├─────────────────────────────────────────────────────────────┤
│  1. 扫描文件，定位所有 InnoDB 页                            │
│  2. 对每个页，验证页类型有效性                               │
│  3. 读取 FIL_PAGE_PREV/FIL_PAGE_NEXT，构建页链              │
│  4. 从数据页反向推导 Page 0                                 │
│  5. 重建 FSP Header 结构                                    │
│  6. 验证校验和                                              │
│  7. 输出恢复结果                                            │
└─────────────────────────────────────────────────────────────┘
```

### 4.2 损坏页识别

```cpp
struct PageHealthInfo {
    bool is_valid;
    std::string status_message;
    uint16_t page_type;
    uint32_t page_offset;
    bool checksum_match;
};

PageHealthInfo check_page_health(const uint8_t* data, size_t size) {
    PageHealthInfo info{};
    
    // Step 1: 检查页大小
    if (size < 4096) {
        info.is_valid = false;
        info.status_message = "Page size too small";
        return info;
    }
    
    // Step 2: 检查页类型
    uint16_t type = read_le<uint16_t>(data, 16);
    info.page_type = type;
    
    if (type != 0x0000 && type != 0x26f1 && type != 0x26f2 && 
        type != 0x26f3 && type != 0x26f4 && type != 0x26f5) {
        info.is_valid = false;
        info.status_message = "Invalid page type: 0x" + hex_string(type);
        return info;
    }
    
    // Step 3: 检查页偏移
    uint32_t offset = read_le<uint32_t>(data, 18);
    info.page_offset = offset;
    
    if (offset >= 0xFFFFFFF0) {
        info.is_valid = false;
        info.status_message = "Invalid page offset";
        return info;
    }
    
    // Step 4: 校验和验证
    info.checksum_match = verify_checksum(data, size);
    
    info.is_valid = true;
    info.status_message = "Page is healthy";
    return info;
}
```

---

## 5. 测试用例

### 5.1 测试文件生成

```bash
# 生成损坏页测试文件
python3 tools/ibd_generator.py test_header_corrupt --scenario header_corrupt
```

### 5.2 单元测试

```cpp
// test/test_page_header_recovery.cpp
#include <gtest/gtest.h>
#include "dbrecovery/innodb/page_header.hpp"
#include "dbrecovery/recovery/page_header_recovery.hpp"

namespace dbrecovery {

TEST(PageHeaderRecoveryTest, RecoverFromDataPage) {
    // 读取测试文件
    std::ifstream file("test_header_corrupt_scenario.ibd", std::ios::binary);
    std::vector<uint8_t> data(std::istreambuf_iterator<char>(file), {});
    
    // 尝试恢复 Page 0
    auto recovered = PageHeaderRecovery::recover_page0_from_data(data.data());
    
    EXPECT_TRUE(recovered.has_value());
    EXPECT_EQ(recovered->page_offset, 0);
    EXPECT_EQ(recovered->prev_page, 0xFFFFFFFF);
}

TEST(PageHeaderRecoveryTest, DetectPageType) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, 0x26f1);  // INDEX page
    
    uint16_t type = PageHeaderRecovery::detect_page_type(page.data());
    EXPECT_EQ(type, 0x26f1);
}

TEST(PageHeaderRecoveryTest, InvalidPageType) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint16_t>(page.data(), 16, 0xFFFF);  // Invalid
    
    uint16_t type = PageHeaderRecovery::detect_page_type(page.data());
    EXPECT_EQ(type, 0xFFFF);
}

} // namespace dbrecovery
```

---

## 6. 性能优化

### 6.1 批量扫描

```cpp
/**
 * @brief 批量扫描文件中的所有 InnoDB 页
 */
std::vector<PageLocation> scan_all_pages(const std::string& file_path) {
    std::vector<PageLocation> pages;
    
    // 使用 mmap 避免数据拷贝
    int fd = open(file_path, O_RDONLY);
    uint8_t* data = (uint8_t*)mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    
    // 以页大小为步长扫描
    for (size_t offset = 0; offset < size; offset += PAGE_SIZE) {
        uint16_t type = read_le<uint16_t>(data, offset + 16);
        
        if (is_innodb_page_type(type)) {
            pages.push_back({offset, type});
        }
    }
    
    munmap(data, size);
    return pages;
}
```

### 6.2 并行处理

```cpp
/**
 * @brief 并行恢复多个损坏页
 */
void parallel_recovery(const std::vector<PageLocation>& pages) {
    #pragma omp parallel for
    for (size_t i = 0; i < pages.size(); i++) {
        auto recovered = recover_page_header(pages[i].offset);
        // 处理恢复结果
    }
}
```

---

## 7. 常见问题

### 7.1 页链断裂

**问题**: FIL_PAGE_PREV 指向无效页号

**解决方案**:
1. 检查页号是否在文件范围内
2. 验证目标页的页类型
3. 尝试从其他页链重建

### 7.2 校验和不匹配

**问题**: 存储的校验和与计算值不一致

**解决方案**:
1. 尝试多种校验和算法 (CRC32C/ADLER32/STATIC)
2. 标记为"可能损坏"但继续解析
3. 提供"忽略校验和"选项

### 7.3 页类型无法识别

**问题**: 页类型值不在有效范围内

**解决方案**:
1. 检查页数据是否对齐
2. 尝试从页尾部读取 (某些损坏场景)
3. 使用启发式方法推断页类型

---

## 8. 参考资料

- MySQL 8.0 源码: `include/fil0fil.h`
- MySQL 8.0 源码: `include/page0page.h`
- InnoDB Page Format Documentation
- CRC32C Algorithm Specification

---

## 9. 版本历史

| 版本 | 日期 | 变更 |
|------|------|------|
| 1.0 | 2026-03-30 | 初始版本，支持 Page 0 完全损坏恢复 |

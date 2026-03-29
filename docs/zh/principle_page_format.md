# InnoDB 页格式原理详解

## 1. 概述

InnoDB 存储引擎使用页（Page）作为数据存储的基本单位。每个页的大小通常为 16KB（可配置为 4KB、8KB、32KB 或 64KB）。理解 InnoDB 页格式是数据恢复的基础。

## 2. 页结构总览

```
+----------------------------------+
| FIL Header (38 bytes)            |
+----------------------------------+
| Page Header (变长)               |
+----------------------------------+
| Insert/Delete Buffer (可选)      |
+----------------------------------+
| User Records (变长)              |
+----------------------------------+
| Free Space (变长)                |
+----------------------------------+
| Page Directory (变长)            |
+----------------------------------+
| Page Trailer (8 bytes)           |
+----------------------------------+
```

## 3. FIL Header（文件页头）

FIL Header 是每个 InnoDB 页的固定部分，位于页的起始位置（偏移 0）。

| 字段 | 偏移 | 大小 | 描述 |
|------|------|------|------|
| FIL_PAGE_PREV | 0 | 4 bytes | 上一页页号（B+Tree 叶子页使用） |
| FIL_PAGE_NEXT | 4 | 4 bytes | 下一页页号（B+Tree 叶子页使用） |
| FIL_PAGE_LSN | 8 | 8 bytes | 日志序列号（LSN） |
| FIL_PAGE_TYPE | 16 | 2 bytes | 页类型 |
| FIL_PAGE_SPACE_ID | 20 | 4 bytes | 表空间 ID |
| FIL_PAGE_DATA_END | 24 | 4 bytes | 数据结束标记（用于校验和） |
| FIL_PAGE_CHECKSUM | 28 | 4 bytes | 校验和 |

### 页类型（FIL_PAGE_TYPE）

| 类型值 | 名称 | 描述 |
|--------|------|------|
| 0 | FIL_PAGE_TYPE_ALLOCATED | 新分配的页 |
| 9969 (0x26F1) | FIL_PAGE_INDEX | B+Tree 索引页 |
| 9970 (0x26F2) | FIL_PAGE_UNDO_LOG | Undo Log 页 |
| 9971 (0x26F3) | FIL_PAGE_SDI | SDI 元数据页 |
| 9972 (0x26F4) | FIL_PAGE_TYPE_BLOB | BLOB 页 |
| 9973 (0x26F5) | FIL_PAGE_COMPRESSED | 压缩页 |

## 4. Page Header（页头）

Page Header 位于 FIL Header 之后（偏移 38），大小可变，包含页的元数据。

| 字段 | 偏移 | 大小 | 描述 |
|------|------|------|------|
| PAGE_N_DIR_SLOTS | 0 | 2 bytes | 目录槽数量 |
| PAGE_HEAP_TOP | 2 | 2 bytes | 堆顶位置 |
| PAGE_N_HEAP | 4 | 2 bytes | 记录数量（15位） |
| PAGE_FREE | 6 | 2 bytes | 空闲空间起始位置 |
| PAGE_GARBAGE_END | 8 | 2 bytes | 垃圾空间结束位置 |
| PAGE_LAST_INSERT | 10 | 2 bytes | 最后插入位置 |
| PAGE_DIRECTION | 12 | 2 bytes | 插入方向 |
| PAGE_N_DIRECT_PAGES | 14 | 2 bytes | 直接页数量 |

## 5. User Records（用户记录）

用户记录存储实际的表数据。每条记录包含：

### 记录头（Record Header）

| 字段 | 大小 | 描述 |
|------|------|------|
| info_bits | 1 byte | 信息位（删除标志、最小标志等） |
| next_record | 2 bytes | 下一条记录的偏移 |
| heap_no | 2 bytes | 堆号 |

### 记录类型

| 类型值 | 名称 | 描述 |
|--------|------|------|
| 0 | REC_STATUS_ORDINARY | 普通记录 |
| 1 | REC_STATUS_NODE_PTR | 节点指针（非叶子页） |
| 2 | REC_STATUS_INFIMUM | 最小值系统记录 |
| 3 | REC_STATUS_SUPREMUM | 最大值系统记录 |

## 6. 行格式

InnoDB 支持多种行格式，影响记录的存储方式：

### REDUNDANT（冗余格式）

- MySQL 4.1 之前默认
- 记录头：6 字节
- 字段长度：2 字节前缀
- 适合小字段

### COMPACT（紧凑格式）

- MySQL 5.0 之后默认
- 记录头：5 字节
- 可变长度字段：1-2 字节前缀
- 更节省空间

### DYNAMIC（动态格式）

- MySQL 5.7 之后默认
- BLOB 字段存储在溢出页
- 记录中只存储指针
- 适合大字段

### COMPRESSED（压缩格式）

- 使用 zlib 压缩
- 页大小可配置
- 需要额外的压缩/解压开销

## 7. 校验和

InnoDB 使用多种校验和算法：

| 算法 | MySQL 版本 | 描述 |
|------|------------|------|
| NONE | 所有版本 | 不校验 |
| INNODB | 5.6/5.7 | 传统 InnoDB 校验和 |
| CRC32C | 8.0+ | Castagnoli CRC32（默认） |

校验和存储在 FIL_PAGE_CHECKSUM 字段（偏移 28）。

## 8. 恢复相关要点

### 8.1 页健康检查

恢复时需要验证：

1. **页类型**：必须是 FIL_PAGE_INDEX（9969）才能恢复数据
2. **校验和**：验证数据完整性
3. **INFIMUM/SUPREMUM**：系统记录必须存在
4. **页链**：通过 FIL_PAGE_PREV/NEXT 验证连续性

### 8.2 页解析步骤

```cpp
1. 读取页数据（16KB）
2. 验证 FIL_HEADER（38 bytes）
3. 检查页类型是否为 FIL_PAGE_INDEX
4. 解析 PAGE_HEADER（偏移 38）
5. 验证 INFIMUM/SUPREMUM 记录
6. 解析用户记录
7. 根据行格式解析字段
```

### 8.3 常见损坏场景

| 场景 | 影响 | 恢复策略 |
|------|------|---------|
| 页头损坏 | 无法解析页 | 从 SDI 恢复元数据 |
| 记录头损坏 | 无法解析记录 | 跳过损坏记录 |
| 校验和错误 | 数据可能损坏 | 使用非严格模式 |
| 页链断裂 | 页不连续 | 重建页链 |

## 9. 参考资料

- MySQL 8.0 源码：`include/page0page.h`
- MySQL 8.0 源码：`include/fil0fil.h`
- InnoDB 内部结构文档

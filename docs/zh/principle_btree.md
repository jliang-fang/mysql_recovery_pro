# InnoDB B+Tree 原理详解

## 1. 概述

B+Tree 是 InnoDB 存储引擎的核心数据结构，用于实现索引。理解 B+Tree 对于数据恢复至关重要。

## 2. B+Tree 特性

### 2.1 基本特性

- **平衡性**：所有叶子页在同一层
- **多路搜索树**：每个节点可以有多个子节点
- **叶子链表**：叶子页通过双向链表连接
- **内部节点**：只存储键值，不存储数据

### 2.2 B+Tree 与 B-Tree 的区别

| 特性 | B-Tree | B+Tree |
|------|--------|--------|
| 数据存储 | 所有节点 | 仅叶子节点 |
| 搜索效率 | O(log n) | O(log n) |
| 范围查询 | 需要回溯 | 叶子链表直接遍历 |
| 磁盘预读 | 效率较低 | 效率高 |

## 3. B+Tree 页类型

### 3.1 根页（Root Page）

- B+Tree 的入口点
- 可以是叶子页或内部页
- 存储在 BTR_ROOT_PAGE_NO

### 3.2 内部页（Internal Page）

- 存储键值和子页指针
- 不存储实际数据
- 用于导航到叶子页

### 3.3 叶子页（Leaf Page）

- 存储实际的表数据
- 通过 FIL_PAGE_PREV/NEXT 形成双向链表
- 包含 INFIMUM 和 SUPREMUM 系统记录

## 4. B+Tree 结构

```
                    +------------------+
                    |    Root Page     |
                    |  (Internal)      |
                    +--------+---------+
                             |
             +---------------+---------------+
             |               |               |
    +--------v---------+ +---v----------+ +--v------------+
    |  Internal Page   | | Internal   | | Internal      |
    |  (Level 1)       | | Page       | | Page          |
    +--------+---------+ +------------+ +-------------+--+
             |                                       |
    +--------v---------+                    +--------v---------+
    |  Leaf Page       |                    |  Leaf Page       |
    |  (Level 0)       |                    |  (Level 0)       |
    +--------+---------+                    +--------+---------+
             |                                       |
    +--------v---------+                    +--------v---------+
    |  Leaf Page       |                    |  Leaf Page       |
    |  (Level 0)       |                    |  (Level 0)       |
    +------------------+                    +------------------+
```

## 5. B+Tree Header

B+Tree Header 位于页头之后（偏移 38 + 4 = 42）：

| 字段 | 偏移 | 大小 | 描述 |
|------|------|------|------|
| BTR_N_LEVELS | 4 | 2 bytes | 树的高度（0 = 叶子） |
| BTR_IND_PAGE_OFFSET | 6 | 8 bytes | 索引 ID |
| BTR_MAX_SIZE | 50 | 2 bytes | 记录最大大小 |

## 6. B+Tree 操作

### 6.1 插入操作

1. **查找叶子页**：从根页开始，递归查找
2. **插入记录**：在叶子页中插入新记录
3. **页分裂**：如果页满，分裂为两个页
4. **更新父页**：更新父页的指针

### 6.2 删除操作

1. **查找记录**：定位要删除的记录
2. **删除记录**：标记为删除
3. **页合并**：如果页空，合并到相邻页
4. **更新父页**：更新父页的指针

### 6.3 搜索操作

1. **从根页开始**：获取根页号
2. **递归查找**：根据键值查找子页
3. **叶子页搜索**：在叶子页中线性搜索

## 7. B+Tree 恢复要点

### 7.1 页链重建

通过 FIL_PAGE_PREV/NEXT 字段重建页链：

```cpp
// 从第一个页开始
current_page = first_page;
while (current_page != 0xFFFFFFFF) {
    // 读取当前页
    page = read_page(current_page);
    
    // 获取下一页
    next_page = page->FIL_PAGE_NEXT;
    
    // 移动到下一页
    current_page = next_page;
}
```

### 7.2 树高度判断

通过 BTR_N_LEVELS 判断树的高度：

- **0**：叶子页，存储数据
- **> 0**：内部页，存储指针

### 7.3 范围查询

B+Tree 支持高效的范围查询：

```cpp
// 从左边界开始
current_page = find_left_boundary(key);
while (current_page != 0xFFFFFFFF) {
    // 读取页中的记录
    records = read_records(current_page);
    
    // 检查是否超出右边界
    if (records.back().key > right_key) break;
    
    // 移动到下一页
    current_page = current_page->FIL_PAGE_NEXT;
}
```

## 8. B+Tree 损坏场景

| 场景 | 影响 | 恢复策略 |
|------|------|---------|
| 根页损坏 | 无法定位树 | 从 SDI 恢复 root_page_no |
| 页链断裂 | 页不连续 | 通过 FIL_PAGE_PREV/NEXT 重建 |
| 索引 ID 不匹配 | 页不属于该索引 | 验证 BTR_IND_PAGE_OFFSET |
| 树高度错误 | 解析错误 | 从实际页数推断高度 |

## 9. 实际案例

### 9.1 Truncate 恢复

Truncate 后，B+Tree 结构仍然存在，只是记录被标记为删除：

```cpp
// 1. 读取所有页
for (page_no = 0; page_no < total_pages; ++page_no) {
    page = read_page(page_no);
    
    // 2. 解析 B+Tree
    parser = BPlusTreeParser(page);
    parser.parse();
    
    // 3. 提取记录（包括已删除的）
    records = parser.parse_records(schema);
}
```

### 9.2 页分裂处理

页分裂后，需要重建页链：

```cpp
// 1. 找到所有页
pages = scan_all_pages();

// 2. 按 FIL_PAGE_PREV/NEXT 排序
std::sort(pages.begin(), pages.end(), [](auto& a, auto& b) {
    return a.FIL_PAGE_PREV < b.FIL_PAGE_PREV;
});

// 3. 重建页链
for (size_t i = 0; i < pages.size() - 1; ++i) {
    pages[i].FIL_PAGE_NEXT = pages[i + 1].page_no;
    pages[i + 1].FIL_PAGE_PREV = pages[i].page_no;
}
```

## 10. 参考资料

- MySQL 8.0 源码：`include/btr0btr.h`
- MySQL 8.0 源码：`include/page0page.h`
- InnoDB B+Tree 实现文档

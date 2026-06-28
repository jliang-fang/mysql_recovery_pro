# MySQL 灾难恢复实战指南

## 目录
1. [TRUNCATE TABLE 恢复](#truncate-table-恢复)
2. [DROP TABLE 恢复](#drop-table-恢复)
3. [rm -rf 恢复](#rm-rf-恢复)
4. [Schema 参数实战](#schema-参数实战)
5. [MySQL版本差异说明](#mysql版本差异说明)
6. [恢复验证结果](#恢复验证结果)

---

## TRUNCATE TABLE 恢复

### MySQL版本差异

TRUNCATE TABLE 在不同MySQL版本有不同实现：

| MySQL版本 | TRUNCATE实现 | 数据可恢复性 | 恢复方法 |
|-----------|--------------|--------------|----------|
| **8.0+** | DROP + CREATE | **数据完全丢失** | 磁盘雕刻（原始磁盘） |
| **5.6/5.7** | DELETE逐行删除 | **可恢复** | Undo Log + 磁盘雕刻 |

### MySQL 8.0+ TRUNCATE恢复流程

```bash
# 注意：MySQL 8.0+ TRUNCATE = DROP + CREATE
# 原数据页被释放，新文件为空表

# 步骤1：立即停止MySQL服务（防止数据覆写）
sudo systemctl stop mysqld

# 步骤2：复制当前.ibd文件（可能已重建为空表）
cp /var/lib/mysql/dbname/table.ibd /recovery/table_after_truncate.ibd

# 步骤3：需要从原始磁盘设备进行磁盘雕刻
./mysql_recovery_pro --disk-carving \
    --device-path=/dev/sda1 \
    --output=/recovery/output \
    --num-workers=8

# 步骤4：使用Schema参数提升数据质量
./mysql_recovery_pro --disk-carving \
    --device-path=/dev/sda1 \
    --schema="id INT PRIMARY KEY, name VARCHAR(100), status ENUM('active','inactive')" \
    --output=/recovery/output
```

### MySQL 5.6/5.7 TRUNCATE恢复流程

```bash
# MySQL 5.6/5.7 使用DELETE方式，可从Undo Log恢复

# 步骤1：停止MySQL服务
sudo systemctl stop mysqld

# 步骤2：复制系统表空间（ibdata1）
cp /var/lib/mysql/ibdata1 /recovery/ibdata1_backup

# 步骤3：从Undo Log恢复
./mysql_recovery_pro --truncate-recovery \
    --system-tablespace=/recovery/ibdata1_backup \
    --output=/recovery/output

# 步骤4：同时尝试磁盘雕刻（从.ibd文件）
cp /var/lib/mysql/dbname/table.ibd /recovery/table.ibd
./mysql_recovery_pro --disk-carving \
    --device-path=/recovery/table.ibd \
    --frm=/var/lib/mysql/dbname/table.frm \
    --output=/recovery/output
```

---

## DROP TABLE 恢复

### 前提条件检查

```bash
# 检查innodb_file_per_table配置
mysql -e "SHOW VARIABLES LIKE 'innodb_file_per_table';"

# 如果ON：每个表独立.ibd文件，DROP TABLE后文件被删除
# 如果OFF：所有数据在ibdata1中，可从ibdata1恢复
```

### innodb_file_per_table=ON场景

```bash
# 步骤1：立即卸载文件系统
sudo umount /dev/sda1
# 或切换为只读
sudo mount -o remount,ro /dev/sda1

# 步骤2：使用testdisk/debugfs恢复被删除的文件
sudo testdisk /dev/sda1
# 或
sudo debugfs -R "lsdel" /dev/sda1 > deleted_files.txt
sudo debugfs -R "dump <inode> /recovery/table.ibd" /dev/sda1

# 步骤3：恢复数据
./mysql_recovery_pro --ibd=/recovery/table.ibd \
    --frm=/backup/table.frm \
    --output=/recovery/output

# 步骤4：如果没有FRM备份，使用Schema参数
./mysql_recovery_pro --ibd=/recovery/table.ibd \
    --schema="id INT PRIMARY KEY, name VARCHAR(100), created_at DATETIME" \
    --output=/recovery/output
```

### innodb_file_per_table=OFF场景

```bash
# 数据仍在ibdata1中，直接从系统表空间恢复
./mysql_recovery_pro --disk-carving \
    --device-path=/var/lib/mysql/ibdata1 \
    --output=/recovery/output
```

---

## rm -rf 恢复

### 紧急响应流程

```bash
# ⚠️ 立即停止所有写入操作

# 步骤1：卸载文件系统（最关键）
sudo umount /dev/sda1

# 步骤2：如果无法卸载，切换为只读
sudo mount -o remount,ro /dev/sda1

# 步骤3：创建恢复工作目录
mkdir -p /recovery/output
```

### 磁盘雕刻恢复

```bash
# 从原始磁盘设备扫描所有InnoDB页
./mysql_recovery_pro --disk-carving \
    --device-path=/dev/sda1 \
    --output=/recovery/output \
    --num-workers=8 \
    --verbose

# 输出结果
ls /recovery/output/
# recovered_index_123.csv  - 每个索引ID生成一个CSV
# stats.txt                - 恢复统计报告
```

### 大容量磁盘优化

```bash
# 100GB+磁盘建议参数
./mysql_recovery_pro --disk-carving \
    --device-path=/dev/sda1 \
    --output=/recovery/output \
    --num-workers=16 \
    --page-size=16384 \
    --verbose

# 仅扫描特定范围（节省时间）
./mysql_recovery_pro --disk-carving \
    --device-path=/dev/sda1 \
    --output=/recovery/output \
    --start-offset=0 \
    --end-offset=107374182400  # 100GB
```

---

## Schema 参数实战

### 三种Schema方式对比

| 方式 | 适用场景 | ENUM支持 | 验证结果 |
|------|----------|----------|----------|
| `--schema=<def>` | 快速定义表结构 | ✅ SQL格式 | 100% |
| `--schema-file=<path>` | 复杂表结构 | ✅ JSON数组 | 100% |
| `--frm=<path>` | MySQL 5.6/5.7 | ✅ 自动解析 | 100% |

### 实战验证：products表

**表结构**：
```sql
CREATE TABLE products (
    id INT PRIMARY KEY AUTO_INCREMENT,
    product_name VARCHAR(200),
    category ENUM('electronics', 'clothing', 'food', 'toys'),
    price DECIMAL(10,2),
    stock INT,
    created_at DATETIME,
    json_attr JSON
);
```

**测试数据**：5条记录，包含ENUM、JSON、DECIMAL复杂类型

#### 方式1：SQL格式Schema字符串

```bash
./mysql_recovery_pro \
    --ibd=/backup/products.ibd \
    --schema="id INT PRIMARY KEY, product_name VARCHAR(200), category ENUM('electronics','clothing','food','toys'), price DECIMAL(10,2), stock INT, created_at DATETIME, json_attr JSON" \
    --output=/recovery/sql_schema
```

**输出**：
```csv
id,product_name,category,price,stock,created_at,json_attr
1,Laptop Pro 15,electronics,1299.99,50,2026-06-28 10:00:00,{"brand":"TechCorp","warranty":2}
2,Cotton T-Shirt,clothing,29.99,200,2026-06-28 11:00:00,{"brand":"ClothCo","sizes":["S","M","L","XL"]}
```

**结果**：5/5恢复，ENUM正确显示为实际值

#### 方式2：JSON Schema文件

```bash
# 创建JSON文件
cat > products_schema.json << 'EOF'
{
  "table_name": "products",
  "columns": [
    {"name": "id", "type": "INT", "is_nullable": false, "column_key": "PRIMARY"},
    {"name": "product_name", "type": "VARCHAR", "length": 200},
    {"name": "category", "type": "ENUM", "elements": ["electronics", "clothing", "food", "toys"]},
    {"name": "price", "type": "DECIMAL", "precision": 10, "scale": 2},
    {"name": "stock", "type": "INT"},
    {"name": "created_at", "type": "DATETIME"},
    {"name": "json_attr", "type": "JSON"}
  ]
}
EOF

./mysql_recovery_pro \
    --ibd=/backup/products.ibd \
    --schema-file=products_schema.json \
    --output=/recovery/json_schema
```

**结果**：5/5恢复，ENUM正确显示

#### 方式3：FRM文件（MySQL 5.6/5.7）

```bash
./mysql_recovery_pro \
    --ibd=/backup/products_56/products.ibd \
    --frm=/backup/products_56/products.frm \
    --output=/recovery/frm_recovery
```

**输出**：
```csv
id,product_name,category,price,stock,created_at,description
1,Laptop Pro 15,electronics,1299.99,50,2026-06-28 10:00:00,High performance laptop
```

**结果**：3/3恢复，ENUM正确显示

---

## MySQL版本差异说明

### TRUNCATE TABLE实现差异

| 版本 | 实现方式 | 文件状态 | 数据残留 |
|------|----------|----------|----------|
| 5.6 | DELETE逐行删除 | 文件大小不变 | 可恢复 |
| 5.7 | DELETE逐行删除 | 文件大小不变 | 可恢复 |
| 8.0 | DROP+CREATE | 文件重建为空 | **数据丢失** |
| 8.4 | DROP+CREATE | 文件重建为空 | **数据丢失** |
| 9.0 | DROP+CREATE | 文件重建为空 | **数据丢失** |

### SDI/FRM差异

| 版本 | Schema存储方式 | 磁盘雕刻Schema来源 |
|------|-----------------|---------------------|
| 5.6 | `.frm`文件 | 必须提供FRM或手动Schema |
| 5.7 | `.frm`文件 | 必须提供FRM或手动Schema |
| 8.0 | SDI内嵌于.ibd | 自动从SDI提取 |
| 8.4 | SDI内嵌于.ibd | 自动从SDI提取 |
| 9.0 | SDI内嵌于.ibd | 自动从SDI提取 |

### JSON字段支持

| 版本 | JSON字段支持 |
|------|--------------|
| 5.6 | ❌ 不支持 |
| 5.7 | ✅ 支持 |
| 8.0+ | ✅ 支持 |

---

## 恢复验证结果

### 测试环境

| 版本 | 容器 | 测试数据量 | 测试场景 |
|------|------|------------|----------|
| MySQL 5.6 | mysql56_test | 5条 | FRM恢复 |
| MySQL 8.0 | mysql80_test | 5条 | Schema参数恢复 |

### Schema参数验证汇总

| 参数方式 | 恢复记录数 | ENUM显示 | 数据完整性 |
|----------|------------|----------|------------|
| `--schema=<def>` (SQL格式) | 5/5 | 实际值 | 100% |
| `--schema-file=<path>` | 5/5 | 实际值 | 100% |
| `--frm=<path>` | 3/3 | 实际值 | 100% |

### TRUNCATE恢复验证

| 版本 | 预期恢复 | 实际恢复 | 说明 |
|------|----------|----------|------|
| MySQL 8.0 | 0条 | 0条 | TRUNCATE=DROP+CREATE，数据丢失 |
| MySQL 5.7 | 可恢复 | 需测试 | Undo Log + 磁盘雕刻 |

### 磁盘雕刻验证

| 来源 | 页数 | 记录数 | Schema方式 |
|------|------|--------|------------|
| ibdata1 | 102 | 9,051 | 自动SDI |
| products.ibd | 7 | 5 | 手动Schema |
| FRM文件 | - | 3 | FRM自动解析 |

---

## 最佳实践总结

### 1. 紧急响应优先级

1. **立即卸载文件系统** - 防止数据覆写
2. **停止MySQL服务** - 防止新数据写入
3. **备份现有文件** - 即使文件可能已损坏
4. **开始恢复操作** - 使用合适的恢复模式

### 2. Schema选择建议

| 场景 | 推荐方式 |
|------|----------|
| MySQL 8.0+ 有SDI | 自动提取（无需指定） |
| MySQL 5.6/5.7 有FRM备份 | `--frm=<path>` |
| 无FRM备份，简单表结构 | `--schema=<def>` SQL格式 |
| 复杂表结构（多ENUM/SET） | `--schema-file=<path>` |

### 3. ENUM/SET处理要点

- **必须使用SQL格式或JSON文件**
- 冒号分隔格式不支持ENUM元素
- FRM文件自动解析ENUM元素

### 4. 数据质量验证

```bash
# 检查恢复记录数
wc -l /recovery/output/*.csv

# 检查ENUM值是否正确显示
grep -E "electronics|clothing|food|toys" /recovery/output/*.csv

# 检查JSON字段完整性
grep "brand" /recovery/output/*.csv
```
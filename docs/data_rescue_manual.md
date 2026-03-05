# 数据救援手册

## 概述
本手册旨在为技术人员提供数据救援（Data Rescue）的标准操作流程与最佳实践指南，涵盖常见故障场景、工具使用方法及安全注意事项。

---

## 一、常见数据丢失场景
1. **误删除文件**：用户或脚本误删重要数据。
2. **磁盘分区损坏**：MBR/GPT 损坏导致系统无法识别分区表。
3. **文件系统崩溃**：如 NTFS/FAT32/ext4 元数据损坏。
4. **逻辑坏道/扇区错误**：硬盘读取异常但物理未完全损坏。
5. **病毒或勒索软件加密**：恶意程序破坏原始数据结构。
6. **RAID 阵列失效**：多盘冗余配置中部分磁盘离线导致阵列降级或崩溃。

---

## 二、救援前准备
### 2.1 安全原则
- ✅ **禁止直接写入原盘**：所有操作应在镜像副本上进行。
- ✅ **创建只读挂载**：使用 `mount -o ro` 或 `ddrescue` 创建镜像。
- ✅ **记录操作日志**：详细记录每一步命令与输出，便于回溯。

### 2.2 必备工具清单
| 工具 | 用途 |
|------|------|
| `ddrescue` | 高效磁盘/分区镜像创建（支持断点续传） |
| `testdisk` | 分区表修复、丢失分区恢复 |
| `photorec` | 基于文件签名的无文件系统数据恢复 |
| `foremost` / `scalpel` | 另类文件 carving 工具 |
| `hexdump` / `xxd` | 低级数据查看与分析 |
| `fsck` / `chkdsk` | 文件系统一致性检查（谨慎使用！）|

---

## 三、标准操作流程（SOP）

### Step 1：评估与诊断
- 使用 `lsblk`, `fdisk -l`, `smartctl -a /dev/sdX` 查看设备状态。
- 检查内核日志：`dmesg | grep -i error`
- 判断是**逻辑故障**（可恢复）还是**物理故障**（需专业硬件支持）。

### Step 2：创建只读镜像（关键！）
```bash
# 使用 ddrescue 创建镜像（推荐）
sudo ddrescue -r3 /dev/sda1 rescue_image.img rescue_logfile.log
```
- `-r3` 表示重试失败区域最多 3 次。
- `rescue_logfile.log` 记录进度，支持中断后恢复。

### Step 3：在镜像上操作（非原盘！）
```bash
# 挂载为只读（若文件系统可识别）
sudo mount -o ro,loop rescue_image.img /mnt/rescue
```

### Step 4：分区恢复尝试
- 运行 `testdisk rescue_image.img` → 分析分区表 → 尝试重建。
- 若成功，导出新分区表并重新生成镜像。

### Step 5：文件级恢复（当文件系统不可用）
```bash
# 使用 photorec 恢复文件
sudo photorec /dev/sda1
```
- 支持按扩展名筛选、跳过已存在文件等选项。
- 输出目录建议单独挂载大容量存储设备。

---

## 四、特殊场景处理
### 4.1 RAID 恢复
- 使用 `mdadm --assemble --scan` 尝试自动组装阵列。
- 若失败，手动指定成员盘顺序：`mdadm --assemble /dev/md0 /dev/sda1 /dev/sdb1 ...`
- 注意：**不要强制启动降级阵列写入操作！**

### 4.2 加密数据恢复（如 BitLocker）
- 若有恢复密钥/密码 → 先解密再救援。
- 否则需结合原始密钥管理服务（如 Active Directory）获取密钥。

---

## 五、注意事项与禁忌
⚠️ **禁止行为**：
- 在原盘上运行 `fsck` 或格式化命令。
- 将恢复数据写回同一设备。
- 使用 Windows 自带的“磁盘检查”工具处理 Linux 分区。

✅ **推荐做法**：
- 所有操作前备份当前状态（如 `sfdisk -d /dev/sda > sda_backup.txt`）。
- 对高价值数据，优先送专业机构处理物理故障。

---

## 六、附录：常用命令速查表
| 场景 | 命令 |
|------|------|
| 查看磁盘健康状态 | `smartctl -a /dev/sdX` |
| 创建镜像（带断点续传） | `sudo ddrescue -r3 /dev/src /path/to/image.img log.txt` |
| 分区恢复 | `sudo testdisk image.img` |
| 文件恢复 | `sudo photorec image.img` |
| 查看文件系统类型 | `file -s /dev/sdX1` |

---

*最后更新：2024年6月*

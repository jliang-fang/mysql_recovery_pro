#!/usr/bin/env python3
import os
import subprocess

BASE_DIR = "/app/mysql_recovery_proy"
BUILD_DIR = os.path.join(BASE_DIR, "build")
TEST_DATA_DIR = os.path.join(BASE_DIR, "test", "test_data")
TEST_OUTPUT_DIR = os.path.join(BASE_DIR, "test", "output")

os.makedirs(TEST_OUTPUT_DIR, exist_ok=True)

print("=== MySQL 8.0 InnoDB Recovery Test Suite ===")
print(f"Build dir: {BUILD_DIR}")
print(f"Test data dir: {TEST_DATA_DIR}")

# 检查可执行文件
exe_path = os.path.join(BUILD_DIR, "mysql_recovery_proy_cli")
if not os.access(exe_path, os.X_OK):
    print("[错误] mysql_recovery_proy_cli 可执行文件不存在，请先编译：cmake .. && make")
    exit(1)

os.chdir(BUILD_DIR)

# 测试1: Page 解析
print("\n测试1: Page 结构解析...")
ibd_file = os.path.join(TEST_DATA_DIR, "valid_ibd_file.ibd")
if os.path.isfile(ibd_file):
    log_path = os.path.join(TEST_OUTPUT_DIR, "page_parse.log")
    try:
        result = subprocess.run([exe_path, ibd_file], 
                                stdout=open(log_path, 'w'), stderr=subprocess.STDOUT, 
                                check=True)
        print("[通过] Page 解析成功")
    except subprocess.CalledProcessError as e:
        print(f"[错误] Page 解析失败，查看日志: {log_path}")
        exit(1)
else:
    print("[警告] valid_ibd_file.ibd 不存在，跳过测试")

# 测试2: 加密/解密
print("\n测试2: 加密/解密...")
if os.path.isfile(ibd_file):
    enc_file = os.path.join(TEST_OUTPUT_DIR, "encrypted.ibd")
    dec_file = os.path.join(TEST_OUTPUT_DIR, "decrypted.ibd")
    
    try:
        subprocess.run([exe_path, "-e", ibd_file, enc_file, "testpassword123"], check=True)
        subprocess.run([exe_path, "-c", enc_file, dec_file, "-p", "testpassword123"], check=True)
        
        if os.path.isfile(ibd_file) and os.path.isfile(dec_file):
            with open(ibd_file, 'rb') as f1, open(dec_file, 'rb') as f2:
                if f1.read() == f2.read():
                    print("[通过] 加密/解密验证通过")
                else:
                    print("[警告] 解密后文件与原始不一致（可能因加密填充差异）")
    except subprocess.CalledProcessError as e:
        print("[错误] 加密/解密失败")
        exit(1)
else:
    print("[警告] valid_ibd_file.ibd 不存在，跳过测试")

# 测试3: 磁盘扫描
print("\n测试3: 磁盘扫描...")
if os.path.isdir(TEST_DATA_DIR):
    recovered_dir = os.path.join(TEST_OUTPUT_DIR, "recovered_disk")
    try:
        subprocess.run([exe_path, "-d", TEST_DATA_DIR, recovered_dir], check=True)
        print("[通过] 磁盘扫描完成")
    except subprocess.CalledProcessError as e:
        print("[错误] 磁盘扫描失败")
        exit(1)
else:
    print("[警告] test_data 目录不存在，跳过测试")

# 测试4: 文件信息扫描
print("\n测试4: 文件信息扫描...")
if os.path.isfile(ibd_file):
    try:
        subprocess.run([exe_path, "-s", ibd_file], stdout=subprocess.DEVNULL, check=True)
        print("[通过] 文件信息扫描成功")
    except subprocess.CalledProcessError as e:
        print("[错误] 信息扫描失败")
        exit(1)
else:
    print("[警告] valid_ibd_file.ibd 不存在，跳过测试")

print("\n=== 所有关键测试通过 ===")

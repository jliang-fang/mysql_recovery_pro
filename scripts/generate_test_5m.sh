#!/bin/bash
# 生成 500万+ 测试数据脚本

DB_HOST="localhost"
DB_PORT="3306"
DB_USER="root"
DB_NAME="recovery_test"
TABLE_NAME="test_recovery_big"

echo "开始生成测试数据..."

# 使用 MySQL 本地插入方式
podman exec mysql56_recovery_test bash -c "
mysql -u root recovery_test -e \"SET SQL_MODE='';\"

# 生成 520万数据（每个batch 10万）
for i in {1..52}; do
    echo \"插入批次 \$i (10万条)...\"
    echo \"INSERT INTO test_recovery_big (user_name, email, age, created_at, status, data) VALUES\"
    
    # 生成单批数据
    for j in {1..100000}; do
        name=\"user_\$(printf '%08d' \$((\$i * 100000 + \$j)))\"
        email=\"\${name}@test.com\"
        age=\$((RANDOM % 80 + 18))
        year=\$((2020 + RANDOM % 4))
        month=\$((RANDOM % 12 + 1))
        day=\$((RANDOM % 28 + 1))
        created=\"\${year}-\$(printf '%02d' \$month)-\$(printf '%02d' \$day) 12:00:00\"
        status=\$((RANDOM % 3))
        data=\"data_\$(printf '%016d' \$((RANDOM * RANDOM)))\"
        
        if [ \$j -lt 100000 ]; then
            echo \"('\${name}', '\${email}', \${age}, '\${created}', \${status}, '\${data}'),\"
        else
            echo \"('\${name}', '\${email}', \${age}, '\${created}', \${status}, '\${data}')\"
        fi
    done | mysql -u root recovery_test
done

echo \"数据插入完成\"
mysql -u root recovery_test -e \"SELECT COUNT(*) as total_records FROM test_recovery_big;\""

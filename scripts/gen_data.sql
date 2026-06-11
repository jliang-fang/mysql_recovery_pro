DELIMITER ;
DROP TABLE IF EXISTS test_recovery_iter;

CREATE TABLE test_recovery_iter (
    id BIGINT UNSIGNED NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 插入索引用于生成数据
INSERT INTO test_recovery_iter (id) VALUES (0),(1),(2),(3),(4),(5),(6),(7),(8),(9);

INSERT INTO test_recovery_iter (id) SELECT id + 10 FROM test_recovery_iter;
INSERT INTO test_recovery_iter (id) SELECT id + 20 FROM test_recovery_iter;
INSERT INTO test_recovery_iter (id) SELECT id + 30 FROM test_recovery_iter;
INSERT INTO test_recovery_iter (id) SELECT id + 40 FROM test_recovery_iter;

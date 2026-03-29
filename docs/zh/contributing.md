# MySQL 8.x 数据恢复引擎 - 贡献指南

## 概述

欢迎贡献！本文档提供了如何为 MySQL 8.x 数据恢复引擎做出贡献的完整指南。

## 代码贡献流程

### 1. Fork 项目

点击 GitHub 项目页面的 "Fork" 按钮。

### 2. 克隆仓库

```bash
git clone https://github.com/your-username/mysql8_recovery.git
cd mysql8_recovery
```

### 3. 创建分支

```bash
git checkout -b feature/your-feature-name
# 或
git checkout -b fix/your-bug-fix
```

### 4. 开发

#### 代码风格

- 遵循 Google C++ Style Guide
- 使用 4 空格缩进
- 行宽不超过 120 字符
- 添加必要的注释

#### 静态分析

```bash
# 运行 clang-tidy
clang-tidy -checks=* src/*.cpp -- -std=c++20

# 运行 cppcheck
cppcheck --enable=all src/
```

#### 动态分析

```bash
# AddressSanitizer
cmake .. -DENABLE_ASAN=ON
make
ctest

# UndefinedBehaviorSanitizer
cmake .. -DENABLE_UBSAN=ON
make
ctest

# LeakSanitizer
cmake .. -DENABLE_LSAN=ON
make
ctest
```

### 5. 测试

```bash
# 运行所有测试
ctest --verbose

# 运行特定测试
./build/tests/test_page_header
./build/tests/test_redo_log
./build/tests/test_sdi_manager
```

### 6. 提交

```bash
git add .
git commit -m "feat: 添加新功能描述"
# 或
git commit -m "fix: 修复 bug 描述"
```

### 7. 推送

```bash
git push origin feature/your-feature-name
```

### 8. 提交 Pull Request

1. 访问 GitHub 项目页面
2. 点击 "Compare & pull request"
3. 填写 PR 描述
4. 提交 PR

## PR 模板

```
## 描述
[简要描述 PR 的内容]

## 类型
- [ ] 新功能
- [ ] 修复
- [ ] 优化
- [ ] 文档
- [ ] 测试

## 影响范围
- [ ] 核心功能
- [ ] 测试
- [ ] 文档
- [ ] 构建系统

## 测试
- [ ] 单元测试通过
- [ ] 集成测试通过
- [ ] 性能测试通过

## 相关 Issue
[链接相关 Issue]

## 备注
[其他需要说明的内容]
```

## 代码审查

### 审查标准

1. **功能正确性**: 代码是否实现了预期功能
2. **代码质量**: 是否遵循编码规范
3. **测试覆盖**: 是否有足够的测试
4. **性能**: 是否有性能问题
5. **安全性**: 是否有安全风险

### 审查流程

1. 至少一名维护者审查通过
2. 所有测试通过
3. 无未解决的评论

## 新功能建议

### 提交 Issue

在提交 PR 之前，建议先提交 Issue 讨论：

1. 功能描述
2. 使用场景
3. 实现思路
4. 潜在影响

### 功能开发 checklist

- [ ] 设计文档
- [ ] 单元测试
- [ ] 集成测试
- [ ] 文档更新
- [ ] 性能测试

## 文档贡献

### 文档类型

1. **API 文档**: 类和函数的使用说明
2. **用户指南**: 功能使用说明
3. **开发者指南**: 构建和开发流程
4. **原理文档**: 技术原理说明

### 文档风格

- 使用 Markdown 格式
- 中英文双语
- 包含代码示例
- 添加图表说明

## 问题报告

### 提交 Issue

使用 GitHub Issues 提交问题：

1. **Bug**: 详细描述复现步骤
2. **Feature Request**: 描述需求场景
3. **Question**: 提问具体问题

### Issue 模板

```
## 类型
- [ ] Bug
- [ ] Feature Request
- [ ] Question

## 描述
[详细描述]

## 环境
- 操作系统: [ ]
- MySQL 版本: [ ]
- 项目版本: [ ]

## 复现步骤
1. [步骤 1]
2. [步骤 2]
3. [步骤 3]

## 期望结果
[期望的行为]

## 实际结果
[实际的行为]

## 日志/截图
[相关日志或截图]
```

## 许可证

通过贡献，您同意您的贡献遵循 MIT License。

## 联系方式

- **作者**: jliang.fang
- **邮箱**: jliang.fang@example.com

## 感谢

感谢您对 MySQL 8.x 数据恢复引擎的贡献！

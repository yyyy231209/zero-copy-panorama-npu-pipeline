---
name: Pull Request
about: 提交代码变更
title: ""
labels: []
assignees: ""
---

## 变更内容 / Changes

[描述你的改动，修复了什么/新增了什么]

## 关联 Issue / Related Issue

Fixes #[issue number]（如有）

## 测试情况 / Testing

- [ ] 代码通过 CI（交叉语法检查 + 结构检查）
- [ ] 已在 RK3588 板编译通过（`cmake --build build -j2`）
- [ ] 输出插件在板端运行验证（null sink 基准 / tcp_h264 推流 ffplay 验证）

## 检查清单 / Checklist

- [ ] 代码遵循项目现有风格（C++17，与现有代码一致）
- [ ] 新输出插件实现 `IOutputSink` 接口并注册（无全局状态）
- [ ] 未包含敏感信息（路径、密钥、IP）
- [ ] README/文档已同步更新（如涉及）

## 备注 / Notes

[其他需要 maintainer 知道的信息]

# 贡献指南 / Contributing Guide

感谢你对本项目感兴趣！任何形式的贡献（bug 报告、文档、代码、想法）都欢迎。

## 🐛 报告问题 / Reporting Issues

1. 先搜索 [Issues](https://github.com/yyyy231209/zero-copy-panorama-npu-pipeline/issues) 是否已存在相同问题
2. 使用 [Bug 报告模板](https://github.com/yyyy231209/zero-copy-panorama-npu-pipeline/issues/new/choose)（完整填写硬件/软件环境 + 上游模块版本）
3. 附上日志：崩溃栈（gdb bt）、程序输出、ffplay 验证结果

## 🛠️ 提交代码 / Submitting Code

### 环境要求

- RK3588 开发板（aarch64）或 GitHub Actions 交叉语法检查
- GCC 10+ / C++17 / CMake ≥ 3.10
- 依赖：MPP / RGA / RKNN Runtime / Mali OpenCL + 两个上游仓库（A 路径变量或 B fetch_deps.sh）

### 开发流程

```bash
# 1. Fork 并克隆
git clone https://github.com/<your-name>/zero-copy-panorama-npu-pipeline.git
cd zero-copy-panorama-npu-pipeline

# 2. 创建分支
git checkout -b feature/your-feature

# 3. 修改 + 本地验证
./fetch_deps.sh                       # 或 -DPANORAMA_ROOT/-DNPU_ROOT
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
./build/panorama_npu_live models/best.rknn assets/open_chain_v1   # null sink 基准

# 4. 提交（信息清晰）
git commit -m "feat: add xxx"      # 新功能
git commit -m "fix: xxx"           # 修复
git commit -m "docs: xxx"          # 文档

# 5. 推送并创建 PR
git push origin feature/your-feature
```

### 代码风格 / Code Style

- **C++17**，与现有代码风格一致（4 空格缩进、`snake_case` 命名）
- 输出插件实现 `IOutputSink` 接口 + 注册工厂（**禁止**新增全局状态）
- 插件 `send()` 不得长时间阻塞（慢消费者内部丢帧，参考 `tcp_h264_sink`）
- 错误处理：返回错误码 + `fprintf(stderr, ...)`，不抛异常
- 不引入新的第三方依赖（除非必要且讨论过）

## 🧩 新输出插件 / New Output Sink

1. 实现 `panorama_npu::IOutputSink`（`include/output_sink.h`）
2. 静态初始化期调用 `register_sink_factory("名字", 工厂)` 注册
3. 源文件加入 `CMakeLists.txt` 的 `panorama_npu_adapter`
4. README「输出插件指南」补充配置说明
5. 用 `null` sink 做性能对照，给出基准数据

## 📝 文档贡献 / Documentation

- README 保持**中英双语**结构
- 技术术语保留英文原文（DMA-BUF、MPP、RGA、RKNN 等）
- 推流协议变更必须同步 `README.md` 与 PC 接收端文档
- 文档中的命令必须可复制粘贴执行（含完整参数）

## 🔒 安全注意 / Security

- **绝不**提交：密钥、密码、内网 IP、个人路径（如 `/home/xxx`）
- 摄像头画面截图注意隐私

## 🙏 感谢 / Thanks

再次感谢你的贡献！问题响应时间通常 1~3 天。

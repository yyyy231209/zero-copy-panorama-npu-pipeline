# RK3588 全景 + NPU 完整链路集成 / Panorama + NPU Full-Pipeline Integration

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-RK3588-red.svg)](https://www.rock-chips.com)
[![Language](https://img.shields.io/badge/Language-C%2B%2B17-blue.svg)]()
[![CI](https://github.com/yyyy231209/zero-copy-panorama-npu-pipeline/actions/workflows/ci.yml/badge.svg)](https://github.com/yyyy231209/zero-copy-panorama-npu-pipeline/actions/workflows/ci.yml)

把两个上游模块串成一条完整的端到端管线：**四路全景拼接 → 三核 NPU 裂缝检测 → 输出插件（TCP H.264 推流）**，全程 DMA-BUF 零拷贝。

The integration layer that connects two upstream modules into one end-to-end pipeline: **four-camera panorama stitching → tri-core NPU crack detection → output sink plugin (TCP H.264 streaming)**, with DMA-BUF zero-copy all the way through.

---

## ✨ 技术亮点 / Highlights

| 亮点 | 说明 |
|---|---|
| 🔗 **全链路零拷贝串联** | 全景 BGR DMA-BUF → `submit_external()` 直接喂 NPU，无任何 CPU 图像拷贝 |
| 🧩 **输出插件化** | `IOutputSink` 抽象接口：TCP 推流 / 存文件 / 自定义协议，注册即用 |
| 📡 **TCP 裸 H.264 推流** | MPP 硬编码 + 多客户端（4 路）、SPS/PPS 补发、慢客户端不阻塞主链路 |
| 📊 **UDP 状态广播** | `FPS=xx.x, detected=n` 文本协议，PC 客户端实时显示板端状态 |
| 🛡️ **所有权契约桥接** | `PanoramaNpuAdapter` 用 ReleaseBridge 精确传递上游释放回调 |
| 📦 **A+B 双模式依赖** | A：路径变量指向已有仓库；B：`fetch_deps.sh` 自动拉取并构建 |

## 🏗️ 系统架构 / Architecture

### 数据流 / Data Flow

```text
┌──────────────────────────────────────────────────────────┐
│ 全景模块（上游仓库 zero-copy-vpu-gpu-rga-stitch）         │
│   4×USB 摄像头 → V4L2 → MPP 解码 → GPU warp → RGA 拼接    │
│   → 2248×330 BGR DMA-BUF（6 槽输出池）                    │
└─────────────────────┬────────────────────────────────────┘
                      │ PanoramaFrameRef（租赁式 + release callback）
                      ▼
┌──────────────────────────────────────────────────────────┐
│ PanoramaNpuAdapter（本仓库）                              │
│   ReleaseBridge：把上游释放回调桥接给 NPU 槽位            │
│   submit_external()：fd 零拷贝交接，所有权转移            │
└─────────────────────┬────────────────────────────────────┘
                      ▼
┌──────────────────────────────────────────────────────────┐
│ NPU 模块（上游仓库 zero-copy-tri-core-npu-inference）     │
│   RGA 打包 → 三核并行推理（CORE_0/1/2）→ RGA 画框         │
│   → 2248×1024 BGR 检测画布（6 槽 FrameSlot）              │
└─────────────────────┬────────────────────────────────────┘
                      │ take_ready() + frame_id 重排
                      ▼
┌──────────────────────────────────────────────────────────┐
│ IOutputSink 输出插件（本仓库，可替换）                     │
│   tcp_h264：RGA 裁剪 → MPP 硬编码 → TCP 多客户端推流       │
│             + UDP 状态广播                                │
│   null：丢弃（基准测试）                                  │
│   （自定义：实现 IOutputSink 注册即可）                    │
└─────────────────────┬────────────────────────────────────┘
                      │ TCP :5000 裸 H.264 + UDP :5003 状态
                      ▼
              PC 端（ffplay / Qt 监控客户端）
```

### 线程模型 / Thread Model

```text
主线程     Producer 线程               Consumer 线程
  │          │ 全景 acquire             │ take_ready
  │          │ → NPU submit             │ → frame_id 重排
  │          │ 循环                     │ → sink->send()
  │          ▼                          ▼
  │    NpuPool（内部 6 Worker 三核并行）
  │
  └── 统计输出（每 5 秒 FPS）
```

## 📁 文件解析 / File Guide

| 文件 | 作用 |
|---|---|
| `include/output_sink.h` | **输出插件接口**：`IOutputSink`（init/send/close/name）+ `OutputFrame` + 工厂注册（`create_sink`/`register_sink_factory`） |
| `src/output_sink.cpp` | 插件注册表实现（线程安全、名称冲突检测、`list_registered_sinks`） |
| `include/sinks/tcp_h264_sink.h` | TCP H.264 推流插件声明（PIMPL，MPP/RGA 头不进公共接口） |
| `src/sinks/tcp_h264_sink.cpp` | **TCP 推流插件实现**（838 行）：配置解析 → NV12 双缓冲 → RGA imcrop → MPP 硬编码 → 4 槽引用计数推流 → UDP 状态；含 `null` sink |
| `include/panorama_npu_adapter.h` | 全景→NPU 桥接声明：`SubmitStats` + `ReleaseBridge`（6 个桥，对应 NPU 槽位） |
| `src/panorama_npu_adapter.cpp` | **所有权桥接实现**：校验全景帧契约（2248×330/stride 2256）→ `submit_external` 移交 → release callback 归还上游槽 |
| `apps/panorama_npu_live.cpp` | **完整链路演示**：Producer（全景→NPU）+ Consumer（重排→插件）+ 5 秒统计 |
| `CMakeLists.txt` | 构建：依赖定位（A 路径变量 / B deps 目录）+ 静态库 + 演示程序 |
| `fetch_deps.sh` | **B 模式**：自动 clone 全景/NPU 仓库（v1.0.0 tag）到 `deps/` 并构建 |

## 🔧 构建与运行 / Build & Run

### 复刻条件 / Reproduction Requirements

| 组件 | 要求 |
|---|---|
| 开发板 | Rockchip RK3588（NPU 3 核 + MPP + Mali GPU + RGA） |
| 系统 | Debian/Ubuntu aarch64，内核 5.10+（dma-heap） |
| 编译 | GCC 10+ / CMake ≥ 3.10 / C++17 |
| 硬件库 | MPP（自编译官方源码）、RGA、RKNN Runtime、Mali OpenCL |
| 摄像头 | 4×USB（MJPG 1280×720@30），接同一 USB Hub |
| 模型 | YOLOv8s-P2 INT8（`models/best.rknn`，来自 NPU 仓库） |

### 依赖准备（二选一）/ Prepare Dependencies

**方案 A：路径变量**（已有两个仓库时）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DPANORAMA_ROOT=/path/to/zero-copy-vpu-gpu-rga-stitch \
  -DNPU_ROOT=/path/to/zero-copy-tri-core-npu-inference
```

**方案 B：自动拉取**（推荐，全新部署时）

```bash
./fetch_deps.sh        # clone v1.0.0 到 deps/ 并构建两个静态库
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

> 方案 B 要求两个依赖仓库都已完成构建（fetch_deps.sh 会代劳）；方案 A 请先
> 分别在两个仓库执行 `make all`（全景）与 `cmake --build build -j2`（NPU）。

### 编译 / Build

```bash
cmake --build build -j2
```

### 运行 / Run

```bash
# 基准测试（无输出，纯链路性能）
./build/panorama_npu_live models/best.rknn assets/open_chain_v1

# TCP H.264 推流（PC 端 ffplay 验证）
./build/panorama_npu_live models/best.rknn assets/open_chain_v1 \
  tcp_h264 "tcp_port=5000,fps=25,bps=4000000,crop_x=0,crop_y=280,crop_w=2248,crop_h=330,udp_stats_port=5003" 6
```

PC 端验证（同一局域网）：

```bash
ffplay -fflags nobuffer -flags low_delay -framedrop -f h264 tcp://<板端IP>:5000
```

## 🧩 输出插件指南 / Output Sink Plugin Guide

### 内置插件 / Built-in Sinks

| 名称 | 配置 | 说明 |
|---|---|---|
| `null` | （无） | 丢弃所有帧，用于链路性能基准 |
| `tcp_h264` | 见下表 | RGA 裁剪 → MPP H.264 → TCP 推流 + UDP 状态 |

### tcp_h264 配置项 / tcp_h264 Config

逗号分隔 `key=value`（全部可选）：

| 键 | 默认 | 说明 |
|---|---|---|
| `tcp_port` | 5000 | TCP 监听端口（裸 H.264 Annex-B） |
| `fps` | 25 | 编码帧率（MPP rc 参数） |
| `bps` | 4000000 | CBR 目标码率（bit/s） |
| `crop_x/y/w/h` | 0/280/2248/330 | 画布裁剪区（BGR 源坐标，全景有效区） |
| `udp_stats_port` | 5003 | UDP 有限广播状态端口；0 = 关闭 |

### 自定义插件 / Custom Sink

```cpp
// my_sink.cpp —— 实现接口并注册
#include "output_sink.h"

class MySink final : public panorama_npu::IOutputSink {
public:
    int init(const std::string &config) override { /* 解析配置 */ return 0; }
    int send(const panorama_npu::OutputFrame &frame) override {
        // frame.bgr_fd / width / height / stride / frame_id / detection_count
        return 0;
    }
    void close() noexcept override {}
    const char *name() const noexcept override { return "my_sink"; }
};

namespace {
struct Register {
    Register() {
        panorama_npu::register_sink_factory("my_sink", []() {
            return std::make_unique<MySink>();
        });
    }
} g_register;
}  // namespace
```

把 `my_sink.cpp` 加进 CMake 的 `panorama_npu_adapter` 源列表，运行时：
`./build/panorama_npu_live MODEL ASSETS_DIR my_sink "你的配置"`

## 📊 性能参考 / Performance Reference

本项目是串联层（不直接计算 NPU/GPU 耗时），性能主要由上游两个模块决定。本节给出本项目**推流链路**的设计目标与实际参数：

| 项 | 设计值 | 来源 |
|---|---|---|
| 全景输入帧率 | 25 FPS（可调到 60/120） | 摄像头 + 上游全景 |
| 全景输出帧率 | ≈ 23.6~25 FPS（实际业务） | PC 端 `FPS=xx.x` 字段 |
| 全景编码输出尺寸 | 2256×336（裁剪后 16 对齐） | TCP 5000 推流 |
| 编码码率 | 4 Mbps CBR | `bps=4000000` |
| GOP | 50 帧（≈ 2 秒） | MPP `rc:gop = fps*2` |
| TCP 推流并发客户端 | 上限 4 个 | kMaxClients = 4 |
| 每个客户端队列深度 | 4 帧（满则丢帧） | kQueueDepth = 4 |
| TCP 连接行为 | 新客户端补发 SPS/PPS，慢客户端不阻塞 | `cache_sps_pps` + 队列丢弃 |
| UDP 状态广播 | 每 10 帧刷新一次 | 集成端 FPS 计算 |
| 板端推理侧 | 30.82 FPS（Packed） / 10.7~10.8 FPS（Legacy） | 上游 NPU 模块 |
| 零拷贝收益 | 全景 fd → NPU 槽位：无 CPU memcpy | `submit_external` |

> **调整建议**：CPU 端（producer）期望帧率 < 25 时，把 `fps` 调小（如 15）可降低编码负担；`bps` 可按网络带宽在 2~6 Mbps 范围内调。

## 📡 推流协议 / Streaming Protocol

| 通道 | 协议 | 端口 | 内容 |
|---|---|---|---|
| 全景检测画面 | TCP Server | 5000 | 裸 H.264 Annex-B（裁剪后 **2256×336**，25 FPS，4 Mbps CBR，GOP 50） |
| 板端状态 | UDP 广播 | 5003 | `FPS=23.7, detected=0`（每 10 帧刷新，无结尾 NUL） |

**注意**：独立的单摄像头画面（1280×720@30，端口 5001）由独立服务 [zero-copy-v4l2-camera-stream](https://github.com/yyyy231209/zero-copy-v4l2-camera-stream) 推流，不在本项目范围内。

- TCP 最多 4 个并发客户端；新客户端接入补发 SPS/PPS
- 客户端**必须**用 H.264 parser（如 FFmpeg 的 `av_parser_parse2` + `AVCodecParserContext`），不能假设一次 `read` 是一帧
- PC 端用 FFmpeg 解码：`avcodec_find_decoder(AV_CODEC_ID_H264)` + `AV_CODEC_FLAG_LOW_DELAY`，像素转 `BGRA`/`RGB24` 给 Qt 显示
- UDP 有限广播 `255.255.255.255`，PC 绑定 `0.0.0.0:5003` + `ShareAddress`
- 状态文本正则：`^FPS=([0-9.]+),\s*detected=([0-9]+)$`

## 🔗 相关项目 / Related Projects

| 项目 | 关系 | 关键输出契约 |
|---|---|---|
| [zero-copy-vpu-gpu-rga-stitch](https://github.com/yyyy231209/zero-copy-vpu-gpu-rga-stitch) | 上游：全景拼接 | **2248×330 BGR888** DMA-BUF（stride 2256，画布 2248×1024 BGR888 含检测框） |
| [zero-copy-tri-core-npu-inference](https://github.com/yyyy231209/zero-copy-tri-core-npu-inference) | 上游：三核 NPU 检测 | 消费上游客景 fd，输出 2248×1024 BGR888 检测画布 |
| galaxy-monitor | 下游：PC 端 Qt 监控客户端（配套开源项目） | 按本文「推流协议」接收 TCP 5000 + UDP 5003 |

> **本项目的「输出契约」就是上游两者的拼接接口**：上游全景 `acquire()` 出 2248×330 BGR888，集成端 `submit_external()` 交给 NPU，NPU 输出 2248×1024 BGR888（含检测框），集成端 `take_ready()` 后按 `frame_id` 重排交给输出插件（默认 tcp_h264 推流 5000 端口）。

## ❓ 常见问题 / FAQ

**Q: 为什么不做成一个仓库？**
A: 全景/NPU 各自可独立部署（无 NPU 场景只看全景、换检测模型只动 NPU）；本仓库只做**串联层 + 输出层**，三个仓库职责清晰、版本独立。

**Q: 输出插件 send() 里能阻塞吗？**
A: 不能久阻塞。tcp_h264 用每客户端独立线程 + 4 帧队列，慢客户端丢帧而非拖慢主链路。自定义插件遵循同样原则。

**Q: 怎么接自己的协议（RTSP/WebRTC）？**
A: 实现 `IOutputSink`，把 `OutputFrame.bgr_fd` 交给你的编码/推流栈。RGA 裁剪参考 `tcp_h264_sink.cpp` 的 `send()`。

**Q: 全景/NPU 版本升级怎么办？**
A: 方案 A 改路径变量指向新版本；方案 B 改 `fetch_deps.sh` 的 `TAG` 后重跑。

## 📄 License

[MIT](LICENSE)

## 🙏 致谢 / Acknowledgements

- Rockchip MPP / RGA / RKNN Runtime
- 上游模块：全景拼接 + NPU 检测（同作者开源项目）

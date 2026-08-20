#pragma once

#include "output_sink.h"

#include <cstdint>
#include <string>

namespace panorama_npu {

// TCP 裸 H.264 推流输出插件（内置）。
//
// 配置格式：逗号分隔 key=value 文本（无 JSON 依赖），全部键可选：
//   tcp_port=5000         TCP 监听端口（PC 端 ffplay -f h264 tcp://<ip>:5000）
//   fps=25                编码帧率（也用作 MPP rc 参数）
//   bps=4000000           CBR 目标码率 (bit/s)
//   crop_x=0 crop_y=280 crop_w=2248 crop_h=330   画布裁剪区域（BGR 源坐标）
//   udp_stats_port=5003   可选：UDP 有限广播状态端口（FPS=<x>, detected=<n>）
//                         udp_stats_port=0 关闭状态广播
//
// 管线：BGR DMA-BUF -> RGA imcrop 转 NV12 -> MPP 硬编码 H.264 ->
//       TCP 多客户端推流（每客户端独立队列，慢客户端不阻塞主链路）。
//
// PC 端验证：
//   ffplay -fflags nobuffer -flags low_delay -framedrop -f h264 tcp://<板端IP>:5000
class TcpH264Sink final : public IOutputSink {
public:
    TcpH264Sink();
    ~TcpH264Sink() override;

    int init(const std::string &config) override;
    int send(const OutputFrame &frame) override;
    void close() noexcept override;
    const char *name() const noexcept override { return "tcp_h264"; }

private:
    struct Impl;
    Impl *impl_;   // PIMPL：MPP/RGA/TCP 头文件只出现在实现文件
};

}  // namespace panorama_npu

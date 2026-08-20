#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace panorama_npu {

// 输出帧：BGR888 DMA-BUF 画布 + 检测元数据。
// 由集成管线（全景 -> NPU）产出，交给注册的输出插件消费。
struct OutputFrame {
    int bgr_fd = -1;              // BGR888 DMA-BUF fd（画布，含检测框）
    int width = 0;                // 画布宽（如 2248）
    int height = 0;               // 画布高（如 1024）
    int stride = 0;               // 行 stride（像素）
    std::uint64_t frame_id = 0;   // 帧号（按到达顺序连续）
    std::size_t detection_count = 0;  // 该帧检测框数量
};

// 输出插件接口：集成管线只依赖此接口，具体输出方式（TCP 推流 /
// 存文件 / 自定义协议）由插件实现。插件不得阻塞发送调用过久；
// 内部自行处理慢速消费者（队列/丢帧策略）。
class IOutputSink {
public:
    virtual ~IOutputSink() = default;

    IOutputSink(const IOutputSink &) = delete;
    IOutputSink &operator=(const IOutputSink &) = delete;

    // 初始化。config 为插件自定义配置（通常 JSON 文本）。
    // 返回 0 成功；非 0 失败（调用方应终止启动）。
    virtual int init(const std::string &config) = 0;

    // 发送一帧。返回 0 成功（所有权未转移：调用方继续管理 fd）；
    // 非 0 失败。必须能在任意单线程顺序调用。
    virtual int send(const OutputFrame &frame) = 0;

    // 关闭并释放全部资源。可重复调用。
    virtual void close() noexcept = 0;

    // 插件名（注册键）。
    virtual const char *name() const noexcept = 0;

protected:
    IOutputSink() = default;
};

// 工厂函数类型：构造一个未初始化的插件实例。
using SinkFactory = std::unique_ptr<IOutputSink> (*)();

// 按名称创建插件（null/tcp_h264 内置，可扩展）。未注册返回 nullptr。
std::unique_ptr<IOutputSink> create_sink(const std::string &name);

// 注册自定义插件工厂（静态初始化期调用）。
// 返回 0 成功；名称冲突返回 -1。
int register_sink_factory(const char *name, SinkFactory factory);

// 列出全部已注册插件名（逗号分隔，日志用）。
std::string list_registered_sinks();

}  // namespace panorama_npu

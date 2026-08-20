#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "output_sink.h"
#include "panorama_npu_adapter.h"

namespace {

using Clock = std::chrono::steady_clock;
std::atomic<bool> g_stop{false};
void signal_handler(int) { g_stop.store(true); }

struct PendingFrame {
    std::uint64_t frame_id = 0;
    npu_detect::FrameSlot *slot = nullptr;
};

}  // namespace

// 全景 + NPU 完整链路演示：
//   PanoramaPipeline::acquire()  -> PanoramaNpuAdapter::submit()
//   -> NpuPool（三核并行推理 + RGA 画框）
//   -> take_ready() 按 frame_id 重排 -> IOutputSink 插件输出
//
// 用法:
//   panorama_npu_live MODEL ASSETS_DIR [SINK=null] [SINK_CONFIG] [WORKERS=6]
//
// 示例（TCP 推流给 PC 端 ffplay / 银河监视器客户端）:
//   panorama_npu_live models/best.rknn assets/open_chain_v1 \
//     tcp_h264 "tcp_port=5000,fps=25,bps=4000000,crop_x=0,crop_y=280,crop_w=2248,crop_h=330,udp_stats_port=5003" 6
int main(int argc, char **argv)
{
    using namespace npu_detect;
    if (argc < 3 || argc > 6) {
        std::cerr << "usage: " << argv[0]
                  << " MODEL ASSETS_DIR [SINK=null] [SINK_CONFIG] [WORKERS=6]\n";
        return 2;
    }
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    const char *model_path = argv[1];
    const char *assets_dir = argv[2];
    const std::string sink_name = (argc >= 4) ? argv[3] : "null";
    const std::string sink_config = (argc >= 5) ? argv[4] : "";

    PipelineConfig config{};
    if (argc >= 6)
        config.worker_count = static_cast<std::uint32_t>(std::stoul(argv[5]));
    if (config.validate() != ConfigError::kOk)
        return 3;

    std::cout << "Output sinks registered: "
              << panorama_npu::list_registered_sinks() << "\n";
    std::cout << "Using sink '" << sink_name << "' (config: '"
              << sink_config << "')\n";

    // 输出插件先初始化（失败即退出）
    auto sink = panorama_npu::create_sink(sink_name);
    if (!sink) {
        std::cerr << "unknown sink: " << sink_name << "\n";
        return 4;
    }
    if (sink->init(sink_config) != 0) {
        std::cerr << "sink init failed: " << sink_name << "\n";
        return 5;
    }

    std::cout << "Starting NPU pool (" << config.worker_count
              << " workers)...\n";
    NpuPool pool(config);
    if (pool.start(model_path) != 0) {
        std::cerr << "pool start failed\n";
        sink->close();
        return 6;
    }

    std::cout << "Starting panorama pipeline...\n";
    PanoramaPipeline panorama;
    if (!panorama.init(assets_dir) || !panorama.start()) {
        pool.shutdown();
        sink->close();
        return 7;
    }

    panorama_npu::PanoramaNpuAdapter adapter;

    std::deque<PendingFrame> pending_queue;
    std::mutex queue_mutex;
    std::uint64_t next_stream_id = 0;
    std::atomic<std::uint64_t> frames_out{0};
    std::atomic<int> producer_error{0};

    // Producer：全景 acquire -> NPU submit
    std::thread producer([&] {
        std::uint64_t frame_id = 0;
        std::uint64_t pano_timeouts = 0;
        while (!g_stop.load() && producer_error.load() == 0) {
            FrameSlot *slot = nullptr;
            if (!pool.acquire(slot)) {
                producer_error.store(7);
                break;
            }
            PanoramaFrameRef frame;
            if (!panorama.acquire(&frame, 2000)) {
                ++pano_timeouts;
                pool.cancel(slot);
                if (pano_timeouts > 1000) {
                    std::cerr << "[PROD] too many panorama timeouts\n";
                    producer_error.store(9);
                    break;
                }
                continue;
            }
            pano_timeouts = 0;
            if (adapter.submit(pool, slot, frame, frame_id) != 0) {
                std::cerr << "[PROD] submit failed frame_id=" << frame_id << "\n";
                producer_error.store(8);
                break;
            }
            ++frame_id;
        }
        pool.close_input();
        std::cerr << "[PROD] done frames=" << frame_id
                  << " err=" << producer_error.load() << "\n";
    });

    // Consumer：take_ready -> 按 frame_id 重排 -> 输出插件
    std::thread consumer([&] {
        while (!g_stop.load()) {
            FrameSlot *slot = nullptr;
            if (!pool.take_ready(slot)) {
                if (g_stop.load())
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                pending_queue.push_back({slot->frame_id, slot});
                while (!pending_queue.empty()) {
                    bool found = false;
                    for (auto it = pending_queue.begin();
                         it != pending_queue.end(); ++it) {
                        if (it->frame_id == next_stream_id) {
                            panorama_npu::OutputFrame out{};
                            out.bgr_fd = it->slot->canvas.fd;
                            out.width = static_cast<int>(it->slot->canvas.width);
                            out.height = static_cast<int>(it->slot->canvas.height);
                            out.stride =
                                static_cast<int>(it->slot->canvas.width_stride);
                            out.frame_id = it->frame_id;
                            out.detection_count =
                                it->slot->detections.size();
                            if (sink->send(out) == 0)
                                frames_out.fetch_add(1);
                            pool.release(it->slot);
                            pending_queue.erase(it);
                            ++next_stream_id;
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                        break;
                }
            }
        }
        pool.shutdown();
    });

    // 统计输出
    auto start_time = Clock::now();
    auto last_report = start_time;
    std::uint64_t last_frames = 0;
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        auto now = Clock::now();
        auto elapsed = std::chrono::duration<double>(now - last_report).count();
        auto frames = frames_out.load();
        double fps = (frames - last_frames) / (elapsed > 0 ? elapsed : 1.0);
        last_frames = frames;
        last_report = now;
        auto total_elapsed =
            std::chrono::duration<double>(now - start_time).count();
        std::cout << "[LIVE] frames=" << frames << " fps=" << std::fixed
                  << std::setprecision(1) << fps << " avg_fps="
                  << (total_elapsed > 0 ? frames / total_elapsed : 0.0)
                  << "      \r" << std::flush;

        if (producer_error.load() != 0 && frames > 2 && fps < 0.1) {
            std::cerr << "\n[MAIN] stuck detected, stopping...\n";
            g_stop.store(true);
        }
    }

    if (producer.joinable())
        producer.join();
    if (consumer.joinable())
        consumer.join();
    sink->close();
    panorama.stop();
    std::cout << "\nStopped. Total output frames: " << frames_out.load()
              << "\n";
    return 0;
}

#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "npu_pool.h"
#include "panorama_pipeline.h"

namespace panorama_npu {

struct SubmitStats {
    std::uint64_t submitted = 0;
    std::uint64_t submit_failures = 0;
    std::uint64_t released = 0;
    std::uint64_t callback_frame_mismatches = 0;
    std::uint64_t bridge_busy = 0;
    int last_submitted_fd = -1;
};

class PanoramaNpuAdapter {
public:
    PanoramaNpuAdapter();
    ~PanoramaNpuAdapter() = default;

    PanoramaNpuAdapter(const PanoramaNpuAdapter &) = delete;
    PanoramaNpuAdapter &operator=(const PanoramaNpuAdapter &) = delete;

    int submit(npu_detect::NpuPool &pool,
               npu_detect::FrameSlot *slot,
               PanoramaFrameRef &frame,
               std::uint64_t frame_id) noexcept;

    [[nodiscard]] SubmitStats stats() const noexcept;
    [[nodiscard]] bool idle() const noexcept;

private:
    struct ReleaseBridge {
        PanoramaNpuAdapter *owner = nullptr;
        PanoramaReleaseCallback upstream_callback = nullptr;
        void *upstream_context = nullptr;
        std::uint64_t expected_frame_id = 0;
        std::atomic<bool> in_use{false};
    };

    static void release_bridge(void *context,
                               std::uint64_t frame_id) noexcept;

    std::array<ReleaseBridge, npu_detect::NpuPool::kMaxWorkerCount> bridges_;
    std::atomic<std::uint64_t> submitted_{0};
    std::atomic<std::uint64_t> submit_failures_{0};
    std::atomic<std::uint64_t> released_{0};
    std::atomic<std::uint64_t> callback_frame_mismatches_{0};
    std::atomic<std::uint64_t> bridge_busy_{0};
    std::atomic<int> last_submitted_fd_{-1};
};

}  // namespace panorama_npu
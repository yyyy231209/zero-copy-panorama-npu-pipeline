#include "panorama_npu_adapter.h"

#include <cerrno>
#include <cstddef>

namespace panorama_npu {

PanoramaNpuAdapter::PanoramaNpuAdapter()
{
    for (auto &bridge : bridges_)
        bridge.owner = this;
}

int PanoramaNpuAdapter::submit(npu_detect::NpuPool &pool,
                               npu_detect::FrameSlot *slot,
                               PanoramaFrameRef &frame,
                               std::uint64_t frame_id) noexcept
{
    constexpr std::size_t kExpectedBytes =
        static_cast<std::size_t>(2256) * 330U * 3U;
    if (!slot || slot->slot_id >= bridges_.size() || !frame.valid() ||
        frame.dma_fd() < 0 || frame.width() != 2248 ||
        frame.height() != 330 || frame.stride() != 2256 ||
        frame.bytes() < kExpectedBytes || !frame.release_callback() ||
        !frame.release_context()) {
        ++submit_failures_;
        return -EINVAL;
    }

    ReleaseBridge &bridge = bridges_[slot->slot_id];
    bool expected = false;
    if (!bridge.in_use.compare_exchange_strong(expected, true)) {
        ++bridge_busy_;
        ++submit_failures_;
        return -EBUSY;
    }
    bridge.upstream_callback = frame.release_callback();
    bridge.upstream_context = frame.release_context();
    bridge.expected_frame_id = frame_id;

    const npu_detect::DmaImageView source{
        frame.dma_fd(),
        nullptr,
        frame.bytes(),
        static_cast<std::uint32_t>(frame.width()),
        static_cast<std::uint32_t>(frame.height()),
        static_cast<std::uint32_t>(frame.stride()),
        static_cast<std::uint32_t>(frame.height()),
        npu_detect::PixelFormat::kBgr888,
    };
    const int result = pool.submit_external(
        slot, source, frame_id, frame.timestamp_ns(),
        &PanoramaNpuAdapter::release_bridge, &bridge);
    if (result != 0) {
        bridge.upstream_callback = nullptr;
        bridge.upstream_context = nullptr;
        bridge.expected_frame_id = 0;
        bridge.in_use.store(false);
        ++submit_failures_;
        return result;
    }

    last_submitted_fd_.store(frame.dma_fd());
    frame.detach();
    ++submitted_;
    return 0;
}

void PanoramaNpuAdapter::release_bridge(void *context,
                                        std::uint64_t frame_id) noexcept
{
    auto *bridge = static_cast<ReleaseBridge *>(context);
    if (!bridge || !bridge->owner)
        return;
    PanoramaNpuAdapter *owner = bridge->owner;
    if (!bridge->in_use.load()) {
        ++owner->callback_frame_mismatches_;
        return;
    }
    if (frame_id != bridge->expected_frame_id)
        ++owner->callback_frame_mismatches_;

    const PanoramaReleaseCallback callback = bridge->upstream_callback;
    void *upstream_context = bridge->upstream_context;
    bridge->upstream_callback = nullptr;
    bridge->upstream_context = nullptr;
    bridge->expected_frame_id = 0;
    bridge->in_use.store(false);
    ++owner->released_;
    if (callback)
        callback(upstream_context, frame_id);
    else
        ++owner->callback_frame_mismatches_;
}

SubmitStats PanoramaNpuAdapter::stats() const noexcept
{
    return {
        submitted_.load(),
        submit_failures_.load(),
        released_.load(),
        callback_frame_mismatches_.load(),
        bridge_busy_.load(),
        last_submitted_fd_.load(),
    };
}

bool PanoramaNpuAdapter::idle() const noexcept
{
    for (const auto &bridge : bridges_) {
        if (bridge.in_use.load())
            return false;
    }
    return true;
}

}  // namespace panorama_npu
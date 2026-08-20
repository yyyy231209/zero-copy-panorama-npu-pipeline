#include "sinks/tcp_h264_sink.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/poll.h>          // pollfd, POLLIN/POLLHUP/POLLERR

#include <im2d.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>

namespace panorama_npu {
namespace {

#define MPP_ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

// ---------- 极简 key=value 配置解析 ----------
struct SinkConfig {
    int tcp_port = 5000;
    int fps = 25;
    int bps = 4000000;
    int crop_x = 0;
    int crop_y = 280;
    int crop_w = 2248;
    int crop_h = 330;
    int udp_stats_port = 5003;
};

bool parse_config(const std::string &text, SinkConfig &out)
{
    std::string rest = text;
    std::string::size_type pos = 0;
    while (pos <= rest.size()) {
        std::string::size_type comma = rest.find(',', pos);
        std::string item = (comma == std::string::npos)
                               ? rest.substr(pos)
                               : rest.substr(pos, comma - pos);
        if (!item.empty()) {
            std::string::size_type eq = item.find('=');
            if (eq == std::string::npos)
                return false;
            const std::string key = item.substr(0, eq);
            const std::string value = item.substr(eq + 1);
            try {
                const long v = std::stol(value);
                if (key == "tcp_port") out.tcp_port = static_cast<int>(v);
                else if (key == "fps") out.fps = static_cast<int>(v);
                else if (key == "bps") out.bps = static_cast<int>(v);
                else if (key == "crop_x") out.crop_x = static_cast<int>(v);
                else if (key == "crop_y") out.crop_y = static_cast<int>(v);
                else if (key == "crop_w") out.crop_w = static_cast<int>(v);
                else if (key == "crop_h") out.crop_h = static_cast<int>(v);
                else if (key == "udp_stats_port") out.udp_stats_port = static_cast<int>(v);
                else return false;  // 未知键
            } catch (...) {
                return false;
            }
        }
        if (comma == std::string::npos)
            break;
        pos = comma + 1;
    }
    return true;
}

// ---------- DMA-heap 分配（与全景项目相同的 cma-uncached 堆） ----------
struct DmaHeapAllocationData {
    unsigned long long len;
    unsigned int fd;
    unsigned int fd_flags;
    unsigned long long heap_flags;
};

#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOCTL_ALLOC \
    _IOWR(DMA_HEAP_IOC_MAGIC, 0x0, DmaHeapAllocationData)

int dma_buf_alloc(std::size_t size, int *out_fd, void **out_va)
{
    const char *path = "/dev/dma_heap/cma-uncached";
    int heap_fd = open(path, O_RDWR | O_CLOEXEC);
    if (heap_fd < 0) {
        std::fprintf(stderr, "tcp_h264_sink: open %s failed\n", path);
        return -1;
    }
    DmaHeapAllocationData data{};
    data.len = size;
    data.fd_flags = O_CLOEXEC | O_RDWR;
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) < 0) {
        std::fprintf(stderr, "tcp_h264_sink: DMA_HEAP_IOCTL_ALLOC failed: %s\n",
                     std::strerror(errno));
        close(heap_fd);
        return -1;
    }
    close(heap_fd);
    void *va = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    static_cast<int>(data.fd), 0);
    if (va == MAP_FAILED) {
        std::fprintf(stderr, "tcp_h264_sink: mmap failed: %s\n",
                     std::strerror(errno));
        close(static_cast<int>(data.fd));
        return -1;
    }
    *out_fd = static_cast<int>(data.fd);
    *out_va = va;
    return 0;
}

void dma_buf_free(std::size_t size, int *fd, void *va)
{
    if (va && va != MAP_FAILED)
        munmap(va, size);
    if (fd && *fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

// ---------- MPP H.264 编码（原 mpp_enc.cpp 改造为类内状态） ----------
class MppEncoder {
public:
    int init(int width, int height, int fps, int bps)
    {
        if (mpp_create(&ctx_, &mpi_) != MPP_OK) {
            std::fprintf(stderr, "tcp_h264_sink: mpp_create failed\n");
            return -1;
        }
        if (mpp_init(ctx_, MPP_CTX_ENC, MPP_VIDEO_CodingAVC) != MPP_OK) {
            std::fprintf(stderr, "tcp_h264_sink: mpp_init failed\n");
            close();
            return -1;
        }

        RK_S64 timeout = 0;
        mpi_->control(ctx_, MPP_SET_OUTPUT_TIMEOUT, &timeout);

        w_ = width;
        h_ = height;
        hstride_ = MPP_ALIGN(width, 16);
        vstride_ = MPP_ALIGN(height, 16);

        if (mpp_enc_cfg_init(&cfg_) != MPP_OK ||
            mpi_->control(ctx_, MPP_ENC_GET_CFG, cfg_) != MPP_OK) {
            std::fprintf(stderr, "tcp_h264_sink: mpp cfg init failed\n");
            close();
            return -1;
        }

        mpp_enc_cfg_set_s32(cfg_, "prep:width", w_);
        mpp_enc_cfg_set_s32(cfg_, "prep:height", h_);
        mpp_enc_cfg_set_s32(cfg_, "prep:hor_stride", hstride_);
        mpp_enc_cfg_set_s32(cfg_, "prep:ver_stride", vstride_);
        mpp_enc_cfg_set_s32(cfg_, "prep:format", MPP_FMT_YUV420SP);
        mpp_enc_cfg_set_s32(cfg_, "prep:range", MPP_FRAME_RANGE_JPEG);
        mpp_enc_cfg_set_s32(cfg_, "codec:type", MPP_VIDEO_CodingAVC);

        mpp_enc_cfg_set_s32(cfg_, "rc:mode", MPP_ENC_RC_MODE_CBR);
        mpp_enc_cfg_set_s32(cfg_, "rc:bps_target", bps);
        mpp_enc_cfg_set_s32(cfg_, "rc:bps_max", bps * 17 / 16);
        mpp_enc_cfg_set_s32(cfg_, "rc:bps_min", bps * 15 / 16);
        mpp_enc_cfg_set_s32(cfg_, "rc:drop_mode", MPP_ENC_RC_DROP_FRM_DISABLED);

        mpp_enc_cfg_set_s32(cfg_, "rc:fps_in_flex", 0);
        mpp_enc_cfg_set_s32(cfg_, "rc:fps_in_num", fps);
        mpp_enc_cfg_set_s32(cfg_, "rc:fps_in_denom", 1);
        mpp_enc_cfg_set_s32(cfg_, "rc:fps_out_flex", 0);
        mpp_enc_cfg_set_s32(cfg_, "rc:fps_out_num", fps);
        mpp_enc_cfg_set_s32(cfg_, "rc:fps_out_denom", 1);
        mpp_enc_cfg_set_s32(cfg_, "rc:gop", fps * 2);

        mpp_enc_cfg_set_s32(cfg_, "rc:qp_init", -1);
        mpp_enc_cfg_set_s32(cfg_, "rc:qp_max", 51);
        mpp_enc_cfg_set_s32(cfg_, "rc:qp_min", 10);
        mpp_enc_cfg_set_s32(cfg_, "rc:qp_max_i", 51);
        mpp_enc_cfg_set_s32(cfg_, "rc:qp_min_i", 10);
        mpp_enc_cfg_set_s32(cfg_, "rc:qp_ip", 2);

        mpp_enc_cfg_set_s32(cfg_, "h264:profile", 100);
        mpp_enc_cfg_set_s32(cfg_, "h264:level", 40);
        mpp_enc_cfg_set_s32(cfg_, "h264:cabac_en", 1);
        mpp_enc_cfg_set_s32(cfg_, "h264:cabac_idc", 0);
        mpp_enc_cfg_set_s32(cfg_, "h264:trans8x8", 1);

        if (mpi_->control(ctx_, MPP_ENC_SET_CFG, cfg_) != MPP_OK) {
            std::fprintf(stderr, "tcp_h264_sink: MPP_ENC_SET_CFG failed\n");
            close();
            return -1;
        }

        RK_S32 header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
        if (mpi_->control(ctx_, MPP_ENC_SET_HEADER_MODE, &header_mode) != MPP_OK) {
            std::fprintf(stderr, "tcp_h264_sink: SET_HEADER_MODE failed\n");
            close();
            return -1;
        }

        mpp_frame_init(&frame_);
        mpp_frame_set_width(frame_, w_);
        mpp_frame_set_height(frame_, h_);
        mpp_frame_set_hor_stride(frame_, hstride_);
        mpp_frame_set_ver_stride(frame_, vstride_);
        mpp_frame_set_fmt(frame_, MPP_FMT_YUV420SP);
        return 0;
    }

    int encode(MppBuffer in_buf)
    {
        if (!ctx_ || !in_buf)
            return -1;
        mpp_frame_set_buffer(frame_, in_buf);
        mpp_frame_set_eos(frame_, 0);
        MPP_RET ret = mpi_->encode_put_frame(ctx_, frame_);
        if (ret != MPP_OK) {
            std::fprintf(stderr, "tcp_h264_sink: encode_put_frame failed %d\n", ret);
            return -1;
        }
        first_packet_ = 1;
        return 0;
    }

    MppPacket take_packet()
    {
        MppPacket pkt = nullptr;
        if (first_packet_) {
            first_packet_ = 0;
            for (int i = 0; i < 40; ++i) {
                MPP_RET ret = mpi_->encode_get_packet(ctx_, &pkt);
                if (ret == MPP_OK && pkt)
                    return pkt;
                usleep(5000);
            }
            return nullptr;
        }
        MPP_RET ret = mpi_->encode_get_packet(ctx_, &pkt);
        return (ret == MPP_OK && pkt) ? pkt : nullptr;
    }

    void close()
    {
        if (frame_) {
            mpp_frame_deinit(&frame_);
            frame_ = nullptr;
        }
        if (cfg_) {
            mpp_enc_cfg_deinit(cfg_);
            cfg_ = nullptr;
        }
        if (ctx_) {
            mpp_destroy(ctx_);
            ctx_ = nullptr;
            mpi_ = nullptr;
        }
    }

private:
    MppApi *mpi_ = nullptr;
    MppCtx ctx_ = nullptr;
    MppEncCfg cfg_ = nullptr;
    MppFrame frame_ = nullptr;
    int w_ = 0, h_ = 0;
    int hstride_ = 0, vstride_ = 0;
    int first_packet_ = 0;
};

// ---------- TCP 多客户端推流（原 tcp_streamer.cpp 改造为类内状态） ----------
// 零拷贝：MppPacket 放入 4 槽引用计数池，每个客户端线程按索引取槽发送，
// 引用归零才 deinit。新客户端接入先发缓存的 SPS/PPS 前缀（ffmpeg 参数集）。
class TcpStreamer {
public:
    static constexpr int kSlots = 4;
    static constexpr int kQueueDepth = 4;
    static constexpr int kMaxClients = 4;

    int init(int port)
    {
        if (port <= 0)
            port = 5000;
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0)
            return -1;
        int one = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(static_cast<std::uint16_t>(port));
        if (bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr),
                 sizeof(addr)) < 0) {
            std::perror("tcp_h264_sink: tcp bind");
            ::close(listen_fd_);
            listen_fd_ = -1;
            return -1;
        }
        if (listen(listen_fd_, 4) < 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return -1;
        }
        std::fprintf(stderr, "tcp_h264_sink: listening 0.0.0.0:%d\n", port);
        running_.store(true);
        listener_tid_ = std::thread(&TcpStreamer::listener_thread, this);
        return 0;
    }

    // 推入一帧编码输出。返回 0：已接管（调用方不得 deinit）；
    // 返回 -1：未接管（调用方自行 deinit）。
    int push(MppPacket pkt)
    {
        const auto *data =
            static_cast<const std::uint8_t *>(mpp_packet_get_pos(pkt));
        const std::size_t len = mpp_packet_get_length(pkt);
        if (!running_.load() || !data || len == 0)
            return -1;

        std::lock_guard<std::mutex> lock(lock_);
        cache_sps_pps(pkt);
        Slot *slot = nullptr;
        for (int i = 0; i < kSlots; ++i) {
            if (slots_[i].refs == 0 && slots_[i].pkt == nullptr) {
                slot = &slots_[i];
                break;
            }
        }
        if (!slot)
            return -1;

        slot->pkt = pkt;
        slot->len = len;
        for (int i = 0; i < client_count_; ++i) {
            Client *c = clients_[i];
            const int next = (c->qtail + 1) % kQueueDepth;
            if (next == c->qhead)
                continue;  // 队列满：跳过该客户端（丢帧不阻塞）
            c->slot_q[c->qtail] = static_cast<int>(slot - slots_);
            c->qtail = next;
            ++slot->refs;
        }
        if (slot->refs == 0) {
            mpp_packet_deinit(&pkt);
            slot->pkt = nullptr;
        }
        return 0;
    }

    void close()
    {
        running_.store(false);
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        // 锁内只 shutdown + 收集指针；join 必须在锁外
        // （client_thread 退出路径需要拿 lock_，锁内 join 会死锁）
        std::vector<Client *> to_join;
        {
            std::lock_guard<std::mutex> lock(lock_);
            for (int i = 0; i < client_count_; ++i) {
                Client *c = clients_[i];
                if (c && c->sock >= 0)
                    ::shutdown(c->sock, SHUT_RDWR);
                to_join.push_back(c);
            }
        }
        // 1. listener 退出（其 reap 逻辑回收已退出的客户端）
        if (listener_tid_.joinable())
            listener_tid_.join();
        // 2. 锁外 join 剩余客户端线程（退出时自移出数组、自入 garbage）
        for (Client *c : to_join) {
            if (c && c->tid.joinable())
                c->tid.join();
        }
        // 3. 收割 garbage（锁外 join + 删除）
        reap_garbage();
        // 4. 全部线程已退出：锁内清理数组 + 剩余槽引用
        {
            std::lock_guard<std::mutex> lock(lock_);
            for (int i = 0; i < client_count_; ++i) {
                Client *c = clients_[i];
                while (c && c->qhead != c->qtail) {
                    const int sidx = c->slot_q[c->qhead];
                    c->qhead = (c->qhead + 1) % kQueueDepth;
                    release_slot_ref(sidx);
                }
                clients_[i] = nullptr;
            }
            client_count_ = 0;
        }
    }

private:
    struct Slot {
        MppPacket pkt = nullptr;
        std::size_t len = 0;
        int refs = 0;
    };

    struct Client {
        int sock = -1;
        bool dead = false;
        bool prepend_done = false;
        int slot_q[kQueueDepth] = {};
        int qhead = 0;
        int qtail = 0;
        std::thread tid;
    };

    void release_slot_ref(int sidx)
    {
        Slot &slot = slots_[sidx];
        if (--slot.refs <= 0 && slot.pkt) {
            mpp_packet_deinit(&slot.pkt);
            slot.pkt = nullptr;
            slot.refs = 0;
        }
    }

    // 从 H.264 packet 提取 SPS/PPS，合成 Annex-B 前缀（缓存一次）。
    void cache_sps_pps(MppPacket pkt)
    {
        if (prepend_len_ > 0)
            return;
        const auto *data =
            static_cast<const std::uint8_t *>(mpp_packet_get_pos(pkt));
        const std::size_t len = mpp_packet_get_length(pkt);
        if (!data || len < 8)
            return;

        std::size_t pos = 0;
        while (pos + 3 <= len) {
            if (data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 1)
                break;
            ++pos;
        }
        if (pos + 3 > len)
            return;

        const std::uint8_t *sps = nullptr;
        const std::uint8_t *pps = nullptr;
        std::size_t sps_len = 0;
        std::size_t pps_len = 0;
        std::size_t content = pos + 3;

        while (content + 3 <= len) {
            std::size_t code = static_cast<std::size_t>(-1);
            int sc_len = 3;
            for (std::size_t j = content; j + 4 <= len; ++j) {
                if (data[j] == 0 && data[j + 1] == 0 && data[j + 2] == 0 &&
                    data[j + 3] == 1) {
                    code = j;
                    sc_len = 4;
                    break;
                }
            }
            if (code == static_cast<std::size_t>(-1)) {
                for (std::size_t j = content; j + 3 <= len; ++j) {
                    if (data[j] == 0 && data[j + 1] == 0 && data[j + 2] == 1) {
                        code = j;
                        sc_len = 3;
                        break;
                    }
                }
            }
            if (code == static_cast<std::size_t>(-1))
                break;
            if (code > content) {
                const std::uint8_t type = data[content] & 0x1f;
                const std::size_t nlen = code - content;
                if (type == 7 && nlen < 128) {
                    sps = data + content;
                    sps_len = nlen;
                } else if (type == 8 && nlen < 128) {
                    pps = data + content;
                    pps_len = nlen;
                }
            }
            content = code + static_cast<std::size_t>(sc_len);
            if (sps && pps)
                break;
        }

        if (sps && pps && sps_len + pps_len + 8 <= sizeof(prepend_)) {
            std::size_t o = 0;
            prepend_[o++] = 0;
            prepend_[o++] = 0;
            prepend_[o++] = 0;
            prepend_[o++] = 1;
            std::memcpy(prepend_ + o, sps, sps_len);
            o += sps_len;
            prepend_[o++] = 0;
            prepend_[o++] = 0;
            prepend_[o++] = 0;
            prepend_[o++] = 1;
            std::memcpy(prepend_ + o, pps, pps_len);
            o += pps_len;
            prepend_len_ = o;
        }
    }

    static int send_all(int fd, const std::uint8_t *data, std::size_t len)
    {
        std::size_t off = 0;
        while (off < len) {
            const ssize_t w = send(fd, data + off, len - off, MSG_NOSIGNAL);
            if (w <= 0)
                return -1;
            off += static_cast<std::size_t>(w);
        }
        return 0;
    }

    void client_thread(Client *c)
    {
        for (;;) {
            pollfd pfd{};
            pfd.fd = c->sock;
            pfd.events = POLLIN | POLLHUP | POLLERR;
            const int pr = poll(&pfd, 1, 100);
            if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
                c->dead = true;
                break;
            }
            if (c->dead)
                break;

            // 先发 SPS/PPS 前缀
            if (!c->prepend_done) {
                std::size_t plen = 0;
                {
                    std::lock_guard<std::mutex> lock(lock_);
                    plen = prepend_len_;
                }
                if (plen > 0) {
                    std::uint8_t buf[256];
                    {
                        std::lock_guard<std::mutex> lock(lock_);
                        std::memcpy(buf, prepend_, plen);
                    }
                    if (send_all(c->sock, buf, plen) < 0) {
                        c->dead = true;
                        break;
                    }
                    c->prepend_done = true;
                }
            }

            // 取队列并发送（锁外发送）
            for (;;) {
                int sidx = -1;
                {
                    std::lock_guard<std::mutex> lock(lock_);
                    if (c->qhead != c->qtail) {
                        sidx = c->slot_q[c->qhead];
                        c->qhead = (c->qhead + 1) % kQueueDepth;
                    }
                }
                if (sidx < 0)
                    break;
                Slot *slot = &slots_[sidx];
                if (send_all(c->sock,
                             static_cast<const std::uint8_t *>(
                                 mpp_packet_get_pos(slot->pkt)),
                             slot->len) < 0)
                    c->dead = true;
                {
                    std::lock_guard<std::mutex> lock(lock_);
                    release_slot_ref(sidx);
                }
                if (c->dead)
                    break;
            }
            if (c->dead)
                break;
        }

        std::lock_guard<std::mutex> lock(lock_);
        for (int i = 0; i < client_count_; ++i) {
            if (clients_[i] == c) {
                clients_[i] = clients_[client_count_ - 1];
                clients_[client_count_ - 1] = c;
                --client_count_;
                break;
            }
        }
        // 归还未消费引用
        while (c->qhead != c->qtail) {
            const int sidx = c->slot_q[c->qhead];
            c->qhead = (c->qhead + 1) % kQueueDepth;
            release_slot_ref(sidx);
        }
        ::close(c->sock);
        c->sock = -1;
        // 交给 listener/close 收割：这里只入 garbage，不 join 不 delete。
        // garbage 中的线程已完成全部锁操作，收割方锁外 join 是安全的。
        {
            std::lock_guard<std::mutex> lock(lock_);
            garbage_.push_back(c);
        }
    }

    // listener 与 close() 调用：锁外 join 已退出客户端线程并释放内存
    void reap_garbage()
    {
        std::vector<Client *> candidates;
        {
            std::lock_guard<std::mutex> lock(lock_);
            candidates = garbage_;
        }
        std::vector<Client *> joined;
        for (Client *c : candidates) {
            if (c && c->tid.joinable()) {
                c->tid.join();
                joined.push_back(c);
            }
        }
        if (!joined.empty()) {
            std::lock_guard<std::mutex> lock(lock_);
            for (Client *c : joined) {
                auto it = std::find(garbage_.begin(), garbage_.end(), c);
                if (it != garbage_.end())
                    garbage_.erase(it);
            }
        }
        for (Client *c : joined)
            delete c;
    }

    void listener_thread()
    {
        while (running_.load()) {
            reap_garbage();
            pollfd pfd{};
            pfd.fd = listen_fd_;
            pfd.events = POLLIN;
            if (poll(&pfd, 1, 200) <= 0)
                continue;

            const int cfd = accept(listen_fd_, nullptr, nullptr);
            if (cfd < 0)
                continue;
            int one = 1;
            setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            timeval tv{};
            tv.tv_sec = 2;
            tv.tv_usec = 0;
            setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

            Client *c = new Client();
            c->sock = cfd;
            {
                std::lock_guard<std::mutex> lock(lock_);
                if (client_count_ >= kMaxClients) {
                    delete c;
                    ::close(cfd);
                    continue;
                }
                clients_[client_count_++] = c;
            }
            std::fprintf(stderr, "tcp_h264_sink: client connected fd=%d\n", cfd);
            c->tid = std::thread(&TcpStreamer::client_thread, this, c);
        }
        reap_garbage();
    }

    Slot slots_[kSlots];
    Client *clients_[kMaxClients] = {};
    int client_count_ = 0;
    std::vector<Client *> garbage_;
    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread listener_tid_;
    std::mutex lock_;
    std::uint8_t prepend_[256] = {};
    std::size_t prepend_len_ = 0;
};

// ---------- UDP 状态广播 ----------
class UdpStats {
public:
    int init(int port)
    {
        if (port <= 0)
            return 0;  // 关闭状态广播
        sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ < 0)
            return -1;
        int one = 1;
        setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        int bc = 1;
        setsockopt(sock_, SOL_SOCKET, SO_BROADCAST, &bc, sizeof(bc));
        addr_{};
        addr_.sin_family = AF_INET;
        addr_.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        addr_.sin_port = htons(static_cast<std::uint16_t>(port));
        return 0;
    }

    void send_text(const char *msg)
    {
        if (sock_ < 0 || !msg)
            return;
        sendto(sock_, msg, std::strlen(msg), 0,
               reinterpret_cast<sockaddr *>(&addr_), sizeof(addr_));
    }

    void close()
    {
        if (sock_ >= 0) {
            ::close(sock_);
            sock_ = -1;
        }
    }

private:
    int sock_ = -1;
    sockaddr_in addr_{};
};

}  // namespace

// ---------- TcpH264Sink::Impl ----------
struct TcpH264Sink::Impl {
    SinkConfig cfg;
    MppEncoder enc;
    TcpStreamer streamer;
    UdpStats stats;

    // NV12 双缓冲（RGA imcrop 输出）
    static constexpr int kPoolCount = 2;
    struct Nv12Buf {
        int fd = -1;
        void *va = nullptr;
        std::size_t size = 0;
    };
    Nv12Buf pool[kPoolCount];
    int pool_idx = 0;

    int hstride = 0, vstride = 0;
    bool inited = false;

    // UDP 文本统计（每 10 帧刷新）
    std::atomic<std::uint64_t> frames_sent{0};
    std::uint64_t last_udp_frames = 0;
    std::chrono::steady_clock::time_point last_udp_time{};

    ~Impl() = default;
};

TcpH264Sink::TcpH264Sink() : impl_(new Impl()) {}

TcpH264Sink::~TcpH264Sink()
{
    close();
    delete impl_;
}

int TcpH264Sink::init(const std::string &config)
{
    if (impl_->inited)
        return -1;
    if (!parse_config(config, impl_->cfg)) {
        std::fprintf(stderr, "tcp_h264_sink: bad config '%s'\n", config.c_str());
        return -1;
    }
    const SinkConfig &c = impl_->cfg;
    if (c.crop_w <= 0 || c.crop_h <= 0 || c.tcp_port <= 0) {
        std::fprintf(stderr, "tcp_h264_sink: invalid crop/port\n");
        return -1;
    }

    impl_->hstride = MPP_ALIGN(c.crop_w, 16);
    impl_->vstride = MPP_ALIGN(c.crop_h, 16);
    const std::size_t nv12_size =
        static_cast<std::size_t>(impl_->hstride) * impl_->vstride * 3U / 2U;

    for (auto &buf : impl_->pool) {
        if (dma_buf_alloc(nv12_size, &buf.fd, &buf.va) != 0) {
            close();
            return -1;
        }
        buf.size = nv12_size;
    }

    if (impl_->enc.init(impl_->hstride, impl_->vstride, c.fps, c.bps) != 0) {
        close();
        return -1;
    }
    if (impl_->streamer.init(c.tcp_port) != 0) {
        close();
        return -1;
    }
    if (c.udp_stats_port > 0 && impl_->stats.init(c.udp_stats_port) != 0) {
        close();
        return -1;
    }

    impl_->last_udp_time = std::chrono::steady_clock::now();
    impl_->inited = true;
    return 0;
}

int TcpH264Sink::send(const OutputFrame &frame)
{
    if (!impl_->inited || frame.bgr_fd < 0)
        return -1;
    const SinkConfig &c = impl_->cfg;

    Impl::Nv12Buf &buf = impl_->pool[impl_->pool_idx];
    impl_->pool_idx = (impl_->pool_idx + 1) % Impl::kPoolCount;

    rga_buffer_t src = wrapbuffer_fd_t(frame.bgr_fd, frame.width, frame.height,
                                       frame.stride, frame.height,
                                       RK_FORMAT_BGR_888);
    rga_buffer_t dst = wrapbuffer_fd_t(buf.fd, c.crop_w, c.crop_h,
                                       impl_->hstride, impl_->vstride,
                                       RK_FORMAT_YCbCr_420_SP);
    im_rect crop_rect{c.crop_x, c.crop_y, c.crop_w, c.crop_h};
    IM_STATUS st = imcrop(src, dst, crop_rect);
    if (st != IM_STATUS_SUCCESS) {
        std::fprintf(stderr, "tcp_h264_sink: imcrop failed st=%d\n", st);
        return -1;
    }

    MppBufferInfo info{};
    info.type = MPP_BUFFER_TYPE_EXT_DMA;
    info.fd = buf.fd;
    info.size = buf.size;
    MppBuffer nv12_buf = nullptr;
    if (mpp_buffer_import(&nv12_buf, &info) != MPP_OK) {
        std::fprintf(stderr, "tcp_h264_sink: mpp_buffer_import failed\n");
        return -1;
    }

    if (impl_->enc.encode(nv12_buf) != 0) {
        mpp_buffer_put(nv12_buf);
        return -1;
    }

    MppPacket pkt;
    while ((pkt = impl_->enc.take_packet()) != nullptr) {
        impl_->streamer.push(pkt);  // push 内部 deinit
    }
    mpp_buffer_put(nv12_buf);

    // UDP 文本状态：每 10 帧重算一次 FPS
    impl_->frames_sent.fetch_add(1);
    const std::uint64_t total = impl_->frames_sent.load();
    if (total - impl_->last_udp_frames >= 10) {
        const auto now = std::chrono::steady_clock::now();
        const double secs =
            std::chrono::duration<double>(now - impl_->last_udp_time).count();
        const double fps = (secs > 0)
                               ? static_cast<double>(total - impl_->last_udp_frames) / secs
                               : 0.0;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "FPS=%.1f, detected=%zu",
                      fps, frame.detection_count);
        impl_->stats.send_text(buf);
        impl_->last_udp_frames = total;
        impl_->last_udp_time = now;
    }
    return 0;
}

void TcpH264Sink::close() noexcept
{
    if (!impl_->inited)
        return;
    impl_->streamer.close();
    impl_->enc.close();
    impl_->stats.close();
    for (auto &buf : impl_->pool)
        dma_buf_free(buf.size, &buf.fd, buf.va);
    impl_->inited = false;
}

}  // namespace panorama_npu

// ---------- 内置插件注册 ----------
namespace {
struct TcpH264Register {
    TcpH264Register()
    {
        panorama_npu::register_sink_factory(
            "tcp_h264", []() -> std::unique_ptr<panorama_npu::IOutputSink> {
                return std::make_unique<panorama_npu::TcpH264Sink>();
            });
    }
} g_tcp_h264_register;

struct NullSink final : panorama_npu::IOutputSink {
    int init(const std::string &) override { return 0; }
    int send(const panorama_npu::OutputFrame &) override { return 0; }
    void close() noexcept override {}
    const char *name() const noexcept override { return "null"; }
};

struct NullRegister {
    NullRegister()
    {
        panorama_npu::register_sink_factory(
            "null", []() -> std::unique_ptr<panorama_npu::IOutputSink> {
                return std::make_unique<NullSink>();
            });
    }
} g_null_register;
}  // namespace

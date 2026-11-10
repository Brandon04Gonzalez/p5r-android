#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace nx::movie {

struct Nv12Layout {
    int32_t width = 0;
    int32_t height = 0;
    int32_t stride = 0;
    int32_t rows = 0;
    int32_t crop_left = 0;
    int32_t crop_top = 0;
    bool planar = false;

    int32_t lines() const { return height + height / 2; }
    size_t bytes() const { return size_t(width) * size_t(height) * 3 / 2; }
};

inline bool nv12_copy_line(const Nv12Layout& l, const uint8_t* src, size_t size, uint8_t* out, int32_t line) {
    const int32_t w = l.width, h = l.height;
    if (w <= 0 || h <= 0 || line < 0 || line >= l.lines()) return true;
    if (line < h) {
        const size_t at = size_t(l.crop_top) * size_t(l.stride) + size_t(l.crop_left) + size_t(line) * size_t(l.stride);
        if (at + size_t(w) > size) return false;
        std::memcpy(out + size_t(line) * size_t(w), src + at, size_t(w));
        return true;
    }
    const int32_t r = line - h;
    const size_t chroma = size_t(l.stride) * size_t(l.rows);
    uint8_t* uv = out + size_t(w) * size_t(h) + size_t(r) * size_t(w);
    if (l.planar) {
        const size_t cs = size_t(l.stride) / 2;
        const size_t plane = cs * (size_t(l.rows) / 2);
        const size_t at = chroma + size_t(l.crop_top / 2 + r) * cs + size_t(l.crop_left / 2);
        if (at + plane + size_t(w / 2) > size) return false;
        for (int32_t x = 0; x < w / 2; x++) {
            uv[size_t(2 * x)] = src[at + size_t(x)];
            uv[size_t(2 * x + 1)] = src[at + plane + size_t(x)];
        }
        return true;
    }
    const size_t at = chroma + size_t(l.crop_top / 2 + r) * size_t(l.stride) + (size_t(l.crop_left) & ~size_t(1));
    if (at + size_t(w) > size) return false;
    std::memcpy(uv, src + at, size_t(w));
    return true;
}

inline bool nv12_copy_lines(const Nv12Layout& l, const uint8_t* src, size_t size, uint8_t* out, int32_t begin,
                            int32_t end) {
    for (int32_t line = begin; line < end; line++)
        if (!nv12_copy_line(l, src, size, out, line)) return false;
    return true;
}

inline bool nv12_input_fits(const Nv12Layout& l, size_t size) {
    const int32_t w = l.width, h = l.height;
    if (w <= 0 || h <= 0) return true;
    if (size_t(l.crop_top) * size_t(l.stride) + size_t(l.crop_left) + size_t(h - 1) * size_t(l.stride) + size_t(w) > size)
        return false;
    if (h / 2 <= 0) return true;
    const size_t chroma = size_t(l.stride) * size_t(l.rows);
    if (l.planar) {
        const size_t cs = size_t(l.stride) / 2;
        const size_t plane = cs * (size_t(l.rows) / 2);
        const size_t at = chroma + size_t(l.crop_top / 2 + h / 2 - 1) * cs + size_t(l.crop_left / 2);
        if (at + plane + size_t(w / 2) > size) return false;
    } else {
        const size_t at = chroma + size_t(l.crop_top / 2 + h / 2 - 1) * size_t(l.stride) +
                          (size_t(l.crop_left) & ~size_t(1));
        if (at + size_t(w) > size) return false;
    }
    return true;
}

class FrameConverter {
public:
    explicit FrameConverter(int max_workers, bool adaptive)
        : max_(std::clamp(max_workers, 1, 8)), active_(max_), adaptive_(adaptive) {
        for (int k = 1; k < max_; k++) threads_.emplace_back([this, k] { worker(k); });
    }

    ~FrameConverter() {
        {
            std::lock_guard lk(m_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : threads_) t.join();
    }

    FrameConverter(const FrameConverter&) = delete;
    FrameConverter& operator=(const FrameConverter&) = delete;

    int workers() const { return active_.load(std::memory_order_relaxed); }
    int max_workers() const { return max_; }
    bool adaptive() const { return adaptive_; }
    uint64_t switches() const { return switches_.load(std::memory_order_relaxed); }

    void convert(const Nv12Layout& l, const uint8_t* src, size_t size, std::vector<uint8_t>& out) {
        out.resize(l.bytes());
        const int lines = l.lines();
        if (lines <= 0) return;
        const int parts = std::min(active_.load(std::memory_order_relaxed), max_);
        if (parts <= 1 || !nv12_input_fits(l, size)) {
            nv12_copy_lines(l, src, size, out.data(), 0, lines);
            return;
        }
        {
            std::lock_guard lk(m_);
            task_ = Task{&l, src, size, out.data(), lines, parts};
            generation_++;
            pending_.store(max_ - 1, std::memory_order_release);
        }
        cv_.notify_all();
        run_part(0, parts);
        std::unique_lock lk(m_);
        done_.wait(lk, [this] { return pending_.load(std::memory_order_acquire) == 0; });
    }

    void observe(uint64_t convert_ns, uint64_t pump_ns, size_t backlog, uint64_t gap_ns) {
        if (!adaptive_) return;
        const uint64_t now = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
        window_convert_ns_ += convert_ns;
        window_max_pump_ns_ = std::max(window_max_pump_ns_, pump_ns);
        window_max_backlog_ = std::max(window_max_backlog_, backlog);
        window_max_gap_ns_ = std::max(window_max_gap_ns_, gap_ns);
        window_samples_++;
        avg_convert_ns_ = avg_convert_ns_ ? (avg_convert_ns_ * 7 + convert_ns) / 8 : convert_ns;
        if (!window_from_ns_) window_from_ns_ = now;
        if (now - window_from_ns_ < 750'000'000ull || window_samples_ < 8) return;
        const double avg = double(window_convert_ns_) / double(window_samples_);
        const bool pressure = window_max_backlog_ >= 2 || window_max_pump_ns_ > 4'000'000ull ||
                              window_max_gap_ns_ > 40'000'000ull;
        const bool calm = window_max_backlog_ == 0 && window_max_pump_ns_ < 2'000'000ull &&
                          window_max_gap_ns_ < 25'000'000ull;
        pressure_windows_ = pressure ? pressure_windows_ + 1 : 0;
        calm_windows_ = calm ? calm_windows_ + 1 : 0;
        int want = active_.load(std::memory_order_relaxed);
        if (want < max_ && (pressure_windows_ >= 2 || avg > 800'000.0))
            want++;
        else if (want > 1 && calm_windows_ >= 3 && avg < 200'000.0)
            want--;
        if (want != active_.load(std::memory_order_relaxed) && now - last_switch_ns_ >= 1'500'000'000ull) {
            active_.store(want, std::memory_order_relaxed);
            switches_.fetch_add(1, std::memory_order_relaxed);
            last_switch_ns_ = now;
        }
        window_convert_ns_ = window_max_pump_ns_ = window_max_backlog_ = window_max_gap_ns_ = window_samples_ = 0;
        window_from_ns_ = now;
    }

private:
    struct Task {
        const Nv12Layout* layout = nullptr;
        const uint8_t* src = nullptr;
        size_t size = 0;
        uint8_t* out = nullptr;
        int lines = 0;
        int parts = 1;
    };

    void run_part(int index, int parts) {
        if (index >= parts || !task_.layout) return;
        const int lines = task_.lines;
        const int64_t begin = int64_t(lines) * index / parts;
        const int64_t end = int64_t(lines) * (index + 1) / parts;
        nv12_copy_lines(*task_.layout, task_.src, task_.size, task_.out, int32_t(begin), int32_t(end));
    }

    void worker(int index) {
        uint64_t seen = 0;
        for (;;) {
            Task task;
            int parts = 1;
            {
                std::unique_lock lk(m_);
                cv_.wait(lk, [this, seen] { return stop_ || generation_ != seen; });
                if (stop_) return;
                seen = generation_;
                task = task_;
                parts = task.parts;
            }
            if (index < parts) {
                const int lines = task.lines;
                const int64_t begin = int64_t(lines) * index / parts;
                const int64_t end = int64_t(lines) * (index + 1) / parts;
                nv12_copy_lines(*task.layout, task.src, task.size, task.out, int32_t(begin), int32_t(end));
            }
            if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                std::lock_guard lk(m_);
                done_.notify_all();
            }
        }
    }

    const int max_;
    std::atomic<int> active_;
    const bool adaptive_;
    std::mutex m_;
    std::condition_variable cv_, done_;
    uint64_t generation_ = 0;
    bool stop_ = false;
    Task task_;
    std::atomic<int> pending_{0};
    std::vector<std::thread> threads_;
    std::atomic<uint64_t> switches_{0};

    uint64_t avg_convert_ns_ = 0, window_convert_ns_ = 0, window_max_pump_ns_ = 0, window_max_gap_ns_ = 0;
    size_t window_max_backlog_ = 0, window_samples_ = 0;
    uint64_t window_from_ns_ = 0, last_switch_ns_ = 0;
    int pressure_windows_ = 0, calm_windows_ = 0;
};

}  // namespace nx::movie

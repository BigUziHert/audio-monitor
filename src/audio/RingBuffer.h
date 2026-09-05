#pragma once
//
// Lock-free single-producer / single-consumer ring of interleaved stereo
// float frames.
//
// Exactly one producer thread writes; exactly one consumer thread reads. No
// mutexes, no allocation after construction, no blocking. The begin/commit
// shape means each side touches the shared atomics twice per period rather
// than once per frame, which matters because the consumer pulls frames
// one at a time through the resampler.
//
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>

namespace audiomon {

#if defined(_MSC_VER)
#  pragma warning(push)
// C4324: "structure was padded due to alignment specifier". That padding is
// the entire point of the alignas below -- keeping the two cursors off each
// other's cache line -- so the warning is noise here.
#  pragma warning(disable : 4324)
#endif

class StereoRing {
public:
    StereoRing() = default;
    StereoRing(const StereoRing&) = delete;
    StereoRing& operator=(const StereoRing&) = delete;

    // Allocates once, at startup. capacityFrames is rounded up to a power of
    // two so the index wrap is a mask rather than a modulo.
    void init(uint32_t capacityFrames) {
        uint32_t cap = 1;
        while (cap < capacityFrames) cap <<= 1;
        capacity_ = cap;
        mask_     = cap - 1;
        data_     = std::make_unique<float[]>(static_cast<size_t>(cap) * 2);
        std::memset(data_.get(), 0, static_cast<size_t>(cap) * 2 * sizeof(float));
        write_.store(0, std::memory_order_relaxed);
        read_.store(0, std::memory_order_relaxed);
    }

    uint32_t capacity() const noexcept { return capacity_; }

    // Depth in frames. Safe to call from either side; the value is a snapshot
    // and may be stale the instant it is returned, which is fine for both the
    // drift controller and the UI.
    uint32_t depth() const noexcept {
        const uint32_t w = write_.load(std::memory_order_acquire);
        const uint32_t r = read_.load(std::memory_order_acquire);
        return w - r;   // unsigned wraparound is well defined and correct here
    }

    // ---------------------------------------------------------- producer ---
    // Frames of headroom currently available to write.
    uint32_t beginWrite() noexcept {
        const uint32_t w = write_.load(std::memory_order_relaxed);
        // acquire: we must see the consumer's reads before reusing those slots
        const uint32_t r = read_.load(std::memory_order_acquire);
        return capacity_ - (w - r);
    }

    // i is an offset from the current write cursor, 0 <= i < beginWrite().
    void writeFrame(uint32_t i, float l, float r) noexcept {
        const uint32_t w   = write_.load(std::memory_order_relaxed);
        const size_t   idx = static_cast<size_t>((w + i) & mask_) * 2;
        data_[idx]     = l;
        data_[idx + 1] = r;
    }

    // release: publishes the frames written above to the consumer.
    void endWrite(uint32_t produced) noexcept {
        write_.store(write_.load(std::memory_order_relaxed) + produced,
                     std::memory_order_release);
    }

    // ---------------------------------------------------------- consumer ---
    // Frames currently available to read.
    uint32_t beginRead() noexcept {
        // acquire: pairs with endWrite's release so the frame data is visible
        const uint32_t w = write_.load(std::memory_order_acquire);
        const uint32_t r = read_.load(std::memory_order_relaxed);
        return w - r;
    }

    // i is an offset from the current read cursor, 0 <= i < beginRead().
    void readFrame(uint32_t i, float& l, float& r) const noexcept {
        const uint32_t rd  = read_.load(std::memory_order_relaxed);
        const size_t   idx = static_cast<size_t>((rd + i) & mask_) * 2;
        l = data_[idx];
        r = data_[idx + 1];
    }

    // release: frees those slots for the producer to overwrite.
    void endRead(uint32_t consumed) noexcept {
        read_.store(read_.load(std::memory_order_relaxed) + consumed,
                    std::memory_order_release);
    }

    // Consumer-side only. Used when a stream reconnects and we want to start
    // from a known depth rather than whatever stale audio is sitting there.
    void dropAllFromConsumer() noexcept {
        read_.store(write_.load(std::memory_order_acquire), std::memory_order_release);
    }

private:
    std::unique_ptr<float[]> data_;
    uint32_t capacity_ = 0;
    uint32_t mask_     = 0;

    // Padded onto separate cache lines: the producer spins on write_ and the
    // consumer on read_, and sharing a line would cost a bus transaction per
    // access for no reason.
    alignas(64) std::atomic<uint32_t> write_{0};
    alignas(64) std::atomic<uint32_t> read_{0};
};

#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

} // namespace audiomon

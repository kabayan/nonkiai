#pragma once
#include <atomic>
#include <stdint.h>

// One button writer (main), one tap reader (sampler). Atomic snapshots keep a
// release paired with its press. History covers repeated presses in one train.
class ButtonTapWindow {
public:
    void note(uint32_t now_ms, bool pressed) {
        version_.fetch_add(1); // odd while publishing
        if (pressed && !held_) {
            head_ = (head_+1) % kHistory;
            if (count_ < kHistory) ++count_;
            slots_[head_].press.store(now_ms);
            slots_[head_].held.store(true);
            held_ = true;
        } else if (!pressed && held_) {
            slots_[head_].release.store(now_ms);
            slots_[head_].held.store(false);
            held_ = false;
        }
        published_head_.store(head_);
        published_count_.store(count_);
        version_.fetch_add(1);
    }
    bool contains(uint32_t tap_ms) const {
        // Do not spin if the higher-priority sampler preempts the writer.
        // The detector rechecks on each sample and latches matches; a new press
        // can overlap only through age 300 ms, before the 420 ms finalization.
        {
            const uint32_t before = version_.load();
            if (before & 1) return false;
            const unsigned head = published_head_.load(), count = published_count_.load();
            bool found = false;
            for (unsigned i = 0; i < count; ++i) {
                const Slot &s = slots_[(head+kHistory-i)%kHistory];
                const uint32_t press = s.press.load(), release = s.release.load();
                const bool held = s.held.load();
                if (static_cast<int32_t>(tap_ms-(press-300)) >= 0 &&
                    (held || static_cast<int32_t>((release+500)-tap_ms) >= 0)) found = true;
            }
            return version_.load() == before && found;
        }
    }
private:
    static constexpr unsigned kHistory = 16;
    struct Slot {
        std::atomic<uint32_t> press{0}, release{0};
        std::atomic<bool> held{false};
    } slots_[kHistory];
    unsigned head_ = kHistory-1, count_ = 0; // writer only
    bool held_ = false;
    std::atomic<unsigned> published_head_{kHistory-1}, published_count_{0};
    std::atomic<uint32_t> version_{0};
};

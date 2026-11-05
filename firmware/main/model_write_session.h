#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "wire_format.h"

namespace nk {
// Flash / RTOS に依存しない受信判定。next は書き込み成功後だけ進める。
class ModelWriteSession {
public:
    uint8_t state = NK_MODEL_IDLE;
    uint32_t session = 0, size = 0, next = 0;
    uint8_t expected[32]{};

    bool active() const {
        return state == NK_MODEL_ERASING || state == NK_MODEL_RECEIVING || state == NK_MODEL_VERIFYING;
    }
    uint8_t begin(uint32_t id, uint32_t bytes, uint32_t capacity, const uint8_t sha[32]) {
        if (active()) return NK_MODEL_ERR_BUSY;
        if (!id || !sha) return NK_MODEL_ERR_PACKET;
        if (!bytes || bytes > capacity) return NK_MODEL_ERR_SIZE;
        session = id; size = bytes; next = 0;
        std::memcpy(expected, sha, sizeof(expected));
        state = NK_MODEL_ERASING;
        return NK_MODEL_OK;
    }
    void erased() { if (state == NK_MODEL_ERASING) state = NK_MODEL_RECEIVING; }
    uint8_t check_data(uint32_t id, uint32_t offset, size_t bytes) const {
        if (id != session || !id) return NK_MODEL_ERR_SESSION;
        if (state != NK_MODEL_RECEIVING) return NK_MODEL_ERR_BUSY;
        if (!bytes || bytes > NK_MODEL_DATA_BYTES || offset > size || bytes > size - offset)
            return NK_MODEL_ERR_SIZE;
        // 過去の全量再送は無視できる。未確定部分にまたがる再送は拒否。
        if (offset > next || (offset < next && bytes > next - offset)) return NK_MODEL_ERR_ORDER;
        return NK_MODEL_OK;
    }
    void written(size_t bytes) { next += static_cast<uint32_t>(bytes); }
    uint8_t start_verify(uint32_t id) {
        if (id != session || !id) return NK_MODEL_ERR_SESSION;
        if (state != NK_MODEL_RECEIVING) return NK_MODEL_ERR_BUSY;
        if (next != size) return NK_MODEL_ERR_INCOMPLETE;
        state = NK_MODEL_VERIFYING;
        return NK_MODEL_OK;
    }
    uint8_t verified(const uint8_t actual[32]) {
        if (state != NK_MODEL_VERIFYING || !actual) return NK_MODEL_ERR_BUSY;
        const bool equal = std::memcmp(actual, expected, sizeof(expected)) == 0;
        state = equal ? NK_MODEL_VERIFIED : NK_MODEL_FAILED;
        return equal ? NK_MODEL_OK : NK_MODEL_ERR_HASH;
    }
    bool can_reboot(uint32_t id) const { return id && id == session && state == NK_MODEL_VERIFIED; }
    void fail(bool aborted = false) { state = aborted ? NK_MODEL_ABORTED : NK_MODEL_FAILED; }
};
} // namespace nk

#pragma once

#include <cmath>
#include <cstdio>
#include <cstdint>
#include "sano_voice.h"
#include "wire_format.h"

// MODEL read の表現をまとめ、値の極端な組み合わせも同じ書式で試験する。
struct NkModelInfo {
    bool available, boot_ready, voice_ready, dict_available, dict_dirty;
    uint32_t capacity, dict_capacity, session, hash_bytes, probe_request, probe_bytes;
    uint16_t max_data;
    uint8_t state, target, probe_target;
    const char *sha, *probe_sha;
    sano_model_flags_t flags;
    sano_voice_params_t params;
};

inline int nk_model_info_json(char *out, size_t capacity, const NkModelInfo &v) {
    // 非有限値は JSON の数値にできないため null とし、画面は同一判定を拒否する。
    char numbers[6][32];
    const double values[] = {v.params.k, v.params.center_hz, v.params.lo_hz, v.params.hi_hz, v.params.tau_ms, v.params.vnoise};
    for (size_t i = 0; i < 6; ++i) {
        if (std::isfinite(values[i])) std::snprintf(numbers[i], sizeof(numbers[i]), i == 5 ? "%.9g" : "%.17g", values[i]);
        else std::snprintf(numbers[i], sizeof(numbers[i]), "null");
    }
    return std::snprintf(out, capacity,
        "{\"protocol\":%u,\"a\":%u,\"c\":%lu,\"m\":%u,\"b\":%u,\"v\":%u,"
        "\"f\":[%d,%d,%d,%d,%d],\"p\":[%u,%u,%s,%s,%s,%s,%s,%s],"
        "\"w\":[%u,%lu,\"%s\",%lu,%u],"
        "\"d\":[%u,%lu,%u],\"q\":[%lu,%u,%lu,\"%s\"]}",
        NK_MODEL_WIRE_VERSION, (unsigned)v.available, (unsigned long)v.capacity, v.max_data,
        (unsigned)v.boot_ready, (unsigned)v.voice_ready,
        v.flags.dec_head, v.flags.g_excite, v.flags.phrase_rule_sbv2, v.flags.tail_pause, v.flags.int8_act,
        (unsigned)v.params.from_record, (unsigned)v.params.k_from_nvs,
        numbers[0], numbers[1], numbers[2], numbers[3], numbers[4], numbers[5],
        (unsigned)v.state, (unsigned long)v.session, v.sha, (unsigned long)v.hash_bytes, (unsigned)v.target,
        (unsigned)v.dict_available, (unsigned long)v.dict_capacity, (unsigned)v.dict_dirty,
        (unsigned long)v.probe_request, (unsigned)v.probe_target, (unsigned long)v.probe_bytes, v.probe_sha);
}

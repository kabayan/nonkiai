#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Called by the sampler only after its speech-pause flag is set. The worker
// stays busy through capture and the tracked reply; finish is sampler-owned.
esp_err_t nk_voice_listen_init(void);
bool nk_voice_listen_start(uint8_t event, uint32_t seq);
// Raw IMU sampling is safe after codec setup; gesture actions remain paused.
bool nk_voice_listen_sampling(void);
const char *nk_voice_listen_state(void);
const char *nk_voice_listen_block_reason(void);
// Capture state covers recording/inference; Reply/Failed waiting is excluded.
bool nk_voice_listen_capturing(void);
bool nk_voice_listen_busy(void);
bool nk_voice_listen_reply_ready(void);
bool nk_voice_listen_reply_queued(void);
// A configured empty reply completed without queueing audio.
bool nk_voice_listen_reply_silent(void);
// DLS-143: direction (0..3) latched at recognition for an automatic IR send after
// the reply, or 0xFF for none. Read it after reply_ready and before finish.
uint8_t nk_voice_listen_ir_direction(void);
void nk_voice_listen_finish(void);

// stt32 DLS-1082: 起動語の常時待受。起きている間は stt32 の WAKE でマイクを開いて待ち、受理したら待受を止めて
// 1 回タップと同じ聞き取り（合図音 → モードの ONE_UTTERANCE → 返事 / IR）を始める。発話・cue が要るとき、
// 眠りに入ったとき、タップのときは（推論中でなければ）listen_release で譲り、静かになったら戻る。
esp_err_t nk_voice_listen_start_standby(void);

// Development-only serial PCM hex dump; off at boot. Takes effect next time.
void nk_voice_listen_set_pcm_dump(bool enabled);

#ifdef __cplusplus
}
#endif

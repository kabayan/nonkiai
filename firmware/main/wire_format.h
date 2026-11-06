// 収録用 IMU ストリーミングの wire 仕様（草案 DLS-003 / DLS-018）。
//
// 設計上の不変条件:
//   - デバイスは生の ADC 値のみを送る。基準点補正・キャリブレーションを一切かけない。
//     基準点の取り方そのものが後段 (P3) の検討対象なので、原本を加工しない。
//   - サンプルは accel の新着を基準に 1 本立てる。gyro / mag は直近のラッチ値を同梱する。
//   - seq は起動から単調増加。ブラウザ側はこの連番のギャップで取りこぼしを検出する。
#pragma once

#include <stdint.h>

#define NK_WIRE_VERSION 1

// パケット = ヘッダ 11B + サンプル 18B × n。
// notif 実績の MTU 247 (payload 244B) で n = (244 - 11) / 18 = 12。
#define NK_SAMPLE_BYTES 18
#define NK_PKT_HEADER_BYTES 11
#define NK_MAX_SAMPLES_PER_PKT 12

// CTRL characteristic (write) のオペコード
enum {
    NK_CTRL_START          = 0x01,  // [0x01]
    NK_CTRL_STOP           = 0x02,  // [0x02]
    NK_CTRL_SET_ODR        = 0x03,  // [0x03][odr_hz:u16 LE]
    NK_CTRL_MARKER         = 0x04,  // [0x04][marker_id:u32 LE]
    // 誘導音。第 2 音は省略可 (5 バイトなら単音、9 バイトなら 2 音を連続再生)。
    NK_CTRL_BEEP           = 0x05,  // [0x05][freq1:u16 LE][ms1:u16 LE]([freq2:u16 LE][ms2:u16 LE])
    // P5-4: 音声設定制御
    NK_CTRL_VOICE_CFG_SET  = 0x10,  // [0x10][action_name_null_term][utf8_text_null_term]
    NK_CTRL_VOICE_CFG_SAVE = 0x11,  // [0x11] (NVSへ保存)
    NK_CTRL_VOICE_CFG_LOAD = 0x12,  // [0x12] (NVSから再読み込みして全通知)
    NK_CTRL_VOICE_SAY      = 0x13,  // [0x13][utf8_text_null_term] (WebUIからのテスト発声)
    NK_CTRL_VOICE_SET_VOL  = 0x14,  // [0x14][volume:u8 (0-255)] (DLS-021: 音量設定)
    // DLS-026: 音声・量子化テスト制御
    NK_CTRL_VOICE_TEST_QUANT = 0x15, // [0x15][quant_mode:u8 (0=16bit, 1=8bit, 2=4bit-adpcm)]
    NK_CTRL_VOICE_SAY_WAV    = 0x16, // [0x16][utf8_text_null_term] (WAV連結限定テスト)
    // 専用設定画面の名前。SAVE は [0x18] または [0x18][name\0]。
    NK_CTRL_NAME_SET         = 0x17, // [0x17][utf8_name_null_term] (RAMのみ)
    NK_CTRL_NAME_SAVE        = 0x18, // [0x18]([utf8_name_null_term]) (NVSへ保存)
    NK_CTRL_NAME_LOAD        = 0x19, // [0x19] (NVSから読み込み)
    NK_CTRL_SWAP_NOD_TILT    = 0x1A, // [0x1A][enabled:u8] (即時反映してNVS保存)
    NK_CTRL_TIME_SYNC        = 0x1B, // [0x1B][unix_ms:i64 LE][utc_offset_minutes:i16 LE]
    NK_CTRL_WEATHER_SET      = 0x1C, // [0x1C][year:u16 LE][month:u8][day:u8][code:u8] x 2 (today, tomorrow)
    NK_CTRL_WEATHER_GET      = 0x1D, // [0x1D] (today's and tomorrow's valid stored forecasts)
    // 0x1E / 0x1F は声モデルの取得・切替だった（DLS-150 で廃止。声は model パーティションの 1 本だけ）。再利用しない
    // Only supported by builds with NONKI_LISTEN_DIAGNOSTICS=ON.
    NK_CTRL_LISTEN_DIAGNOSTIC = 0x20, // [0x20][dump_pcm:u8 0/1] (sampler-owned listen request)
    NK_CTRL_IR_TEST          = 0x21, // [0x21][direction:u8][request:u32 LE], exactly 6B
    // 0x22 は聞き取った方向の IR 自動送信の ON/OFF だった（認識モードに置き換え、stt32 DLS-1082）。再利用しない
    NK_CTRL_IR_HOLD          = 0x23, // [0x23][direction:u8][mode:u8 0=NEC repeat/1=full frames][count:u8 1..30][interval_ms:u16 LE 108..500][request:u32 LE], exactly 10B
    NK_CTRL_VOICE_F0K        = 0x24, // [0x24][k:f32 LE] = 5B: 声の高さの倍率 k（0.5〜3.0）を変えて NVS に保存 / [0x24] = 1B: 書き込んだ記録の値に戻す。記録が無い・中心が無いモデルは拒否（DLS-157）
    NK_CTRL_LISTEN_MODE      = 0x25, // [0x25][mode:u8 0=ぬいぐるみ/1=おもちゃ], exactly 2B (即時反映してNVS保存。stt32 DLS-1082)
    NK_CTRL_IR_PIN           = 0x26, // [0x26][pin:u8 0=GPIO47/1=GPIO2], exactly 2B; apply and save NVS
};

// STATUS characteristic (notify) のオペコード
enum {
    NK_STATUS_MARKER       = 0x84,  // [0x84][marker_id:u32][seq:u32][t_us:u32]
    NK_STATUS_STATE        = 0x85,  // [0x85][streaming:u8][odr_hz:u16][dropped_pkts:u32]
    NK_STATUS_BEEP         = 0x86,  // [0x86][seq:u32][t_us:u32][freq1:u16][total_ms:u16]
    NK_STATUS_MOTION       = 0x87,  // [0x87][event_id:u8][seq:u32][t_us:u32]
    // 専用設定画面が接続時点の検出状態を表示するスナップショット。
    // [0x88][detector_state:u8][sleeping:u8][imu_suspended:u8][voice_playing:u8][seq:u32]
    NK_STATUS_DETECTOR_STATE = 0x88,
    // DLS-105: [0x94][posture:u8][angle_cdeg:u16 LE][seq:u32 LE]
    // posture: 0=基準待ち, 1=起立, 2=寝姿勢
    NK_STATUS_POSTURE       = 0x94,
    NK_STATUS_SWAP_NOD_TILT = 0x95, // [0x95][enabled:u8]
    // [0x89][event:u8][motion_seq:u32 LE][delivery:u8][part:u8][total:u8][UTF-8 chunk]
    // delivery: 0=silent, 1=voice request accepted, 2=voice request rejected.
    NK_STATUS_MOTION_SPEECH = 0x89,
    // DLS-136: 14B, fits the default ATT payload. Labels follow word23_intents.json.
    // [0x8A][event:u8][seq:u32 LE][intent:u8][reason:u8][raw1:u8][raw2:u8]
    // [score1:u16 LE per mille][score2:u16 LE per mille]
    // intent: 0..16, 255=not accepted; reason: 0=accepted, 1=silence, 2=confidence, 3=failure.
    // stt32: raw1=accepted product id (else 255), raw2=255 (unavailable).
    NK_STATUS_INTENT        = 0x8A,
    // P5-4: 音声設定通知
    NK_STATUS_VOICE_CFG_ITEM = 0x90, // [0x90][idx:u8][total:u8][action_key_null_term][text_null_term]
    NK_STATUS_VOICE_VOL      = 0x91, // [0x91][volume:u8] (DLS-021: 現在の音量通知)
    // 名前は既定 ATT payload (20B) に収めるため 16B ごとに分割する。
    NK_STATUS_NAME           = 0x92, // [0x92][part:u8][total:u8][utf8_name_chunk]
    NK_STATUS_NAME_RESULT    = 0x93, // [0x93][op:u8][esp_err_t:u32 LE]
    NK_STATUS_TIME_RESULT    = 0x96, // [0x96][esp_err_t:u32 LE][local_ms:i64 LE] (-1 if rejected)
    NK_STATUS_WEATHER_RESULT = 0x97, // [0x97][esp_err_t:u32 LE][year:u16 LE][month:u8][day:u8][code:u8] x 2; zeros on error
    NK_STATUS_WEATHER_STATE  = 0x98, // [0x98][known:u8][year:u16 LE][month:u8][day:u8][code:u8] x 2; zeros if unknown
    // 0x99 は声モデルの通知だった（DLS-150 で廃止）。再利用しない
    // direction: 0=forward/hrf, 1=back/hrb, 2=right/hrr, 3=left/hrl.
    // Success means full-frame TX completed, not a target-device acknowledgment.
    NK_STATUS_IR_RESULT      = 0x9A, // [0x9A][direction:u8][request:u32 LE][esp_err_t:u32 LE], 10B
    // 0x9B は聞き取った方向の IR 自動送信の現在設定だった（認識モードに置き換え、stt32 DLS-1082）。再利用しない
    NK_STATUS_LISTEN_MODE    = 0x9C, // [0x9C][mode:u8 0=ぬいぐるみ/1=おもちゃ], 2B（変更後と STATUS 購読開始時に通知）
    NK_STATUS_IR_PIN         = 0x9D, // [0x9D][pin:u8], 2B; changed and STATUS subscription
};

enum {
    NK_NAME_OP_SET         = 1,
    NK_NAME_OP_SAVE        = 2,
    NK_NAME_OP_LOAD        = 3,
    NK_NAME_OP_CONFIG_SAVE = 4,
};

#define NK_NAME_NOTIFY_CHUNK_BYTES 16
#define NK_SPEECH_NOTIFY_CHUNK_BYTES 11
#define NK_SPEECH_TEXT_MAX_BYTES 127

// モデル専用 characteristic（既存 CTRL / STATUS の形は変えない）。
// MODEL a1b20005: read=JSON、write=下記コマンド。整数はすべて LE。
// MODEL_STATE a1b20006: read / notify、常に 20 B。
// [version:u8][state:u8][error:u8][op:u8][session:u32][next:u32][size:u32][request:u32]
// next は処理済みの次の offset（先頭 4 B は照合まで RAM に保留）。
// SHA-256 本体は MODEL の JSON で読む。
// 版 2（DLS-159）: BEGIN に書き込み先（target）を足し、PROBE（書く前の照合）を足した。
// 版 1 の画面は protocol != 1 で止まり、版 2 の画面は protocol != 2 で止まる（黙って別の領域へ書かない）。
#define NK_MODEL_WIRE_VERSION 3
#define NK_MODEL_PACKET_BYTES 244
#define NK_MODEL_DATA_HEADER_BYTES 9
#define NK_MODEL_DATA_BYTES (NK_MODEL_PACKET_BYTES - NK_MODEL_DATA_HEADER_BYTES)
#define NK_MODEL_WINDOW_CHUNKS 8
#define NK_MODEL_STATE_BYTES 20
enum {
    NK_MODEL_BEGIN = 1,  // [op][session:u32 !=0][size:u32][sha256:32B][target:u8] = 42B。41B（target 無し）は拒否
    NK_MODEL_DATA = 2,   // [op][session:u32][offset:u32][data:1..235B]
    NK_MODEL_END = 3,    // [op][session:u32]、Flash を読み直して照合
    NK_MODEL_ABORT = 4,  // [op][session:u32]
    NK_MODEL_REBOOT = 5, // [op][session:u32]、同じ session の照合成功時だけ
    NK_MODEL_QUERY = 6,  // [op][session:u32][request:u32]、キューの処理完了位置を返す
    // [op][session:u32][model_sha256:32B][k:f64][center_hz:f64][lo_hz:f64][hi_hz:f64][tau_ms:f64][vnoise:f32] = 81B
    // 声ごとの値。END の照合成功後・REBOOT 前に同じ session で送る。sha256 が書いたモデルと違う・値が範囲外なら拒否。
    // model 領域の最後の 4 KB セクタに記録する（BEGIN で消えるので、モデルを書き直せば必ず消える）。
    NK_MODEL_PARAMS = 7,
    // [op][request:u32 !=0][target:u8][bytes:u32] = 10B。書き込み先の先頭 bytes の SHA-256 を計算し、MODEL の JSON の
    // probe に置く。書き込み前の「同じなら書かない」用。書き込みの最中（ERASING〜VERIFYING）は ERR_BUSY。
    // 通知は op=8、request に同じ値が入る。session は触らない（直前の書き込みの状態は変わらない）。
    NK_MODEL_PROBE = 8,
};
enum {
    NK_MODEL_TARGET_MODEL = 0,  // model 領域（subtype 0x40）。末尾 4 KB は声の値の記録
    NK_MODEL_TARGET_DICT = 1,   // dict 領域（subtype 0x41）。全域が辞書
};
enum {
    NK_MODEL_IDLE, NK_MODEL_ERASING, NK_MODEL_RECEIVING, NK_MODEL_VERIFYING,
    NK_MODEL_VERIFIED, NK_MODEL_ABORTED, NK_MODEL_FAILED, NK_MODEL_RESTARTING,
};
enum {
    NK_MODEL_OK, NK_MODEL_ERR_PACKET, NK_MODEL_ERR_SIZE, NK_MODEL_ERR_BUSY,
    NK_MODEL_ERR_SESSION, NK_MODEL_ERR_ORDER, NK_MODEL_ERR_FLASH,
    NK_MODEL_ERR_HASH, NK_MODEL_ERR_INCOMPLETE, NK_MODEL_ERR_TIMEOUT,
    NK_MODEL_ERR_DISCONNECTED, NK_MODEL_ERR_CANCELLED, NK_MODEL_ERR_NOT_VERIFIED,
    NK_MODEL_ERR_TARGET,  // 書き込み先が不正・その領域が無い・その先に使えない操作（PARAMS）・辞書の形ではない
};

typedef struct __attribute__((packed)) {
    int16_t ax, ay, az;  // accel ADC (±8g full scale)
    int16_t gx, gy, gz;  // gyro  ADC (±2000dps full scale)
    int16_t mx, my, mz;  // mag   ADC (BMM150、30Hz 更新の最新値ラッチ)
} nk_sample_t;

_Static_assert(sizeof(nk_sample_t) == NK_SAMPLE_BYTES, "nk_sample_t must be 18 bytes");

typedef struct __attribute__((packed)) {
    uint32_t seq_start;  // 先頭サンプルの通し番号
    uint32_t t_us;       // 先頭サンプルの esp_timer 下位 32bit (µs)
    uint16_t odr_hz;     // 実際に設定されている accel ODR
    uint8_t  n;          // 本パケットのサンプル数
} nk_pkt_header_t;

_Static_assert(sizeof(nk_pkt_header_t) == NK_PKT_HEADER_BYTES, "nk_pkt_header_t must be 11 bytes");

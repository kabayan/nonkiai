#include "ble_stream.h"

#include <stdio.h>
#include <string.h>
#include <atomic>
#include <algorithm>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "audio_cue.h"
#include "device_clock.h"
#include "device_weather.h"
#include "direction_ir.h"
#include "imu_sampler.h"
#include "model_writer.h"
#include "wire_format.h"
#include "voice_config.h"
#include "sano_voice.h"
#include "voice_player.h"
#include "esp_littlefs.h"

static const char *const kTag = "nk-ble";
static const char *const kDeviceName = "nonkiact2";

// BLE_UUID128_INIT は canonical 表記の逆順 (little-endian) でバイトを取る。
// canonical: a1b2000X-5d3f-4e8a-9c21-7f6e5d4c3b2a
#define NK_UUID128(lsb0, lsb1)                                     \
    BLE_UUID128_INIT(0x2a, 0x3b, 0x4c, 0x5d, 0x6e, 0x7f, 0x21,     \
                     0x9c, 0x8a, 0x4e, 0x3f, 0x5d, lsb0, lsb1, 0xb2, 0xa1)

static const ble_uuid128_t kSvcUuid    = NK_UUID128(0x00, 0x00);
static const ble_uuid128_t kStreamUuid = NK_UUID128(0x01, 0x00);
static const ble_uuid128_t kCtrlUuid   = NK_UUID128(0x02, 0x00);
static const ble_uuid128_t kStatusUuid = NK_UUID128(0x03, 0x00);
static const ble_uuid128_t kInfoUuid   = NK_UUID128(0x04, 0x00);
static const ble_uuid128_t kModelUuid  = NK_UUID128(0x05, 0x00);
static const ble_uuid128_t kModelStateUuid = NK_UUID128(0x06, 0x00);

static uint16_t s_stream_handle = 0;
static uint16_t s_status_handle = 0;
static uint16_t s_model_state_handle = 0;
static bool s_model_subscribed = false;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static std::atomic<uint32_t> s_connection_generation{0};
static bool s_stream_subscribed = false;
static uint8_t s_own_addr_type = 0;

static void start_advertising(void);

// ---------------------------------------------------------------- GATT

static int model_access(uint16_t conn_handle, uint16_t, struct ble_gatt_access_ctxt *ctxt, void *)
{
    s_conn_handle = conn_handle;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        char json[513];
        const uint16_t mtu = ble_att_mtu(conn_handle);
        const uint16_t max_data = mtu > 12 ? std::min<unsigned>(NK_MODEL_DATA_BYTES, mtu - 12) : 0;
        const int len = nk_model_writer_info(json, sizeof(json), max_data);
        if (len <= 0 || (size_t)len >= sizeof(json)) return BLE_ATT_ERR_UNLIKELY;
        return os_mbuf_append(ctxt->om, json, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t payload[NK_MODEL_PACKET_BYTES]; uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, payload, sizeof(payload), &len) != 0)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    return nk_model_writer_submit(payload, len, s_connection_generation.load()) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int model_state_access(uint16_t conn_handle, uint16_t, struct ble_gatt_access_ctxt *ctxt, void *)
{
    s_conn_handle = conn_handle;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_READ_NOT_PERMITTED;
    uint8_t state[NK_MODEL_STATE_BYTES]; nk_model_writer_status(state);
    return os_mbuf_append(ctxt->om, state, sizeof(state)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int info_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle; (void)arg;
    // A failed GAP connect callback can leave the cached handle unset even
    // while GATT access is active. This callback supplies the live handle.
    s_conn_handle = conn_handle;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    float accel_res = 0.0f, gyro_res = 0.0f, mag_res = 0.0f;
    nk_imu_convert_param(&accel_res, &gyro_res, &mag_res);

    // ブラウザ側が ADC→物理量の換算と wire の解釈を自己完結でできるように、
    // スケール係数とパケット諸元を JSON で持たせる。
    size_t fs_total = 0, fs_used = 0;
    esp_littlefs_info("littlefs", &fs_total, &fs_used);

    char json[384];
    const int len = snprintf(json, sizeof(json),
        "{\"wire\":%d,\"fw\":\"0.1.0\",\"imu\":\"BMI270+BMM150\","
        "\"odr_hz\":%u,\"mag_odr_hz\":30,\"sample_bytes\":%d,\"hdr_bytes\":%d,"
        "\"n_max\":%u,\"axes\":\"sensor-native\",\"calibrated\":false,"
        "\"accel_res\":%.9g,\"gyro_res\":%.9g,\"mag_res\":%.9g,"
        "\"fs_total\":%u,\"fs_used\":%u}",
        NK_WIRE_VERSION, (unsigned)nk_imu_get_odr(),
        NK_SAMPLE_BYTES, NK_PKT_HEADER_BYTES,
        (unsigned)nk_ble_max_samples_per_pkt(),
        accel_res, gyro_res, mag_res,
        (unsigned)fs_total, (unsigned)fs_used);

    if (len <= 0 || (size_t)len >= sizeof(json)) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return os_mbuf_append(ctxt->om, json, (uint16_t)len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int ctrl_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle; (void)arg;
    s_conn_handle = conn_handle;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t payload[128] = {0};
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, payload, sizeof(payload), &len) != 0 || len < 1) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    if (nk_model_writer_busy() && (payload[0] == NK_CTRL_BEEP || payload[0] == NK_CTRL_VOICE_SAY ||
        payload[0] == NK_CTRL_VOICE_TEST_QUANT || payload[0] == NK_CTRL_VOICE_SAY_WAV ||
        payload[0] == NK_CTRL_LISTEN_DIAGNOSTIC)) return BLE_ATT_ERR_UNLIKELY;

    switch (payload[0]) {
        case NK_CTRL_START:
            nk_imu_set_streaming(true);
            ESP_LOGI(kTag, "CTRL start");
            nk_ble_notify_status_state();
            return 0;

        case NK_CTRL_STOP:
            nk_imu_set_streaming(false);
            ESP_LOGI(kTag, "CTRL stop");
            nk_ble_notify_status_state();
            return 0;

        case NK_CTRL_SET_ODR: {
            if (len < 3) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            const uint16_t hz = (uint16_t)(payload[1] | (payload[2] << 8));
            if (!nk_imu_set_odr(hz)) {
                ESP_LOGW(kTag, "CTRL set_odr rejected: %u", (unsigned)hz);
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            nk_ble_notify_status_state();
            return 0;
        }

        case NK_CTRL_MARKER: {
            if (len < 5) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            const uint32_t id = (uint32_t)payload[1] | ((uint32_t)payload[2] << 8)
                              | ((uint32_t)payload[3] << 16) | ((uint32_t)payload[4] << 24);
            // マーカーを打った瞬間の seq / デバイス時刻を返す。ブラウザ時刻と
            // デバイス時刻の対応が取れ、ラベル窓を seq 基準で正確に切り出せる。
            nk_ble_notify_status_marker(id, nk_imu_seq(), (uint32_t)esp_timer_get_time());
            return 0;
        }

        case NK_CTRL_BEEP: {
            if (len < 5) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            const uint16_t freq1 = (uint16_t)(payload[1] | (payload[2] << 8));
            const uint16_t ms1   = (uint16_t)(payload[3] | (payload[4] << 8));
            // 第 2 音は省略可。9 バイト来ていれば 2 音の誘導音として鳴らす。
            uint16_t freq2 = 0, ms2 = 0;
            if (len >= 9) {
                freq2 = (uint16_t)(payload[5] | (payload[6] << 8));
                ms2   = (uint16_t)(payload[7] | (payload[8] << 8));
            }
            // 鳴らす直前の seq / デバイス時刻を控えてから鳴らす。スピーカーは
            // おもちゃ内で IMU と同じ筐体に載るため、ビープの振動が加速度に乗る。
            // P3 でその区間を識別できるように位置を残す (DLS-008)。
            const uint32_t seq = nk_imu_seq();
            const uint32_t t_us = (uint32_t)esp_timer_get_time();
            if (!nk_audio_beep(freq1, ms1, freq2, ms2)) {
                ESP_LOGW(kTag, "CTRL beep ignored (speaker not ready)");
                return 0;
            }
            nk_ble_notify_status_beep(seq, t_us, freq1, (uint16_t)(ms1 + ms2));
            return 0;
        }

        case NK_CTRL_VOICE_CFG_SET: {
            // payload: [0x10][action_key\0][text\0]
            if (len < 3) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            const char *key = (const char *)&payload[1];
            size_t key_len = strnlen(key, len - 1);
            if (1 + key_len + 1 >= len) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            const char *text = (const char *)&payload[1 + key_len + 1];
            ESP_LOGI(kTag, "CTRL set voice cfg: '%s' -> '%s'", key, text);
            nk_voice_config_set_text_by_key(key, text);
            return 0;
        }

        case NK_CTRL_VOICE_CFG_SAVE: {
            ESP_LOGI(kTag, "CTRL save voice cfg to NVS");
            const esp_err_t save_err = nk_voice_config_save();
            nk_ble_notify_voice_volume(nk_voice_config_get_volume());
            nk_ble_notify_all_voice_configs();
            // The legacy page ignores this newer status packet; the simple
            // page uses it when persisting the volume through the existing
            // configuration save command.
            nk_ble_notify_name_result(NK_NAME_OP_CONFIG_SAVE, save_err);
            return 0;
        }

        case NK_CTRL_VOICE_CFG_LOAD: {
            ESP_LOGI(kTag, "CTRL load voice cfg from NVS");
            nk_voice_config_load();
            uint8_t vol = nk_voice_config_get_volume();
            voice_player_set_volume(vol);
            nk_ble_notify_voice_volume(vol);
            nk_ble_notify_all_voice_configs();
            return 0;
        }

        case NK_CTRL_VOICE_SAY: {
            if (len < 2) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            char text[128] = {0};
            size_t tlen = len - 1;
            if (tlen >= sizeof(text)) tlen = sizeof(text) - 1;
            memcpy(text, &payload[1], tlen);
            text[tlen] = 0;
            ESP_LOGI(kTag, "CTRL test say: %s", text);
            voice_player_say(text);
            return 0;
        }

        case NK_CTRL_VOICE_SET_VOL: {
            if (len < 2) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            uint8_t vol = payload[1];
            ESP_LOGI(kTag, "CTRL set voice vol: %u", (unsigned)vol);
            voice_player_set_volume(vol);
            nk_voice_config_set_volume(vol);
            nk_ble_notify_voice_volume(vol);
            return 0;
        }

        case NK_CTRL_VOICE_TEST_QUANT: {
            if (len < 2) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            uint8_t mode = payload[1];
            char text[128] = {0};
            if (len > 2) {
                size_t tlen = len - 2;
                if (tlen >= sizeof(text)) tlen = sizeof(text) - 1;
                memcpy(text, &payload[2], tlen);
                text[tlen] = 0;
            }
            ESP_LOGI(kTag, "CTRL test quant: mode=%u text='%s'", (unsigned)mode, text);
            voice_player_play_test_quant(mode, text[0] != '\0' ? text : nullptr);
            return 0;
        }

        case NK_CTRL_VOICE_SAY_WAV: {
            if (len < 2) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            char text[128] = {0};
            size_t tlen = len - 1;
            if (tlen >= sizeof(text)) tlen = sizeof(text) - 1;
            memcpy(text, &payload[1], tlen);
            text[tlen] = 0;
            ESP_LOGI(kTag, "CTRL test say wav only: %s", text);
            voice_player_say_wav_only(text);
            return 0;
        }

        case NK_CTRL_NAME_SET: {
            // payload: [0x17][utf8_name\0]. This changes RAM only; NAME_SAVE
            // is the explicit NVS operation used by the simple settings page.
            if (len < 2) {
                nk_ble_notify_name_result(NK_NAME_OP_SET, ESP_ERR_INVALID_ARG);
                return 0;
            }
            const char *name = reinterpret_cast<const char *>(&payload[1]);
            const size_t name_len = strnlen(name, len - 1);
            esp_err_t err = (1 + name_len + 1 == len)
                ? nk_voice_config_set_name(name) : ESP_ERR_INVALID_ARG;
            if (err == ESP_OK) nk_ble_notify_name();
            nk_ble_notify_name_result(NK_NAME_OP_SET, err);
            return 0;
        }

        case NK_CTRL_NAME_SAVE: {
            // Accept an optional name so the UI can perform set+save as one
            // user action. An empty payload saves the current RAM name.
            esp_err_t err = ESP_OK;
            if (len > 1) {
                const char *name = reinterpret_cast<const char *>(&payload[1]);
                const size_t name_len = strnlen(name, len - 1);
                if (1 + name_len + 1 != len) {
                    err = ESP_ERR_INVALID_ARG;
                } else {
                    err = nk_voice_config_set_name(name);
                }
            }
            if (err == ESP_OK) {
                err = nk_voice_config_save_name();
            }
            // Always report the current RAM value as well as the result. On a
            // storage failure this lets the page keep the unsaved preview
            // distinct while offering a retry with the same name.
            nk_ble_notify_name();
            nk_ble_notify_name_result(NK_NAME_OP_SAVE, err);
            return 0;
        }

        case NK_CTRL_NAME_LOAD: {
            ESP_LOGI(kTag, "CTRL load name/config from NVS");
            const esp_err_t err = nk_voice_config_load();
            voice_player_set_volume(nk_voice_config_get_volume());
            nk_ble_notify_name();
            nk_ble_notify_voice_volume(nk_voice_config_get_volume());
            nk_ble_notify_name_result(NK_NAME_OP_LOAD, err);
            return 0;
        }

        case NK_CTRL_SWAP_NOD_TILT: {
            if (len != 2 || payload[1] > 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            const bool enabled = payload[1] != 0;
            nk_imu_set_swap_nod_tilt(enabled);
            nk_voice_config_set_swap_nod_tilt(enabled);
            const esp_err_t err = nk_voice_config_save();
            if (err != ESP_OK) {
                ESP_LOGW(kTag, "CTRL swap nod/tilt save failed: %s", esp_err_to_name(err));
            }
            nk_ble_notify_swap_nod_tilt();
            ESP_LOGI(kTag, "CTRL swap nod/tilt: %s", enabled ? "on" : "off");
            return 0;
        }

        case NK_CTRL_LISTEN_MODE: {
            if (len != 2 || !nk_voice_config_change_listen_mode(payload[1])) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            return 0;
        }

        case NK_CTRL_TIME_SYNC: {
            if (len != 11) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            uint64_t raw_unix_ms = 0;
            for (int i = 0; i < 8; ++i) {
                raw_unix_ms |= static_cast<uint64_t>(payload[1 + i]) << (8 * i);
            }
            const int16_t offset_minutes = static_cast<int16_t>(
                static_cast<uint16_t>(payload[9]) |
                (static_cast<uint16_t>(payload[10]) << 8));
            const esp_err_t err = nk_clock_sync(static_cast<int64_t>(raw_unix_ms), offset_minutes)
                ? ESP_OK : ESP_ERR_INVALID_ARG;
            nk_ble_notify_time_result(err);
            ESP_LOGI(kTag, "CTRL time sync: %s", esp_err_to_name(err));
            return 0;
        }

        case NK_CTRL_WEATHER_SET: {
            if (len != 11) {
                nk_ble_notify_weather_result(ESP_ERR_INVALID_ARG, nullptr);
                return 0;
            }
            const nk_weather_forecast_t forecast = {
                {static_cast<uint16_t>(payload[1] | (payload[2] << 8)), payload[3], payload[4], payload[5]},
                {static_cast<uint16_t>(payload[6] | (payload[7] << 8)), payload[8], payload[9], payload[10]},
            };
            const esp_err_t err = nk_weather_set_forecast(&forecast) ? ESP_OK : ESP_ERR_INVALID_ARG;
            nk_ble_notify_weather_result(err, &forecast);
            ESP_LOGI(kTag, "CTRL weather set %04u-%02u-%02u code=%u / %04u-%02u-%02u code=%u: %s",
                     forecast.today.year, forecast.today.month, forecast.today.day, forecast.today.code,
                     forecast.tomorrow.year, forecast.tomorrow.month, forecast.tomorrow.day,
                     forecast.tomorrow.code, esp_err_to_name(err));
            return 0;
        }

#ifdef NONKI_LISTEN_DIAGNOSTICS
        case NK_CTRL_LISTEN_DIAGNOSTIC:
            if (len != 2 || payload[1] > 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            return nk_imu_request_listen(payload[1] != 0) ? 0 : BLE_ATT_ERR_UNLIKELY;
#endif

        case NK_CTRL_WEATHER_GET:
            if (len != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            nk_ble_notify_weather_state();
            return 0;

        case NK_CTRL_IR_TEST: {
            if (len != 6) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            uint32_t request = 0;
            memcpy(&request, payload + 2, sizeof(request));
            const uint32_t generation = s_connection_generation.load();
            const esp_err_t err = nk_ir_request(payload[1], request, generation);
            if (err != ESP_OK) nk_ble_notify_ir_result(payload[1], request, err, generation);
            return 0;
        }

        case NK_CTRL_IR_HOLD: {
            if (len != 10) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            const uint16_t interval_ms = static_cast<uint16_t>(payload[4]) |
                (static_cast<uint16_t>(payload[5]) << 8);
            const uint32_t request = static_cast<uint32_t>(payload[6]) |
                (static_cast<uint32_t>(payload[7]) << 8) |
                (static_cast<uint32_t>(payload[8]) << 16) |
                (static_cast<uint32_t>(payload[9]) << 24);
            const uint32_t generation = s_connection_generation.load();
            const esp_err_t err = nk_ir_request_hold(payload[1], payload[2], payload[3],
                                                      interval_ms, request, generation);
            if (err != ESP_OK) nk_ble_notify_ir_result(payload[1], request, err, generation);
            return 0;
        }

        case NK_CTRL_VOICE_F0K: {
            // 声の高さの倍率 k。NVS（フラッシュ）を書くので、モデル書き込み中と発話中は受け付けない。
            if (len != 1 && len != 5) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            if (nk_model_writer_busy() || voice_player_is_playing()) return BLE_ATT_ERR_UNLIKELY;
            esp_err_t err;
            if (len == 1) {
                err = sano_voice_reset_k();
            } else {
                float k32;
                memcpy(&k32, payload + 1, sizeof(k32));
                err = sano_voice_set_k(static_cast<double>(k32));
            }
            ESP_LOGI(kTag, "CTRL voice k: %s", esp_err_to_name(err));
            return err == ESP_OK ? 0 : BLE_ATT_ERR_UNLIKELY;
        }

        default:
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
}

static int notify_only_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)ctxt; (void)arg;
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

// C++ の指示付き初期化は宣言順を守る必要がある
// (ble_gatt_chr_def: uuid, access_cb, arg, descriptors, flags, min_key_size, val_handle, cpfd)
static const struct ble_gatt_chr_def kChrs[] = {
    {
        .uuid = &kStreamUuid.u,
        .access_cb = notify_only_access,
        .flags = BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_stream_handle,
    },
    {
        .uuid = &kCtrlUuid.u,
        .access_cb = ctrl_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid = &kStatusUuid.u,
        .access_cb = notify_only_access,
        .flags = BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_status_handle,
    },
    {
        .uuid = &kInfoUuid.u,
        .access_cb = info_access,
        .flags = BLE_GATT_CHR_F_READ,
    },
    {
        .uuid = &kModelUuid.u,
        .access_cb = model_access,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid = &kModelStateUuid.u,
        .access_cb = model_state_access,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_model_state_handle,
    },
    { 0 },
};

static const struct ble_gatt_svc_def kSvcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kSvcUuid.u,
        .characteristics = kChrs,
    },
    { 0 },
};

// ---------------------------------------------------------------- GAP

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                s_conn_handle = event->connect.conn_handle;
                ESP_LOGI(kTag, "connected (handle=%d)", s_conn_handle);
            } else {
                ESP_LOGW(kTag, "connect failed status=%d", event->connect.status);
                start_advertising();
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(kTag, "disconnected reason=%d", event->disconnect.reason);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_connection_generation.fetch_add(1);
            nk_model_writer_disconnected(s_connection_generation.load());
            s_model_subscribed = false;
            s_stream_subscribed = false;
            nk_imu_set_streaming(false);
            start_advertising();
            return 0;

        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.cur_notify != 0) {
                s_conn_handle = event->subscribe.conn_handle;
            }
            if (event->subscribe.attr_handle == s_stream_handle) {
                s_stream_subscribed = event->subscribe.cur_notify != 0;
                ESP_LOGI(kTag, "stream notify %s", s_stream_subscribed ? "on" : "off");
            }
            if (event->subscribe.attr_handle == s_model_state_handle)
                s_model_subscribed = event->subscribe.cur_notify != 0;
            if (event->subscribe.attr_handle == s_status_handle &&
                event->subscribe.cur_notify != 0) {
                // STATUS はイベントが発生するまで何も送らないため、接続時点で
                // sleep / 較正 / 発声中を画面が知れるよう初回スナップショットを返す。
                nk_ble_notify_status_detector_state();
                nk_ble_notify_status_posture();
                nk_ble_notify_swap_nod_tilt();
                nk_ble_notify_listen_mode();
            }
            return 0;

        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(kTag, "MTU=%d -> %u samples/pkt",
                     event->mtu.value, (unsigned)nk_ble_max_samples_per_pkt());
            return 0;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            start_advertising();
            return 0;

        default:
            return 0;
    }
}

static void start_advertising(void)
{
    // adv には 128bit サービス UUID を載せる (ブラウザの
    // requestDevice({filters:[{services:[...]}]}) がこれを見る)。
    // 31 byte に名前が同居できないので、名前は scan response に回す。
    struct ble_hs_adv_fields adv = {};
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.uuids128 = (ble_uuid128_t *)&kSvcUuid;
    adv.num_uuids128 = 1;
    adv.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&adv);
    if (rc != 0) {
        ESP_LOGE(kTag, "adv_set_fields rc=%d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp = {};
    rsp.name = (const uint8_t *)kDeviceName;
    rsp.name_len = (uint8_t)strlen(kDeviceName);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(kTag, "adv_rsp_set_fields rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params params = {};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(kTag, "adv_start rc=%d", rc);
        return;
    }
    ESP_LOGI(kTag, "advertising as %s", kDeviceName);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(kTag, "ensure_addr rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(kTag, "infer_auto rc=%d", rc);
        return;
    }
    start_advertising();
}

static void on_reset(int reason)
{
    ESP_LOGW(kTag, "nimble reset reason=%d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ---------------------------------------------------------------- API

esp_err_t nk_ble_init(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(kSvcs);
    if (rc != 0) {
        ESP_LOGE(kTag, "gatts_count_cfg rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(kSvcs);
    if (rc != 0) {
        ESP_LOGE(kTag, "gatts_add_svcs rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_svc_gap_device_name_set(kDeviceName);
    if (rc != 0) {
        ESP_LOGE(kTag, "device_name_set rc=%d", rc);
        return ESP_FAIL;
    }

    // BLE controller の内部 DRAM 確保を先に済ませてから常駐タスクを作る。
    const esp_err_t model_err = nk_model_writer_init();
    if (model_err != ESP_OK) ESP_LOGW(kTag, "モデル書き込みを使えません: %s", esp_err_to_name(model_err));
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

void nk_ble_notify_model_state(const uint8_t *data, size_t len, uint32_t generation)
{
    if (generation != s_connection_generation.load() || s_conn_handle == BLE_HS_CONN_HANDLE_NONE ||
        !s_model_subscribed) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (om) ble_gatts_notify_custom(s_conn_handle, s_model_state_handle, om);
}

void nk_ble_model_transfer_mode(bool active, uint32_t generation)
{
    if (generation != s_connection_generation.load() || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    // 転送中だけ短い接続間隔を要求し、終了時は元の値へ戻す。採否は PC 側が決める。
    static ble_gap_upd_params normal{};
    static uint32_t saved_generation = UINT32_MAX;
    ble_gap_upd_params params{};
    if (active) {
        ble_gap_conn_desc desc{};
        if (ble_gap_conn_find(s_conn_handle, &desc) != 0) return;
        normal.itvl_min = normal.itvl_max = desc.conn_itvl;
        normal.latency = desc.conn_latency;
        normal.supervision_timeout = desc.supervision_timeout;
        saved_generation = generation;
        params.itvl_min = 6; params.itvl_max = 12; // 7.5〜15 ms
        params.latency = 0; params.supervision_timeout = 600;
        const int data_rc = ble_gap_set_data_len(s_conn_handle, 251, 2120);
        if (data_rc) ESP_LOGI(kTag, "モデル転送のデータ長要求: 結果=%d", data_rc);
    } else {
        if (saved_generation != generation) return;
        params = normal;
    }
    const int rc = ble_gap_update_params(s_conn_handle, &params);
    if (rc) ESP_LOGI(kTag, "モデル転送の接続間隔要求: 結果=%d", rc);
}

bool nk_ble_stream_ready(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE && s_stream_subscribed;
}

uint8_t nk_ble_max_samples_per_pkt(void)
{
    uint16_t mtu = 23;
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        const uint16_t m = ble_att_mtu(s_conn_handle);
        if (m > mtu) mtu = m;
    }
    const int payload = (int)mtu - 3;
    int n = (payload - NK_PKT_HEADER_BYTES) / NK_SAMPLE_BYTES;
    if (n < 1) n = 1;
    if (n > NK_MAX_SAMPLES_PER_PKT) n = NK_MAX_SAMPLES_PER_PKT;
    return (uint8_t)n;
}

bool nk_ble_notify_stream(const uint8_t *data, size_t len)
{
    if (!nk_ble_stream_ready()) {
        return false;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, (uint16_t)len);
    if (om == NULL) {
        return false;  // msys 枯渇。呼び手が dropped として数える
    }
    return ble_gatts_notify_custom(s_conn_handle, s_stream_handle, om) == 0;
}

static void notify_status(const uint8_t *data, size_t len)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_status_handle == 0) {
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, (uint16_t)len);
    if (om != NULL) {
        ble_gatts_notify_custom(s_conn_handle, s_status_handle, om);
    }
}

void nk_ble_notify_ir_result(uint8_t direction, uint32_t request, int32_t err, uint32_t generation)
{
    if (generation != s_connection_generation.load()) return;
    uint8_t buf[10] = {NK_STATUS_IR_RESULT, direction};
    memcpy(buf + 2, &request, sizeof(request));
    memcpy(buf + 6, &err, sizeof(err));
    notify_status(buf, sizeof(buf));
}

void nk_ble_notify_status_beep(uint32_t seq, uint32_t t_us, uint16_t freq_hz, uint16_t ms)
{
    uint8_t buf[13];
    buf[0] = NK_STATUS_BEEP;
    memcpy(&buf[1], &seq, 4);
    memcpy(&buf[5], &t_us, 4);
    memcpy(&buf[9], &freq_hz, 2);
    memcpy(&buf[11], &ms, 2);
    notify_status(buf, sizeof(buf));
}

void nk_ble_notify_status_marker(uint32_t marker_id, uint32_t seq, uint32_t t_us)
{
    uint8_t buf[13];
    buf[0] = NK_STATUS_MARKER;
    memcpy(&buf[1], &marker_id, 4);
    memcpy(&buf[5], &seq, 4);
    memcpy(&buf[9], &t_us, 4);
    notify_status(buf, sizeof(buf));
}

void nk_ble_notify_status_state(void)
{
    const uint16_t odr = nk_imu_get_odr();
    const uint32_t dropped = nk_imu_dropped();
    uint8_t buf[8];
    buf[0] = NK_STATUS_STATE;
    buf[1] = nk_imu_is_streaming() ? 1 : 0;
    memcpy(&buf[2], &odr, 2);
    memcpy(&buf[4], &dropped, 4);
    notify_status(buf, sizeof(buf));
}

void nk_ble_notify_status_motion(uint8_t event_id, uint32_t seq, uint32_t t_us)
{
    uint8_t buf[10];
    buf[0] = NK_STATUS_MOTION;
    buf[1] = event_id;
    memcpy(&buf[2], &seq, 4);
    memcpy(&buf[6], &t_us, 4);
    notify_status(buf, sizeof(buf));
}

void nk_ble_notify_status_detector_state(void)
{
    uint8_t buf[9];
    const uint32_t seq = nk_imu_seq();
    buf[0] = NK_STATUS_DETECTOR_STATE;
    buf[1] = nk_imu_motion_state();
    buf[2] = nk_imu_is_sleeping() ? 1 : 0;
    buf[3] = nk_imu_is_suspended() ? 1 : 0;
    buf[4] = voice_player_is_playing() ? 1 : 0;
    memcpy(&buf[5], &seq, sizeof(seq));
    notify_status(buf, sizeof(buf));
}

void nk_ble_notify_status_posture(void)
{
    uint8_t buf[8];
    const uint16_t angle_cdeg = nk_imu_posture_angle_cdeg();
    const uint32_t seq = nk_imu_seq();
    buf[0] = NK_STATUS_POSTURE;
    buf[1] = nk_imu_posture_state();
    memcpy(&buf[2], &angle_cdeg, sizeof(angle_cdeg));
    memcpy(&buf[4], &seq, sizeof(seq));
    notify_status(buf, sizeof(buf));
}

void nk_ble_notify_swap_nod_tilt(void)
{
    const uint8_t buf[2] = {
        NK_STATUS_SWAP_NOD_TILT,
        static_cast<uint8_t>(nk_imu_swap_nod_tilt() ? 1 : 0),
    };
    notify_status(buf, sizeof(buf));
}

void nk_ble_notify_listen_mode(void)
{
    const uint8_t buf[2] = {
        NK_STATUS_LISTEN_MODE,
        nk_voice_config_get_listen_mode(),
    };
    notify_status(buf, sizeof(buf));
}

void nk_ble_notify_intent(uint8_t event_id, uint32_t seq, uint8_t intent, uint8_t reason,
                          uint8_t raw1, uint8_t raw2, float score1, float score2)
{
    // 製品の意図は 0..16 のまま。stt32 の 18 番目（起動語 のんきちゃん）は製品の意図に写さないのでここに来ない（stt32 DLS-1082）。
    if (reason > 3 || (reason == 0 ? intent > 16 : intent != 255)
        || (raw1 > 16 && raw1 != 255) || (raw2 > 16 && raw2 != 255)) return;
    const auto per_mille = [](float score) -> uint16_t {
        if (!(score > 0.0f)) return 0; // Also handles NaN.
        if (score >= 1.0f) return 1000;
        return static_cast<uint16_t>(score * 1000.0f + 0.5f);
    };
    uint8_t packet[14] = {NK_STATUS_INTENT, event_id};
    for (unsigned i = 0; i < 4; ++i) packet[2 + i] = static_cast<uint8_t>(seq >> (8 * i));
    packet[6] = intent;
    packet[7] = reason;
    packet[8] = raw1;
    packet[9] = raw2;
    const uint16_t scores[] = {per_mille(score1), per_mille(score2)};
    for (unsigned i = 0; i < 2; ++i) {
        packet[10 + 2 * i] = static_cast<uint8_t>(scores[i]);
        packet[11 + 2 * i] = static_cast<uint8_t>(scores[i] >> 8);
    }
    notify_status(packet, sizeof(packet));
}

void nk_ble_notify_motion_speech(uint8_t event_id, uint32_t seq, const char *text, bool accepted)
{
    if (text == nullptr) text = "";
    const size_t len = strnlen(text, NK_SPEECH_TEXT_MAX_BYTES + 1);
    if (len > NK_SPEECH_TEXT_MAX_BYTES) return;
    const size_t total = len == 0 ? 1 : (len + NK_SPEECH_NOTIFY_CHUNK_BYTES - 1) / NK_SPEECH_NOTIFY_CHUNK_BYTES;
    for (size_t part = 0; part < total; ++part) {
        const size_t offset = part * NK_SPEECH_NOTIFY_CHUNK_BYTES;
        const size_t remaining = len - offset;
        const size_t chunk = remaining < NK_SPEECH_NOTIFY_CHUNK_BYTES ? remaining : NK_SPEECH_NOTIFY_CHUNK_BYTES;
        uint8_t packet[9 + NK_SPEECH_NOTIFY_CHUNK_BYTES];
        packet[0] = NK_STATUS_MOTION_SPEECH;
        packet[1] = event_id;
        for (unsigned i = 0; i < 4; ++i) packet[2 + i] = static_cast<uint8_t>(seq >> (8 * i));
        packet[6] = len == 0 ? 0 : (accepted ? 1 : 2);
        packet[7] = static_cast<uint8_t>(part);
        packet[8] = static_cast<uint8_t>(total);
        memcpy(packet + 9, text + offset, chunk);
        notify_status(packet, 9 + chunk);
        // Let NimBLE drain its notification queue, including on MTU=23 peers.
        if (part + 1 < total) vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void nk_ble_notify_all_voice_configs(void)
{
    const size_t total = nk_voice_config_count();
    for (size_t i = 0; i < total; ++i) {
        const nk_action_voice_config_t *cfg = nk_voice_config_get_at(i);
        if (!cfg) continue;
        uint8_t buf[128];
        buf[0] = NK_STATUS_VOICE_CFG_ITEM;
        buf[1] = (uint8_t)i;
        buf[2] = (uint8_t)total;
        size_t klen = strlen(cfg->key_name);
        size_t tlen = strlen(cfg->text);
        if (3 + klen + 1 + tlen + 1 > sizeof(buf)) continue;
        memcpy(&buf[3], cfg->key_name, klen + 1);
        memcpy(&buf[3 + klen + 1], cfg->text, tlen + 1);
        notify_status(buf, 3 + klen + 1 + tlen + 1);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void nk_ble_notify_voice_volume(uint8_t volume)
{
    uint8_t buf[2];
    buf[0] = NK_STATUS_VOICE_VOL;
    buf[1] = volume;
    notify_status(buf, sizeof(buf));
}

void nk_ble_notify_name(void)
{
    const char *name = nk_voice_config_get_name();
    const size_t len = name != nullptr ? strlen(name) : 0;
    const size_t total = (len + NK_NAME_NOTIFY_CHUNK_BYTES - 1)
                       / NK_NAME_NOTIFY_CHUNK_BYTES;
    if (total == 0 || total > 255) return;

    for (size_t part = 0; part < total; ++part) {
        const size_t offset = part * NK_NAME_NOTIFY_CHUNK_BYTES;
        const size_t chunk = (len - offset > NK_NAME_NOTIFY_CHUNK_BYTES)
                           ? NK_NAME_NOTIFY_CHUNK_BYTES : len - offset;
        uint8_t buf[3 + NK_NAME_NOTIFY_CHUNK_BYTES];
        buf[0] = NK_STATUS_NAME;
        buf[1] = (uint8_t)part;
        buf[2] = (uint8_t)total;
        memcpy(&buf[3], name + offset, chunk);
        notify_status(buf, 3 + chunk);
        if (part + 1 < total) vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void nk_ble_notify_name_result(uint8_t operation, int32_t err)
{
    uint8_t buf[6];
    const uint32_t code = static_cast<uint32_t>(err);
    buf[0] = NK_STATUS_NAME_RESULT;
    buf[1] = operation;
    memcpy(&buf[2], &code, sizeof(code));
    notify_status(buf, sizeof(buf));
}

void nk_ble_notify_time_result(int32_t err)
{
    uint8_t buf[13];
    const uint32_t code = static_cast<uint32_t>(err);
    const int64_t local_ms = err == ESP_OK ? nk_clock_now_local_ms() : -1;
    buf[0] = NK_STATUS_TIME_RESULT;
    memcpy(&buf[1], &code, sizeof(code));
    memcpy(&buf[5], &local_ms, sizeof(local_ms));
    notify_status(buf, sizeof(buf));
}

static void put_weather_date_code(uint8_t *buf, const nk_weather_t &weather)
{
    buf[0] = static_cast<uint8_t>(weather.year);
    buf[1] = static_cast<uint8_t>(weather.year >> 8);
    buf[2] = weather.month;
    buf[3] = weather.day;
    buf[4] = weather.code;
}

void nk_ble_notify_weather_result(int32_t err, const nk_weather_forecast_t *forecast)
{
    uint8_t buf[15] = {NK_STATUS_WEATHER_RESULT};
    const uint32_t error_code = static_cast<uint32_t>(err);
    memcpy(&buf[1], &error_code, sizeof(error_code));
    if (err == ESP_OK && forecast != nullptr) {
        put_weather_date_code(&buf[5], forecast->today);
        put_weather_date_code(&buf[10], forecast->tomorrow);
    }
    notify_status(buf, sizeof(buf));
}

void nk_ble_notify_weather_state(void)
{
    uint8_t buf[13] = {NK_STATUS_WEATHER_STATE};
    nk_weather_forecast_t current;
    bool today_known, tomorrow_known;
    nk_weather_get_current(&current, &today_known, &tomorrow_known);
    if (today_known) {
        buf[1] = 1;
        put_weather_date_code(&buf[2], current.today);
    }
    if (tomorrow_known) {
        buf[7] = 1;
        put_weather_date_code(&buf[8], current.tomorrow);
    }
    notify_status(buf, sizeof(buf));
}

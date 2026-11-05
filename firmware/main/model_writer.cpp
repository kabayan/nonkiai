#include "model_writer.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "model_write_session.h"
#include "model_info_json.h"
#include "sano_voice.h"
#include "task_stack_sizes.h"
#include "voice_params.h"
#include "voice_player.h"

namespace {
constexpr char kTag[] = "nk-model";
constexpr int64_t kIdleTimeoutUs = 30000000;
constexpr size_t kSectorBytes = 4096;
struct Request {
    uint32_t generation;
    uint16_t len;
    uint8_t data[NK_MODEL_PACKET_BYTES];
};
QueueHandle_t s_queue;
const esp_partition_t *s_model_part, *s_dict_part;
const esp_partition_t *s_partition;   // 今の（直近の）書き込み先。BEGIN で決まる
std::atomic<uint8_t> s_target{NK_MODEL_TARGET_MODEL};
// 辞書の消去を始めた後は、再起動まで true のまま。辞書は起動時に mmap されて発話が読み続けるので、
// 消去・書き込み中はもちろん、書き終えても開き直す（再起動する）まで音声の占有を手放さない。
std::atomic<bool> s_dict_dirty{false};
struct Probe { uint32_t request; uint8_t target; uint32_t bytes; char sha[65]; } s_probe{};
nk::ModelWriteSession s_session;
std::atomic<uint32_t> s_generation{0}, s_abort_session{0};
std::atomic<bool> s_busy{false};
uint32_t s_owner_generation;
int64_t s_last_activity;
portMUX_TYPE s_snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
uint8_t s_snapshot[NK_MODEL_STATE_BYTES] = {NK_MODEL_WIRE_VERSION};
char s_actual_sha[65]{};
uint32_t s_hash_bytes;
uint8_t s_prefix[4]{};
bool s_prefix_committed;

constexpr size_t kDictEraseBytes = 65536;   // dict は 64 KB ブロック消去を使えるよう 64 KB ずつ消す
const esp_partition_t *part_of(uint8_t target) {
    return target == NK_MODEL_TARGET_MODEL ? s_model_part : target == NK_MODEL_TARGET_DICT ? s_dict_part : nullptr;
}
// model 領域の末尾 1 セクタは声ごとの値の記録（voice_params.h）。モデルとして受け付けるのはその手前まで。
// dict 領域は全域が辞書（記録を置く場所は無い。照合は端末が返す SHA-256 で行う）。
size_t capacity_of(uint8_t target) {
    const esp_partition_t *part = part_of(target);
    if (!part) return 0;
    return target == NK_MODEL_TARGET_MODEL ? part->size - NK_VP_SECTOR_BYTES : part->size;
}

uint32_t read32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
void write32(uint8_t *p, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(n >> (8 * i));
}
void publish(uint8_t error, uint8_t op, uint32_t request = 0) {
    uint8_t out[NK_MODEL_STATE_BYTES] = {NK_MODEL_WIRE_VERSION, s_session.state, error, op};
    write32(out + 4, s_session.session);
    write32(out + 8, s_session.next);
    write32(out + 12, s_session.size);
    write32(out + 16, request);
    portENTER_CRITICAL(&s_snapshot_mux);
    std::memcpy(s_snapshot, out, sizeof(out));
    portEXIT_CRITICAL(&s_snapshot_mux);
    nk_ble_notify_model_state(out, sizeof(out), s_owner_generation);
}
void release_audio() {
    if (!s_busy.load()) return;
    // 辞書を消し始めた後は手放さない。発話は起動時に開いた辞書の見出し情報で flash を読むので、
    // 中身が変わった（または消えた）辞書を読ませない。手放すのは再起動で開き直したとき。
    if (!s_dict_dirty.load()) {
        s_busy.store(false);
        voice_player_model_write_release();
    }
    nk_ble_model_transfer_mode(false, s_owner_generation);
}
void stop(uint8_t error, uint8_t op) {
    // 有効な magic は全データの予備照合後にだけ書く。最終照合で失敗した
    // 場合も、不完全な重みを次の起動で読ませない。
    if (s_session.active() && s_prefix_committed) {
        const uint8_t invalid[4]{};
        if (esp_partition_write(s_partition, 0, invalid, std::min<size_t>(4, s_session.size)) != ESP_OK)
            ESP_LOGW(kTag, "失敗したモデルの先頭を無効にできませんでした");
        s_prefix_committed = false;
    }
    s_session.fail(error == NK_MODEL_ERR_CANCELLED || error == NK_MODEL_ERR_DISCONNECTED);
    release_audio();
    publish(error, op);
    ESP_LOGW(kTag, "書き込みを終了: 理由=%u 確定=%lu/%lu B", error,
             (unsigned long)s_session.next, (unsigned long)s_session.size);
}
uint8_t cancellation() {
    if (s_owner_generation != s_generation.load()) return NK_MODEL_ERR_DISCONNECTED;
    if (s_abort_session.load() == s_session.session) return NK_MODEL_ERR_CANCELLED;
    return NK_MODEL_OK;
}
bool check_cancel(uint8_t op) {
    const uint8_t error = cancellation();
    if (!error) return false;
    stop(error, op);
    return true;
}

void begin(const Request &r) {
    const uint32_t id = read32(r.data + 1), bytes = read32(r.data + 5);
    const uint8_t target = r.data[41];
    const esp_partition_t *part = part_of(target);
    // 領域が無い・target が範囲外。どちらでも別の領域には書かない。
    if (!part) { publish(NK_MODEL_ERR_TARGET, NK_MODEL_BEGIN); return; }
    nk::ModelWriteSession candidate = s_session;
    const uint8_t error = candidate.begin(id, bytes, capacity_of(target), r.data + 9);
    if (error) { publish(error, NK_MODEL_BEGIN); return; }
    // 同じ音声ロックの下でアイドル判定と占有を行い、タップとの競合を防ぐ。
    // 辞書を書いた後に占有したまま残っているとき（s_busy）は、取り直さずにそのまま使う。
    if (!s_busy.load() && !voice_player_model_write_acquire()) { publish(NK_MODEL_ERR_BUSY, NK_MODEL_BEGIN); return; }
    s_busy.store(true);
    s_partition = part; s_target.store(target);
    s_session = candidate;
    s_prefix_committed = false;
    std::memset(s_prefix, 0xff, sizeof(s_prefix));
    s_owner_generation = r.generation;
    nk_ble_model_transfer_mode(true, s_owner_generation);
    portENTER_CRITICAL(&s_snapshot_mux);
    s_actual_sha[0] = '\0'; s_hash_bytes = 0;
    portEXIT_CRITICAL(&s_snapshot_mux);
    publish(NK_MODEL_OK, NK_MODEL_BEGIN);
    ESP_LOGI(kTag, "%s消去を開始: %lu B", target == NK_MODEL_TARGET_DICT ? "辞書" : "モデル", (unsigned long)bytes);
    // モデル: 末尾の古い重みと声の値の記録も消すため全域を 4 KB ずつ消す。
    // 辞書: 書く範囲（4 KB 切り上げ）だけを 64 KB ずつ消す。先頭が消えた時点で、辞書は開けない（K1D1 が無い）。
    const bool dict = target == NK_MODEL_TARGET_DICT;
    if (dict) s_dict_dirty.store(true);
    const size_t erase_end = dict ? (size_t(bytes) + kSectorBytes - 1) / kSectorBytes * kSectorBytes : s_partition->size;
    const size_t erase_step = dict ? kDictEraseBytes : kSectorBytes;
    // 各ブロックの間で切断・中止を検査する。
    for (size_t offset = 0; offset < erase_end; offset += erase_step) {
        if (check_cancel(NK_MODEL_BEGIN)) return;
        if (esp_partition_erase_range(s_partition, offset, std::min(erase_step, erase_end - offset)) != ESP_OK) {
            stop(NK_MODEL_ERR_FLASH, NK_MODEL_BEGIN); return;
        }
        vTaskDelay(1);
    }
    if (check_cancel(NK_MODEL_BEGIN)) return;
    s_session.erased();
    s_last_activity = esp_timer_get_time();
    publish(NK_MODEL_OK, NK_MODEL_BEGIN);
    ESP_LOGI(kTag, "消去完了、モデルの受信を開始");
}

int partition_sha(uint8_t digest[32], bool staged_prefix) {
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    int rc = mbedtls_sha256_starts(&sha, 0);
    uint8_t buffer[1024];
    for (size_t offset = 0; !rc && offset < s_session.size; offset += sizeof(buffer)) {
        if (check_cancel(NK_MODEL_END)) { mbedtls_sha256_free(&sha); return -2; }
        const size_t count = std::min(sizeof(buffer), size_t(s_session.size - offset));
        if (esp_partition_read(s_partition, offset, buffer, count) != ESP_OK) { rc = -1; break; }
        if (staged_prefix && offset == 0) std::memcpy(buffer, s_prefix, std::min(sizeof(s_prefix), count));
        rc = mbedtls_sha256_update(&sha, buffer, count);
        if (offset % (16 * sizeof(buffer)) == 0) vTaskDelay(1);
    }
    if (!rc) rc = mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    return rc;
}

void finish() {
    uint8_t error = s_session.start_verify(s_session.session);
    if (error) { publish(error, NK_MODEL_END); return; }
    publish(NK_MODEL_OK, NK_MODEL_END);
    uint8_t digest[32];
    int rc = partition_sha(digest, true);
    if (rc == -2) return;
    if (rc) { stop(NK_MODEL_ERR_FLASH, NK_MODEL_END); return; }
    if (check_cancel(NK_MODEL_END)) return;
    // 途中の電源断でも SAAN magic が残らないよう、本文が期待値と
    // 一致してから先頭 4 B を書く。その後 Flash 全体をもう一度照合する。
    if (std::memcmp(digest, s_session.expected, sizeof(digest)) == 0) {
        // 辞書は先頭の K1D1 が無ければ jdict_open が開けない。本文が期待値と一致していても、
        // 辞書の形でないものに有効な先頭を書かない（先頭は消去のままなので、起動時に辞書として使われない）。
        if (s_target.load() == NK_MODEL_TARGET_DICT && std::memcmp(s_prefix, "K1D1", 4) != 0) {
            stop(NK_MODEL_ERR_TARGET, NK_MODEL_END); return;
        }
        s_prefix_committed = true;
        if (esp_partition_write(s_partition, 0, s_prefix, std::min(sizeof(s_prefix), size_t(s_session.size))) != ESP_OK) {
            stop(NK_MODEL_ERR_FLASH, NK_MODEL_END); return;
        }
    }
    rc = partition_sha(digest, false);
    if (rc == -2) return;
    if (rc) { stop(NK_MODEL_ERR_FLASH, NK_MODEL_END); return; }
    if (check_cancel(NK_MODEL_END)) return;
    char hex[65];
    for (size_t i = 0; i < sizeof(digest); ++i) std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    portENTER_CRITICAL(&s_snapshot_mux);
    std::memcpy(s_actual_sha, hex, sizeof(hex)); s_hash_bytes = s_session.size;
    portEXIT_CRITICAL(&s_snapshot_mux);
    if (std::memcmp(digest, s_session.expected, sizeof(digest)) != 0) {
        stop(NK_MODEL_ERR_HASH, NK_MODEL_END); return;
    }
    error = s_session.verified(digest);
    release_audio();
    publish(error, NK_MODEL_END);
    ESP_LOGI(kTag, "SHA-256 照合%s: %s", error ? "不一致" : "一致", hex);
    ESP_LOGI(kTag, "[STACK] nk_model size=%u min_free=%u", (unsigned)nk::kModelWriterStackBytes,
             (unsigned)uxTaskGetStackHighWaterMark(nullptr));
}

// END の照合成功後・REBOOT 前。書いたモデルの SHA-256 と値を組にして末尾セクタへ記録する。
void write_params(const Request &r) {
    const uint8_t op = NK_MODEL_PARAMS;
    // 声の値の記録は model 領域の末尾にしか置けない。辞書の書き込みの後には使えない。
    if (s_target.load() != NK_MODEL_TARGET_MODEL || !s_model_part) { publish(NK_MODEL_ERR_TARGET, op); return; }
    if (!s_session.can_reboot(s_session.session)) { publish(NK_MODEL_ERR_NOT_VERIFIED, op); return; }
    if (std::memcmp(r.data + 5, s_session.expected, 32) != 0) { publish(NK_MODEL_ERR_HASH, op); return; }
    nk_voice_values_t v;
    v.k = nk_vp_getd(r.data + 37); v.center_hz = nk_vp_getd(r.data + 45); v.lo_hz = nk_vp_getd(r.data + 53);
    v.hi_hz = nk_vp_getd(r.data + 61); v.tau_ms = nk_vp_getd(r.data + 69);
    const uint32_t vn = read32(r.data + 77); std::memcpy(&v.vnoise, &vn, 4);
    uint8_t record[NK_VP_RECORD_BYTES];
    if (!nk_vp_encode(record, s_session.size, s_session.expected, &v)) { publish(NK_MODEL_ERR_PACKET, op); return; }
    const size_t offset = capacity_of(NK_MODEL_TARGET_MODEL);
    uint8_t back[NK_VP_RECORD_BYTES];
    // 消去の間は Flash のキャッシュが止まるので、BEGIN と同じく音声を占有してから消す（発話中なら BUSY で断る）。
    // 辞書の書き込みの後で占有したままのとき（s_busy）は、取り直さずにそのまま使い、手放さない。
    const bool held = s_busy.load();
    if (!held && !voice_player_model_write_acquire()) { publish(NK_MODEL_ERR_BUSY, op); return; }
    // 2 回目の PARAMS でも書けるよう、毎回セクタを消してから書く。
    const bool ok = esp_partition_erase_range(s_model_part, offset, kSectorBytes) == ESP_OK &&
        esp_partition_write(s_model_part, offset, record, sizeof(record)) == ESP_OK &&
        esp_partition_read(s_model_part, offset, back, sizeof(back)) == ESP_OK &&
        std::memcmp(record, back, sizeof(record)) == 0;
    if (!held) voice_player_model_write_release();
    if (!ok) {
        ESP_LOGW(kTag, "声の値の記録に失敗");
        publish(NK_MODEL_ERR_FLASH, op); return;
    }
    ESP_LOGI(kTag, "声の値を記録: k=%.4f vnoise=%.4f", v.k, (double)v.vnoise);
    publish(NK_MODEL_OK, op);
}

// 書き込み前の照合。書き込み先の先頭 bytes の SHA-256 を計算して probe に置く（session・音声の占有には触らない）。
void probe(const Request &r) {
    const uint8_t op = NK_MODEL_PROBE;
    const uint32_t request = read32(r.data + 1), bytes = read32(r.data + 6);
    const uint8_t target = r.data[5];
    if (!s_session.active()) s_owner_generation = r.generation;
    if (!request) { publish(NK_MODEL_ERR_PACKET, op); return; }
    // 書き込みの最中に別の読み出しを混ぜない（消去中の領域を読んだ値を、画面が「違う」と読む）。
    if (s_session.active()) { publish(NK_MODEL_ERR_BUSY, op, request); return; }
    const esp_partition_t *part = part_of(target);
    if (!part) { publish(NK_MODEL_ERR_TARGET, op, request); return; }
    if (!bytes || bytes > capacity_of(target)) { publish(NK_MODEL_ERR_SIZE, op, request); return; }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    int rc = mbedtls_sha256_starts(&sha, 0);
    uint8_t buffer[1024];
    for (size_t offset = 0; !rc && offset < bytes; offset += sizeof(buffer)) {
        // 切断された接続の問い合わせは捨てる（結果を読む相手がいない）。
        if (r.generation != s_generation.load()) { mbedtls_sha256_free(&sha); return; }
        const size_t count = std::min(sizeof(buffer), size_t(bytes - offset));
        if (esp_partition_read(part, offset, buffer, count) != ESP_OK) { rc = -1; break; }
        rc = mbedtls_sha256_update(&sha, buffer, count);
        if (offset % (16 * sizeof(buffer)) == 0) vTaskDelay(1);
    }
    uint8_t digest[32];
    if (!rc) rc = mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    if (rc) { publish(NK_MODEL_ERR_FLASH, op, request); return; }
    Probe p{request, target, bytes, {}};
    for (size_t i = 0; i < sizeof(digest); ++i) std::snprintf(p.sha + i * 2, 3, "%02x", digest[i]);
    portENTER_CRITICAL(&s_snapshot_mux);
    s_probe = p;
    portEXIT_CRITICAL(&s_snapshot_mux);
    publish(NK_MODEL_OK, op, request);
}

void worker(void *) {
    Request r{};
    unsigned chunks_since_ack = 0;
    for (;;) {
        if (s_session.active() && check_cancel(NK_MODEL_ABORT)) chunks_since_ack = 0;
        if (s_session.active() && esp_timer_get_time() - s_last_activity > kIdleTimeoutUs)
            stop(NK_MODEL_ERR_TIMEOUT, NK_MODEL_ABORT);
        if (xQueueReceive(s_queue, &r, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        if (r.generation != s_generation.load()) continue;
        const uint8_t op = r.data[0];
        if (op == NK_MODEL_BEGIN) {
            begin(r); chunks_since_ack = 0; continue;
        }
        if (op == NK_MODEL_PROBE) { probe(r); continue; }
        const uint32_t id = read32(r.data + 1);
        // 切断後の QUERY / 再起動は新しい接続へ返す。未完了の転送は再開しない。
        if (!s_session.active()) s_owner_generation = r.generation;
        const uint32_t request = op == NK_MODEL_QUERY ? read32(r.data + 5) : 0;
        if (id != s_session.session || !id) { publish(NK_MODEL_ERR_SESSION, op, request); continue; }
        s_last_activity = esp_timer_get_time();
        if (op == NK_MODEL_ABORT) {
            if (s_session.active() || s_session.can_reboot(id)) stop(NK_MODEL_ERR_CANCELLED, op);
            else publish(NK_MODEL_OK, op);
        } else if (op == NK_MODEL_QUERY) {
            // この通知より前の DATA は処理済み。欠落した最初の位置から再送できる。
            publish(NK_MODEL_OK, op, request); chunks_since_ack = 0;
        } else if (op == NK_MODEL_DATA) {
            const uint32_t offset = read32(r.data + 5);
            const size_t count = r.len - NK_MODEL_DATA_HEADER_BYTES;
            const uint8_t error = s_session.check_data(id, offset, count);
            if (error) { publish(error, op); continue; }
            if (offset == s_session.next) {
                const size_t prefix_bytes = offset < sizeof(s_prefix) ? std::min<size_t>(count, sizeof(s_prefix) - offset) : 0;
                if (prefix_bytes) std::memcpy(s_prefix + offset, r.data + NK_MODEL_DATA_HEADER_BYTES, prefix_bytes);
                if (count > prefix_bytes && esp_partition_write(s_partition, offset + prefix_bytes,
                        r.data + NK_MODEL_DATA_HEADER_BYTES + prefix_bytes, count - prefix_bytes) != ESP_OK) {
                    stop(NK_MODEL_ERR_FLASH, op); continue;
                }
                s_session.written(count);
            }
            if (++chunks_since_ack >= NK_MODEL_WINDOW_CHUNKS || s_session.next == s_session.size) {
                publish(NK_MODEL_OK, op); chunks_since_ack = 0; vTaskDelay(1);
            }
        } else if (op == NK_MODEL_END) {
            finish();
        } else if (op == NK_MODEL_PARAMS) {
            write_params(r);
        } else if (op == NK_MODEL_REBOOT) {
            if (!s_session.can_reboot(id)) { publish(NK_MODEL_ERR_NOT_VERIFIED, op); continue; }
            s_session.state = NK_MODEL_RESTARTING;
            publish(NK_MODEL_OK, op);
            ESP_LOGI(kTag, "照合済みモデルで再起動します");
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }
    }
}
} // namespace

esp_err_t nk_model_writer_init(void) {
    s_model_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x40), "model");
    if (!s_model_part || s_model_part->size % kSectorBytes || s_model_part->size <= NK_VP_SECTOR_BYTES) return ESP_ERR_NOT_FOUND;
    s_partition = s_model_part;
    // 辞書の領域は無くてもモデルの書き込みは使える（無いときは BEGIN の target=1 を ERR_TARGET で断る）。
    s_dict_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x41), "dict");
    if (s_dict_part && s_dict_part->size % kSectorBytes) s_dict_part = nullptr;
    s_queue = xQueueCreate(16, sizeof(Request));
    if (!s_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(worker, "nk_model", nk::kModelWriterStackBytes, nullptr, 2, nullptr, 0) != pdPASS) {
        vQueueDelete(s_queue); s_queue = nullptr; return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool nk_model_writer_submit(const uint8_t *data, size_t len, uint32_t generation) {
    if (!s_queue || !data || len < 5 || len > NK_MODEL_PACKET_BYTES) return false;
    const uint8_t op = data[0];
    const bool valid = (op == NK_MODEL_BEGIN && len == 42) ||
        (op == NK_MODEL_PROBE && len == 10) ||
        (op == NK_MODEL_DATA && len > NK_MODEL_DATA_HEADER_BYTES) ||
        ((op == NK_MODEL_END || op == NK_MODEL_ABORT || op == NK_MODEL_REBOOT) && len == 5) ||
        (op == NK_MODEL_QUERY && len == 9) ||
        (op == NK_MODEL_PARAMS && len == 81);
    if (!valid) return false;
    Request r{}; r.generation = generation; r.len = len; std::memcpy(r.data, data, len);
    // 消去・照合中も中止を即座に観測できる。Flash 操作はしない。
    if (op == NK_MODEL_ABORT) s_abort_session.store(read32(data + 1));
    return xQueueSend(s_queue, &r, 0) == pdTRUE;
}
void nk_model_writer_disconnected(uint32_t generation) { s_generation.store(generation); }
bool nk_model_writer_busy(void) { return s_busy.load(); }
void nk_model_writer_status(uint8_t out[20]) {
    portENTER_CRITICAL(&s_snapshot_mux);
    std::memcpy(out, s_snapshot, NK_MODEL_STATE_BYTES);
    portEXIT_CRITICAL(&s_snapshot_mux);
}
int nk_model_writer_info(char *json, size_t capacity_bytes, uint16_t max_data) {
    uint8_t snapshot[NK_MODEL_STATE_BYTES]; char sha[65]; uint32_t hash_bytes; Probe probe_copy;
    portENTER_CRITICAL(&s_snapshot_mux);
    std::memcpy(snapshot, s_snapshot, sizeof(snapshot));
    std::memcpy(sha, s_actual_sha, sizeof(sha)); hash_bytes = s_hash_bytes;
    probe_copy = s_probe;
    portEXIT_CRITICAL(&s_snapshot_mux);
    const bool boot_ready = voice_player_init_finished();
    const NkModelInfo info{
        bool(s_queue), boot_ready, boot_ready && sano_voice_is_ready(), bool(s_dict_part), s_dict_dirty.load(),
        static_cast<uint32_t>(capacity_of(NK_MODEL_TARGET_MODEL)), s_dict_part ? s_dict_part->size : 0,
        read32(snapshot + 4), hash_bytes, probe_copy.request, probe_copy.bytes, max_data,
        snapshot[1], s_target.load(), probe_copy.target, sha, probe_copy.sha,
        sano_voice_model_flags(), sano_voice_params()
    };
    return nk_model_info_json(json, capacity_bytes, info);
}

// saan_model.h の実装。model パーティションを mmap したあと、全体を PSRAM（16 B 境界）に写し、
// memcmp で照合してから、その写しで saan_weights_open する。
// 参考: sanoTTS-jp-2/esp32/main/saan_model.c（パーティション mmap）と saan_model_rodata.c（PSRAM への写し）。
#include "saan_model.h"

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"

static const char *TAG = "saan_model";

#define SAAN_MODEL_PART_LABEL   "model"
#define SAAN_MODEL_PART_SUBTYPE 0x40

static esp_partition_mmap_handle_t s_handle;
static bool s_mapped;
static void *s_copy;
static size_t s_copy_bytes;

bool saan_model_open(saan_weights *w) {
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, SAAN_MODEL_PART_SUBTYPE, SAAN_MODEL_PART_LABEL);
    if (part == NULL) {
        ESP_LOGE(TAG, "パーティション '%s' が無い", SAAN_MODEL_PART_LABEL);
        return false;
    }
    ESP_LOGI(TAG, "model パーティション: offset 0x%08" PRIx32 " size %" PRIu32 " B",
             (uint32_t)part->address, (uint32_t)part->size);

    const void *ptr = NULL;
    esp_err_t err = esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &ptr, &s_handle);
    if (err != ESP_OK || ptr == NULL) {
        ESP_LOGE(TAG, "esp_partition_mmap 失敗: %s", esp_err_to_name(err));
        return false;
    }
    s_mapped = true;

    const size_t n = part->size;
    const int64_t t0 = esp_timer_get_time();
    s_copy = heap_caps_aligned_alloc(16, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_copy) {
        ESP_LOGE(TAG, "PSRAM に %u B を確保できない", (unsigned)n);
        return false;
    }
    memcpy(s_copy, ptr, n);
    s_copy_bytes = n;
    if (memcmp(s_copy, ptr, n) != 0) {
        ESP_LOGE(TAG, "PSRAM の写しが flash と一致しない");
        return false;
    }
    ESP_LOGI(TAG, "blob %u B を PSRAM (%p) に写して memcmp 一致（%.2f ms）。以後こちらを読む", (unsigned)n, s_copy,
             (double)(esp_timer_get_time() - t0) / 1000.0);

    saan_status s = saan_weights_open(w, s_copy, n);
    if (s != SAAN_OK) {
        ESP_LOGE(TAG, "saan_weights_open: %s", saan_strerror(s));
        return false;
    }
    ESP_LOGI(TAG, "重み OK: %" PRIu32 " tensors / base %p", w->n_tensors, (const void *)w->base);
    return true;
}

void saan_model_close(void) {
    if (s_mapped) {
        esp_partition_munmap(s_handle);
        s_mapped = false;
    }
    if (s_copy) {
        heap_caps_free(s_copy);
        s_copy = NULL;
        s_copy_bytes = 0;
    }
}

const void *sano_model_image(size_t *partition_bytes) {
    if (partition_bytes) *partition_bytes = s_copy_bytes;
    return s_copy;
}

/* hout を 2 コアで割る実装（DLS-205）。`-DSAAN_DUALCORE=1` のときだけビルドされる。
 *
 * 方式: 合成タスク（コア 0）が hout の前半 [o0, o_mid) を自分で計算し、後半 [o_mid, o1) を
 * 作業タスク（コア 1。固定）に任せる。合流は 2 本のバイナリセマフォ（go / done）。
 *   par:     job を書く → give(go) → 前半を計算 → take(done)
 *   worker:  take(go) → job を読む → 後半を計算 → give(done)
 * 範囲どうしは書き込みも読み込みも重ならない（csrc/saanotts_internal.h の契約。
 * ホストでは csrc/oc_split_test.c の G-OC2 が被覆を見ている）。
 *
 * ⚠️ PIE（ee.*）を作業タスクで使う根拠:
 *    - コプロセッサ状態は**タスクごと**の保存領域（TCB の CPSA）と**コアごと**の所有者表
 *      （_xt_coproc_owner_sa[core][cp]）で管理される（components/xtensa/xtensa_vectors.S:947-1066）。
 *    - 初めて ee.* を実行したタスクは Coprocessor-disabled 例外を起こし、_frxt_coproc_exc_hook が
 *      **そのタスクを例外を起こしたコアに固定する**（FreeRTOS-Kernel/portable/xtensa/portasm.S の「CP operations are incompatible
 *      with unpinned tasks. Thus we pin the task to the current running core」）。
 *      だから両タスクを最初から固定しておく（既定の未固定のままでも固定されるが、どちらのコアに
 *      なるかが実行時任せになる）。
 *    - 2 つのタスクが別コアで同時に PIE を使うとき、それぞれのコアの q レジスタ / ACCX は
 *      そのコア自身のもの。保存・復元はタスクの切り替え時にそのコアで行われる。
 *    ⚠️ 上はソースを読んだ根拠で、実機で同時に走らせた結果ではない。bit 一致は QEMU（TCG）で確かめる。 */
#include "saan_dualcore.h"

#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "saanotts_internal.h"

static const char *TAG = "saan_dc";

/* 作業タスクのスタック。呼ぶのは saan_conv1d_*（i8a_core は 256 B 程度）だけなので小さくてよいが、
 * PIE の保存領域（CPSA）もここから取られるので余裕を見る */
#define SAAN_DC_STACK 4096
#define SAAN_DC_WORKER_CORE 1

static SemaphoreHandle_t s_go;
static SemaphoreHandle_t s_done;
/* job。give(go) の前に書き、take(go) の後に読む（セマフォが順序を作る） */
static saan_oc_fn s_fn;
static void *s_ctx;
static int s_o0, s_o1;

static void worker(void *arg) {
    (void)arg;
    int first = 1;
    for (;;) {
        xSemaphoreTake(s_go, portMAX_DELAY);
        if (first) {   /* 作業タスクが実際にどのコアで走っているかの証拠（1 回だけ） */
            ESP_LOGI(TAG, "作業タスクの初回: core %d / 範囲 [%d, %d)", (int)xPortGetCoreID(), s_o0, s_o1);
            first = 0;
        }
#ifdef SAAN_DC_TEST_SKIP_WORKER
        /* 陽性対照（QEMU 専用）: 後半を計算しない → PCM の checksum が変わるはず */
#else
        s_fn(s_ctx, s_o0, s_o1);
#endif
        xSemaphoreGive(s_done);
    }
}

static void par(saan_oc_fn fn, void *ctx, int o0, int o_mid, int o1) {
    s_fn = fn;
    s_ctx = ctx;
    s_o0 = o_mid;
    s_o1 = o1;
    xSemaphoreGive(s_go);
    fn(ctx, o0, o_mid);
    xSemaphoreTake(s_done, portMAX_DELAY);
}

bool saan_dualcore_start(void) {
    if (xPortGetCoreID() != 0) {
        ESP_LOGE(TAG, "合成タスクがコア 0 にいない（core %d）。固定して作ること", (int)xPortGetCoreID());
        return false;
    }
    s_go = xSemaphoreCreateBinary();
    s_done = xSemaphoreCreateBinary();
    if (!s_go || !s_done) { ESP_LOGE(TAG, "セマフォを作れない"); return false; }
    if (xTaskCreatePinnedToCore(worker, "saan_hout", SAAN_DC_STACK, NULL, uxTaskPriorityGet(NULL),
                                NULL, SAAN_DC_WORKER_CORE) != pdPASS) {
        ESP_LOGE(TAG, "作業タスクを作れない");
        return false;
    }
    saan_set_oc_par(par);
    ESP_LOGI(TAG, "hout を 2 コアで計算する（前半: コア 0 / 後半: コア %d）", SAAN_DC_WORKER_CORE);
    return true;
}

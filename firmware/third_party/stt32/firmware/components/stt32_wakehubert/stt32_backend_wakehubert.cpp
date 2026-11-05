/*
 * stt32_backend_t over the wakehubert core. Model output index i is the
 * sorted-intent index of training (INT = sorted(intent names)); it equals the
 * stt32_intent_t value, which is the stable semantic id (DLS-240).
 */
#include "stt32_intents.h"
#include "stt32_backend.h"
#include "stt32_wakehubert.h"

static const stt32_semantic_id_t kSemanticIds[STT32_WH_CLASSES] = {
    STT32_INTENT_IIE,   STT32_INTENT_OHAYOU, STT32_INTENT_OYASUMI, STT32_INTENT_NANJI,
    STT32_INTENT_HAI,   STT32_INTENT_HOMERU, STT32_INTENT_UE,      STT32_INTENT_SHITA,
    STT32_INTENT_TEISHI, STT32_INTENT_GENKI, STT32_INTENT_ZENSHIN, STT32_INTENT_MIGI,
    STT32_INTENT_NAMAE, STT32_INTENT_TENKI,  STT32_INTENT_HIDARI,  STT32_INTENT_KOUTAI,
    STT32_INTENT_KAISHI,
#if STT32_WH_CLASSES == 18   /* a number, not STT32_INTENT_COUNT: an enum constant is 0 in #if (checked by the static_asserts below) */
    STT32_INTENT_NONKICHAN,                    /* 18th class (のんきちゃん, wake word; DLS-251 / DLS-1082) */
#endif
};
static_assert(STT32_INTENT_COUNT == 18 && STT32_INTENT_NONKICHAN == 17, "the #if above hard-codes 18 classes with NONKICHAN as the 18th");
static_assert(STT32_WH_CLASSES == 17 || STT32_WH_CLASSES == 18,
              "model classes must be the 17 intents, or the 17 plus the wake word (18)");

static size_t wh_class_count(void) { return STT32_WH_CLASSES; }
static const stt32_semantic_id_t *wh_semantic_ids(void) { return kSemanticIds; }
static int wh_infer(const int16_t *pcm, size_t n, float *scores, size_t count) {
    return stt32_wakehubert_infer(pcm, n, scores, count, nullptr);
}

static const stt32_backend_t kBackend = {
    stt32_wakehubert_init, stt32_wakehubert_deinit, wh_class_count, wh_semantic_ids, wh_infer,
};

extern "C" const stt32_backend_t *stt32_backend_default(void) { return &kBackend; }

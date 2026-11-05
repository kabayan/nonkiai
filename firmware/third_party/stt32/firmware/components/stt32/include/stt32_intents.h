#pragma once

/*
 * Stable semantic ids of the intent vocabulary (DLS-240; 17 intents, plus the wake word as the 18th class,
 * DLS-1082). These values are the ABI; a model update must keep the same id for the same intent. A model with
 * 17 classes has no NONKICHAN.
 */

typedef enum {
    STT32_INTENT_IIE = 0,      /* いいえ */
    STT32_INTENT_OHAYOU = 1,   /* おはよう */
    STT32_INTENT_OYASUMI = 2,  /* おやすみ */
    STT32_INTENT_NANJI = 3,    /* なんじ */
    STT32_INTENT_HAI = 4,      /* はい */
    STT32_INTENT_HOMERU = 5,   /* ほめる */
    STT32_INTENT_UE = 6,       /* 上 */
    STT32_INTENT_SHITA = 7,    /* 下 */
    STT32_INTENT_TEISHI = 8,   /* 停止 */
    STT32_INTENT_GENKI = 9,    /* 元気 */
    STT32_INTENT_ZENSHIN = 10, /* 前進 */
    STT32_INTENT_MIGI = 11,    /* 右 */
    STT32_INTENT_NAMAE = 12,   /* 名前 */
    STT32_INTENT_TENKI = 13,   /* 天気 */
    STT32_INTENT_HIDARI = 14,  /* 左 */
    STT32_INTENT_KOUTAI = 15,  /* 後退 */
    STT32_INTENT_KAISHI = 16,  /* 開始 */
    STT32_INTENT_NONKICHAN = 17, /* のんきちゃん (wake word, DLS-251 / DLS-1082) */
    STT32_INTENT_COUNT = 18,
} stt32_intent_t;

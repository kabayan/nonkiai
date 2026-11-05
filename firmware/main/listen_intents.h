#pragma once

#include <stdint.h>
#include "listen_mode.h"
#include "stt32_intents.h"

// Product/BLE numbers: retain the original nine and append the new eight.
enum NkListenIntent : uint8_t {
    NK_LISTEN_FORWARD = 0, NK_LISTEN_BACKWARD, NK_LISTEN_RIGHT, NK_LISTEN_LEFT,
    NK_LISTEN_NAME, NK_LISTEN_WEATHER, NK_LISTEN_TIME, NK_LISTEN_ENERGY, NK_LISTEN_PRAISE,
    NK_LISTEN_YES, NK_LISTEN_NO, NK_LISTEN_MORNING, NK_LISTEN_NIGHT,
    NK_LISTEN_UP, NK_LISTEN_DOWN, NK_LISTEN_STOP, NK_LISTEN_START,
    NK_LISTEN_COUNT,
    NK_LISTEN_NONE = 255,
};

// Product names follow the stable vocabulary in stt32_intents.h.
inline constexpr const char *kListenIntentNames[] = {
    "前進", "後退", "右", "左", "名前", "天気", "なんじ", "元気", "ほめる",
    "はい", "いいえ", "おはよう", "おやすみ", "上", "下", "停止", "開始"
};
inline const char *nk_listen_intent_name(unsigned intent) {
    return intent < NK_LISTEN_COUNT ? kListenIntentNames[intent] : "不明";
}

struct NkListenIntentMap { stt32_intent_t semantic; NkListenIntent intent; };
inline constexpr NkListenIntentMap kListenIntentMap[] = {
    {STT32_INTENT_IIE, NK_LISTEN_NO},
    {STT32_INTENT_OHAYOU, NK_LISTEN_MORNING},
    {STT32_INTENT_OYASUMI, NK_LISTEN_NIGHT},
    {STT32_INTENT_NANJI, NK_LISTEN_TIME},
    {STT32_INTENT_HAI, NK_LISTEN_YES},
    {STT32_INTENT_HOMERU, NK_LISTEN_PRAISE},
    {STT32_INTENT_UE, NK_LISTEN_UP},
    {STT32_INTENT_SHITA, NK_LISTEN_DOWN},
    {STT32_INTENT_TEISHI, NK_LISTEN_STOP},
    {STT32_INTENT_GENKI, NK_LISTEN_ENERGY},
    {STT32_INTENT_ZENSHIN, NK_LISTEN_FORWARD},
    {STT32_INTENT_MIGI, NK_LISTEN_RIGHT},
    {STT32_INTENT_NAMAE, NK_LISTEN_NAME},
    {STT32_INTENT_TENKI, NK_LISTEN_WEATHER},
    {STT32_INTENT_HIDARI, NK_LISTEN_LEFT},
    {STT32_INTENT_KOUTAI, NK_LISTEN_BACKWARD},
    {STT32_INTENT_KAISHI, NK_LISTEN_START},
};

inline constexpr NkListenIntent nk_listen_intent_from_semantic(uint16_t semantic)
{
    for (const auto &entry : kListenIntentMap)
        if (entry.semantic == semantic) return entry.intent;
    return NK_LISTEN_NONE;
}

// 認識モード（listen_mode.h）ごとの意図の集合。起動語（STT32_INTENT_NONKICHAN）はどちらにも入れない
// （1 発話の結果として返事や IR の対象にしない）。
inline constexpr uint32_t nk_listen_intent_bit(stt32_intent_t semantic) { return uint32_t{1} << semantic; }
// stt32_listen_options_t.intent_mask（bit i = stt32_intent_t i）。
// おもちゃ: 前進・後退・右・左・開始・停止（stt32 側は全体の 1 位が集合内なら受け付ける）。
// ぬいぐるみ: いいえ・おはよう・おやすみ・なんじ・はい・ほめる・元気・名前・天気・上・下（集合内の最大の確率が下の下限以上）。
inline constexpr uint32_t kListenToyMask =
    nk_listen_intent_bit(STT32_INTENT_ZENSHIN) | nk_listen_intent_bit(STT32_INTENT_KOUTAI)
    | nk_listen_intent_bit(STT32_INTENT_MIGI) | nk_listen_intent_bit(STT32_INTENT_HIDARI)
    | nk_listen_intent_bit(STT32_INTENT_KAISHI) | nk_listen_intent_bit(STT32_INTENT_TEISHI);
inline constexpr uint32_t kListenPlushMask =
    nk_listen_intent_bit(STT32_INTENT_IIE) | nk_listen_intent_bit(STT32_INTENT_OHAYOU)
    | nk_listen_intent_bit(STT32_INTENT_OYASUMI) | nk_listen_intent_bit(STT32_INTENT_NANJI)
    | nk_listen_intent_bit(STT32_INTENT_HAI) | nk_listen_intent_bit(STT32_INTENT_HOMERU)
    | nk_listen_intent_bit(STT32_INTENT_GENKI) | nk_listen_intent_bit(STT32_INTENT_NAMAE)
    | nk_listen_intent_bit(STT32_INTENT_TENKI)
    | nk_listen_intent_bit(STT32_INTENT_UE) | nk_listen_intent_bit(STT32_INTENT_SHITA);
inline constexpr float kListenPlushMinScore = 0.1f;

inline constexpr uint32_t nk_listen_mode_mask(uint8_t mode)
{
    return mode == NK_LISTEN_MODE_TOY ? kListenToyMask : kListenPlushMask;
}

static_assert(NK_LISTEN_FORWARD == 0 && NK_LISTEN_BACKWARD == 1
              && NK_LISTEN_RIGHT == 2 && NK_LISTEN_LEFT == 3, "intent == IR direction");
// 起動語（NONKICHAN = 17）は製品の意図に写さない。写像は 0..16 の 17 件。
static_assert(sizeof(kListenIntentMap) / sizeof(kListenIntentMap[0]) == STT32_INTENT_NONKICHAN
              && STT32_INTENT_NONKICHAN + 1 == STT32_INTENT_COUNT);
static_assert((kListenToyMask & kListenPlushMask) == 0 && !(kListenToyMask >> STT32_INTENT_NONKICHAN)
              && !(kListenPlushMask >> STT32_INTENT_NONKICHAN), "mode sets are disjoint and exclude the wake word");

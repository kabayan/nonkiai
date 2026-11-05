#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "listen_intents.h"

inline constexpr int kListenVolumeStep = 20;
inline constexpr int kListenVolumeMin = 60;
inline constexpr int kListenVolumeMax = 200;

inline uint8_t nk_listen_step_volume(uint8_t current, bool up)
{
    const int stepped = static_cast<int>(current) + (up ? kListenVolumeStep : -kListenVolumeStep);
    return static_cast<uint8_t>(stepped < kListenVolumeMin ? kListenVolumeMin
                               : stepped > kListenVolumeMax ? kListenVolumeMax : stepped);
}

inline bool nk_listen_build_volume_reply(uint8_t volume, char *out, size_t size)
{
    if (!out || size == 0) return false;
    const int written = snprintf(out, size, "音量は%uです", static_cast<unsigned>(volume));
    return written >= 0 && static_cast<size_t>(written) < size;
}

inline constexpr const char *kListenFixedReplies[] = {
    "前に進むよ", "うしろに下がるよ", "右を向くよ", "左を向くよ",
    "元気だよ", "ありがとう、うれしいな",
};
// DLS-179: 数字をかなにし、読点で辞書の誤分割を防ぐ。8 時・18 時は「は」を助詞（わ）と読むので片仮名。
inline constexpr const char *kListenHourKana[] = {
    "れいじ", "いちじ", "にじ", "さんじ", "よじ", "ごじ", "ろくじ", "しちじ",
    "ハチジ", "くじ", "じゅうじ", "じゅういちじ", "じゅうにじ", "じゅうさんじ",
    "じゅうよじ", "じゅうごじ", "じゅうろくじ", "じゅうしちじ", "ジュウハチジ",
    "じゅうくじ", "にじゅうじ", "にじゅういちじ", "にじゅうにじ", "にじゅうさんじ",
};
inline constexpr const char *kListenMinuteUnits[] = {
    "ちょうど", "いっぷん", "にふん", "さんぷん", "よんぷん", "ごふん",
    "ろっぷん", "ななふん", "はっぷん", "きゅうふん",
};
inline constexpr const char *kListenMinuteTens[] = {
    "", "じゅう", "にじゅう", "さんじゅう", "よんじゅう", "ごじゅう",
};
inline constexpr const char *kListenMinuteExactTens[] = {
    "ちょうど", "じゅっぷん", "にじゅっぷん", "さんじゅっぷん", "よんじゅっぷん", "ごじゅっぷん",
};
inline constexpr char kListenRetry[] = "もう一回、お話して";
inline constexpr char kListenUnknown[] = "わからないの";

inline constexpr const char *kListenGreetingReplies[2][10] = {
    {"おはよう！今日もよろしくね", "おはよう、よく眠れた？", "おはよう！いい朝だね",
     "おはよう、今日はなにして遊ぶ？", "おはよう！会えてうれしいな", "ふわあ、おはよう",
     "おはよう、今日もがんばろうね", "おはよう！元気いっぱいだよ",
     "おはよう、顔は洗ったかな？", "おはよう！今日もいっしょだね"},
    {"おやすみなさい、また明日ね", "おやすみ、いい夢見てね", "おやすみ、今日も楽しかったね",
     "ふわあ、私も眠くなってきたよ", "おやすみ、ぐっすり眠ってね",
     "おやすみなさい、あったかくしてね", "おやすみ、明日も遊ぼうね",
     "また明日ね、おやすみ", "おやすみ、そばにいるからね", "今日もありがとう、おやすみ"},
};

inline constexpr const char *kListenConversationReplies[4][10] = {
    {"うん、わかったよ", "はーい、了解", "うん、そうしよう", "オッケー、まかせてね", "わかった、ありがとう", "うんうん、いいよ", "はい、ちゃんときいてるよ", "うん、いいね", "そうだね、そうしよう", "わかったよ、うれしいな"},
    {"そっか、わかったよ", "そっか、ざんねん", "わかった、またこんどね", "うん、だいじょうぶだよ", "そうなんだ、わかったよ", "いいよ、気にしないでね", "そっか、むりしないでね", "うん、ちがうんだね", "わかった、ほかのにしよう", "そっかそっか、了解だよ"},
    {"元気だよ、きみはどうかな？", "とっても元気だよ、きみと会えたからね", "元気いっぱいだよ、なにして遊ぼうか？", "うん、元気だよ。きみも元気だったかな？", "げんきげんき、今日もばっちりだよ。きみのおかげだね", "きみに会えて、すっかり元気になったよ。ありがとう", "元気だよ、聞いてくれてありがとう。うれしいな", "ちょっとねむいけど、君の声で元気になったよ", "元気だよ、きみと一緒にいっぱい遊びたいな", "今日も元気だよ、そばにいてくれてありがとう"},
    {"ありがとう、そう言ってもらえてとってもうれしいな", "えへへ、そんなにほめられたらてれちゃうな", "ほんと？うれしいな。もっとがんばっちゃおう", "ありがとう、きみのためにもっとがんばるね", "わあい、ほめられちゃった。今日はいい日だな", "きみにほめられると、いちばんうれしいな。ありがとう", "えへへ、ありがとう。もう一回言ってほしいな", "そう言ってくれて、うれしいよ。ずっとおぼえておくね", "ありがとう、きみもとってもすてきだよ", "うれしくて、からだじゅうがぽかぽかするよ"},
};

inline constexpr const char *kListenIrRequestReplies[4][5] = {
    {"まえにすすんで！", "まっすぐいってみて！", "まえへゴー！",
     "ねえ、まえにすすんでくれる？", "一緒にまえへいこう！"},
    {"うしろにさがって！", "ちょっとうしろへもどって", "バックして！",
     "うしろにさがってくれる？", "あぶない、さがって〜！"},
    {"みぎにまわって！", "みぎへくるっとまわって",
     "こんどはみぎにまわってね", "くるっとみぎだよ"},
    {"ひだりにまわって！", "ひだりへくるっとまわって",
     "こんどはひだりにまわってね", "くるっとひだりだよ"},
};
inline constexpr uint8_t kListenIrRequestCounts[4] = {5, 5, 4, 4};

// The caller owns the previous index and supplies one uniform 32-bit draw.
// -1 means either invalid input or a rejected draw; draw again only for valid
// count/previous inputs. Rejection avoids modulo bias for non-power-of-two choices.
inline int nk_listen_choose_index(unsigned count, uint32_t random, int previous)
{
    if (count < 2) return -1;
    if (previous < -1 || previous >= static_cast<int>(count)) return -1;
    const unsigned available = count - (previous >= 0 ? 1 : 0);
    constexpr uint64_t kRandomRange = uint64_t{1} << 32;
    if (random >= kRandomRange - kRandomRange % available) return -1;
    int index = static_cast<int>(random % available);
    if (previous >= 0 && index >= previous) ++index;
    return index;
}

inline int nk_listen_choose_ir_request_index(uint8_t direction, uint32_t random, int previous)
{
    return direction < 4 ? nk_listen_choose_index(kListenIrRequestCounts[direction], random, previous) : -1;
}

inline int nk_listen_choose_greeting_index(uint8_t intent, uint32_t random, int previous)
{
    return intent == NK_LISTEN_MORNING || intent == NK_LISTEN_NIGHT
         ? nk_listen_choose_index(10, random, previous) : -1;
}

// Rows are yes, no, energy, praise; unrelated intents have no row.
inline int nk_listen_conversation_row(uint8_t intent)
{
    switch (intent) {
    case NK_LISTEN_YES: return 0;
    case NK_LISTEN_NO: return 1;
    case NK_LISTEN_ENERGY: return 2;
    case NK_LISTEN_PRAISE: return 3;
    default: return -1;
    }
}

inline int nk_listen_choose_conversation_index(uint8_t intent, uint32_t random, int previous)
{
    return nk_listen_conversation_row(intent) >= 0
         ? nk_listen_choose_index(10, random, previous) : -1;
}

// Pure reply formatting. Intent numbers are the stable product/BLE order.
// The caller supplies one clock/weather/name snapshot; no device calls here.
inline bool nk_listen_build_reply(uint8_t intent, int64_t local_ms,
                                  const char *weather, const char *greeting,
                                  char *out, size_t size, int greeting_index = 0)
{
    if (!out || size == 0) return false;
    int written;
    if (intent <= 3) {
        written = snprintf(out, size, "%s", kListenFixedReplies[intent]);
    } else if (intent == 4) {
        written = snprintf(out, size, "%s", greeting && *greeting ? greeting : kListenUnknown);
    } else if (intent == 5) {
        written = snprintf(out, size, "%s%s", weather && *weather
                           && strcmp(weather, kListenUnknown) != 0 ? "きょうは" : "",
                           weather && *weather ? weather : kListenUnknown);
    } else if (intent == 6 && local_ms >= 0) {
        const unsigned minutes = static_cast<unsigned>((local_ms / 60000) % 1440);
        const unsigned minute = minutes % 60;
        const char *prefix = minute % 10 ? kListenMinuteTens[minute / 10] : "";
        const char *suffix = minute % 10 ? kListenMinuteUnits[minute % 10]
                                         : kListenMinuteExactTens[minute / 10];
        written = snprintf(out, size, "いまは、%s、%s%sだよ",
                           kListenHourKana[minutes / 60], prefix, suffix);
    } else if (intent == 6) {
        written = snprintf(out, size, "%s", kListenUnknown);
    } else if (nk_listen_conversation_row(intent) >= 0) {
        if (greeting_index < 0 || greeting_index >= 10) return false;
        written = snprintf(out, size, "%s", kListenConversationReplies[nk_listen_conversation_row(intent)][greeting_index]);
    } else if (intent == NK_LISTEN_MORNING || intent == NK_LISTEN_NIGHT) {
        if (greeting_index < 0 || greeting_index >= 10) return false;
        written = snprintf(out, size, "%s", kListenGreetingReplies[intent - NK_LISTEN_MORNING][greeting_index]);
    } else if (intent >= NK_LISTEN_YES && intent < NK_LISTEN_COUNT) {
        constexpr const char *replies[] = {"うん、わかったよ", "そっか、わかったよ", "", "",
                                          "上だね", "下だね", "とまるよ", "はじめよう！"};
        written = snprintf(out, size, "%s", replies[intent - NK_LISTEN_YES]);
    } else {
        written = snprintf(out, size, "%s", kListenRetry);
    }
    return written >= 0 && static_cast<size_t>(written) < size;
}

#pragma once

#include <stddef.h>

typedef struct {
    const char *text;       // 照合用テキスト (UTF-8)
    const char *wav_path;   // LittleFS 内パス (例: "/filler/un.wav")
} nk_filler_entry_t;

static const nk_filler_entry_t kFillerTable[] = {
    {"そうだね", "/filler/soudane.wav"},
    {"なるほど", "/filler/naruhodo.wav"},
    {"えっとね", "/filler/ettone.wav"},
    {"えーっと", "/filler/eetto.wav"},
    {"うーんと", "/filler/uunto.wav"},
    {"うんうん", "/filler/unun.wav"},
    {"いいね", "/filler/iine.wav"},
    {"えへへ", "/filler/ehehe.wav"},
};

static const size_t kFillerTableCount = sizeof(kFillerTable) / sizeof(kFillerTable[0]);

// 完全一致または前方一致するフィラー WAV パスを検索 (見つからなければ NULL)
const char *nk_find_filler_wav(const char *text);

#pragma once
// 生徒の語彙 57 トークン（ids の値 = 生徒のインデックス）。ログで ids を記号に戻して読みを確かめるための表。
// 正本は sanoTTS-jp-2 の src/saanotts_jp/vocab.py の VOCAB_TABLE（student_index 順の token）。
// 取り違えは tools/test_serial_say.py が 2 つの方法で検査する:
//   (1) csrc/g2p_table.h の「か -> k a」のコメント（195 行）と、この表で引いた記号が一致する
//   (2) sanoTTS-jp-2 の vocab.py が手元にあれば、57 個すべてが一致する
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SANO_VOCAB_N 57

static const char *const kSanoVocab[SANO_VOCAB_N] = {
    "_", "^", "$", "?", "?!", "?.", "?~", "#", "[", "]",                    /*  0〜 9 */
    "a", "i", "u", "e", "o", "A", "I", "U", "E", "O",                       /* 10〜19 */
    "N_m", "N_n", "N_ng", "N_uvular", "cl",                                 /* 20〜24 */
    "k", "ky", "kw", "g", "gy", "gw", "t", "ty", "d", "dy",                 /* 25〜34 */
    "p", "py", "b", "by", "ch", "ts", "s", "sh", "z", "j",                  /* 35〜44 */
    "f", "h", "hy", "v", "n", "ny", "m", "my", "r", "ry", "w", "y",         /* 45〜56 */
};

// id の記号。範囲外は NULL。
static inline const char *sano_vocab_symbol(int id) {
    return (id >= 0 && id < SANO_VOCAB_N) ? kSanoVocab[id] : NULL;
}

// ids を記号の列に直す（空白区切り）。PAD（`_`）は読みにくいので省く。範囲外は `?<id>`。
// 切り詰めたときは末尾に `...` を付ける。out の長さが 0 なら何もしない。
static inline void sano_vocab_format(const int *ids, int n, char *out, size_t cap) {
    if (!out || cap == 0) return;
    size_t len = 0;
    out[0] = '\0';
    for (int i = 0; i < n; ++i) {
        if (ids[i] == 0) continue;
        char tmp[16];
        const char *sym = sano_vocab_symbol(ids[i]);
        if (!sym) { snprintf(tmp, sizeof tmp, "?%d", ids[i]); sym = tmp; }
        size_t need = strlen(sym) + (len ? 1 : 0);
        if (len + need + 4 > cap) {
            if (cap >= 4 && len + 4 <= cap) { snprintf(out + len, cap - len, "..."); }
            return;
        }
        if (len) out[len++] = ' ';
        memcpy(out + len, sym, strlen(sym));
        len += strlen(sym);
        out[len] = '\0';
    }
}

#ifdef __cplusplus
}
#endif

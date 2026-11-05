#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

// DLS-180: 半角数字を全角にそろえ（辞書は半角を未知語として捨てる）、数の直前のかな「やく」を「約」にする
// （かなのままだと「やく１０」を「やく ひゃくじゅう」と読む）。容量を先に確認し、不足時は出力を変えない。
static inline bool sano_digit_at(const unsigned char *p) {
    return (*p >= '0' && *p <= '9') || (p[0] == 0xef && p[1] == 0xbc && p[2] >= 0x90 && p[2] <= 0x99);
}
// p が「やく」+ 数字なら置き換える長さ（6 バイト）、そうでなければ 0。
static inline size_t sano_yaku_before_digit(const unsigned char *p) {
    static const unsigned char yaku[] = {0xe3, 0x82, 0x84, 0xe3, 0x81, 0x8f};
    return memcmp(p, yaku, 6) == 0 && sano_digit_at(p + 6) ? 6 : 0;
}
static inline bool sano_normalize_digits(const char *input, char *output, size_t capacity) {
    if (!input || !output || capacity == 0) return false;
    size_t needed = 1;
    for (const unsigned char *p = (const unsigned char *)input; *p;) {
        size_t in = 1, width = 1;
        if (*p >= '0' && *p <= '9') width = 3;
        else if (*p == 0xe3 && strlen((const char *)p) >= 7 && sano_yaku_before_digit(p)) in = 6, width = 3;
        if (width > capacity - needed) return false;
        needed += width;
        p += in;
    }
    unsigned char *out = (unsigned char *)output;
    for (const unsigned char *p = (const unsigned char *)input; *p;) {
        if (*p >= '0' && *p <= '9') {
            *out++ = 0xef;
            *out++ = 0xbc;
            *out++ = 0x90 + (*p++ - '0');
        } else if (*p == 0xe3 && strlen((const char *)p) >= 7 && sano_yaku_before_digit(p)) {
            *out++ = 0xe7;  // 約 U+7D04
            *out++ = 0xb4;
            *out++ = 0x84;
            p += 6;
        } else {
            *out++ = *p++;
        }
    }
    *out = 0;
    return true;
}

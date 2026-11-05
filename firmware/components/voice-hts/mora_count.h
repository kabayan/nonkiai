/* ----------------------------------------------------------------- */
/* Phase G G-CP1: pron → モーラ数 導出 (host/fw 共有ヘルパ)            */
/*                                                                   */
/* NJDNode.mora_size は本来辞書が供給するが、fw には golden が無い。   */
/* mora_size は jpcommon_mora_list (jpcommon が pron をモーラ分割する   */
/* のと同一テーブル) で pron から決定的に導出できる。Trie 14列化を     */
/* せず実行時導出する (DRY: 容量を増やさず host/fw 完全一致を保証)。    */
/*                                                                   */
/* 規則は jpcommon_label.c JPCommonLabel_push_word (L472-588) と同順:   */
/*   - 長音「ー」     = 1 モーラ                                        */
/*   - 無声化「’」   = モーラでない (直前音素の修飾フラグ)            */
/*   - 句点「、」/「？」= モーラでない (pause/emotion マーカー)         */
/*   - それ以外       = jpcommon_mora_list を最長一致 (表は長い順) で 1  */
/*                                                                   */
/* upstream jpcommon_rule_utf_8.h の表をそのまま参照する (二重定義回避)。*/
/* ----------------------------------------------------------------- */
#ifndef MORA_COUNT_H
#define MORA_COUNT_H

#include <cstddef>
#include <cstring>
#include <string>

extern "C" {
#include "jpcommon_rule_utf_8.h" /* jpcommon_mora_list / JPCOMMON_MORA_* */
}

namespace mora {

// p が prefix で始まれば prefix の byte 長、さもなくば -1
// (jpcommon_label.c strtopcmp と同義)。
inline int starts_with(const char *p, const char *prefix) {
   size_t n = std::strlen(prefix);
   return (std::strncmp(p, prefix, n) == 0) ? (int)n : -1;
}

// pron (NJD の発音表記、無声化「’」/長音「ー」反映済) のモーラ数を返す。
inline int mora_count(const char *pron) {
   int n = 0;
   while (pron[0] != '\0') {
      int find = starts_with(pron, JPCOMMON_MORA_LONG_VOWEL);  // ー = 1 モーラ
      if (find != -1) {
         ++n;
         pron += find;
         continue;
      }
      find = starts_with(pron, JPCOMMON_MORA_UNVOICE);  // ’ = モーラでない
      if (find != -1) {
         pron += find;
         continue;
      }
      // 「、」「？」(pause/emotion) はモーラでない。語の pron として単独で来る
      // ことが多いが、防御的にスキップ扱いにする。
      find = starts_with(pron, JPCOMMON_MORA_SHORT_PAUSE);
      if (find != -1) {
         pron += find;
         continue;
      }
      find = starts_with(pron, JPCOMMON_MORA_QUESTION);
      if (find != -1) {
         pron += find;
         continue;
      }
      // 通常モーラ: 表 (長い順) を先頭一致で走査し最初に当たった 1 件 = 最長一致。
      int matched = -1;
      for (int i = 0; jpcommon_mora_list[i] != NULL; i += 3) {
         int f = starts_with(pron, jpcommon_mora_list[i]);
         if (f != -1) {
            matched = f;
            break;
         }
      }
      if (matched > 0) {
         ++n;
         pron += matched;
      } else {
         // 表に無い文字 (想定外): 1 byte 進めてモーラに数えない (安全側)。
         ++pron;
      }
   }
   return n;
}

inline int mora_count(const std::string &pron) { return mora_count(pron.c_str()); }

}  // namespace mora

#endif /* !MORA_COUNT_H */

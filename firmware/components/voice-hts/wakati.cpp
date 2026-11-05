/* Phase E E-CP4 (DLS-180): Viterbi 分かち書き (案α) 本体。wakati.h 参照。 */
#include "wakati.h"

#include <limits>

namespace wakati {

// UTF-8 文字境界の byte オフセット列を返す (先頭 0 / 末尾 text.size() を含む)。
static std::vector<size_t> char_boundaries(const std::string &text) {
   std::vector<size_t> b;
   b.push_back(0);
   for (size_t i = 0; i < text.size();) {
      ++i;
      while (i < text.size() && ((uint8_t)text[i] & 0xC0) == 0x80) ++i;
      b.push_back(i);
   }
   return b;
}

// 指定 byte 位置から 1 codepoint を読む (UTF-8、境界前提)。
static uint32_t codepoint_at(const std::string &s, size_t i) {
   uint8_t c0 = (uint8_t)s[i];
   size_t n = (c0 < 0x80) ? 1 : (c0 & 0xE0) == 0xC0 ? 2 : (c0 & 0xF0) == 0xE0 ? 3 : 4;
   if (i + n > s.size()) return c0;
   uint32_t cp = (n == 1) ? c0 : (n == 2) ? (c0 & 0x1F) : (n == 3) ? (c0 & 0x0F) : (c0 & 0x07);
   for (size_t k = 1; k < n; ++k) cp = (cp << 6) | ((uint8_t)s[i + k] & 0x3F);
   return cp;
}

// 直前の基底モーラへ結合する小書き仮名 (拗音 ゃゅょ / 小母音 ぁぃぅぇぉ / ゎゕゖ、カタカナ等価)。
// 促音「っ」・長音「ー」は独立 1 モーラなので含めない (jpcommon/mora_count が個別処理)。
static bool is_combining_small_kana(uint32_t cp) {
   switch (cp) {
      // ひらがな小書き
      case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049:  // ぁぃぅぇぉ
      case 0x3083: case 0x3085: case 0x3087:                            // ゃゅょ
      case 0x308E: case 0x3095: case 0x3096:                            // ゎゕゖ
      // カタカナ小書き
      case 0x30A1: case 0x30A3: case 0x30A5: case 0x30A7: case 0x30A9:  // ァィゥェォ
      case 0x30E3: case 0x30E5: case 0x30E7:                            // ャュョ
      case 0x30EE: case 0x30F5: case 0x30F6:                            // ヮヵヶ
         return true;
      default:
         return false;
   }
}

std::vector<LatNode> segment(const std::string &text, const trie::Trie &t, long unk_cost) {
   std::vector<LatNode> result;
   if (text.empty()) return result;

   const std::vector<size_t> bnd = char_boundaries(text);  // 文字境界 byte 位置
   const size_t M = bnd.size() - 1;                        // 文字数
   // byte 位置 → 境界インデックス (char 境界以外は -1)
   std::vector<int> pos2idx(text.size() + 1, -1);
   for (size_t k = 0; k < bnd.size(); ++k) pos2idx[bnd[k]] = (int)k;

   const long INF = std::numeric_limits<long>::max() / 4;
   std::vector<long> dp(M + 1, INF);
   std::vector<int> back(M + 1, -1);          // 直前境界インデックス
   std::vector<LatNode> chosen(M + 1);        // back[k] からこの境界 k へ来たノード
   std::vector<size_t> best_len(M + 1, 0);    // 同コスト tie 用: 到達ノードの文字長
   dp[0] = 0;

   for (size_t bi = 0; bi < M; ++bi) {        // 境界 bi (= byte bnd[bi]) を始点に
      if (dp[bi] >= INF) continue;
      const size_t pbyte = bnd[bi];

      auto relax = [&](size_t end_byte, const LatNode &node, long cost) {
         int ei = pos2idx[end_byte];
         if (ei < 0) return;  // 文字境界でない (UTF-8 途中) は無効
         size_t nlen = (size_t)ei - bi;
         if (cost < dp[ei] || (cost == dp[ei] && nlen > best_len[ei])) {
            dp[ei] = cost;
            back[ei] = (int)bi;
            chosen[ei] = node;
            best_len[ei] = nlen;
         }
      };

      // 辞書マッチ (最長一致まで全長)
      std::vector<trie::Trie::Match> ms = t.lookup_prefixes(text, pbyte);
      for (auto &m : ms) {
         LatNode n;
         n.begin = pbyte;
         n.end = pbyte + m.byte_len;
         n.nodes = m.nodes;
         n.wcost = m.nodes.empty() ? unk_cost : (long)m.nodes[0].wcost;
         n.unknown = false;
         relax(n.end, n, dp[bi] + n.wcost + NODE_COST);
      }
      // unknown 1 文字ノード (必ず立てて経路を保証)
      size_t end1 = bnd[bi + 1];
      LatNode u;
      u.begin = pbyte;
      u.end = end1;
      u.wcost = unk_cost;
      u.unknown = true;
      relax(end1, u, dp[bi] + unk_cost + NODE_COST);

      // 辞書外かな語の拗音割れ対策: 直後が結合小書き仮名 (ゃゅょ等) なら
      // 「基底 + 小書き」を 1 ノードに束ねた unknown 候補も追加する。これにより
      // hira_to_kata("ちゃ")="チャ" → jpcommon "ch a" となり「ちや」化を防ぐ。
      // 1 文字 unknown も残すため到達性・辞書語探索に無影響。コストは 1 ノード分
      // (unk_cost) で 2 連続 1 文字 unknown (2*unk_cost) より低く、束ね側が採用される。
      if (bi + 2 <= M && is_combining_small_kana(codepoint_at(text, end1))) {
         size_t end2 = bnd[bi + 2];
         LatNode u2;
         u2.begin = pbyte;
         u2.end = end2;
         u2.wcost = unk_cost;
         u2.unknown = true;
         relax(end2, u2, dp[bi] + unk_cost + NODE_COST);
      }
   }

   // 経路復元 (末尾境界 M から先頭へ)
   std::vector<LatNode> rev;
   for (int k = (int)M; k > 0;) {
      int pk = back[k];
      if (pk < 0) break;  // 到達不能 (理論上 unknown で常に到達するが安全側)
      rev.push_back(chosen[k]);
      k = pk;
   }
   result.assign(rev.rbegin(), rev.rend());
   return result;
}

}  // namespace wakati

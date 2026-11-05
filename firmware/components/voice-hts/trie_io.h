/* ----------------------------------------------------------------- */
/* Phase D-CP2: 表記Trie binary (FTRI) lookup + tier1_njd.tsv 読込    */
/*                                                                   */
/* trie_build.py が出力した tier1_trie.bin を load し、surface で      */
/* NJD ノード列を引く。fw 既存 FMAK と同じ FNV-1a + 昇順 index 二分    */
/* 探索パターン (formA_pack.h 準拠)。Phase G で fw がこの lookup を     */
/* そのまま移植する想定。                                            */
/* ----------------------------------------------------------------- */
#ifndef TRIE_IO_H
#define TRIE_IO_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace trie {

// NJD 1ノード (jpcg::NjdNode と同一フィールド)。
// ver2 (E-CP4 / DLS-180) で base/wcost を追加 (Viterbi 分割の原形参照 + word cost)。
struct TrieNode {
   std::string pron, pos, pos_group1, pos_group2, pos_group3, ctype, cform;
   int acc = 0;
   int chain_flag = -1;
   std::string base;       // 原形 surface (ver2)
   int wcost = 0;          // word cost = round(-log(freq/total)*100) (ver2)
};

// FNV-1a 32bit (formA_pack.h fnv1a_32 / trie_build.py と同一)。
inline uint32_t fnv1a_32(const uint8_t *data, size_t len) {
   uint32_t h = 0x811C9DC5u;
   for (size_t i = 0; i < len; ++i) {
      h ^= data[i];
      h *= 16777619u;
   }
   return h;
}

// ---- binary 読み出しヘルパ (little-endian) ----
inline uint16_t rd_u16(const uint8_t *p) {
   return (uint16_t)(p[0] | (p[1] << 8));
}
inline uint32_t rd_u32(const uint8_t *p) {
   return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}
inline int16_t rd_i16(const uint8_t *p) {
   return (int16_t)rd_u16(p);
}
inline int32_t rd_i32(const uint8_t *p) {
   return (int32_t)rd_u32(p);
}

// 表記Trie (FTRI binary を lookup)。
// Phase G fw 統合 (DLS-186): blob 所有を外に出し base_/len_ の span 参照のみ保持する。
// host は load(path) でファイル全読み→owned_ に保持→attach、fw は LittleFS→PSRAM の
// 生ポインタを attach に直接渡す (非所有)。検証ロジックは attach に一本化 (DRY)。
class Trie {
 public:
   // 与えられた span (FTRI binary 先頭) を参照する。所有しない。
   // ヘッダ (magic/ver2/count 範囲) を検証して base_/len_/ver_/count_ をセット。
   bool attach(const uint8_t *base, size_t len) {
      if (base == nullptr || len < 12) return false;
      if (std::memcmp(base, "FTRI", 4) != 0) return false;
      uint16_t ver = rd_u16(base + 4);
      if (ver != 2) return false;  // ver2 のみ受理 (base/wcost 列前提、E-CP4/DLS-180)
      uint32_t count = rd_u32(base + 8);
      if (12 + (size_t)count * 12 > len) return false;  // index 範囲チェック
      base_ = base;
      len_ = len;
      ver_ = ver;
      count_ = count;
      return true;
   }

   // host 互換: ファイル全読み→owned_ 保持→attach。fw は attach を直接呼ぶ。
   bool load(const std::string &path) {
      std::ifstream ifs(path, std::ios::binary);
      if (!ifs) return false;
      owned_.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
      return attach(owned_.data(), owned_.size());
   }

   uint32_t count() const { return count_; }
   size_t size_bytes() const { return len_; }
   uint16_t version() const { return ver_; }

   // surface のノード列を out へ。見つかれば true。
   bool lookup(const std::string &surface, std::vector<TrieNode> &out) const {
      const uint32_t target = fnv1a_32(
          reinterpret_cast<const uint8_t *>(surface.data()), surface.size());
      const uint8_t *idx = base_ + 12;  // index 先頭

      // hash 昇順 index を二分探索 (target の下限を求める)
      int32_t lo = 0, hi = (int32_t)count_ - 1, found = -1;
      while (lo <= hi) {
         int32_t mid = lo + (hi - lo) / 2;
         uint32_t h = rd_u32(idx + (size_t)mid * 12);
         if (h < target) {
            lo = mid + 1;
         } else if (h > target) {
            hi = mid - 1;
         } else {
            found = mid;
            hi = mid - 1;  // 同 hash の先頭へ
         }
      }
      if (found < 0) return false;
      // 同 hash 範囲を surface 文字列で verify (衝突対策)
      for (int32_t i = found; i < (int32_t)count_; ++i) {
         const uint8_t *e = idx + (size_t)i * 12;
         if (rd_u32(e) != target) break;
         uint32_t off = rd_u32(e + 4);
         if (parse_record_surface_match(off, surface, out)) return true;
      }
      return false;
   }

   // E-CP4 (DLS-180): text の byte 位置 i から始まる全マッチを長さ別に返す。
   // Viterbi lattice 構築用。UTF-8 文字境界ごとに部分文字列を作り lookup() を反復する
   // (sorted-array hash trie には prefix 探索が無いので長さ別反復で代替。host 測定には十分)。
   struct Match {
      size_t byte_len;
      std::vector<TrieNode> nodes;
   };
   std::vector<Match> lookup_prefixes(const std::string &text, size_t i,
                                      size_t max_byte_len = 48) const {
      std::vector<Match> out;
      size_t limit = std::min(text.size(), i + max_byte_len);
      // i から UTF-8 文字境界ごとに end を伸ばす
      size_t j = i;
      while (j < limit) {
         // 次の文字境界へ (継続バイト 0x80..0xBF を読み飛ばす)
         ++j;
         while (j < text.size() && ((uint8_t)text[j] & 0xC0) == 0x80) ++j;
         std::vector<TrieNode> nodes;
         if (lookup(text.substr(i, j - i), nodes)) {
            out.push_back({j - i, std::move(nodes)});
         }
      }
      return out;
   }

 private:
   // record を off からパース。先頭の surface が一致したら out に node 列を詰めて true。
   bool parse_record_surface_match(uint32_t off, const std::string &surface,
                                   std::vector<TrieNode> &out) const {
      const uint8_t *p = base_ + off;
      const uint8_t *end = base_ + len_;
      if (p + 2 > end) return false;
      uint16_t slen = rd_u16(p);
      p += 2;
      if (p + slen > end) return false;
      if (slen != surface.size() || std::memcmp(p, surface.data(), slen) != 0) return false;
      p += slen;
      if (p + 2 > end) return false;
      uint16_t ncount = rd_u16(p);
      p += 2;
      out.clear();
      out.reserve(ncount);
      for (uint16_t n = 0; n < ncount; ++n) {
         TrieNode nd;
         std::string *fields[7] = {&nd.pron, &nd.pos, &nd.pos_group1, &nd.pos_group2,
                                   &nd.pos_group3, &nd.ctype, &nd.cform};
         for (int k = 0; k < 7; ++k) {
            if (p + 2 > end) return false;
            uint16_t fl = rd_u16(p);
            p += 2;
            if (p + fl > end) return false;
            fields[k]->assign(reinterpret_cast<const char *>(p), fl);
            p += fl;
         }
         if (p + 4 > end) return false;
         nd.acc = rd_i16(p);
         nd.chain_flag = rd_i16(p + 2);
         p += 4;
         // ver2: base [len u16 | utf8] + wcost i32
         if (p + 2 > end) return false;
         uint16_t bl = rd_u16(p);
         p += 2;
         if (p + bl > end) return false;
         nd.base.assign(reinterpret_cast<const char *>(p), bl);
         p += bl;
         if (p + 4 > end) return false;
         nd.wcost = rd_i32(p);
         p += 4;
         out.push_back(std::move(nd));
      }
      return true;
   }

   const uint8_t *base_ = nullptr;  // FTRI binary 先頭 (非所有 span)
   size_t len_ = 0;
   std::vector<uint8_t> owned_;     // host load(path) のみ使用 (fw は attach で非所有)
   uint32_t count_ = 0;
   uint16_t ver_ = 0;
};

// ---- tier1_njd.tsv 読込 (round-trip 期待値) ----
// surface → ノード列。tsv は surface 連続・node_idx 昇順前提。
// 重複見出し (非連続再登場) は trie_build.py と同様に最初を採用。
inline std::map<std::string, std::vector<TrieNode>> read_njd_tsv(const std::string &path) {
   std::ifstream ifs(path);
   if (!ifs) throw std::runtime_error("cannot open: " + path);
   std::map<std::string, std::vector<TrieNode>> out;
   std::string line;
   bool header = true;
   std::string cur;  // 直近 surface (連続性判定)
   while (std::getline(ifs, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (header) {
         header = false;
         continue;
      }
      if (line.empty()) continue;
      std::vector<std::string> f;
      std::string cell;
      std::stringstream ss(line);
      while (std::getline(ss, cell, '\t')) f.push_back(cell);
      // surface,node_idx,pron,pos,pg1,pg2,pg3,ctype,cform,acc,chain_flag,base,wcost = 13列 (ver2)
      if (f.size() != 13)
         throw std::runtime_error("tier1_njd_infl.tsv: 13列でない行: " + line);
      const std::string &surf = f[0];
      int node_idx = std::stoi(f[1]);
      // 非連続で同一 surface が node_idx=0 で再登場 = 重複見出し → 棄却
      if (node_idx == 0 && out.count(surf) && cur != surf) continue;
      TrieNode nd;
      nd.pron = f[2]; nd.pos = f[3]; nd.pos_group1 = f[4]; nd.pos_group2 = f[5];
      nd.pos_group3 = f[6]; nd.ctype = f[7]; nd.cform = f[8];
      nd.acc = std::stoi(f[9]); nd.chain_flag = std::stoi(f[10]);
      nd.base = f[11]; nd.wcost = std::stoi(f[12]);
      out[surf].push_back(nd);
      cur = surf;
   }
   return out;
}

inline bool node_eq(const TrieNode &a, const TrieNode &b) {
   return a.pron == b.pron && a.pos == b.pos && a.pos_group1 == b.pos_group1 &&
          a.pos_group2 == b.pos_group2 && a.pos_group3 == b.pos_group3 &&
          a.ctype == b.ctype && a.cform == b.cform && a.acc == b.acc &&
          a.chain_flag == b.chain_flag && a.base == b.base && a.wcost == b.wcost;
}

}  // namespace trie

#endif /* !TRIE_IO_H */

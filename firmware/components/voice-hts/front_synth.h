/* ----------------------------------------------------------------- */
/* Phase G fw 統合 (DLS-186): 任意テキスト → fullcontext label 共有層   */
/*                                                                   */
/* host (frontend_parity) と fw (voice_hts) が共有する単一エントリ:     */
/*                                                                   */
/*   front::front_synthesize_labels(text, trie, acc_ctx, kanji_fb)    */
/*     = 自前 Viterbi 分割 (wakati) → 内容語=Trie生値 / unknown=漢字    */
/*       side table or カタカナ最小 → NJD 4パス正規化 → JPCommon label。*/
/*                                                                   */
/* これは frontend_parity.cpp の C3 経路 (DLS-183「真の fw 到達ヘッド    */
/* ライン」= fw が実際に持てる情報だけ) をそのまま切り出したもの。      */
/* frontend_parity はこのヘッダを include して C3 を本関数呼び出しに     */
/* 置換することで、切り出しが C3 数値を変えないことを回帰で証明する。    */
/*                                                                   */
/* 全関数 inline (header-only、複数 TU include でも ODR 安全)。         */
/* side table ローダは istream 版 (パース本体) + path 版 (host ifstream) */
/* に分離し、fw は LittleFS で読んだ文字列を istringstream で渡せる。    */
/* ----------------------------------------------------------------- */
#ifndef FRONT_SYNTH_H
#define FRONT_SYNTH_H

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <istream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "mora_count.h"
#include "trie_io.h"
#include "wakati.h"

extern "C" {
#include "njd.h"
#include "njd_front.h"
#include "njd_set_digit.h"  /* upstream: 数字→漢数字読み (raw-text 経路のみ apply) */
#include "jpcommon.h"
#include "jpcommon_front.h"
}

namespace front {

// ---- tab 分割 ----
inline std::vector<std::string> tsv_split(const std::string &line) {
   std::vector<std::string> f;
   std::string cell;
   std::stringstream ss(line);
   while (std::getline(ss, cell, '\t')) f.push_back(cell);
   return f;
}

// ---- side table 型 ----
// acc_ctx: surface → [(ctx_key=後続形態素, acc), ...] (DLS-181 右文脈 override)
typedef std::map<std::string, std::vector<std::pair<std::string, int>>> CtxTable;
// kanji_fallback: 漢字1文字 → 代表読み (DLS-182 unknown 注入)
struct KFEntry {
   std::string pron, pos, confidence;
   int acc = 0;
};
typedef std::map<std::string, KFEntry> KFTable;

// ---- side table ローダ (istream 版 = パース本体 / path 版 = host ifstream) ----
inline CtxTable parse_acc_ctx(std::istream &ifs) {
   CtxTable out;
   std::string line;
   bool h = true;
   while (std::getline(ifs, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (h) { h = false; continue; }
      if (line.empty()) continue;
      auto f = tsv_split(line);
      if (f.size() != 3) throw std::runtime_error("tier1_acc_ctx.tsv: 3列でない行: " + line);
      out[f[0]].push_back({f[1], std::stoi(f[2])});
   }
   return out;
}
inline CtxTable read_acc_ctx_tsv(const std::string &path) {
   std::ifstream ifs(path);
   if (!ifs) throw std::runtime_error("cannot open: " + path);
   return parse_acc_ctx(ifs);
}

inline KFTable parse_kanji_fallback(std::istream &ifs) {
   KFTable out;
   std::string line;
   bool h = true;
   while (std::getline(ifs, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (h) { h = false; continue; }
      if (line.empty()) continue;
      auto f = tsv_split(line);
      if (f.size() != 5) throw std::runtime_error("tier1_kanji_fallback.tsv: 5列でない行: " + line);
      KFEntry e;
      e.pron = f[1];
      e.acc = std::stoi(f[2]);
      e.pos = f[3];
      e.confidence = f[4];
      out[f[0]] = e;
   }
   return out;
}
inline KFTable read_kanji_fallback_tsv(const std::string &path) {
   std::ifstream ifs(path);
   if (!ifs) throw std::runtime_error("cannot open: " + path);
   return parse_kanji_fallback(ifs);
}

// 数字/助数詞ノードテーブル (DLS-232 CP4): njd_set_digit に食わせる入力ノードの供給。
// digit=全角数字 ０-９ → 名詞,数+基底カナ / counter=助数詞 年/人/号… → 接尾+基底読み。
// tier1_digit.tsv / tier1_counter.tsv 共通形式 (6列): surface pron pos pos_group1 pos_group2 acc
// (mora_size は使用時に pron から実行時導出。既存 tier1_njd と同方針で 14列化しない)
struct NodeEntry {
   std::string pron, pos, pos_group1, pos_group2;
   int acc = 0;
};
typedef std::map<std::string, NodeEntry> NodeTable;

inline NodeTable parse_node_table(std::istream &ifs) {
   NodeTable out;
   std::string line;
   bool h = true;
   while (std::getline(ifs, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (h) { h = false; continue; }
      if (line.empty()) continue;
      auto f = tsv_split(line);
      if (f.size() != 6) throw std::runtime_error("digit/counter tsv: 6列でない行: " + line);
      NodeEntry e;
      e.pron = f[1];
      e.pos = f[2];
      e.pos_group1 = f[3];
      e.pos_group2 = f[4];
      e.acc = std::stoi(f[5]);
      out[f[0]] = e;
   }
   return out;
}
inline NodeTable read_node_table_tsv(const std::string &path) {
   std::ifstream ifs(path);
   if (!ifs) throw std::runtime_error("cannot open: " + path);
   return parse_node_table(ifs);
}

// surface の後続文字列で最長一致する acc override を返す。無ければ -1。
inline int ctx_override(const CtxTable &tbl, const std::string &surface,
                        const std::string &text, size_t span_end) {
   auto it = tbl.find(surface);
   if (it == tbl.end()) return -1;
   std::string after = (span_end <= text.size()) ? text.substr(span_end) : std::string();
   int best_acc = -1;
   size_t best_len = 0;
   for (const auto &kv : it->second) {
      const std::string &key = kv.first;
      if (key.size() > best_len && after.rfind(key, 0) == 0) {
         best_len = key.size();
         best_acc = kv.second;
      }
   }
   return best_acc;
}

// ---- UTF-8 ユーティリティ ----
// s が UTF-8 ちょうど 1 文字ならそのコードポイントを返す。複数文字・不正列は 0。
inline uint32_t single_codepoint_of(const std::string &s) {
   if (s.empty()) return 0;
   uint32_t cp = 0;
   size_t n = 0;
   uint8_t c0 = (uint8_t)s[0];
   if (c0 < 0x80) { cp = c0; n = 1; }
   else if ((c0 & 0xE0) == 0xC0) { cp = c0 & 0x1F; n = 2; }
   else if ((c0 & 0xF0) == 0xE0) { cp = c0 & 0x0F; n = 3; }
   else if ((c0 & 0xF8) == 0xF0) { cp = c0 & 0x07; n = 4; }
   else return 0;
   if (s.size() != n) return 0;
   for (size_t i = 1; i < n; ++i) {
      if (((uint8_t)s[i] & 0xC0) != 0x80) return 0;
      cp = (cp << 6) | ((uint8_t)s[i] & 0x3F);
   }
   return cp;
}

inline bool is_single_kanji_utf8(const std::string &s) {
   const uint32_t cp = single_codepoint_of(s);
   return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
          (cp >= 0xF900 && cp <= 0xFAFF);
}

// 無声化マーカー「’」(U+2019) を除去 (njd 4パスで再付与させるため)。
inline std::string strip_unvoice(const std::string &s) {
   std::string out;
   out.reserve(s.size());
   for (size_t i = 0; i < s.size();) {
      if (i + 3 <= s.size() && (uint8_t)s[i] == 0xE2 && (uint8_t)s[i + 1] == 0x80 &&
          (uint8_t)s[i + 2] == 0x99) { i += 3; }
      else { out.push_back(s[i]); ++i; }
   }
   return out;
}

// ひらがな (U+3041..U+3096) → カタカナ (+0x60)。機能語 unknown の fw 最小 pron 生成用。
inline std::string hira_to_kata(const std::string &s) {
   std::string out;
   out.reserve(s.size());
   for (size_t i = 0; i < s.size();) {
      uint8_t c0 = (uint8_t)s[i];
      size_t n = (c0 < 0x80) ? 1 : (c0 & 0xE0) == 0xC0 ? 2 : (c0 & 0xF0) == 0xE0 ? 3 : 4;
      if (i + n > s.size()) { out.push_back(s[i]); ++i; continue; }
      uint32_t cp = 0;
      if (n == 1) cp = c0;
      else if (n == 2) cp = c0 & 0x1F;
      else if (n == 3) cp = c0 & 0x0F;
      else cp = c0 & 0x07;
      for (size_t k = 1; k < n; ++k) cp = (cp << 6) | ((uint8_t)s[i + k] & 0x3F);
      if (cp >= 0x3041 && cp <= 0x3096) cp += 0x60;  // ひら→カタ
      if (cp < 0x80) out.push_back((char)cp);
      else if (cp < 0x800) {
         out.push_back((char)(0xC0 | (cp >> 6)));
         out.push_back((char)(0x80 | (cp & 0x3F)));
      } else {
         out.push_back((char)(0xE0 | (cp >> 12)));
         out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
         out.push_back((char)(0x80 | (cp & 0x3F)));
      }
      i += n;
   }
   return out;
}

// 半角→全角正規化 (njd_set_digit 前段)。pyopenjtalk は text2mecab で ASCII を全角化して
// から mecab に渡すが、njd_set_digit の numeral_list は全角キーのみ (ASCII キー無し) ゆえ
// 数字を全角化しないと発火しない。text2mecab_rule_utf_8.h の数字関連サブセットを再現:
//   ASCII 0-9 → 全角 ０-９ (U+FF10..FF19) を無条件変換
//   . , % - → ．，％− は「数字に隣接する時のみ」変換 (小数点/カンマ/％/範囲)
// 非 digit 発話 (ASCII 数字を含まない = _digit group 定義の補集合) は入力バイト不変 →
// wakati/njd 出力不変 = 非回帰を text 段階で保証する。
inline std::string normalize_fullwidth_digits(const std::string &text) {
   auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
   auto push_fw = [](std::string &out, uint32_t cp) {  // BMP 3byte UTF-8
      out.push_back((char)(0xE0 | (cp >> 12)));
      out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back((char)(0x80 | (cp & 0x3F)));
   };
   std::string out;
   out.reserve(text.size());
   for (size_t i = 0; i < text.size(); ++i) {
      char c = text[i];
      if (is_digit(c)) { push_fw(out, 0xFF10 + (uint32_t)(c - '0')); continue; }
      bool adj = (i > 0 && is_digit(text[i - 1])) ||
                 (i + 1 < text.size() && is_digit(text[i + 1]));
      if (adj) {
         if (c == '.') { push_fw(out, 0xFF0E); continue; }  // ．
         if (c == ',') { push_fw(out, 0xFF0C); continue; }  // ，
         if (c == '%') { push_fw(out, 0xFF05); continue; }  // ％
         if (c == '-') { push_fw(out, 0x2212); continue; }  // − (text2mecab)
      }
      out.push_back(c);
   }
   return out;
}

// ---- 1 ノードの構築結果 (njd へ push する確定値) ----
struct BuiltNode {
   std::string string_, pos, pos_group1, pos_group2, pos_group3, ctype, cform, orig, pron;
   int acc = 0, mora_size = 0, chain_flag = -1;
   std::string chain_rule;
};

// BuiltNode 列に njd 4パスを適用する。repass=false なら入力をそのまま返す。
inline std::vector<BuiltNode> normalize_nodes(const std::vector<BuiltNode> &nodes, bool repass,
                                              bool apply_digit = false) {
   std::vector<BuiltNode> final_nodes = nodes;
   if (repass) {
      NJD njd;
      NJD_initialize(&njd);
      for (const auto &b : nodes) {
         NJDNode *nd = (NJDNode *)calloc(1, sizeof(NJDNode));
         NJDNode_initialize(nd);
         NJDNode_set_string(nd, b.string_.c_str());
         NJDNode_set_pos(nd, b.pos.c_str());
         NJDNode_set_pos_group1(nd, b.pos_group1.c_str());
         NJDNode_set_pos_group2(nd, b.pos_group2.c_str());
         NJDNode_set_pos_group3(nd, b.pos_group3.c_str());
         NJDNode_set_ctype(nd, b.ctype.c_str());
         NJDNode_set_cform(nd, b.cform.c_str());
         NJDNode_set_orig(nd, b.orig.c_str());
         NJDNode_set_read(nd, b.pron.c_str());
         NJDNode_set_pron(nd, b.pron.c_str());
         NJDNode_set_acc(nd, b.acc);
         NJDNode_set_mora_size(nd, b.mora_size);
         NJDNode_set_chain_rule(nd, b.chain_rule.c_str());
         NJDNode_set_chain_flag(nd, -1);
         NJD_push_node(&njd, nd);
      }
      // pyopenjtalk run_frontend 順 (pron→digit→accent_phrase…): digit は 4パスの前。
      // raw-text 経路 (front_synthesize_labels) のみ apply_digit=true。golden oracle を
      // 直接投入する C0/C1/C2 は既定 false ゆえ二重変換しない (呼び出し式無改変で保証)。
      if (apply_digit) njd_set_digit(&njd);
      njd_front_normalize_context(&njd);
      final_nodes.clear();
      for (NJDNode *p = njd.head; p != NULL; p = p->next) {
         BuiltNode b;
         b.pos = NJDNode_get_pos(p);
         b.pos_group1 = NJDNode_get_pos_group1(p);
         b.pos_group2 = NJDNode_get_pos_group2(p);
         b.pos_group3 = NJDNode_get_pos_group3(p);
         b.ctype = NJDNode_get_ctype(p);
         b.cform = NJDNode_get_cform(p);
         b.pron = NJDNode_get_pron(p);
         b.acc = NJDNode_get_acc(p);
         b.chain_flag = NJDNode_get_chain_flag(p);
         final_nodes.push_back(b);
      }
      NJD_clear(&njd);
   }
   return final_nodes;
}

// BuiltNode 列から njd 4パス → jpcommon ラベルを生成。repass=false なら njd を通さず直接。
inline std::vector<std::string> synth_labels(const std::vector<BuiltNode> &nodes, bool repass,
                                             bool apply_digit = false) {
   std::vector<BuiltNode> final_nodes = normalize_nodes(nodes, repass, apply_digit);
   JPCommon jp;
   JPCommon_initialize(&jp);
   for (const auto &b : final_nodes) {
      jpcommon_front_push(&jp, b.pron.c_str(), b.pos.c_str(), b.pos_group1.c_str(),
                          b.pos_group2.c_str(), b.pos_group3.c_str(), b.ctype.c_str(),
                          b.cform.c_str(), b.acc, b.chain_flag);
   }
   JPCommon_make_label(&jp);
   int size = JPCommon_get_label_size(&jp);
   char **feat = JPCommon_get_label_feature(&jp);
   std::vector<std::string> out;
   out.reserve(size > 2 ? size - 2 : 0);
   for (int i = 0; i < size; ++i) out.push_back(feat[i] ? feat[i] : "");
   JPCommon_clear(&jp);
   return out;
}

// Trie 単一hit ノードを fw フィールドで BuiltNode 化 (内容語 fw 経路)。
//   chain_rule_empty: true=空 (fw), false=golden 流用 (chain_rule 寄与分離用、golden 値を渡す)
inline BuiltNode from_trie(const std::string &surface, const trie::TrieNode &tn,
                           int acc_override, bool chain_rule_empty,
                           const std::string &golden_chain_rule) {
   BuiltNode b;
   b.string_ = surface; b.pos = tn.pos; b.pos_group1 = tn.pos_group1;
   b.pos_group2 = tn.pos_group2; b.pos_group3 = tn.pos_group3; b.ctype = tn.ctype;
   b.cform = tn.cform; b.orig = tn.base.empty() ? surface : tn.base;
   b.pron = strip_unvoice(tn.pron);
   b.acc = (acc_override >= 0) ? acc_override : tn.acc;
   b.mora_size = mora::mora_count(b.pron);  // ← 実行時導出 (Trie 14列化せず)
   b.chain_rule = chain_rule_empty ? "" : golden_chain_rule;
   b.chain_flag = -1;
   return b;
}

// DLS-036: 辞書にない英字は文字名で読む。辞書語の発音は呼び出し側で優先する。
inline const char *alphabet_pron(const std::string &surface) {
   uint32_t cp = single_codepoint_of(surface);
   if (cp >= 0xFF21 && cp <= 0xFF3A) cp -= 0xFEE0;
   if (cp >= 0xFF41 && cp <= 0xFF5A) cp -= 0xFEE0;
   if (cp >= 'a' && cp <= 'z') cp -= 'a' - 'A';
   if (cp < 'A' || cp > 'Z') return nullptr;
   static const char *const names[] = {
      "エー", "ビー", "シー", "ディー", "イー", "エフ", "ジー", "エイチ", "アイ",
      "ジェー", "ケー", "エル", "エム", "エヌ", "オー", "ピー", "キュー", "アール",
      "エス", "ティー", "ユー", "ブイ", "ダブリュー", "エックス", "ワイ", "ゼット",
   };
   return names[cp - 'A'];
}

// 漢字 / 英字 fallback / カタカナ最小で unknown ノードを BuiltNode 化。
inline BuiltNode from_fw_minimal(const std::string &surface, const KFTable &kf, bool kf_on) {
   BuiltNode b;
   b.string_ = surface; b.orig = surface; b.chain_flag = -1; b.chain_rule = "";
   if (kf_on && is_single_kanji_utf8(surface)) {
      auto it = kf.find(surface);
      if (it != kf.end()) {
         b.pron = strip_unvoice(it->second.pron);
         b.acc = it->second.acc;
         b.pos = it->second.pos.empty() ? "名詞" : it->second.pos;
         b.pos_group1 = "一般"; b.pos_group2 = "*"; b.pos_group3 = "*";
         b.ctype = "*"; b.cform = "*";
         b.mora_size = mora::mora_count(b.pron);
         return b;
      }
   }
   // 非漢字 or side table 無し: カタカナ化して名詞最小扱い (安全網)。
   const char *letter = alphabet_pron(surface);
   b.pron = letter ? letter : hira_to_kata(surface);
   b.acc = 0;
   b.pos = "名詞"; b.pos_group1 = "一般"; b.pos_group2 = "*"; b.pos_group3 = "*";
   b.ctype = "*"; b.cform = "*";
   b.mora_size = mora::mora_count(b.pron);
   return b;
}

// 数字/助数詞ノードテーブル (tier1_digit/counter.tsv) から BuiltNode を組む (DLS-232 CP4)。
// string_=surface (全角数字 or 助数詞) を渡し、漢数字化・位ノード挿入・連濁補正は
// njd_set_digit が担う。mora_size は pron から実行時導出 (from_trie と同方針)。
inline BuiltNode from_node_table(const std::string &surface, const NodeEntry &e) {
   BuiltNode b;
   b.string_ = surface; b.orig = surface;
   b.pos = e.pos; b.pos_group1 = e.pos_group1; b.pos_group2 = e.pos_group2;
   b.pos_group3 = "*"; b.ctype = "*"; b.cform = "*";
   b.pron = e.pron;
   b.acc = e.acc;
   b.mora_size = mora::mora_count(b.pron);
   b.chain_flag = -1;
   b.chain_rule = "";
   return b;
}

// DLS-236: 句読点 → pause 判定。golden(pyopenjtalk njd_set_pronunciation) 準拠で「記号」を
// pause 化する。返り値: 0=非句読点 / 1=pause(pron="、") / 2=question(pron="？"=pause+疑問flag)。
// 対象は句読点マーク (読点・句点・感嘆・三点リーダ・中黒・全角セミコロン・各種括弧) + 疑問符。
// 数字/日付/小数に現れる記号 (，U+FF0C ．U+FF0E ／ ： − ＝ ％ 〜) は数値読みを壊すため除外
// (normalize_fullwidth_digits が数字隣接で生成する ，．も含む=「2,377」「3.14」を分断しない)。
// 句読点は wakati で必ず 1 文字 unknown セグメント化されるため surface 文字列一致で確実に捕捉。
inline int punct_pause_kind(const std::string &surface) {
   static const char *kPause[] = {
      "、", "。", "！", "…", "・", "；",
      "「", "」", "『", "』", "（", "）",
   };
   for (const char *p : kPause) if (surface == p) return 1;
   if (surface == "？") return 2;   // pron="？" → jpcommon が pau + 疑問 flag を立てる
   // 非発話文字 → pause。oracle (pyopenjtalk text2mecab/njd) は空白・絵文字を 記号,pron=、 に
   // 正規化する。従来これらは unknown ノード (pron=空白 or 壊れ pron) になり jpcommon が
   // 「wrong mora list」で捨ててノード構造を汚していた。BMP 外 (cp>=0x10000、絵文字等) は
   // 日本語発話文字が存在しないため一律 pause が安全 (🎵 等の個別列挙をしない)。
   const uint32_t cp = single_codepoint_of(surface);
   if (cp == 0x20 || cp == 0x3000 || cp >= 0x10000) return 1;
   return 0;
}

// DLS-236: 句読点 pause ノード。pos="記号" (njd_set_accent_phrase Rule14 で独立アクセント句化) +
// pron="、"/"？" を与えれば jpcommon_label が pau フルコンテキストラベルを自動生成する
// (ラベル文字列の手組みは不要)。mora=0 でアクセント句に属さない。pos_group1 はラベル不寄与のため "*"。
inline BuiltNode from_punctuation(const std::string &surface, bool question) {
   BuiltNode b;
   b.string_ = surface; b.orig = surface;
   b.pos = "記号"; b.pos_group1 = "*"; b.pos_group2 = "*"; b.pos_group3 = "*";
   b.ctype = "*"; b.cform = "*";
   b.pron = question ? "？" : "、";
   b.acc = 0;
   b.mora_size = 0;
   b.chain_flag = -1;
   b.chain_rule = "";
   return b;
}

// 動詞・形容詞の直後の「の」は、連体化助詞ではなく形式名詞として扱う。
// 例: 「みるのがすき」→「みる / の(名詞・非自立) / が」。所有の「ぼくの本」は前が
// 名詞なので対象外。NJD再正規化でchain_flagを既存規則から導出する (DLS-270)。
inline void retype_nominalizer_no(std::vector<BuiltNode> &nodes) {
   for (size_t i = 1; i < nodes.size(); ++i) {
      BuiltNode &prev = nodes[i - 1];
      BuiltNode &cur = nodes[i];
      if (cur.string_ != "の" || cur.pos != "助詞" || cur.pos_group1 != "連体化") continue;
      if (prev.pos != "動詞" && prev.pos != "形容詞") continue;
      cur.pos = "名詞";
      cur.pos_group1 = "非自立";
      cur.pos_group2 = "一般";
      cur.pos_group3 = "*";
      cur.ctype = "*";
      cur.cform = "*";
      cur.acc = 0;
      cur.chain_flag = -1;
      cur.chain_rule.clear();
   }
}

// DLS-264: 句節 pause 用の軽量境界判定。汎用形態素解析や辞書を追加せず、UTF-8 を
// 1 回だけ走査して高 precision な候補を「、」へ仮想変換する。この変換後テキストを
// 既存 front に 1 回だけ渡すので、DLS-263 の「解析済みノードへ後挿入」方式とは異なる。
struct Utf8Char {
   std::string bytes;
   uint32_t cp = 0;
};

inline std::vector<Utf8Char> utf8_chars(const std::string &text) {
   std::vector<Utf8Char> out;
   for (size_t i = 0; i < text.size();) {
      const uint8_t c0 = (uint8_t)text[i];
      size_t n = (c0 < 0x80) ? 1 : ((c0 & 0xE0) == 0xC0) ? 2 :
                 ((c0 & 0xF0) == 0xE0) ? 3 : ((c0 & 0xF8) == 0xF0) ? 4 : 1;
      if (i + n > text.size()) n = 1;
      Utf8Char c;
      c.bytes = text.substr(i, n);
      c.cp = single_codepoint_of(c.bytes);
      out.push_back(c);
      i += n;
   }
   return out;
}

inline bool phrase_boundary_punct(const std::string &s) {
   static const char *kPunct[] = {
      "、", "。", "！", "？", "!", "?", "…", "・", "；", "：", ",", ".", "，", "．",
      "/", "／", " ", "　", "\t", "\r", "\n",
   };
   for (const char *p : kPunct) if (s == p) return true;
   return false;
}

inline bool phrase_terminal(const std::string &s) {
   return s == "。" || s == "！" || s == "？" || s == "!" || s == "?";
}

inline bool phrase_hiragana(const Utf8Char &c) {
   return c.cp >= 0x3041 && c.cp <= 0x3096;
}

inline bool phrase_inside_protected(size_t pos,
                                    const std::vector<std::pair<size_t, size_t>> &spans) {
   for (const auto &span : spans) if (span.first < pos && pos < span.second) return true;
   return false;
}

inline bool phrase_candidate_ok(const std::vector<Utf8Char> &cs, size_t pos,
                                const std::vector<std::pair<size_t, size_t>> &spans) {
   if (pos == 0 || pos >= cs.size() || phrase_inside_protected(pos, spans) ||
       phrase_boundary_punct(cs[pos - 1].bytes) || phrase_boundary_punct(cs[pos].bytes)) return false;
   size_t left = pos;
   while (left > 0 && !phrase_terminal(cs[left - 1].bytes)) --left;
   size_t right = pos;
   while (right < cs.size() && !phrase_terminal(cs[right].bytes)) ++right;
   return pos - left >= 3 && right - pos >= 3;
}

inline bool phrase_is_non_topic_wa(const std::vector<Utf8Char> &cs, size_t i) {
   // 語頭の「は」を主題助詞と誤認しないための最小限の語形ガード。DLS-264 は
   // recall より誤挿入回避を優先する。"もう入った" の助詞も除外する。
   if (i + 1 >= cs.size() || cs[i + 1].bytes == "る" || cs[i + 1].bytes == "じ" ||
       cs[i + 1].bytes == "ん" || cs[i + 1].bytes == "く") return true;
   if (i >= 2 && cs[i - 2].bytes == "も" && cs[i - 1].bytes == "う" &&
       cs[i + 1].bytes == "い") return true;
   return i + 2 < cs.size() && cs[i + 1].bytes == "史" && cs[i + 2].bytes == "上";
}

inline std::string phrase_pause_text(const std::string &text) {
   const std::vector<Utf8Char> cs = utf8_chars(text);
   if (cs.size() < 7) return text;
   const bool outer_quote = cs.front().bytes == "「" && cs.back().bytes == "」";
   std::vector<std::pair<size_t, size_t>> spans;
   std::vector<std::pair<std::string, size_t>> stack;
   auto close_for = [](const std::string &open) -> const char * {
      if (open == "「") return "」";
      if (open == "『") return "』";
      if (open == "（") return "）";
      if (open == "(") return ")";
      if (open == "［") return "］";
      if (open == "[") return "]";
      return "";
   };
   for (size_t i = 0; i < cs.size(); ++i) {
      const std::string &s = cs[i].bytes;
      if (s == "「" || s == "『" || s == "（" || s == "(" || s == "［" || s == "[") {
         stack.push_back({s, i});
      } else if (!stack.empty() && s == close_for(stack.back().first)) {
         const auto open = stack.back(); stack.pop_back();
         if (!(outer_quote && open.first == "「" && open.second == 0 && i + 1 == cs.size()))
            spans.push_back({open.second, i + 1});
      }
   }
   std::vector<bool> insert(cs.size() + 1, false);
   struct Connective {
      const char *chars[3];
      size_t size;
      bool reject_after_n;
   };
   static const Connective kConnectives[] = {
      {{"か", "つ", ""}, 2, false}, {{"し", "か", "し"}, 3, false},
      {{"そ", "し", "て"}, 3, false}, {{"そ", "こ", "で"}, 3, false},
      {{"だ", "か", "ら"}, 3, true}, {{"た", "だ", "し"}, 3, false},
      {{"つ", "ま", "り"}, 3, false}, {{"な", "お", ""}, 2, false},
      {{"一", "方", ""}, 2, false},
   };
   for (size_t i = 0; i < cs.size(); ++i) {
      const std::string &s = cs[i].bytes;
      if (s == "は" && i > 0 && !phrase_boundary_punct(cs[i - 1].bytes) &&
          !phrase_is_non_topic_wa(cs, i) && phrase_candidate_ok(cs, i + 1, spans)) {
         insert[i + 1] = true;
      } else if (s == "と" && i > 0 && i + 1 < cs.size() && cs[i - 1].bytes == "る" &&
                 phrase_hiragana(cs[i + 1]) && phrase_candidate_ok(cs, i + 1, spans)) {
         insert[i + 1] = true;
      }
      for (const auto &word : kConnectives) {
         if (i + word.size > cs.size() || (word.reject_after_n && i > 0 && cs[i - 1].bytes == "ん"))
            continue;
         bool match = true;
         for (size_t k = 0; k < word.size; ++k) {
            if (cs[i + k].bytes != word.chars[k]) { match = false; break; }
         }
         if (match && phrase_candidate_ok(cs, i, spans)) insert[i] = true;
      }
   }
   std::string out;
   out.reserve(text.size() + 3 * cs.size() / 8);
   for (size_t i = 0; i <= cs.size(); ++i) {
      if (insert[i]) out += "、";
      if (i < cs.size()) out += cs[i].bytes;
   }
   return out;
}

// Keep the configured/displayed listen replies unchanged while supplying a
// reading the reduced device dictionary can pronounce. Match only the fixed
// replies, IR request replies, and complete generated clock template.
inline std::string listen_reply_reading(const std::string &text) {
   if (text == "うしろに下がるよ") return "ウシロに下がるよ";
   if (text == "もう一回、お話して") return "もう1回、オハナシシテ";
   if (text == "うしろにさがって！") return "ウシロにさがって！";
   if (text == "ちょっとうしろへもどって") return "ちょっとウシロへもどって";
   if (text == "うしろにさがってくれる？") return "ウシロにさがってくれる？";
   if (text == "あぶない、さがって〜！") return "あぶない、さがってー！";
   const std::string prefix = "いまは";
   if (text.compare(0, prefix.size(), prefix) != 0) return text;
   size_t pos = prefix.size();
   auto number = [&](unsigned &value) {
      value = 0;
      const size_t begin = pos;
      while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
         if (pos - begin >= 2) return false;
         value = value * 10 + static_cast<unsigned>(text[pos++] - '0');
      }
      return pos > begin;
   };
   unsigned hour, minute;
   if (!number(hour) || text.compare(pos, std::string("時").size(), "時") != 0) return text;
   pos += std::string("時").size();
   if (!number(minute) || text.substr(pos) != "分だよ" || hour > 23 || minute > 59) return text;
   static const char *hours[] = {
      "レイ", "イチ", "ニ", "サン", "ヨ", "ゴ", "ロク", "シチ", "ハチ", "ク",
      "ジュウ", "ジュウイチ", "ジュウニ", "ジュウサン", "ジュウヨ", "ジュウゴ",
      "ジュウロク", "ジュウシチ", "ジュウハチ", "ジュウク", "ニジュウ",
      "ニジュウイチ", "ニジュウニ", "ニジュウサン",
   };
   static const char *minute_units[] = {
      "レイフン", "イップン", "ニフン", "サンプン", "ヨンプン", "ゴフン",
      "ロップン", "ナナフン", "ハップン", "キュウフン",
   };
   static const char *tens[] = {"", "", "ニ", "サン", "ヨン", "ゴ"};
   std::string minutes;
   if (minute < 10) {
      minutes = minute_units[minute];
   } else {
      minutes = tens[minute / 10];
      minutes += minute % 10 == 0 ? "ジュップン" : "ジュウ";
      if (minute % 10 != 0) minutes += minute_units[minute % 10];
   }
   return prefix + hours[hour] + "ジ" + minutes + "だよ";
}

inline std::vector<BuiltNode> front_build_nodes(const std::string &text,
                                                const trie::Trie &trie,
                                                const CtxTable &acc_ctx,
                                                const KFTable &kanji_fb,
                                                const NodeTable &digit,
                                                const NodeTable &counter) {
   const bool ctx_on = !acc_ctx.empty();
   const bool kf_on = !kanji_fb.empty();
   // 半角数字→全角 (njd_set_digit 前段)。非 digit 発話は ntext==text でバイト不変。
   const std::string ntext = normalize_fullwidth_digits(phrase_pause_text(listen_reply_reading(text)));
   std::vector<wakati::LatNode> seg = wakati::segment(ntext, trie);
   std::vector<BuiltNode> nodes;
   bool prev_digit = false;  // 直前ノードが数字ラン (pos_group1=数) か
   for (const auto &ln : seg) {
      std::string surface = ntext.substr(ln.begin, ln.end - ln.begin);
      // (0) 句読点 → pause ノード (DLS-236)。複合語/数字分岐より前に置く。連続句読点
      //     (。」/！？/…。等) は jpcommon が連鎖 pause の 2 個目を落とす (警告) ため
      //     golden 準拠で先に 1 pause へ畳む。ただし ？ は疑問 flag を優先する。
      int pk = punct_pause_kind(surface);
      if (pk != 0) {
         const bool prev_pause = !nodes.empty() && nodes.back().pos == "記号"
                                 && (nodes.back().pron == "、" || nodes.back().pron == "？");
         if (!prev_pause) {
            nodes.push_back(from_punctuation(surface, pk == 2));
         } else if (pk == 2 && nodes.back().pron == "、") {
            nodes.back() = from_punctuation(surface, true);  // 、+？ → 疑問を残す
         }
         prev_digit = false;
         continue;
      }
      // (1) 全角数字ノード: 名詞,数 + 基底カナ (njd_set_digit の入力)。
      auto dit = digit.find(surface);
      if (dit != digit.end()) {
         nodes.push_back(from_node_table(surface, dit->second));
         prev_digit = true;
         continue;
      }
      // (2) 助数詞: 直前が数字ランの時のみ付け替え (単独読み=Trie を温存)。
      if (prev_digit) {
         auto cit = counter.find(surface);
         if (cit != counter.end()) {
            nodes.push_back(from_node_table(surface, cit->second));
            prev_digit = false;
            continue;
         }
      }
      prev_digit = false;
      // (3) 通常経路: 内容語=Trie 生値 / unknown=漢字 fallback or カタカナ最小。
      if (!ln.unknown && !ln.nodes.empty()) {
         // 複合語 (複数ノード) は全ノードを emit する。従来は nodes[0] のみで末尾ノード
         // (記念日→ビ, 令和→ワ 等 90 複合語) が脱落し読みが切れていた (DLS-232 CP7 修正)。
         // acc override は surface 全体キーゆえ先頭ノードのみ適用 (単一ノード時は従来と完全一致)。
         int ov = ctx_on ? ctx_override(acc_ctx, surface, ntext, ln.end) : -1;
         for (size_t k = 0; k < ln.nodes.size(); ++k) {
            nodes.push_back(from_trie(surface, ln.nodes[k], (k == 0) ? ov : -1, true, ""));
         }
      } else {
         nodes.push_back(from_fw_minimal(surface, kanji_fb, kf_on));
      }
   }
   retype_nominalizer_no(nodes);
   return nodes;
}

// ---- 任意テキスト → fullcontext label 列 (host/fw 共有エントリ) ----
// = frontend_parity C3 経路。空 table を渡すと該当 fallback 無効 (ctx/kf/digit OFF)。
inline std::vector<std::string> front_synthesize_labels(const std::string &text,
                                                        const trie::Trie &trie,
                                                        const CtxTable &acc_ctx,
                                                        const KFTable &kanji_fb,
                                                        const NodeTable &digit,
                                                        const NodeTable &counter) {
   std::vector<BuiltNode> nodes = front_build_nodes(text, trie, acc_ctx, kanji_fb, digit, counter);
   return synth_labels(nodes, true, /*apply_digit=*/true);
}

}  // namespace front

#endif /* !FRONT_SYNTH_H */

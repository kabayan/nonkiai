/* ----------------------------------------------------------------- */
/* Phase E E-CP4 (DLS-180): 表記列 lattice + Viterbi 分かち書き (案α)    */
/*                                                                   */
/* 内容語表記 Trie (ver2, tier1_infl_trie.bin) を使い、正規化済み文字列  */
/* を最小コスト経路で分割する。コスト = 各語の wcost (word cost) + ノード */
/* 固定ペナルティ。Trie に当たらない位置は 1 文字 unknown                   */
/* ノードを高コストで挿入し経路が途切れないようにする。                  */
/*                                                                   */
/* tier1 辞書は内容語主体 (機能語の大半は非収録) なので、本分割は         */
/* 「内容語スパンの回復」を担う。機能語領域は unknown で埋まる。          */
/* (漢字 1 文字 fallback の read 注入は Phase F、unknown フラグが接続点)  */
/* ----------------------------------------------------------------- */
#ifndef WAKATI_H
#define WAKATI_H

#include <string>
#include <vector>

#include "trie_io.h"

namespace wakati {

// 全実在語 wcost の最大 (~1422) より十分大きい unknown ノードのコスト。
// これにより辞書語が当たる位置では必ず辞書語が選ばれる。
constexpr long UNK_COST = 3000;

// 低 wcost 機能語が語内部へ食い込む過分割を抑えるためのノード固定コスト。
// 例: こんばんは(1422)+450 < こんばん(889)+は(100)+2*450。
constexpr long NODE_COST = 450;

// lattice 上で採用された 1 語 (begin,end は text 内 byte 範囲 [begin,end))。
struct LatNode {
   size_t begin = 0, end = 0;
   std::vector<trie::TrieNode> nodes;  // Trie ヒット時のノード列 (unknown 時は空)
   long wcost = 0;
   bool unknown = false;               // Trie miss の 1 文字 fallback か
};

// text (正規化済み文字列) を分割し採用ノード列 (begin 昇順・隙間/重複なし) を返す。
// t は ver2 Trie。unk_cost は unknown ノードのコスト (既定 UNK_COST)。
std::vector<LatNode> segment(const std::string &text, const trie::Trie &t,
                             long unk_cost = UNK_COST);

}  // namespace wakati

#endif /* !WAKATI_H */

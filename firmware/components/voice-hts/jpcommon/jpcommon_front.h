/* ----------------------------------------------------------------- */
/* DLS-174 HTS フロント整備: JPCommon フロントアダプタ                 */
/*                                                                   */
/* upstream/njd2jpcommon.c は njd.h (NJDNode + njd モジュール全体) に  */
/* 依存するため、本アダプタは njd2jpcommon の変換3関数のみを移植し    */
/* (jpcommon_front.cpp 内)、NJD特徴量9フィールドを直接 JPCommon へ    */
/* push する薄い API を提供する。フロント (Trie/Viterbi/漢字fallback) */
/* が生成した1ノードぶんの特徴量を、語ごとに本関数へ流す想定。        */
/* ----------------------------------------------------------------- */
#ifndef JPCOMMON_FRONT_H
#define JPCOMMON_FRONT_H

#include <stdio.h>    /* upstream jpcommon.h が FILE を前方宣言なしで使うため先行 include */
#include "jpcommon.h" /* upstream: JPCommon / JPCommonNode */

#ifdef __cplusplus
extern "C" {
#endif

/* NJD特徴量1ノードを JPCommon へ push する。
 * pos / pos_group1..3 / ctype / cform は naist-jdic 表記の生文字列
 * (例: pos="名詞", pos_group1="普通名詞", ctype="*", cform="*")。
 * 内部で njd2jpcommon 変換表により JPCommon 中間文字列へ変換してから
 * JPCommonNode 化する (upstream njd2jpcommon() と同一処理)。
 * pron は読み (カタカナ + 長音「ー」/ 無声化「’」マーカー込み)。 */
void jpcommon_front_push(JPCommon *jpcommon,
                         const char *pron,
                         const char *pos, const char *pos_group1,
                         const char *pos_group2, const char *pos_group3,
                         const char *ctype, const char *cform,
                         int acc, int chain_flag);

#ifdef __cplusplus
}
#endif

#endif /* !JPCOMMON_FRONT_H */

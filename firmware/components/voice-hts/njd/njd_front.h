/* ----------------------------------------------------------------- */
/* DLS Phase E 文脈正規化: NJD フロントアダプタ                        */
/*                                                                   */
/* Trie/Viterbi が生成した NJD ノード列 (= Open JTalk の                */
/* njd_set_digit 直後に相当する「語の素の特徴量」) に、文脈依存の      */
/* 正規化4パスを pyopenjtalk run_frontend と同一順序で適用する。       */
/* 規則ロジックは upstream/ の無改変ソースが持つ。本アダプタは正準な   */
/* パス順を1箇所に集約するだけ (host parity と Phase G fw が共有)。    */
/* ----------------------------------------------------------------- */
#ifndef NJD_FRONT_H
#define NJD_FRONT_H

#include <stdio.h> /* upstream njd.h が FILE を前方宣言なしで使うため先行 include */
#include "njd.h"    /* upstream: NJD / NJDNode */

#ifdef __cplusplus
extern "C" {
#endif

/* NJD に文脈正規化を適用する。
 * pyopenjtalk 0.4.1 openjtalk.pyx:216-219 と同一順序:
 *   accent_phrase → accent_type → unvoiced_vowel → long_vowel
 * 入力 NJD は njd_set_pronunciation / digit 相当まで済んだ状態
 * (pron は無声化マーカー無し、acc は語の素のアクセント核、chain_flag=-1)。
 * 適用後、文中の chain_flag (0/1)・アクセント核移動・無声化「’」・長音「ー」が
 * 反映される。 */
void njd_front_normalize_context(NJD *njd);

#ifdef __cplusplus
}
#endif

#endif /* !NJD_FRONT_H */

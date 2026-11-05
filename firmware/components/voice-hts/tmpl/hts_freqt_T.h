/* ----------------------------------------------------------------- */
/* DLS-169 段3: メルケプストラムの周波数変換 (freqt) の template 移植。     */
/*                                                                   */
/* 正本 = 純C `lib/HTS_vocoder.c` の HTS_freqt (260-285、Oppenheim 再帰)。 */
/* α_src 基準のメルケプ c1 を α_dst 基準の c2 に再ワーピングする。           */
/* ワーピング係数 a = (α_dst - α_src) / (1 - α_src·α_dst)。              */
/* a=0 (α_dst=α_src) のとき恒等変換 c2==c1 (self-check に使う)。           */
/*                                                                   */
/* 用途: 48kHz 学習モデル (α=0.55) を 16kHz 直接合成する際、mgc を         */
/* 16kHz 基準 α' へ変換してから VocoderT を rate=16000 で駆動する。        */
/* 純C は内部作業バッファ v->freqt_buff を使い回すが、template では旧 g を    */
/* work[] に退避して同一漸化式を再現する (状態を持たない pure 関数)。        */
/* ----------------------------------------------------------------- */
#ifndef HTS_FREQT_T_H
#define HTS_FREQT_T_H

#include <cstddef>
#include <vector>

namespace hts_tmpl {

/* freqt: c1[0..m1] (α_src 基準) → c2[0..m2] (α_dst 基準)。a=ワーピング係数。
   c2 は呼び出し側で m2+1 確保。HTS_freqt と同一漸化式。 */
template <typename Real>
inline void freqt_T(const Real *c1, int m1, Real *c2, int m2, Real a)
{
   const Real b = (Real) 1.0 - a * a;
   std::vector<Real> work(m2 + 1, (Real) 0.0);  /* 純C の freqt_buff (前段 g の退避) */
   std::vector<Real> g(m2 + 1, (Real) 0.0);

   for (int i = -m1; i <= 0; i++) {
      if (0 <= m2) {
         work[0] = g[0];
         g[0] = c1[-i] + a * work[0];
      }
      if (1 <= m2) {
         work[1] = g[1];
         g[1] = b * work[0] + a * work[1];
      }
      for (int j = 2; j <= m2; j++) {
         work[j] = g[j];
         g[j] = work[j - 1] + a * (work[j] - g[j - 1]);
      }
   }

   for (int i = 0; i <= m2; i++)
      c2[i] = g[i];
}

}  /* namespace hts_tmpl */

#endif  /* HTS_FREQT_T_H */

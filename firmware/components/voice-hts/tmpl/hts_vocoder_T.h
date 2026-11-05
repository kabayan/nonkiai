/* ----------------------------------------------------------------- */
/* DLS-166 ST1: HTS ボコーダ (MLSA + 混合励振) の template<Real,Acc> 移植   */
/*                                                                   */
/* 正本 = vendored 純C `lib/HTS_vocoder.c` の stage=0 (MCP) 活性経路のみ。 */
/*   写経した全行は host 段A0 (template<double,double> == 純C double の    */
/*   ビット一致) で機械検証される。beta=0 (mei/pyopenjtalk 既定) 前提のため */
/*   postfilter_mcp / LSP 経路 / b2en・freqt・c2ir は移植対象外 (dead path)。*/
/*                                                                   */
/* 型方針:                                                            */
/*   Real = 信号・フィルタ状態・係数の格納型 (double=基準 / float=MCU)     */
/*   Acc  = 累算・乱数内部・ピッチ累算の型 (段B stF で Acc=double に昇格可)  */
/*   <double,double> なら純C と完全同一演算 → 段A0 max|Δ|==0.0。          */
/* ----------------------------------------------------------------- */
#ifndef HTS_VOCODER_T_H
#define HTS_VOCODER_T_H

#include <cmath>
#include <cstddef>
#include <vector>

namespace hts_tmpl {

/* MLSA Pade 係数 (純C HTS_vocoder.c:63 HTS_pade[21] と同一)。
   定数は double 保持、使用時に Acc へキャストする。 */
static const double HTS_PADE_T[21] = {
   1.00000000000, 1.00000000000, 0.00000000000, 1.00000000000,
   0.00000000000, 0.00000000000, 1.00000000000, 0.00000000000,
   0.00000000000, 0.00000000000, 1.00000000000, 0.49992730000,
   0.10670050000, 0.01170221000, 0.00056562790, 1.00000000000,
   0.49993910000, 0.11070980000, 0.01369984000, 0.00095648530,
   0.00003041721
};

/* 乱数定数 (HTS_hidden.h と同一) */
#define HTS_T_RANDMAX 32767
#define HTS_T_SEED    1
#define HTS_T_B0      0x00000001
#define HTS_T_B28     0x10000000
#define HTS_T_B31     0x80000000
#define HTS_T_B31_    0x7fffffff

/* lf0->pitch 変換境界 (HTS_hidden.h) */
#ifndef HTS_T_LZERO
#define HTS_T_LZERO   (-1.0e+10)
#endif
#define HTS_T_MAX_F0  20000.0
#define HTS_T_MIN_F0  20.0
#define HTS_T_MAX_LF0 9.9034875525361280454891979401956    /* log(20000.0) */
#define HTS_T_MIN_LF0 2.9957322735539909934352235761425    /* log(20.0) */

/* HTS_movem: メモリ移動 (純C HTS_vocoder.c:88)。重なり方向で前後を切替。 */
template <typename Real>
static inline void movem_T(const Real *a, Real *b, const int nitem)
{
   long i = (long) nitem;
   if (a > b)
      while (i--)
         *b++ = *a++;
   else {
      a += i;
      b += i;
      while (i--)
         *--b = *--a;
   }
}

/* HTS_mlsafir: MLSA フィルタ副関数 (純C HTS_vocoder.c:104)。状態 d[]=Real、累算=Acc。
   元の3ループ (全域通過更新 → 内積 → シフト、各 m 反復で d[] を 3 周) を1ループに融合し、
   d[] のロード/ストアを約半減する (毎サンプル×pd 回呼ばれる vocoder 最内ループ)。
   更新済み d[i-1] を u_prev にレジスタ持ち回りし、シフト (新d[i]=更新済みd[i-1]) は
   更新値を読み終えた位置へその場で書く。全域通過更新は Real、内積は従来と同一順序 (i=2..m)
   の Acc 累算 → 数値結果ビット不変 (段A0=0.0 保持、音質透明)。融合手法は
   kunsen-an/openjtalk-m5stack-onchip-tts (MIT) の HTS_vocoder.c を参考に当 template
   <Real,Acc> 向けへ移植 (DLS-171)。 */
template <typename Real, typename Acc>
static inline Acc mlsafir_T(const Real x, const Real *b, const int m, const Real a, const Real aa, Real *d)
{
   Acc y = (Acc) 0.0;
   Real u_prev, old_curr, old_next, u;
   int i;

   d[0] = x;
   u_prev = aa * d[0] + a * d[1];                /* = 更新済み d[1] */
   old_curr = d[2];
   d[1] = u_prev;
   for (i = 2; i <= m; i++) {
      old_next = d[i + 1];
      u = old_curr + a * (old_next - u_prev);    /* = 更新済み d[i] = d[i] + a*(d[i+1]-d[i-1]) */
      y += (Acc) u * (Acc) b[i];
      d[i] = u_prev;                             /* シフト: 新d[i] = 更新済みd[i-1] */
      u_prev = u;
      old_curr = old_next;
   }
   d[m + 1] = u_prev;                            /* シフト末尾: 新d[m+1] = 更新済みd[m] */

   return (y);
}

/* HTS_mlsadf1: MLSA Pade 分子段 (純C HTS_vocoder.c:125)。x/out/v=Acc、状態 d[]=Real。 */
template <typename Real, typename Acc>
static inline Acc mlsadf1_T(Acc x, const Real *b, const int m, const Real a, const Real aa, const int pd, Real *d, const double *ppade)
{
   Acc v, out = (Acc) 0.0;
   Real *pt;
   int i;

   (void) m;
   pt = &d[pd + 1];

   for (i = pd; i >= 1; i--) {
      d[i] = aa * pt[i - 1] + a * d[i];
      pt[i] = d[i] * b[1];
      v = (Acc) pt[i] * (Acc) ppade[i];
      x += (1 & i) ? v : -v;
      out += v;
   }

   pt[0] = (Real) x;
   out += x;

   return (out);
}

/* HTS_mlsadf2: MLSA Pade 分母段 (純C HTS_vocoder.c:147)。mlsafir を pd 回。 */
template <typename Real, typename Acc>
static inline Acc mlsadf2_T(Acc x, const Real *b, const int m, const Real a, const Real aa, const int pd, Real *d, const double *ppade)
{
   Acc v, out = (Acc) 0.0;
   Real *pt;
   int i;

   pt = &d[pd * (m + 2)];

   for (i = pd; i >= 1; i--) {
      pt[i] = (Real) mlsafir_T<Real, Acc>(pt[i - 1], b, m, a, aa, &d[(i - 1) * (m + 2)]);
      v = (Acc) pt[i] * (Acc) ppade[i];

      x += (1 & i) ? v : -v;
      out += v;
   }

   pt[0] = (Real) x;
   out += x;

   return (out);
}

/* HTS_mlsadf: MLSA フィルタ本体 (純C HTS_vocoder.c:169)。pd=PADEORDER。 */
template <typename Real, typename Acc>
static inline Acc mlsadf_T(Acc x, const Real *b, const int m, const Real a, const int pd, Real *d)
{
   const Real aa = (Real) 1.0 - a * a;
   const double *ppade = &(HTS_PADE_T[pd * (pd + 1) / 2]);

   x = mlsadf1_T<Real, Acc>(x, b, m, a, aa, pd, d, ppade);
   x = mlsadf2_T<Real, Acc>(x, b, m, a, aa, pd, &d[2 * (pd + 1)], ppade);

   return (x);
}

/* HTS_mc2b: メルケプ -> MLSA フィルタ係数 (純C HTS_vocoder.c:232)。 */
template <typename Real>
static inline void mc2b_T(const Real *mc, Real *b, int m, const Real a)
{
   if (mc != b) {
      if (a != (Real) 0.0) {
         b[m] = mc[m];
         for (m--; m >= 0; m--)
            b[m] = mc[m] - a * b[m + 1];
      } else
         movem_T<Real>(mc, b, m + 1);
   } else if (a != (Real) 0.0)
      for (m--; m >= 0; m--)
         b[m] -= a * b[m + 1];
}

}  /* namespace hts_tmpl */

#endif  /* HTS_VOCODER_T_H */

/* ----------------------------------------------------------------- */
/* DLS-166 ST1-2: 48kHz -> 16kHz 3:1 decimate (anti-alias FIR + 間引き)。  */
/*                                                                   */
/* 出力 m サンプル目 = Σ_k h[k]*x[3m-k] (因果FIR、x[<0]=0)。これは        */
/* scipy `lfilter(h,1,x48k)[::3]` と数学的に同一 (kept サンプルのみ計算)。  */
/* 係数 h は hts_coeffs.h (firwin、hts_gate.py --emit-decimate 自動生成)。  */
/* 型: 係数は double 保持、累算=Acc、出力=Real。<double,double> で         */
/*     Python golden (pcm16k_ref.bin) と max|Δ|<1e-9 一致 (段A6)。         */
/* ----------------------------------------------------------------- */
#ifndef HTS_DECIMATE_T_H
#define HTS_DECIMATE_T_H

#include <cstddef>
#include <cstring>
#include <vector>

namespace hts_tmpl {

/* 生ポインタ版 3:1 decimate (fw 用、PSRAM 生バッファを std::vector へコピーせず処理)。
   x[0..n_in-1] → y_out[0..n_out-1] (y_out は呼び出し側で n_out 確保)。
   n_out = ceil(n_in/factor) を返す。vector 版と完全に同一演算。 */
template <typename Real, typename Acc>
inline std::size_t decimate_T(const Real *x, std::size_t n_in, const double *h, int ntaps,
                              Real *y_out, int factor = 3)
{
   std::size_t n_out = (n_in + (std::size_t) factor - 1) / (std::size_t) factor;  /* ceil = x[::factor] */
   for (std::size_t m = 0; m < n_out; m++) {
      std::size_t n = (std::size_t) factor * m;   /* 入力中心 index */
      Acc acc = (Acc) 0.0;
      for (int k = 0; k < ntaps; k++) {
         if (n >= (std::size_t) k)
            acc += (Acc) h[k] * (Acc) x[n - (std::size_t) k];
      }
      y_out[m] = (Real) acc;
   }
   return n_out;
}

/* 3:1 decimate。h=FIR係数(double, ntaps)、factor=3。出力長=ceil(len/factor)。
   生ポインタ版に委譲 (DRY)。 */
template <typename Real, typename Acc>
inline std::vector<Real> decimate_T(const std::vector<Real> &x, const double *h, int ntaps, int factor = 3)
{
   std::size_t n_in = x.size();
   std::size_t n_out = (n_in + (std::size_t) factor - 1) / (std::size_t) factor;
   std::vector<Real> y(n_out, (Real) 0.0);
   if (n_in > 0)
      decimate_T<Real, Acc>(x.data(), n_in, h, ntaps, y.data(), factor);
   return y;
}

/* DLS-201 (パイプライン化): streaming 版 3:1 decimate。
   フレーム逐次に push し、全バッファ一括の decimate_T とビット一致する出力を返す。
   合成しながら再生するパイプライン化で、vocoder 1 フレーム (fperiod samp) 出力ごとに
   間引く用途。状態は FIR が境界をまたいで参照する直前 (ntaps-1) サンプルの履歴のみ。

   DLS-204 (overhead 再構成): 演算 (h[k]*x[c-k] の k 昇順 Acc 累算) は decimate_T と
   完全同一に保ったまま、overhead-bound だった内部実装を再構成した (実機 277ns/MAC)。
   ① 履歴と今回 x を「連続バッファ buf = [先頭 ntaps-1: 履歴 | 今回 x]」に統合し、内側
      ループの per-tap 分岐 (x か hist かのバッファ選択 + x[<0]=0 の continue) を撤去
      → buf[lc-k] の分岐ゼロ連続 MAC に。先頭 ntaps-1 を実ゼロ初期化するので x[<0]=0 は
      「h[k]*0.0 の加算」で表現され、IEEE754 で acc 不変 = decimate_T とビット一致。
   ② 毎フレームの std::vector 再確保 (reserve+push_back+swap) を、固定容量 buf への
      memmove 前詰め (末尾 ntaps-1 を先頭へ) に置換し heap churn を撤去。
   ③ 係数 h (double 配列) を初回に Acc 精度へ事前変換 (h_f[k]=(Acc)h[k]) し、内側ループの
      毎 tap double load + double→float 変換を撤去 (§18-voc で残 overhead の主因と判明)。
      h_f[k]*buf[lc-k] = (Acc)h[k]*(Acc)buf[lc-k] ゆえ演算はビット不変。
   ビット不変は host self_test 段A6s/A6sf (double/float とも max|Δ|=0.0) で機械担保。
   (fperiod=240 が factor=3 の倍数だが、本実装は global index 基準で位相を追うため
    任意のチャンク長でも decimate_T と一致する。) */
template <typename Real, typename Acc>
struct DecimateStreamT {
   std::vector<Real> buf;      /* [先頭 ntaps-1: 直前履歴(初回は実ゼロ) | 今回投入 x] 連続領域 */
   std::vector<Acc> h_f;       /* 係数 h[k] を Acc 精度に事前変換 (毎 tap の double load を撤去) */
   std::size_t global_n = 0;   /* 累積投入サンプル数 */
   std::size_t produced = 0;   /* 産出済み出力数 (出力中心 = factor*produced) */
   bool inited = false;        /* buf 先頭履歴域の初回ゼロ初期化フラグ */

   /* x[0..n-1] を 1 チャンク投入。今回産出した出力を y_out に書き、その個数を返す。
      y_out は呼び出し側で >= ceil(n/factor)+1 確保すること。 */
   std::size_t push(const Real *x, std::size_t n, const double *h, int ntaps,
                    Real *y_out, int factor = 3)
   {
      const std::size_t P = (std::size_t)(ntaps - 1);   /* 履歴/パディング幅 */
      if (!inited) {
         buf.assign(P, (Real) 0.0);   /* 履歴域を実ゼロで初期化 (x_global[<0]=0 を再現) */
         h_f.resize((std::size_t) ntaps);
         for (int k = 0; k < ntaps; k++) h_f[k] = (Acc) h[k];   /* (Acc)h[k] と同値=ビット不変 */
         inited = true;
      }
      buf.resize(P + n);              /* 容量内なら再 alloc なし (heap churn 撤去) */
      for (std::size_t i = 0; i < n; i++)
         buf[P + i] = x[i];           /* 今回 x を履歴の後ろへ連結 */

      const std::size_t end = global_n + n;   /* 投入後の絶対終端 (排他) */
      std::size_t out_cnt = 0;
      for (;; produced++) {
         const std::size_t c = (std::size_t) factor * produced;   /* 出力中心 (絶対 index) */
         if (c >= end) break;
         /* buf ローカル中心 lc。c は前 push 終端 (= 今回 global_n) 以上ゆえ c>=global_n、
            k<=ntaps-1=P かつ lc>=P より lc-k は常に [0, P+n) に収まる (負 index/分岐不要)。 */
         const std::size_t lc = P + (c - global_n);
         Acc acc = (Acc) 0.0;
         for (int k = 0; k < ntaps; k++)   /* 分岐ゼロの連続 MAC。k 昇順・加算項は decimate_T と同一 */
            acc += h_f[k] * (Acc) buf[lc - (std::size_t) k];   /* h_f[k]=(Acc)h[k] ゆえ演算不変 */
         y_out[out_cnt++] = (Real) acc;
      }

      /* 履歴更新: 連結 (履歴 + 今回 x、計 P+n) の末尾 P サンプルを buf 先頭へ前詰め。
         先頭 P を常に保持するので base=global_n-P 固定 = 上の lc 不変条件が成立する。 */
      std::memmove(buf.data(), buf.data() + n, P * sizeof(Real));
      global_n += n;
      return out_cnt;
   }
};

}  /* namespace hts_tmpl */

#endif  /* HTS_DECIMATE_T_H */

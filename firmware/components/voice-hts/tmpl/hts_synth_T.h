/* ----------------------------------------------------------------- */
/* DLS-166 ST1: HTS ボコーダ状態機械 (励振 + MLSA 合成) の template 移植    */
/*                                                                   */
/* 正本 = vendored 純C `lib/HTS_vocoder.c` の stage=0(MCP) 経路:          */
/*   HTS_Vocoder_initialize(852-862) / HTS_Vocoder_synthesize(866-950) /  */
/*   励振系(644-744) / 乱数(181-229,635-641)。                          */
/* 励振は Vocoder 状態と密結合のため (plan の hts_excite_T.h を) 本ファイル  */
/* に統合した。LSP 経路・postfilter(beta>0)・b2en は dead path で非移植。   */
/*                                                                   */
/* 乱数は整数 LFSR/LCG (next:unsigned long / x:int) を型不変で保持する      */
/* (Real 化すると励振列が変わり段A0/段A4 が崩れる)。                       */
/* PADEORDER/GAUSS は pyopenjtalk(非 HTS_EMBEDDED) 既定 = 5/true を既定値。  */
/* fw が HTS_EMBEDDED(=4/false) でビルドする場合は parity 再評価が必要 (ST2)。*/
/* ----------------------------------------------------------------- */
#ifndef HTS_SYNTH_T_H
#define HTS_SYNTH_T_H

#include <cmath>
#include <cstddef>
#include <vector>

#include "hts_vocoder_T.h"

namespace hts_tmpl {

/* HTS ボコーダ (stage=0 / MCP)。Real=信号・状態、Acc=累算・乱数内部。 */
template <typename Real, typename Acc>
struct VocoderT {
   /* 設定 */
   bool is_first;
   int pade_order;              /* PADEORDER (pyopenjtalk=5) */
   bool gauss;                  /* GAUSS (pyopenjtalk=true → nrandom) */
   std::size_t fprd;            /* frame shift (fperiod) */
   double rate;                 /* sampling rate */

   /* 乱数状態 (型不変) */
   unsigned long next;          /* LCG 状態 (HTS_rnd) */
   int x;                       /* M 系列 LFSR 状態 (HTS_mseq) */
   unsigned char sw;            /* nrandom の状態切替 */
   Acc r1, r2, s;              /* nrandom (Box-Muller) */

   /* 励振状態 */
   Acc pitch_of_curr_point;
   Acc pitch_counter;
   Acc pitch_inc_per_point;
   std::vector<Real> excite_ring_buff;
   std::size_t excite_buff_size;
   std::size_t excite_buff_index;

   /* MLSA 係数・状態 (純C の c/cc/cinc/d1 を 1 本の連続領域に配置) */
   std::vector<Real> cblock;
   Real *c, *cc, *cinc, *d1;

   /* HTS_rnd: 一様乱数 (純C HTS_vocoder.c:181)。next は unsigned long 不変。 */
   Acc rnd()
   {
      double r;
      next = next * 1103515245UL + 12345;
      r = (double) ((next / 65536UL) % 32768UL);
      return (Acc) (r / HTS_T_RANDMAX);
   }

   /* HTS_nrandom: ガウス乱数 (純C HTS_vocoder.c:192) */
   Acc nrandom()
   {
      if (sw == 0) {
         sw = 1;
         do {
            r1 = (Acc) 2.0 * rnd() - (Acc) 1.0;
            r2 = (Acc) 2.0 * rnd() - (Acc) 1.0;
            s = r1 * r1 + r2 * r2;
         } while (s > (Acc) 1.0 || s == (Acc) 0.0);
         /* 段2: (double) キャストを除き Acc 精度で評価 (Acc=float→logf/sqrtf 単精度FPU、
            Acc=double 経路は同一演算で段A0 ビット一致を維持)。-2.0 は Acc 化して float 昇格回避。 */
         s = std::sqrt(((Acc) -2.0 * std::log(s)) / s);
         return (r1 * s);
      } else {
         sw = 0;
         return (r2 * s);
      }
   }

   /* HTS_mseq: M 系列乱数 (純C HTS_vocoder.c:210)。x は int 不変。 */
   int mseq()
   {
      int x0, x28;
      x >>= 1;
      if (x & HTS_T_B0)
         x0 = 1;
      else
         x0 = -1;
      if (x & HTS_T_B28)
         x28 = 1;
      else
         x28 = -1;
      if (x0 + x28)
         x &= HTS_T_B31_;
      else
         x |= HTS_T_B31;
      return (x0);
   }

   /* HTS_white_noise (純C HTS_vocoder.c:635) */
   Acc white_noise()
   {
      if (gauss)
         return (Acc) nrandom();
      else
         return (Acc) mseq();
   }

   /* HTS_Vocoder_initialize_excitation (純C HTS_vocoder.c:644) */
   void initialize_excitation(Acc pitch, std::size_t nlpf)
   {
      pitch_of_curr_point = pitch;
      pitch_counter = pitch;
      pitch_inc_per_point = (Acc) 0.0;
      if (nlpf > 0) {
         excite_buff_size = nlpf;
         excite_ring_buff.assign(excite_buff_size, (Real) 0.0);
         excite_buff_index = 0;
      } else {
         excite_buff_size = 0;
         excite_ring_buff.clear();
         excite_buff_index = 0;
      }
   }

   /* HTS_Vocoder_start_excitation (純C HTS_vocoder.c:665) */
   void start_excitation(Acc pitch)
   {
      if (pitch_of_curr_point != (Acc) 0.0 && pitch != (Acc) 0.0) {
         pitch_inc_per_point = (pitch - pitch_of_curr_point) / (Acc) fprd;
      } else {
         pitch_inc_per_point = (Acc) 0.0;
         pitch_of_curr_point = pitch;
         pitch_counter = pitch;
      }
   }

   /* HTS_Vocoder_excite_unvoiced_frame (純C HTS_vocoder.c:677) */
   void excite_unvoiced_frame(Acc noise)
   {
      std::size_t center = (excite_buff_size - 1) / 2;
      excite_ring_buff[(excite_buff_index + center) % excite_buff_size] += (Real) noise;
   }

   /* HTS_Vocoder_excite_voiced_frame (純C HTS_vocoder.c:684) */
   void excite_voiced_frame(Acc noise, Acc pulse, const Real *lpf)
   {
      std::size_t i;
      std::size_t center = (excite_buff_size - 1) / 2;

      if (noise != (Acc) 0.0) {
         for (i = 0; i < excite_buff_size; i++) {
            if (i == center)
               excite_ring_buff[(excite_buff_index + i) % excite_buff_size] += (Real) (noise * ((Acc) 1.0 - (Acc) lpf[i]));
            else
               excite_ring_buff[(excite_buff_index + i) % excite_buff_size] += (Real) (noise * ((Acc) 0.0 - (Acc) lpf[i]));
         }
      }
      if (pulse != (Acc) 0.0) {
         for (i = 0; i < excite_buff_size; i++)
            excite_ring_buff[(excite_buff_index + i) % excite_buff_size] += (Real) (pulse * (Acc) lpf[i]);
      }
   }

   /* HTS_Vocoder_get_excitation (純C HTS_vocoder.c:704) */
   Acc get_excitation(const Real *lpf)
   {
      Acc x_out;
      Acc noise, pulse = (Acc) 0.0;

      if (excite_buff_size > 0) {
         noise = white_noise();
         pulse = (Acc) 0.0;
         if (pitch_of_curr_point == (Acc) 0.0) {
            excite_unvoiced_frame(noise);
         } else {
            pitch_counter += (Acc) 1.0;
            if (pitch_counter >= pitch_of_curr_point) {
               pulse = std::sqrt(pitch_of_curr_point);   /* 段2: Acc 精度 sqrt */
               pitch_counter -= pitch_of_curr_point;
            }
            excite_voiced_frame(noise, pulse, lpf);
            pitch_of_curr_point += pitch_inc_per_point;
         }
         x_out = (Acc) excite_ring_buff[excite_buff_index];
         excite_ring_buff[excite_buff_index] = (Real) 0.0;
         excite_buff_index++;
         if (excite_buff_index >= excite_buff_size)
            excite_buff_index = 0;
      } else {
         if (pitch_of_curr_point == (Acc) 0.0) {
            x_out = white_noise();
         } else {
            pitch_counter += (Acc) 1.0;
            if (pitch_counter >= pitch_of_curr_point) {
               x_out = std::sqrt(pitch_of_curr_point);   /* 段2: Acc 精度 sqrt */
               pitch_counter -= pitch_of_curr_point;
            } else {
               x_out = (Acc) 0.0;
            }
            pitch_of_curr_point += pitch_inc_per_point;
         }
      }
      return x_out;
   }

   /* HTS_Vocoder_end_excitation (純C HTS_vocoder.c:747) */
   void end_excitation(Acc pitch)
   {
      pitch_of_curr_point = pitch;
   }

   /* HTS_Vocoder_initialize (純C HTS_vocoder.c:819) の MCP(stage=0) 部のみ。
      m = static mgc 次元 - 1。pade_order=5 / gauss=true は pyopenjtalk 既定。 */
   void initialize(std::size_t m, std::size_t rate_, std::size_t fperiod, int pade_order_, bool gauss_)
   {
      is_first = true;
      pade_order = pade_order_;
      gauss = gauss_;
      fprd = fperiod;
      next = HTS_T_SEED;
      rate = (double) rate_;
      pitch_of_curr_point = (Acc) 0.0;
      pitch_counter = (Acc) 0.0;
      pitch_inc_per_point = (Acc) 0.0;
      excite_ring_buff.clear();
      excite_buff_size = 0;
      excite_buff_index = 0;
      sw = 0;
      x = 0x55555555;
      r1 = r2 = s = (Acc) 0.0;
      /* stage=0(MCP): c の総量は純C と同一 m*(3+PADEORDER)+5*PADEORDER+6 */
      std::size_t total = m * (3 + (std::size_t) pade_order) + 5 * (std::size_t) pade_order + 6;
      cblock.assign(total, (Real) 0.0);
      c = cblock.data();
      cc = c + m + 1;
      cinc = cc + m + 1;
      d1 = cinc + m + 1;
   }

   /* HTS_Vocoder_synthesize (純C HTS_vocoder.c:866) の MCP/beta=0 経路。
      spectrum/lpf は per-frame の Real コピー、out へ 1 フレーム分(fprd)の PCM を追記。
      beta は受けるが beta>0(postfilter) は dead path のため assert で弾く。 */
   void synthesize_frame(std::size_t m, Acc lf0, Real *spectrum, const Real *lpf, std::size_t nlpf,
                         Real alpha, Real volume, std::vector<Real> &out)
   {
      Acc xx;
      int i, j;
      Acc p;

      /* lf0 -> pitch (純C 874-882) */
      if (lf0 == (Acc) HTS_T_LZERO)
         p = (Acc) 0.0;
      else if (lf0 <= (Acc) HTS_T_MIN_LF0)
         p = (Acc) (rate / HTS_T_MIN_F0);
      else if (lf0 >= (Acc) HTS_T_MAX_LF0)
         p = (Acc) (rate / HTS_T_MAX_F0);
      else
         /* 段2: rate も Acc 化して float/float 除算 (Acc=double 経路は同一)。 */
         p = (Acc) rate / std::exp(lf0);

      /* first frame (純C 884-898) */
      if (is_first) {
         initialize_excitation(p, nlpf);
         mc2b_T<Real>(spectrum, c, (int) m, alpha);   /* stage 0 */
         is_first = false;
      }

      start_excitation(p);
      /* postfilter_mcp は beta=0 で no-op (dead path)。beta>0 は未移植。 */
      mc2b_T<Real>(spectrum, cc, (int) m, alpha);
      for (i = 0; i <= (int) m; i++)
         cinc[i] = (cc[i] - c[i]) / (Real) fprd;

      for (j = 0; j < (int) fprd; j++) {
         /* DLS-206 CP1 計測専用フラグ。通常ビルドは両方未定義=挙動・出力ビット完全不変。
            定義時は出力が不正になる「計測専用ビルド」で、voc-synth の差分から区間シェアを採る。
            HTS_MEAS_SKIP_MLSADF   : MLSA 再帰核(天井) を素通し → voc-synth = 非再帰部(励振+LPF+gain)。
            HTS_MEAS_SKIP_EXCITATION: get_excitation(励振+LPF) を固定値化 → voc-synth から励振+LPF を除外。 */
#ifdef HTS_MEAS_SKIP_EXCITATION
         xx = (Acc) 1.0;               /* 励振スキップ (計測専用)。固定非ゼロでゲイン exp を温存 */
#else
         xx = get_excitation(lpf);
#endif
         if (xx != (Acc) 0.0)
            xx *= std::exp((Acc) c[0]);   /* 段2: 有声サンプル毎のゲイン exp を Acc 精度で */
#ifndef HTS_MEAS_SKIP_MLSADF
         xx = mlsadf_T<Real, Acc>(xx, c, (int) m, alpha, pade_order, d1);
#endif
         xx *= (Acc) volume;

         out.push_back((Real) xx);

         for (i = 0; i <= (int) m; i++)
            c[i] += cinc[i];
      }

      end_excitation(p);
      movem_T<Real>(cc, c, (int) m + 1);
   }
};

}  /* namespace hts_tmpl */

#endif  /* HTS_SYNTH_T_H */

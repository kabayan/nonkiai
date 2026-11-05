/* ----------------------------------------------------------------- */
/* DLS-166 ST2: pss (MLPG 出力) からフレーム毎パラメータを抽出する共有ヘッダ */
/*                                                                   */
/* fw (voice_hts.cpp) と host (self_test.cpp) が同一コードを使う。       */
/*                                                                   */
/* 公開 API `HTS_Engine_get_generated_parameter` は gss (= 第3段          */
/* `HTS_Engine_generate_sample_sequence` = 純C double whole-buffer       */
/* vocoder) に依存する。fw は float VocoderT で再合成するため、第3段の遅い  */
/* double vocoder を走らせたくない (ESP32 で double は soft-emulation =     */
/* RTF 悪化 + Flash の純C vocoder を gc-section で落とせる)。              */
/*                                                                   */
/* そこで第2段 (`HTS_Engine_generate_parameter_sequence`) 完了後の pss から */
/* 直接読む。値は HTS_GStreamSet_create のパラメータコピー部 (HTS_gstream.c  */
/* 99-115) と**完全に同一ロジック** (MSD 展開: 非ボイス frame は HTS_NODATA、 */
/* msd_frame カウンタを voiced frame でのみ進める) で抽出するため、         */
/* get_generated_parameter と bit 一致する。host self_test がこの一致を       */
/* 機械検証する (段A0 と同水準の担保)。                                  */
/* ----------------------------------------------------------------- */
#ifndef HTS_PARAM_EXTRACT_H
#define HTS_PARAM_EXTRACT_H

#include <cstddef>
#include <vector>

#include "HTS_engine.h"
#include "HTS_hidden.h"   /* HTS_PStreamSet_* (内部アクセサ): fw は -I.../voice-hts/lib で可視 */

namespace hts_extract {

/* 1 ストリーム・1 フレーム分のパラメータを out[] に書く。
   HTS_gstream.c のコピー部と同一: MSD ストリームは msd_frame を使い、非 voiced
   フレームは HTS_NODATA を書く。voiced (= data present) なら true を返し
   *msd_frame を進める。非 MSD ストリームは frame_index を直接使う。 */
inline bool copy_pss_frame(HTS_PStreamSet *pss, std::size_t stream_index,
                           std::size_t frame_index, std::size_t *msd_frame,
                           double *out, std::size_t vlen)
{
   std::size_t k;
   if (HTS_PStreamSet_is_msd(pss, stream_index)) {       /* MSD (lf0) */
      if (HTS_PStreamSet_get_msd_flag(pss, stream_index, frame_index)) {
         for (k = 0; k < vlen; k++)
            out[k] = HTS_PStreamSet_get_parameter(pss, stream_index, *msd_frame, k);
         (*msd_frame)++;
         return true;
      } else {
         for (k = 0; k < vlen; k++)
            out[k] = HTS_NODATA;
         return false;
      }
   } else {                                              /* 非 MSD (mgc / lpf) */
      for (k = 0; k < vlen; k++)
         out[k] = HTS_PStreamSet_get_parameter(pss, stream_index, frame_index, k);
      return true;
   }
}

/* pss を frame 0..total_frame-1 で走査し、各 frame の mgc/lf0/lpf を fn に渡す。
   fn(frame_index, mgc[], mgc_len, lf0, lpf[], nlpf) — mgc_len = m+1, nlpf は lpf 長(0可)。
   大配列を作らず frame 毎にストリームするため fw でも内部 RAM を消費しない。
   呼び出し前に HTS_Engine_generate_parameter_sequence が成功している必要がある。
   戻り値 = total_frame。 */
template <typename FrameFn>
inline std::size_t extract_from_pss(HTS_Engine *engine, FrameFn fn)
{
   HTS_PStreamSet *pss = &engine->pss;
   std::size_t total = HTS_PStreamSet_get_total_frame(pss);
   std::size_t nstream = HTS_PStreamSet_get_nstream(pss);
   std::size_t mgc_len = HTS_PStreamSet_get_vector_length(pss, 0);          /* = m + 1 */
   std::size_t nlpf = (nstream >= 3) ? HTS_PStreamSet_get_vector_length(pss, 2) : 0;

   std::vector<double> mgc(mgc_len, 0.0);
   std::vector<double> lpf(nlpf > 0 ? nlpf : 1, 0.0);
   std::size_t msd_mgc = 0, msd_lf0 = 0, msd_lpf = 0;
   std::size_t i;

   for (i = 0; i < total; i++) {
      double lf0_vec[1];
      copy_pss_frame(pss, 0, i, &msd_mgc, mgc.data(), mgc_len);
      copy_pss_frame(pss, 1, i, &msd_lf0, lf0_vec, 1);
      if (nlpf > 0)
         copy_pss_frame(pss, 2, i, &msd_lpf, lpf.data(), nlpf);
      fn(i, mgc.data(), mgc_len, lf0_vec[0], lpf.data(), nlpf);
   }
   return total;
}

/* DLS-251 F-dup CP3: stream0(mgc) の par を pss(double) からでなく外部 float par から供給する
   3-arg overload。fw は stream0 の double par を確保しない (二重持ち解消 -0.27KB/f) ため、
   mlpg0.par(float) を直接読む。s0_par(frame, k) → float。stream0 は非MSD(mgc) 前提で、
   従来 copy_pss_frame(pss,0,...) が返す par[frame][k] を s0_par(frame,k) で置換する
   (msd_frame 進行なし=非MSD ゆえ 2-arg と等価な frame index 直読)。stream1(lf0)/stream2(lpf) は
   従来通り copy_pss_frame で pss double から読む。host 経路 (上の 2-arg) は無改変。 */
template <typename FrameFn, typename S0ParFn>
inline std::size_t extract_from_pss(HTS_Engine *engine, FrameFn fn, S0ParFn s0_par)
{
   HTS_PStreamSet *pss = &engine->pss;
   std::size_t total = HTS_PStreamSet_get_total_frame(pss);
   std::size_t nstream = HTS_PStreamSet_get_nstream(pss);
   std::size_t mgc_len = HTS_PStreamSet_get_vector_length(pss, 0);          /* = m + 1 */
   std::size_t nlpf = (nstream >= 3) ? HTS_PStreamSet_get_vector_length(pss, 2) : 0;

   std::vector<double> mgc(mgc_len, 0.0);
   std::vector<double> lpf(nlpf > 0 ? nlpf : 1, 0.0);
   std::size_t msd_lf0 = 0, msd_lpf = 0;
   std::size_t i, k;

   for (i = 0; i < total; i++) {
      double lf0_vec[1];
      for (k = 0; k < mgc_len; k++)                 /* stream0(mgc, 非MSD): 外部 float par を給送 */
         mgc[k] = (double) s0_par(i, k);
      copy_pss_frame(pss, 1, i, &msd_lf0, lf0_vec, 1);
      if (nlpf > 0)
         copy_pss_frame(pss, 2, i, &msd_lpf, lpf.data(), nlpf);
      fn(i, mgc.data(), mgc_len, lf0_vec[0], lpf.data(), nlpf);
   }
   return total;
}

}  /* namespace hts_extract */

#endif  /* HTS_PARAM_EXTRACT_H */

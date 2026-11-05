/* ----------------------------------------------------------------- */
/* MLPG (パラメータ生成) の template 移植 (host de-risk 専用)。           */
/*                                                                   */
/* HTS_pstream.c の中核 (calc_wuw_and_wum / ldl_factorization /         */
/* forward/backward_substitution + GV 経路) を template<Real> で再実装。  */
/* AtomS3R で double ソフトエミュが遅い MLPG を float ハードFPU 化できるか */
/* を host (x86 g++) で音質劣化込みで定量化する。                        */
/*                                                                   */
/* 設計: 読み取り専用の const HTS_PStream* を受け、入力 (mean/ivar/        */
/*   win_coefficient/widths/各スカラ) を Real へキャストして取り込み、     */
/*   ワーク配列 (wuw/wum/g/par) は Real で内部確保する。これにより        */
/*   <double> 経路は純C と「同一演算順序」になり bit 一致するはず          */
/*   (検証ゲートで担保)。HTS lib C files は一切変更しない (純追加)。       */
/*                                                                   */
/* HTS_finv / 各定数 (INFTY 等) は純C と同値を Real で再定義する。         */
/* ----------------------------------------------------------------- */
#ifndef HTS_MLPG_T_H
#define HTS_MLPG_T_H

#include <cstddef>
#include <cmath>
#include <vector>
#include <cstdlib>
#include <cstdio>
#include <new>
#include <cassert>
#include <atomic>
#include <algorithm>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#endif

#include "HTS_engine.h"   /* HTS_PStream / HTS_Boolean / TRUE 等 */
#include "HTS_hidden.h"   /* DLS-251 F-dup: HTS_SStreamSet_* (sss 直読アクセサ)。
                             fw は -I.../voice-hts/lib、host は include_directories(../lib) で可視
                             (前例: hts_param_extract.h)。 */

namespace hts_tmpl {

template <typename T>
struct MlpgAllocator {
   typedef T value_type;

   MlpgAllocator() noexcept {}
   template <typename U>
   MlpgAllocator(const MlpgAllocator<U>&) noexcept {}

   T* allocate(std::size_t n) {
      if (n > static_cast<std::size_t>(-1) / sizeof(T))
         throw std::bad_alloc();
#if defined(ESP_PLATFORM)
      void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
      void* p = std::malloc(n * sizeof(T));
#endif
      if (!p) throw std::bad_alloc();
      return static_cast<T*>(p);
   }

   void deallocate(T* p, std::size_t) noexcept {
#if defined(ESP_PLATFORM)
      heap_caps_free(p);
#else
      std::free(p);
#endif
   }
};

template <typename T, typename U>
inline bool operator==(const MlpgAllocator<T>&, const MlpgAllocator<U>&) { return true; }

template <typename T, typename U>
inline bool operator!=(const MlpgAllocator<T>&, const MlpgAllocator<U>&) { return false; }

/* 内部 SRAM 用アロケータ (マルチコア並列ワーク用: PSRAM バス競合回避) */
template <typename T>
struct MlpgInternalAllocator {
   typedef T value_type;

   MlpgInternalAllocator() noexcept {}
   template <typename U>
   MlpgInternalAllocator(const MlpgInternalAllocator<U>&) noexcept {}

   T* allocate(std::size_t n) {
      if (n > static_cast<std::size_t>(-1) / sizeof(T))
         throw std::bad_alloc();
#if defined(ESP_PLATFORM)
      void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (!p) {
         // 内部 SRAM 不足時は PSRAM にフォールバック
         p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      }
#else
      void* p = std::malloc(n * sizeof(T));
#endif
      if (!p) throw std::bad_alloc();
      return static_cast<T*>(p);
   }

   void deallocate(T* p, std::size_t) noexcept {
#if defined(ESP_PLATFORM)
      heap_caps_free(p);
#else
      std::free(p);
#endif
   }
};

template <typename T, typename U>
inline bool operator==(const MlpgInternalAllocator<T>&, const MlpgInternalAllocator<U>&) { return true; }

template <typename T, typename U>
inline bool operator!=(const MlpgInternalAllocator<T>&, const MlpgInternalAllocator<U>&) { return false; }

/* 純C HTS_hidden.h の定数を Real で再現 (同値)。 */
template <typename Real>
struct MlpgConst {
   static Real infty()   { return (Real) 1.0e+38; }
   static Real infty2()  { return (Real) 1.0e+19; }
   static Real invinf2() { return (Real) 1.0e-19; }
   static Real stepinit(){ return (Real) 0.1; }
   static Real stepdec() { return (Real) 0.5; }
   static Real stepinc() { return (Real) 1.2; }
   static Real w1()      { return (Real) 1.0; }
   static Real w2()      { return (Real) 1.0; }
};
static const std::size_t MLPG_GV_MAX_ITERATION = 5;

/* HTS_finv 相当 (1/variance, INFTY ガード)。純C HTS_finv と同一分岐・同一定数。
   DLS-251 F-dup: load_from_sss が Real=double で呼び、sss の生 variance から ivar を計算する。
   mlpg_finv<double>(x) は純C HTS_finv(x) とビット一致 (同一定数 1e19/1e38/1e-19・同一 1.0/x)。
   これを (Real) cast することで、純C fill (HTS_pstream.c) が作る double ivar を (Real) cast した
   現行 load 経路と ivar がビット一致する (DLS-195 segSNR 監査を保持)。 */
template <typename Real>
static Real mlpg_finv(const Real x)
{
   if (x >= MlpgConst<Real>::infty2())
      return (Real) 0.0;
   if (x <= -MlpgConst<Real>::infty2())
      return (Real) 0.0;
   if (x <= HTS_NODATA)
      return (Real) 0.0;
   if (x <= MlpgConst<Real>::invinf2() && x >= (Real) 0)
      return (Real) 1.0e+5;
   if (x >= -MlpgConst<Real>::invinf2() && x < (Real) 0)
      return (Real) -1.0e+5;
   Real inv = ((Real) 1.0 / x);
   if (inv > (Real) 1.0e+5) inv = (Real) 1.0e+5;
   else if (inv < (Real) -1.0e+5) inv = (Real) -1.0e+5;
   return inv;
}

/* MLPG 1 stream 分の Real ワークステート。const HTS_PStream から入力を取り込む。 */
template <typename Real>
struct MlpgT {
   typedef std::vector<Real, MlpgAllocator<Real> > RealVec;
   typedef std::vector<RealVec, MlpgAllocator<RealVec> > RealMat;
   typedef std::vector<int, MlpgAllocator<int> > IntVec;
   typedef std::vector<unsigned char, MlpgAllocator<unsigned char> > U8Vec;

   /* --- pst から借りるスカラ --- */
   std::size_t length;
   std::size_t width;
   std::size_t win_size;
   std::size_t vector_length;
   std::size_t gv_length;
   /* --- 入力 (Real 化して保持) --- */
   /* mean/ivar: [t][i*vector_length + m] (i=win index, m=次元) */
   RealMat mean;
   RealMat ivar;
   /* win_coefficient: [i] を [-l_width .. r_width] でアクセスするため、
      原本同様にオフセットポインタを持つ (coeff_store がストレージ、coeff が
      win_coefficient[i][shift] を直接引けるよう base を l_width 分ずらす)。 */
   RealMat coeff_store;  /* [i] 実体 (index 0 = l_width) */
   IntVec win_l_width;
   IntVec win_r_width;
   /* GV (gv_length>0 のときのみ) */
   RealVec gv_mean;
   RealVec gv_vari;
   U8Vec gv_switch;  /* HTS_Boolean を bool 相当で保持 */
   /* --- ワーク (各コア専用に分離可能な構造体: 内部 SRAM を優先使用して PSRAM バス競合を回避) --- */
   typedef std::vector<Real, MlpgInternalAllocator<Real> > WorkVec;

   struct MlpgWorkspace {
      WorkVec wum;
      WorkVec wuw;     /* [t * width + i] フラット化 (5,500回の微小malloc→1回に削減し断片化防止) */
      WorkVec wum_bak;
      WorkVec wuw_bak;
      WorkVec g;
      std::size_t stride;

      void init(std::size_t length, std::size_t width, bool has_gv) {
         stride = width;
         wum.assign(length, (Real) 0.0);
         wuw.assign(length * width, (Real) 0.0);
         if (has_gv) {
            wum_bak.assign(length, (Real) 0.0);
            wuw_bak.assign(length * width, (Real) 0.0);
         }
         g.assign(length, (Real) 0.0);
      }

      inline Real &wuw_at(std::size_t t, std::size_t i) {
         return wuw[t * stride + i];
      }
      inline const Real &wuw_at(std::size_t t, std::size_t i) const {
         return wuw[t * stride + i];
      }
      inline Real &wuw_bak_at(std::size_t t, std::size_t i) {
         return wuw_bak[t * stride + i];
      }
      inline const Real &wuw_bak_at(std::size_t t, std::size_t i) const {
         return wuw_bak[t * stride + i];
      }

      void release() {
         WorkVec().swap(wum);
         WorkVec().swap(wuw);
         WorkVec().swap(wum_bak);
         WorkVec().swap(wuw_bak);
         WorkVec().swap(g);
      }
   };

   MlpgWorkspace ws_main;
   MlpgWorkspace ws_sub;
   RealMat par;     /* [t][vector_length] 出力 (次元毎に直交して書き込まれるため並列安全) */

   /* win_coefficient[i][shift] アクセサ (shift は負も可)。 */
   Real win_coef(std::size_t i, int shift) const {
      return coeff_store[i][(std::size_t) (shift - win_l_width[i])];
   }

   /* pst からスカラ・窓・ワーク・GV を Real 化して構築する (mean/ivar は別途 fill)。
      DLS-251 F-dup: load / load_from_sss が共有する。mean/ivar は row 長で確保だけしておき、
      値の充填は呼び分ける (pst->sm double 複製 = load / sss 直読 = load_from_sss)。
      win_coefficient/win_widths/gv は stream0 でも HTS_PStreamSet_create で確保済 (F-dup 未ガード)
      なので pst から従来通り借りる。 */
   void load_common(const HTS_PStream *pst) {
      length = pst->length;
      width = pst->width;
      win_size = pst->win_size;
      vector_length = pst->vector_length;
      gv_length = pst->gv_length;

      const std::size_t row = vector_length * win_size;
      mean.assign(length, RealVec(row, (Real) 0.0));
      ivar.assign(length, RealVec(row, (Real) 0.0));

      win_l_width.assign(win_size, 0);
      win_r_width.assign(win_size, 0);
      coeff_store.assign(win_size, RealVec());
      for (std::size_t i = 0; i < win_size; i++) {
         win_l_width[i] = pst->win_l_width[i];
         win_r_width[i] = pst->win_r_width[i];
         const int span = pst->win_r_width[i] - pst->win_l_width[i] + 1;  /* >=1 */
         coeff_store[i].assign((std::size_t) span, (Real) 0.0);
         for (int shift = pst->win_l_width[i]; shift <= pst->win_r_width[i]; shift++)
            coeff_store[i][(std::size_t) (shift - pst->win_l_width[i])] =
               (Real) pst->win_coefficient[i][shift];
      }

      par.assign(length, RealVec(vector_length, (Real) 0.0));

      if (gv_length > 0) {
         gv_mean.assign(vector_length, (Real) 0.0);
         gv_vari.assign(vector_length, (Real) 0.0);
         for (std::size_t j = 0; j < vector_length; j++) {
            gv_mean[j] = (Real) pst->gv_mean[j];
            gv_vari[j] = (Real) pst->gv_vari[j];
         }
         gv_switch.assign(length, 0);
         for (std::size_t t = 0; t < length; t++)
            gv_switch[t] = pst->gv_switch[t] ? 1 : 0;
      }
   }

   /* const HTS_PStream から Real ワークを構築する (mean/ivar は pst->sm double から複製)。
      host 検証 / 全 stream double 確保時に使う (F-dup 未ガード経路)。 */
   void load(const HTS_PStream *pst) {
      load_common(pst);
      const std::size_t row = vector_length * win_size;
      for (std::size_t t = 0; t < length; t++)
         for (std::size_t k = 0; k < row; k++) {
            mean[t][k] = (Real) pst->sm.mean[t][k];
            ivar[t][k] = (Real) pst->sm.ivar[t][k];
         }
   }

   /* DLS-251 F-dup CP2: mean/ivar を pst->sm (double) からでなく sss から直接 Real fill する。
      fw で stream0 の double mean/ivar を確保しない経路用 (二重持ち解消 = 合成ピーク -1.64KB/f)。
      非MSD stream 専用 (mgc)。boundary logic は HTS_pstream.c 非MSD fill と同一 (not_bound / k==0 /
      double finv→Real cast)。→ 現行 load が読む double ivar を (Real) cast したものとビット一致。
      pst は scalars/windows/work/gv の供給元として引き続き使う (それらは stream0 でも確保済)。 */
   void load_from_sss(const HTS_PStream *pst, HTS_SStreamSet *sss, std::size_t stream_index) {
      assert(!HTS_SStreamSet_is_msd(sss, stream_index));   /* MSD(lf0) は境界処理が別。mgc 専用 */
      load_common(pst);
      /* HTS_pstream.c 非MSD fill と同一。非MSD は total_frame == length。frame は duration 単位で進む。 */
      std::size_t frame = 0;
      const std::size_t total_state = HTS_SStreamSet_get_total_state(sss);
      for (std::size_t state = 0; state < total_state; state++) {
         const std::size_t dur = HTS_SStreamSet_get_duration(sss, state);
         for (std::size_t j = 0; j < dur; j++) {
            if (frame >= length) {
               std::fprintf(stderr, "[ERROR] frame index %zu out of bounds (length=%zu)\n", frame, length);
               std::exit(1);
            }
            for (std::size_t k = 0; k < win_size; k++) {
               bool not_bound = true;
               for (int shift = win_l_width[k]; shift <= win_r_width[k]; shift++)
                  if ((int) frame + shift < 0 || (int) length <= (int) frame + shift) {
                     not_bound = false;
                     break;
                  }
               for (std::size_t l = 0; l < vector_length; l++) {
                  const std::size_t m = vector_length * k + l;
                  mean[frame][m] = (Real) HTS_SStreamSet_get_mean(sss, stream_index, state, m);
                  if (not_bound || k == 0)
                     ivar[frame][m] =
                        (Real) mlpg_finv<double>(HTS_SStreamSet_get_vari(sss, stream_index, state, m));
                  else
                     ivar[frame][m] = (Real) 0.0;
               }
            }
            frame++;
         }
      }
   }

   /* ===== 中核4関数 (HTS_pstream.c L78-145 を Real で複製) ===== */

   /* calc_wuw_and_wum: W'U^{-1}W と W'U^{-1}M */
   void calc_wuw_and_wum(std::size_t m, MlpgWorkspace &ws) {
      std::size_t t, i, j;
      int shift;
      Real wu;
      for (t = 0; t < length; t++) {
         ws.wum[t] = (Real) 0.0;
         for (i = 0; i < width; i++)
            ws.wuw_at(t, i) = (Real) 0.0;
         for (i = 0; i < win_size; i++)
            for (shift = win_l_width[i]; shift <= win_r_width[i]; shift++)
               if (((int) t + shift >= 0) && ((int) t + shift < (int) length) && (win_coef(i, -shift) != (Real) 0.0)) {
                  wu = win_coef(i, -shift) * ivar[t + shift][i * vector_length + m];
                  ws.wum[t] += wu * mean[t + shift][i * vector_length + m];
                  for (j = 0; (j < width) && (t + j < length); j++)
                     if (((int) j <= win_r_width[i] + shift) && (win_coef(i, (int) j - shift) != (Real) 0.0))
                        ws.wuw_at(t, j) += wu * win_coef(i, (int) j - shift);
               }
      }
   }

   /* ldl_factorization: W'U^{-1}W を L*D*L' に分解 (wuw を破壊) */
   void ldl_factorization(MlpgWorkspace &ws) {
      std::size_t t, i, j;
      for (t = 0; t < length; t++) {
         for (i = 1; (i < width) && (t >= i); i++)
            ws.wuw_at(t, 0) -= ws.wuw_at(t - i, i) * ws.wuw_at(t - i, i) * ws.wuw_at(t - i, 0);
         for (i = 1; i < width; i++) {
            for (j = 1; (i + j < width) && (t >= j); j++)
               ws.wuw_at(t, i) -= ws.wuw_at(t - j, j) * ws.wuw_at(t - j, i + j) * ws.wuw_at(t - j, 0);
            ws.wuw_at(t, i) /= ws.wuw_at(t, 0);
         }
      }
   }

   /* forward_substitution */
   void forward_substitution(MlpgWorkspace &ws) {
      std::size_t t, i;
      for (t = 0; t < length; t++) {
         ws.g[t] = ws.wum[t];
         for (i = 1; (i < width) && (t >= i); i++)
            ws.g[t] -= ws.wuw_at(t - i, i) * ws.g[t - i];
      }
   }

   /* backward_substitution */
   void backward_substitution(std::size_t m, MlpgWorkspace &ws) {
      std::size_t rev, t, i;
      for (rev = 0; rev < length; rev++) {
         t = length - 1 - rev;
         par[t][m] = ws.g[t] / ws.wuw_at(t, 0);
         for (i = 1; (i < width) && (t + i < length); i++)
            par[t][m] -= ws.wuw_at(t, i) * par[t + i][m];
      }
   }

   /* ===== GV 経路 (HTS_pstream.c L147-253 を Real で複製) ===== */

   void calc_gv(std::size_t m, Real *mean_out, Real *vari_out) {
      std::size_t t;
      Real mn = (Real) 0.0;
      for (t = 0; t < length; t++)
         if (gv_switch[t])
            mn += par[t][m];
      mn /= (Real) gv_length;
      Real vr = (Real) 0.0;
      for (t = 0; t < length; t++)
         if (gv_switch[t])
            vr += (par[t][m] - mn) * (par[t][m] - mn);
      vr /= (Real) gv_length;
      *mean_out = mn;
      *vari_out = vr;
   }

   void conv_gv(std::size_t m) {
      std::size_t t;
      Real mean_v, vari;
      calc_gv(m, &mean_v, &vari);
      Real ratio = (Real) std::sqrt((double) (gv_mean[m] / vari));
      for (t = 0; t < length; t++)
         if (gv_switch[t])
            par[t][m] = ratio * (par[t][m] - mean_v) + mean_v;
   }

   Real calc_derivative(std::size_t m, MlpgWorkspace &ws) {
      std::size_t t, i;
      Real mean_v, vari, dv, h, gvobj, hmmobj;
      Real w = (Real) 1.0 / (Real) (win_size * length);
      calc_gv(m, &mean_v, &vari);
      gvobj = (Real) -0.5 * MlpgConst<Real>::w2() * vari * gv_vari[m] * (vari - (Real) 2.0 * gv_mean[m]);
      dv = (Real) -2.0 * gv_vari[m] * (vari - gv_mean[m]) / (Real) length;

      for (t = 0; t < length; t++) {
         ws.g[t] = ws.wuw_at(t, 0) * par[t][m];
         for (i = 1; i < width; i++) {
            if (t + i < length)
               ws.g[t] += ws.wuw_at(t, i) * par[t + i][m];
            if (t + 1 > i)
               ws.g[t] += ws.wuw_at(t - i, i) * par[t - i][m];
         }
      }

      for (t = 0, hmmobj = (Real) 0.0; t < length; t++) {
         hmmobj += MlpgConst<Real>::w1() * w * par[t][m] * (ws.wum[t] - (Real) 0.5 * ws.g[t]);
         h = -MlpgConst<Real>::w1() * w * ws.wuw_at(t, 1 - 1)
             - MlpgConst<Real>::w2() * (Real) 2.0 / (Real) (length * length)
               * ((Real) (length - 1) * gv_vari[m] * (vari - gv_mean[m])
                  + (Real) 2.0 * gv_vari[m] * (par[t][m] - mean_v) * (par[t][m] - mean_v));
         if (gv_switch[t])
            ws.g[t] = (Real) 1.0 / h * (MlpgConst<Real>::w1() * w * (-ws.g[t] + ws.wum[t]) + MlpgConst<Real>::w2() * dv * (par[t][m] - mean_v));
         else
            ws.g[t] = (Real) 1.0 / h * (MlpgConst<Real>::w1() * w * (-ws.g[t] + ws.wum[t]));
      }
      return -(hmmobj + gvobj);
   }

   /* process_dimension: 1次元 m について MLPG 計算を実行 */
   void process_dimension(std::size_t m, MlpgWorkspace &ws) {
      calc_wuw_and_wum(m, ws);
      if (gv_length > 0) {
         for (std::size_t bt = 0; bt < length; bt++) {
            ws.wum_bak[bt] = ws.wum[bt];
            for (std::size_t bi = 0; bi < width; bi++)
               ws.wuw_bak_at(bt, bi) = ws.wuw_at(bt, bi);
         }
      }
      ldl_factorization(ws);
      forward_substitution(ws);
      backward_substitution(m, ws);
      if (gv_length > 0) {
         conv_gv(m);
         if (MLPG_GV_MAX_ITERATION > 0) {
            /* wuw/wum を退避値から復元 */
            for (std::size_t bt = 0; bt < length; bt++) {
               ws.wum[bt] = ws.wum_bak[bt];
               for (std::size_t bi = 0; bi < width; bi++)
                  ws.wuw_at(bt, bi) = ws.wuw_bak_at(bt, bi);
            }
            Real step = MlpgConst<Real>::stepinit();
            Real prev = (Real) 0.0;
            for (std::size_t i = 1; i <= MLPG_GV_MAX_ITERATION; i++) {
               Real obj = calc_derivative(m, ws);
               if (i > 1) {
                  if (obj > prev)
                     step *= MlpgConst<Real>::stepdec();
                  if (obj < prev)
                     step *= MlpgConst<Real>::stepinc();
               }
               for (std::size_t t = 0; t < length; t++)
                  par[t][m] += step * ws.g[t];
               prev = obj;
            }
         }
      }
   }

#if defined(ESP_PLATFORM)
   /* DLS-032/DLS-033: Core 1 ワーカーは mlpg() のたびに作り直さず、初期化時に 1 回だけ生成して
      常駐させる。内部 SRAM がちょうど使い切られる長さの文でも、ワーカーのスタック (4096B) を
      後から確保しようとして失敗することがなくなる (DLS-032 の原因分析)。

      仕事の受け渡しは FreeRTOS タスク通知 (既定インデックス0) を双方向で使う:
      caller -> worker (仕事が来たことを知らせる) / worker -> caller (完了を知らせる)。
      DLS-033: nk_voice タスク (呼び出し元) は他の目的でタスク通知を使っていない
      (main/ 配下を grep 済み。voice_player.cpp の音声メッセージ受け渡しは xQueue)。ワーカー
      タスクも本用途専用の新設タスクで他の通知主体を持たない。よって通知インデックスを分けたり
      セマフォ/キューに置き換える必要はない。worker -> caller の通知は DLS-024 の既存コードが
      すでに同じ機構 (xTaskNotifyGive/ulTaskNotifyTake) を使っており、双方向化はその対称な
      拡張として追加のヘッダ依存なしで実装できる。

      同時に 2 つの mlpg() が常駐ワーカーを取り合わないよう、ミューテックスの try-take
      (timeout=0) で排他する。取れなければ (ワーカー使用中) その mlpg() は 1 コアで処理する。 */
   struct ResidentJob {
      MlpgT *self;
      MlpgWorkspace *ws;
      std::atomic<std::size_t> *next_m;
      std::size_t total_m;
   };

   static inline TaskHandle_t s_resident_worker_handle = nullptr;
   static inline TaskHandle_t s_resident_caller_handle = nullptr;
   static inline SemaphoreHandle_t s_resident_busy_mutex = nullptr;
   static inline ResidentJob s_resident_job{};

   static void mlpg_core1_resident_worker(void *) {
      for (;;) {
         ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   // 次の仕事が来るまで待機
         ResidentJob job = s_resident_job;           // caller が通知前にセット済み
         for (;;) {
            std::size_t m = job.next_m->fetch_add(1, std::memory_order_relaxed);
            if (m >= job.total_m) {
               break;
            }
            job.self->process_dimension(m, *job.ws);
         }
         xTaskNotifyGive(s_resident_caller_handle);  // 完了通知。自分は削除せず次の仕事を待つ
      }
   }

   /* HTS エンジン初期化経路から 1 回だけ呼ぶ (モデルロードより前が望ましい)。内部 SRAM に余裕が
      ある間にワーカーのスタックを確保しておく。冪等 (2 回目以降は何もせず true を返す)。 */
   static bool init_resident_worker() {
      if (s_resident_worker_handle != nullptr) {
         return true;
      }
      if (s_resident_busy_mutex == nullptr) {
         s_resident_busy_mutex = xSemaphoreCreateMutex();
         if (s_resident_busy_mutex == nullptr) {
            ESP_LOGE("MLPG0", "resident worker init: failed to create busy mutex");
            return false;
         }
      }
      size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      BaseType_t ret = xTaskCreatePinnedToCore(
         mlpg_core1_resident_worker, "mlpg_c1r", 4096, nullptr, 5, &s_resident_worker_handle, 1);
      if (ret != pdPASS) {
         s_resident_worker_handle = nullptr;
         ESP_LOGW("MLPG0", "resident Core1 worker create failed (ret=%d); MLPG will run single core "
                  "until restart; heap internal free=%u largest_free_block=%u",
                  (int)ret, (unsigned)free_internal, (unsigned)largest_internal);
         return false;
      }
      ESP_LOGI("MLPG0", "resident Core1 worker created; heap internal free=%u largest_free_block=%u",
               (unsigned)free_internal, (unsigned)largest_internal);
      return true;
   }

   /* 作業領域が内部 SRAM / PSRAM のどちらに確保されたかを代表ポインタで判定する (DLS-033 診断)。 */
   static bool workspace_is_internal(const MlpgWorkspace &ws) {
      const void *p = !ws.wuw.empty() ? static_cast<const void *>(ws.wuw.data())
                     : (!ws.wum.empty() ? static_cast<const void *>(ws.wum.data()) : nullptr);
      return p == nullptr ? true : esp_ptr_internal(p);
   }
#endif

   /* mlpg: 全次元 m について生成 (HTS_PStream_mlpg と同順)。
      Core 0 と Core 1 の常駐ワーカー (DLS-032/DLS-033) で動的ディスパッチ（作業キュー方式）
      により並列実行する (DLS-024)。 */
   void mlpg() {
      if (length == 0)
         return;

      const bool has_gv = (gv_length > 0);

#if defined(ESP_PLATFORM)
      int64_t t_total_start = esp_timer_get_time();
      int64_t t_ws_main_start = t_total_start;
#endif
      ws_main.init(length, width, has_gv);
#if defined(ESP_PLATFORM)
      int64_t t_ws_main_init_us = esp_timer_get_time() - t_ws_main_start;
      bool ws_main_internal = workspace_is_internal(ws_main);

      bool dual_core = false;
      int64_t t_ws_sub_init_us = 0;
      bool ws_sub_internal = false;

      if (vector_length > 1 && s_resident_worker_handle != nullptr && s_resident_busy_mutex != nullptr &&
          xSemaphoreTake(s_resident_busy_mutex, 0) == pdTRUE) {
         int64_t t_ws_sub_start = esp_timer_get_time();
         try {
            ws_sub.init(length, width, has_gv);
         } catch (const std::bad_alloc &) {
            // ワーカーにはまだ通知していない (xTaskNotifyGive 前) ので、ここで返せば
            // busy mutex が取られっぱなしにならない。以後の mlpg() が単一コアに
            // 落ち続ける (ミューテックス飢餓) のを防ぐため、返却してから再送出する。
            xSemaphoreGive(s_resident_busy_mutex);
            throw;
         }
         t_ws_sub_init_us = esp_timer_get_time() - t_ws_sub_start;
         ws_sub_internal = workspace_is_internal(ws_sub);

         std::atomic<std::size_t> next_m(0);
         s_resident_job.self = this;
         s_resident_job.ws = &ws_sub;
         s_resident_job.next_m = &next_m;
         s_resident_job.total_m = vector_length;
         s_resident_caller_handle = xTaskGetCurrentTaskHandle();

         xTaskNotifyGive(s_resident_worker_handle);   // ジョブをセットし終えてから起こす

         // Core 0 (呼び出し元タスク) も同じキューから次元をフェッチして処理
         for (;;) {
            std::size_t m = next_m.fetch_add(1, std::memory_order_relaxed);
            if (m >= vector_length) {
               break;
            }
            process_dimension(m, ws_main);
         }
         // Core 1 ワーカーの完了を待機してから ws_sub を解放する
         // (ワーカーが ws_sub を触り終わる前に解放しない保証はこの一本のコードパスで担保する)
         ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
         ws_sub.release();
         xSemaphoreGive(s_resident_busy_mutex);
         dual_core = true;
      } else if (vector_length > 1) {
         if (s_resident_worker_handle == nullptr) {
            size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            ESP_LOGW("MLPG0", "resident Core1 worker unavailable (not initialized or create failed), "
                     "fallback to single core; heap internal free=%u largest_free_block=%u",
                     (unsigned)free_internal, (unsigned)largest_internal);
         } else {
            ESP_LOGW("MLPG0", "resident Core1 worker busy (concurrent mlpg()), fallback to single core");
         }
         for (std::size_t m = 0; m < vector_length; m++) {
            process_dimension(m, ws_main);
         }
      } else {
         for (std::size_t m = 0; m < vector_length; m++) {
            process_dimension(m, ws_main);
         }
      }

      int64_t t_total_us = esp_timer_get_time() - t_total_start;
      char ws_sub_buf[48];
      if (dual_core) {
         std::snprintf(ws_sub_buf, sizeof(ws_sub_buf), "%lldms(%s)",
                        (long long)(t_ws_sub_init_us / 1000), ws_sub_internal ? "internal" : "psram");
      } else {
         std::snprintf(ws_sub_buf, sizeof(ws_sub_buf), "n/a");
      }
      ESP_LOGI("MLPG_TIMING",
               "frames=%u dims=%u (%s) | total=%lldms ws_main_init=%lldms(%s) ws_sub_init=%s",
               (unsigned)length, (unsigned)vector_length, dual_core ? "dual" : "single",
               (long long)(t_total_us / 1000), (long long)(t_ws_main_init_us / 1000),
               ws_main_internal ? "internal" : "psram", ws_sub_buf);
#else
      for (std::size_t m = 0; m < vector_length; m++) {
         process_dimension(m, ws_main);
      }
#endif
      ws_main.release();
   }

   /* DLS-251 F-dup CP3: mlpg() 完了後、par 以外の Real ワーク (mean/ivar/gv/coeff) を
      即解放する。extract は par だけを読むので、vocoder 区間の footprint を par (float) のみに落とす。
      swap-with-empty で確実に容量を返す (clear() は capacity を残す)。 */
   void release_after_mlpg() {
      RealMat().swap(mean);
      RealMat().swap(ivar);
      ws_main.release();
      ws_sub.release();
      RealMat().swap(coeff_store);
      RealVec().swap(gv_mean);
      RealVec().swap(gv_vari);
      U8Vec().swap(gv_switch);
   }
};

}  /* namespace hts_tmpl */

#endif  /* HTS_MLPG_T_H */

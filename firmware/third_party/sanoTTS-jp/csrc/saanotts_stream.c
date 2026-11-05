/* ストリーミング推論の本体（Phase D-2 / D-029）
 *
 * **方式: ステート保持型パイプライン。**
 *
 * 各段は入力を `[C][2*pad + CHUNK]` のバッファに溜める。左 pad は前チャンクの
 * 実データ、右 pad は次チャンクの実データ。**バッファを入力にして、下流へ渡す
 * 中央 CHUNK フレーム（とその計算に要る範囲）だけを出力する。**
 *
 * ⚠️ **これが bit 一致（D-029 の G2）の根拠**: 中央フレーム t の計算に必要な
 * 入力は `[t-pad, t+pad]` だけで、それは全部バッファの中にある。バッファの外を
 * ゼロとみなす既存カーネルの挙動は、**中央 CHUNK フレームには影響しない**。
 * だから一括版とまったく同じ積和になる。カーネルも一括版と共有する。
 *
 * S9（T2、2026-09-03）: 以前は**バッファ全体**に conv / LN / GELU を掛けて中央だけ取り出して
 * いたが、捨てられる列（AC の 8/16、DEC の 6/14、TOKEN の (6n−84)/6n）の計算が MAC の 37%・
 * GELU の 42% だった（maps / 計画 §1）。今は各カーネルの出力範囲版（`saan_conv1d_wr` /
 * `saan_dwconv1d_wr`。出力は圧縮 [C][t1−t0]）で**要る列だけ**を計算する。
 * 出力要素ごとの積和順序・per-frame の量子化・per-frame の LN は変わらないので bit 同一
 * （stream G2 多文 3 レーン + QEMU の checksum で確認）。
 *
 * 計算量は k=5 / k=7 の conv だけが `(CHUNK + 2*pad) / CHUNK` 倍で、1×1 conv / LN / GELU /
 * 残差は CHUNK ぶんちょうど。ハロー再計算方式（全体で 10 倍）よりはるかに軽い。
 *
 * ⚠️ **代償はレイテンシ。** 出力は受容野ぶん 36 フレーム = 0.42 秒遅れる。
 */
#include "saanotts_stream.h"
#include "saanotts_internal.h"
#include "saan_prof.h"

#include "fft.h"
#if SAAN_G_EXCITE
/* sin の表の置き場所（-DSAAN_EXC_TAB_RAM=1 で内部 DRAM。既定は .rodata） */
#if defined(SAAN_EXC_TAB_RAM) && SAAN_EXC_TAB_RAM && defined(ESP_PLATFORM)
#include "esp_attr.h"
#define SAAN_EXC_TAB DRAM_ATTR
#else
#define SAAN_EXC_TAB
#endif
#include "exc_sin_table.h"
#endif

#include <math.h>
#include <string.h>

/* ⚠️ **移植性。** `M_PI` は C99 の <math.h> に無い（POSIX の拡張）。
 * macOS では既定で見えるが、**Linux + glibc の厳密 `-std=c99` では見えない**。
 * `bench.c` と同じ形で自前に持つ（C-033 / CI が Linux で見つけた）。 */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define AC_W  SAAN_AC_W
#define DEC_W SAAN_DEC_W
#define CD    SAAN_CDIM
#define E     SAAN_DEC_E
#define CH    SAAN_CHUNK
#define NB    SAAN_NBINS

/* --- token block をトークン単位のパイプに（S6 / T3、2026-09-03）------------
 *
 * 旧: 毎 step「チャンクが跨ぐトークン範囲 ± 12」を 3 段まるごと再計算していた
 *     （±12 のハローが窓の 84%。TOKEN は実機 1 step の 11.3%。M-82 / 計画 §1）。
 * 今: フレーム側と**同じ**ステート保持型パイプ（同じ pipe_t / 同じ acblk_step）。3 段それぞれが
 *     [AC_W][2·TOK_PAD + TOK_K] の窓を持ち、TOK_K トークンずつ押し込んで中央 TOK_K だけを
 *     次段へ渡す。各トークンは段ごとに **1 回だけ**計算される。最終段の出力は TOK_G 群の
 *     リング tok_ring [TOK_G][AC_W][TOK_K] に置き、make_hf はそこから引く（tok_pipe_advance）。
 *
 * 受容野: (c1 k=5 + c2 k=5) × 3 段 = ±12 トークン = TOK_PAD 4 × 3 段。段を通るごとに TOK_PAD
 * ずつ過去にずれるので、g 回押した後に出そろっている出力トークンは [−TOK_HALO, g·K − TOK_HALO)。
 *
 * bit 同一の根拠はフレーム側（M-42 の G2）と同じ: 中央トークン t の計算に要る入力は
 * [t − pad, t + pad] だけで、それは全部窓の中にある。発話外（< 0 / ≥ n_ids）は
 * zero_outside_n でゼロにする（一括版では配列外 = ゼロ）。token block の出力はトークン位置に
 * 依存しない（積和順序は T に依らず、LN は per-token、W8A8 の量子化も per-token）。
 *
 * リングの深さ: 1 チャンク CH フレームが跨ぐトークンは最大 CH（各フレームは 1 トークンに
 * 属し d̂ ≥ SAAN_CLIP_LO = 1）。押した直後の出そろい末尾（exclusive）P = g·K − TOK_HALO に対し
 * i1 ∈ [P − K, P) なので i0 ≥ i1 − (CH − 1) ≥ P − K − CH + 1。リングは K + CH − 1 トークンを
 * 持てばよく、K 単位の群で TOK_G = ceil((K + CH − 1) / K) 群（K = CH = 8 で 2 群 = 16 トークン）。
 * ⚠️ 足りなければ make_hf が SAAN_ERR_SHAPE で止まる（黙って古い群を読まない）。
 *
 * ⚠️ 陽性対照（計画 T3）: TOK_PAD を 3 にすると受容野が ±9 に縮み、各段の中央先頭 / 末尾の列が
 *    ゼロパディングを実データの代わりに読むので stream G2（多文）が落ちる
 *    （2026-09-03 に実際に落ちるのを見て戻した。TOK_HALO も 9 になるので簿記は崩れず、
 *    崩れるのは値だけ = 受容野の仮定そのものを検査している）。 */
/* --- MEM-5: decoder の出口を 1 フレームずつにする（`-DSAAN_MEM_HEAD_PF=1`）------
 *
 * 既定（0）は hout（1×1 conv、48 → 1539ch）を CH フレームまとめて計算し、
 * `o1539 [1539][CH]` に置く = **49,248 B で arena の 31.4%**（M-139 §4）。
 * 1 にすると出力範囲版 `saan_conv1d_wr` で列 m だけを計算し、`o1539 [1539][1]`
 * = 6,160 B になる（**−43,088 B**）。
 *
 * ⚠️ **bit 一致の根拠**: 1×1 conv の出力要素 (c, m) は入力列 m だけから決まり、
 *    積和は cin=48 の順で回る。W8A8 の量子化も per-frame。どちらも T に依らない
 *    ので、範囲 [m, m+1) で呼んでも [0, CH) で呼んでも同じ値になる
 *    （`make -C csrc range` が範囲版と [0,T) 版の bit 一致を陽性対照つきで見ている）。
 * ⚠️ **既定を変えていない。** 速度の代償は M-139 §6 に実機で測った値がある（粒度 4 が折衷点）。 */
#ifndef SAAN_MEM_HEAD_PF
/* ⚠️ **2026-09-17 に既定を 4 にした**（M-140）。`o1539` が 49,248 → 24,624 B に減り、
 * 実機（CoreS3 / W8A8+PIE）の定常 xRT は 0.448 → 0.483（要件 ≤ 0.5 の内側）。
 * **PCM は 1 bit も変わらない**（実機 4 文 + ホスト 50 文 + 陽性対照 3 件）。
 * ⚠️ **既定をここに置くのは、ESP-IDF / wasm / Arduino の 3 経路が全部この 1 行を見るため。**
 *    CMake 側に既定を置くと wasm と Arduino だけ別の値になり、[C-099](../docs/decisions.md#c-099)
 *    と同じ「同じフラグが場所で効き方が違う」を作る。
 * `-DSAAN_MEM_HEAD_PF=0` で元に戻る（その場合は `-DSAAN_ARENA_BYTES=180224` も要る。
 * main.c が起動時に `saan_stream_arena_peak` と突き合わせて止める）。 */
#define SAAN_MEM_HEAD_PF 4
#endif
/* 1 回の hout 呼び出しで計算する列数。`SAAN_MEM_HEAD_PF` が 0 なら CH（既定）、
 * 1 なら 1 列ずつ、2 以上ならその粒度（**CH を割り切ること**）。
 * ⚠️ **粒度を下げるほど重みを読み直す回数が増える** — S5b の weight-stationary
 *    カーネルは重み 1 行を T 列に使い回すので、T を半分にすると flash からの
 *    重み転送が 2 倍になる。arena と速度の折衷点は実機で測った（M-139 §6。**粒度 4 が折衷点** — 24,624 B 減って xRT 0.483）。 */
#define HEAD_COLS (SAAN_MEM_HEAD_PF ? SAAN_MEM_HEAD_PF : CH)
typedef char saan_head_group_divides_chunk[(CH % HEAD_COLS == 0) ? 1 : -1];

/* --- hout の出力 `o1539` を arena の外に置く（`-DSAAN_HEAD_EXT=1`。既定 0 = arena の中）------
 * `o1539 [1539][HEAD_COLS]` は `SAAN_MEM_HEAD_PF=8` で 49,248 B あり、arena（内部 RAM）の最大の 1 本。
 * 1 にすると init は arena から取らず、呼び出し側が `saan_stream_set_head_ext` で渡した領域を使う
 * （ESP32 では PSRAM）。値の計算は同じ（置き場所だけが違う）ので PCM は変わらない。
 * `saan_stream_arena_used/peak` からも抜ける（確保の一覧と 1:1 のまま）。
 * ⚠️ 渡していない / 小さいなら init が SAAN_ERR_ARENA を返す。
 * ⚠️ グローバル 1 つ。合成中に差し替えない（`saan_set_oc_par` と同じ扱い）。 */
/* --- decoder の pw1 / GELU / pw2 と hdown / GELU も出力チャネルで 2 つの実行者に割る（`-DSAAN_DEC_OC=1`。既定 0）---
 * 既定は hout だけを割る（DLS-205）。幅を広げた G（W228 / E912）では pw1 / pw2 が step の大半になるので、
 * 同じ `saan_conv1d_wr_oc` に載せる。行ごとの演算（足し込み順を含む）は変わらないので PCM は同じ。 */
#ifndef SAAN_DEC_OC
#define SAAN_DEC_OC 0
#endif
/* decoder の depthwise も割る（量子化はフレーム、積和はチャネル。`saan_dwconv1d_wr_oc`。-DSAAN_DEC_DWOC=1。既定 0） */
#ifndef SAAN_DEC_DWOC
#define SAAN_DEC_DWOC 0
#endif

#ifndef SAAN_HEAD_EXT
#define SAAN_HEAD_EXT 0
#endif
#if SAAN_HEAD_EXT
static float *g_head_ext;
static size_t g_head_ext_n;
#endif
size_t saan_stream_head_ext_bytes(void) { return sizeof(float) * 1539 * HEAD_COLS; }
void saan_stream_set_head_ext(float *buf, size_t bytes) {
#if SAAN_HEAD_EXT
    g_head_ext = buf;
    g_head_ext_n = bytes;
#else
    (void)buf; (void)bytes;
#endif
}

#define TOK_PAD  4
#define TOK_HALO (3 * TOK_PAD)   /* パイプ全体の遅延 = 受容野 ±12 */
#define TOK_K    CH              /* 1 回に進めるトークン数。⚠️ pipe_push が CH 単位なので CH に固定 */
#define TOK_G    ((TOK_K + CH - 1 + TOK_K - 1) / TOK_K)   /* リングの群数 = ceil((K+CH−1)/K) = 2 */

/* --- GD（音素段の膨張畳み込み。saanotts.h の SAAN_GD_N の注記）をトークンパイプの後ろに足す ------
 *
 * 案 B（状態を持つパイプを延ばす）: token ブロック 3 段（出力は t − 12）の後ろに、pad = 膨張 d の
 * 段を 5 つ（1 / 2 / 4 / 8 / 16）つなぐ。段 s は窓 [AC_W][2d + K] を持ち、中央 K 列だけ
 * k=3・膨張 d の conv で計算する（`saan_conv1d_wrd` の出力範囲 [d, d + K)）。各トークンは
 * **段ごとに 1 回だけ**計算される（Duration の窓分割のような再計算は無い）。
 *   - 段を通るごとに d だけ過去にずれる: 出力の遅延 = 12 + 31 = **43**（GD 有効時の `tok_halo`）。
 *   - 段どうしの間に relu（最終段の後は無し）。発話外はゼロ（一括版は配列の外 = ゼロパディング）。
 *   - 残差 `h + y` の h（token ブロック 3 段目の出力）は **31 列ぶん過去**まで要るので、
 *     `gd_h [AC_W][GD_HR]`（GD_HR = 31 + K = 39 列）のリングに持つ。最終段の出力時刻 t_h − 31 の
 *     K 列は、リングの列 [0, K) にちょうど重なる。
 *   - リング `tok_ring` の群数 TOK_G は `tok_halo` に依らない（導出は TOK_K + CH − 1 だけを使う）。
 * ⚠️ 陽性対照: 最終段の膨張を 16 → 8 にする（`-DSAAN_TEST_GD_STREAM_DIL16_AS_8`）/ bf を 1 音素ずらす
 *    （`-DSAAN_TEST_GD_STREAM_BF_SHIFT`）と `make -C csrc gd` が落ちる。 */
#define GD_HR       (SAAN_GD_RF + TOK_K)
#define GD_MAXPAD   16
static int gd_pad(int s) {
    static const int d[SAAN_GD_N] = {1, 2, 4, 8, 16};
    return d[s];
}
/* conv に渡す膨張。窓の寸法（pad）は gd_pad のまま。⚠️ 陽性対照専用の差し替えは本番では定義しない */
static int gd_conv_dil(int s) {
#ifdef SAAN_TEST_GD_STREAM_DIL16_AS_8
    if (s == SAAN_GD_N - 1) return 8;
#endif
    return gd_pad(s);
}
/* 共用窓の float 数（GD 有効時は pad 16 の段 = AC_W × (2·16 + CH) が最大になる） */
static size_t pwnd_n_gd(void) { return (size_t)AC_W * (2 * GD_MAXPAD + CH); }

/* --- パイプ段 ------------------------------------------------------------- */

/* --- MEM-6: パイプ段は**ハローだけ**持つ（`-DSAAN_MEM_PIPE_HALO=1`）-----------
 *
 * 各段の窓は `[C][W]`（W = 2·pad + CH）だが、**step をまたいで生き残るのは末尾の
 * `W − CH = 2·pad` 列だけ**である（`pipe_push` は左へ CH シフトして末尾に CH 列
 * 足すので、次の step の窓の先頭 2·pad 列 = 今の窓の末尾 2·pad 列）。
 * 中央の CH 列は「その step の作業用」なので、**全段で 1 本の窓を共用できる**。
 *
 *   既定(0): ac 15,360 + dinp 1,600 + dblk 21,280 + tok 9,216 = **47,456 B**
 *   1      : ハロー 21,728 + 共用窓 4,256 = **25,984 B**（**−21,472**）
 *
 * ⚠️ **共用が成り立つ根拠**（`step_chunk_body` の順序）: どの段も
 *    「窓を組む → カーネル → 残差で窓を読む → 終わり」で閉じており、
 *    **窓を次の段まで持ち越す段が 1 つも無い**（段どうしの受け渡しは
 *    `w_full` / `w_e` / `h_out` などの別バッファ）。
 * ⚠️ 代償は step ごとの memcpy（ハローの出し入れ）。実測は M-142。
 * ⚠️ `in` と `out` が同じ領域でもよい性質は保たれる（窓に写した後 `in` を読まない）。 */
#ifndef SAAN_MEM_PIPE_HALO
/* ⚠️ **既定 1**（M-142）。arena −21,472 B で、実機の定常 xRT は 0.483 → **0.474**
 * （**遅くなるどころか速くなった**。作業窓が 47,456 → 4,256 B になって D-cache に
 * 収まるのが効いているように見えるが、**その機構は未検証**）。
 * PCM は bit 一致（実機 3 文 + ホスト 50 文 + 陽性対照 3 種）。
 * `-DSAAN_MEM_PIPE_HALO=0` で元に戻る（arena も 139264 以上に戻すこと）。 */
#define SAAN_MEM_PIPE_HALO 1
#endif

typedef struct {
    float *buf;   /* 0: [C][W] の窓そのもの / 1: [C][W − CH] のハロー */
    int C, pad, W;
} pipe_t;

/* --- 解決済みの重み（S1）---------------------------------------------------
 *
 * ⚠️ **テンソル検索（saan_w / saan_tf）は init で 1 回だけ。** step の中では呼ばない。
 *    検索は名前の vsnprintf + ヘッダ（104 B × 183 エントリ）の strncmp 線形走査で、
 *    step ごとに 102 回・約 16,000 エントリ（1.66 MB 相当）を舐めていた（M-80）。
 *    実機では flash から読むので、積和と同じ桁のコストになる。
 *    ここに置くのは**ポインタだけ**で、計算の順序は 1 つも変わらない（bit 同一）。 */
typedef struct {
    saan_wref c1w, c2w;
    const float *c1b, *c2b, *ng, *nb;
} saan_acblk_w;

typedef struct {
    saan_wref dw, p1w, p2w, cdw, cuw;
    const float *p1b, *p2b, *cdb, *cub, *gm;
} saan_decblk_w;

/* 1 段が確保する float 数（MEM-6 で窓 → ハローに変わる）。
 * ⚠️ **`saan_stream_arena_used` と 1:1**。片方だけ変えると `make -C csrc arena` §5 が落ちる。 */
/* 共用窓の float 数 = 段ごとの C×W の最大（ac 48x16 / dinp 40x10 / dblk 76x14 / tok 48x16）。
 * ⚠️ **段を足したらここも足す。** 足りないと隣のバッファを黙って壊す。 */
#define PIPE_WND_N ((size_t)DEC_W * (2 * 3 + CH) > (size_t)AC_W * (2 * TOK_PAD + CH) \
                    ? (size_t)DEC_W * (2 * 3 + CH) : (size_t)AC_W * (2 * TOK_PAD + CH))

/* GD 有効なら共用窓は大きい方（pwnd_n_gd）。arena の和と確保は**この関数を通す**（1:1） */
static size_t pwnd_n_for(int gd);
#define PIPE_N(C, pad) ((size_t)(C) * (size_t)(SAAN_MEM_PIPE_HALO ? (2 * (pad)) : (2 * (pad) + CH)))

static int pipe_init(pipe_t *p, saan_arena *a, int C, int pad) {
    p->C = C; p->pad = pad; p->W = 2 * pad + CH;
    p->buf = (float *)saan_alloc(a, sizeof(float) * PIPE_N(C, pad));
    if (!p->buf) return 0;
    memset(p->buf, 0, sizeof(float) * PIPE_N(C, pad));   /* 先頭はゼロパディング */
    return 1;
}

/* CH フレームを末尾に押し込む（左へ CH シフト）。`src` は [C][CH]。
 * NULL なら**ゼロ**を押し込む = 発話末尾の右パディング（一括版と同じ挙動）
 * ⚠️ MEM-6（ハロー方式）では使わない（pipe_open が同じ内容を共用窓に組む）。 */
#if !SAAN_MEM_PIPE_HALO
static void pipe_push(pipe_t *p, const float *src) {
    SAAN_PROF_BEGIN(SAAN_PROF_PIPE);
    for (int c = 0; c < p->C; ++c) {
        float *row = p->buf + (size_t)c * p->W;
        memmove(row, row + CH, sizeof(float) * (size_t)(p->W - CH));
        float *tail = row + p->W - CH;
        if (src) memcpy(tail, src + (size_t)c * CH, sizeof(float) * CH);
        else memset(tail, 0, sizeof(float) * CH);
    }
    SAAN_PROF_END(SAAN_PROF_PIPE);
}
#endif

/* （旧 pipe_center は T4 で消えた: 中央を取り出していたのは cdel だけで、cring_center が代わる） */

/* --- MEM-6 の窓の出し入れ -----------------------------------------------------
 *
 * `PIPE_OPEN` が返すのは「`pipe_push` 後の窓と**同じ内容・同じ stride `p->W`**」の
 * ポインタで、既定（0）では `p->buf` そのもの。1 ではハロー + `src` を共用窓に組む。
 * `PIPE_CLOSE` は次の step に残る `[CH, W)` を保存する（既定では何もしない）。 */
#if SAAN_MEM_PIPE_HALO
static float *pipe_open(float *wnd, pipe_t *p, const float *src) {
    SAAN_PROF_BEGIN(SAAN_PROF_PIPE);
    const int H = p->W - CH;          /* = 2*pad。持ち越す列数 */
    for (int c = 0; c < p->C; ++c) {
        float *row = wnd + (size_t)c * p->W;
        memcpy(row, p->buf + (size_t)c * H, sizeof(float) * (size_t)H);
        if (src) memcpy(row + H, src + (size_t)c * CH, sizeof(float) * CH);
        else     memset(row + H, 0, sizeof(float) * CH);   /* 発話末尾の右パディング */
    }
    SAAN_PROF_END(SAAN_PROF_PIPE);
    return wnd;
}
static void pipe_close(pipe_t *p, const float *wnd) {
    SAAN_PROF_BEGIN(SAAN_PROF_PIPE);
    const int H = p->W - CH;
    for (int c = 0; c < p->C; ++c)
        memcpy(p->buf + (size_t)c * H, wnd + (size_t)c * p->W + CH,
               sizeof(float) * (size_t)H);
    SAAN_PROF_END(SAAN_PROF_PIPE);
}
#define PIPE_OPEN(im, p, src)  pipe_open((im)->pwnd, (p), (src))
#define PIPE_CLOSE(p, w)       pipe_close((p), (w))
#else
#define PIPE_OPEN(im, p, src)  ((void)(im), pipe_push((p), (src)), (p)->buf)
#define PIPE_CLOSE(p, w)       ((void)(p), (void)(w))
#endif

/* --- c のリング（T4 = MEM-1）------------------------------------------------
 *
 * 幅 CRING_W = SAAN_HALO_DEC + CH。decoder の段 i（inp = 0、dw ブロック = 1..5）の出力時刻は
 * c の時刻 t_c から d_i = 1 + 3·i だけ過去（d ∈ {1, 4, 7, 10, 13, 16}）で、その段が読む c は
 * [t_c − d_i, t_c − d_i + CH)。cring_push で最新の [t_c, t_c + CH) を末尾に押し込むと列 0 の時刻は
 * t_c + CH − CRING_W = t_c − SAAN_HALO_DEC なので、段 i は列 SAAN_HALO_DEC − d_i から CH 列を読む。
 * 位置は**絶対時刻の差**で決める（cring_center の t_out − cring_t0）。範囲外なら SAAN_ERR_SHAPE で
 * 止める（黙って別の時刻の c を読まない）。
 * ⚠️ 陽性対照（計画 T4）: cring_center の列に +1（遅延を 1 フレームずらす）すると stream G2（多文）が
 *    落ちる（2026-09-03 に実際に落ちるのを見て戻した）。 */
#define CRING_W (SAAN_HALO_DEC + CH)

/* w_e [E][CH] の中の re / im / frm の配置（float 単位、各 16 B 境界） */
#define ISTFT_RE_OFF  0
#define ISTFT_IM_OFF  ((int)(SAAN_ALIGN16(sizeof(float) * NB) / sizeof(float)))
#define ISTFT_FRM_OFF (2 * ISTFT_IM_OFF)
#define ISTFT_END     (ISTFT_FRM_OFF + SAAN_NFFT)
/* C99 には _Static_assert が無いので配列の typedef で検査する: w_e に re / im / frm が収まること */
typedef char saan_t4_istft_fits_in_w_e[(E * CH >= ISTFT_END) ? 1 : -1];

/* --- GD の状態（blob に acoustic.g.* が在る or bf が渡されたときだけ arena に取る）--------- */
struct saan_gdx {
    int gd_on;
    int32_t tok_halo;        /* token パイプ全体の遅延 = 12（GD 無し）/ 12 + 31（GD 有り） */
    pipe_t gd[SAAN_GD_N];
    saan_wref gdw[SAAN_GD_N];
    const float *gdb[SAAN_GD_N];
    float *gd_h;             /* [AC_W][GD_HR]: token ブロック出力の遅延リング（残差用） */
    const float *bf;         /* 音素ごとの 48 次元の加算ベクトル [n_ids][AC_W]。NULL なら加算しない */
};

#define OLA_LEN (SAAN_NFFT + 2 * SAAN_HOP)   /* ola / olw の長さ。out_pos が N/2 先行する分 */

#if SAAN_G_EXCITE
/* 倍音励起の状態（処理は istft の後ろの「倍音励起つきの G」節） */
#define EXC_NY 4        /* Y のリングの枠数。未出力の Y は最大 3 + 新しい 1 */
#define EXC_NF 16       /* F0 / v / 埋めた log F0 のリング（絶対フレーム mod 16） */
#define EXC_H 32        /* F0 ヘッドの隠れ幅 */
#define EXC_LOGREF 5.991464547107982f      /* ln 400 */
#define EXC_NOISE_SPEC 13.063945294843618f /* 256 / sqrt(384) */
/* 有声区間に足す雑音の比の既定（学習形と同じ 0.03。0.1 はノイズが増えたので全声共通の既定には採らない = DLS-300・原本 §24）。**ここ 1 か所**。変えるなら Python の端末形（g_excite.py の VNOISE）も同じに。
 * 声ごとの値は実行時に saan_stream_set_vnoise で変える（init で この既定に戻る）。ホストの合成は -DSAAN_EXC_VNOISE=<値>f で既定ごと変えられる */
#ifndef SAAN_EXC_VNOISE
#define SAAN_EXC_VNOISE 0.03f
#endif
#ifndef SAAN_EXC_SEED
#define SAAN_EXC_SEED 0x2545F491u
#endif

struct saan_exc {
    const float *f0w1, *f0b1, *f0w2, *f0b2;   /* F0 ヘッド: blob の decoder.f0h.{0,2}.{weight,bias}（fp32）*/
    float hp[DEC_W * 2];               /* 直前のチャンクの h の最後の 2 フレーム [c][2] */
    float hx[DEC_W * (CH + 2)];        /* [c][CH+2] = 直前 2 + 今回 CH */
    float lfr[EXC_NF];                 /* F0 ヘッドの出力 ln F0（生） */
    float ffl[EXC_NF];                 /* 無声を埋めた ln F0 */
    unsigned char vr[EXC_NF];          /* 有声 */
    float fcur;                        /* 直前の有声の ln F0 */
    int have_cur;
    float vnoise;                      /* 有声区間の雑音比（既定 SAAN_EXC_VNOISE。saan_stream_set_vnoise で声ごとに変える） */
    /* 声の高さの整形（V4。saanotts_stream.h の saan_f0shape）。fs_on == 0 なら元の 1 行を通す */
    saan_f0shape fs;                   /* 利用者が設定した値（get 用） */
    int fs_on;                         /* k != 1 で検査を通った */
    float fs_lc, fs_hi, fs_lo, fs_alpha;   /* ln c / ln(hi) - ln c / ln(lo) - ln c / lp の係数 */
    float lp; int lp_init;             /* 有声フレームだけで更新する低域通過の状態 */
    int32_t fill_next;                 /* ffl を埋めた次のフレーム */
    int32_t known;                     /* F0 を出した最後の絶対フレーム（-1 = まだ） */
    int32_t next_x;                    /* 次に X を作るフレーム */
    int32_t ymax;                      /* Y を積んだ最後のフレーム */
    uint32_t ph, rng;                  /* 位相（1 周 = 2^32）/ xorshift32 の状態 */
    int32_t blk_next;                  /* 次に作る e のブロック（256 サンプル） */
    int32_t n_voiced, n_f0;            /* F0 ヘッドが有声と判定したフレーム数 / 出したフレーム数（測定用） */
    float eb[4 * SAAN_HOP];            /* e のリング（4 ブロック） */
    float ehead[SAAN_NFFT / 2 + 1];    /* e[0..512]（先頭の反射用） */
    float y[EXC_NY * 2 * NB];          /* Y のリング [slot][re NB | im NB] */
};

#endif

/* --- 内部状態 ------------------------------------------------------------- */

struct saan_stream_impl {
    pipe_t ac[5];        /* AcBlock。pad=4（c1 k=5 と c2 k=5 で ±2+±2） */
    pipe_t dinp;         /* decoder inp。pad=1 */
    pipe_t dblk[5];      /* dw ブロック。pad=3 */
    /* T4（MEM-1、2026-09-03）: decoder の各段に同期させる c は **1 本のリング** cring [CD][CRING_W]。
     * 旧 cdel[0..5] は同じ c を 6 本の pipe（pad 1 / 3×5、計 12,800 B）に別々の遅延で持っていたが、
     * 中身は 1 本の c のシフトなので、最新 CRING_W = SAAN_HALO_DEC + CH フレームを 1 本持てば
     * 各段は自分の遅延位置（cring_t0 からの列）を読むだけで済む（3,840 B。純粋なデータ移動 = bit 同一）。
     * ⚠️ この幅は **T2（S9）で cdown / cup を中央 CH だけにした後**の下限。窓全部（W=14）に掛けて
     *    いた頃は段 4 の窓の左端 t − 16 − 3 まで要り、27 フレームだった。 */
    float *cring;        /* [CD][CRING_W] */
#if SAAN_MEM_PIPE_HALO
    float *pwnd;         /* MEM-6: 全段で共用する作業窓。stride は段ごとの p->W */
#endif
    int32_t cring_t0;    /* cring の列 0 の絶対フレーム番号（cring_push が更新） */

    /* S9 で作業領域は「要る列だけ」の圧縮形になった（arena −11 KB。CH=8）。
     * w_full: AC では c1 の出力 [AC_W][W_AC − 4]、DEC では dw の出力 + g [DEC_W][CH]、
     *         step_chunk では decoder の h [DEC_W][CH]（同じ領域を段ごとに使い回す） */
    float *w_full;       /* [max(AC_W * (W_AC − 4), DEC_W * CH)] */
    float *w_c;          /* [CD * CH] dec_step の c（リングから取り出した中央 CH。旧 c_sync） */
    float *w_ch;         /* [max(C) * CH] 中央の取り出し */
    float *w_ch2;
    float *w_e;          /* [E * CH] pw1 の出力（旧 [E][W_DEC]）。⚠️ iSTFT の re / im / frm と共用（下記） */
    float *w_r;          /* [R * CH]（旧 [R][W_DEC]） */
    float *w_g;          /* [DEC_W * CH]（旧 [DEC_W][W_DEC]） */
    float *o1539;        /* [1539] decoder の生出力 **1 フレーム分**（mag/cos/sin のビュー） */
    float *hr;           /* [DEC_HEAD * CH] */

    /* iSTFT。⚠️ **re / im / frm は w_e の中に置く**（T4 = MEM-2。arena −8,224 B）。
     * 生存期間が重ならない根拠（step_chunk_body の順序）:
     *   w_e が生きるのは dec_step_body の中だけ: pw1 が書き → GELU → pw2 が読む。
     *   pw2 の後は誰も w_e を読まず、次の step の pw1 が**全要素**を書き直す。
     *   re / im / frm が生きるのは istft_push の中だけ: 先頭のループが re / im を [0, NB) **全部**書き、
     *   irfft が frm を [0, N) 全部書き、同じ関数の末尾の overlap-add で読み切る。
     *   step_chunk_body では 5 段の dec_step → head → istft_push の順で、両者は同じ step の中でも
     *   同時には生きていない。step をまたいで持ち越す値もどちらにも無い。
     * 大きさ: w_e は E×CH = 2,432 float、re / im / frm は 516 + 516 + 1,024 = 2,056 float
     *   （各 16 B 境界。下の typedef で静的に検査） */
    float *ola, *olw, *win, *re, *im, *frm;
    int32_t fpush;       /* push 済みフレーム数（絶対フレーム番号 + 1） */
    int32_t out_pos;     /* 次に出す絶対サンプル位置 */
    int32_t skip_hops;   /* 捨てる先頭 hop 数（一括版の N/2 切り出しに対応） */
    /* GD / bf の状態 `struct saan_gdx` の位置（impl の先頭からのバイト数。0 = 無し）。
     * ⚠️ **ポインタにしない**: GD を使わない構成の `sizeof(impl)` と arena を 1 バイトも増やさないため。
     *    ここは旧 `int ola_len`（常に SAAN_NFFT + 2·SAAN_HOP = 定数。`OLA_LEN`）の席で、メンバーの
     *    総サイズは変わらない。gdx は impl の直後に同じ確保で置く（要るときだけ）。 */
    int32_t gdx_off;
    float *obuf;         /* [SAAN_OBUF_HOPS * HOP] 出力の詰め替え。
                          * 深さは 2·CH − (SAAN_LATENCY mod CH) = CH+4（saanotts_stream.h） */
    int32_t ofill;
    /* `saan_stream_pull_ptr` が前回返した hop 数（**まだ obuf から退けていない**）。
     * ポインタで返す版は呼び出し側が読み終わるまで obuf を動かせないので、
     * **次の呼び出しの先頭で退ける**（obuf_retire）。コピー版は即座に退く。 */
    int32_t opend;
    /* S6（T3）: token block のパイプ 3 段（pad=TOK_PAD、幅 2·TOK_PAD + TOK_K）と出力リング
     * tok_ring [TOK_G][AC_W][TOK_K]。tok_pushed は押し込んだ群の数（出そろい末尾は
     * tok_pushed·TOK_K − TOK_HALO）。段の作業領域は w_full / w_ch2 を借りる（tok_pipe_advance_body） */
    pipe_t tok[3];
    float *tok_ring;
    int32_t tok_pushed;
    /* GD（膨張畳み込み 5 段）と bf は別の構造体 `struct saan_gdx`（`gdx_off` の先）に分けて、
     * 要るときだけ arena に取る。impl に直接置くと GD を使わない構成でも `sizeof(impl)` が増え、
     * arena が 352 B（ホスト 64 bit）増えた（実測。後方互換を崩す） */

    /* S1: 解決済みの重み。init の resolve_weights() が埋める */
    saan_acblk_w  tokw[3];   /* acoustic.token.%d */
    saan_acblk_w  acw[5];    /* acoustic.frame.%d */
    saan_decblk_w decw[5];   /* decoder.{dw,pw1,pw2,cdown,cup,gamma}.%d */
    saan_wref ow, iw, hdw, how;
    const float *ib, *hdb, *hob, *emb, *pos;
#if SAAN_G_EXCITE
    struct saan_exc *exc;   /* 倍音励起の状態（G_EXCITE のときだけ。arena に取る） */
#endif
};

static struct saan_gdx *gdx_of(struct saan_stream_impl *im) {
    return im->gdx_off ? (struct saan_gdx *)((char *)im + im->gdx_off) : NULL;
}

/* 1 段ぶんの重みを引く。**欠けを許すのは元の step と同じ場所だけ**（bias / LN の bias） */
static int acblk_resolve(const saan_weights *w, const char *pfx, int bi, saan_acblk_w *o) {
    o->c1w = saan_w(w, "%s.%d.c1.weight", pfx, bi);
    o->c1b = saan_tf(w, "%s.%d.c1.bias", pfx, bi);
    o->c2w = saan_w(w, "%s.%d.c2.weight", pfx, bi);
    o->c2b = saan_tf(w, "%s.%d.c2.bias", pfx, bi);
    o->ng  = saan_tf(w, "%s.%d.norm.weight", pfx, bi);
    o->nb  = saan_tf(w, "%s.%d.norm.bias", pfx, bi);
    return SAAN_W_OK(o->c1w) && SAAN_W_OK(o->c2w) && o->ng != NULL;
}

static int decblk_resolve(const saan_weights *w, int i, saan_decblk_w *o) {
    o->dw  = saan_w(w, "decoder.dw.%d.weight", i);
    o->p1w = saan_w(w, "decoder.pw1.%d.weight", i);
    o->p1b = saan_tf(w, "decoder.pw1.%d.bias", i);
    o->p2w = saan_w(w, "decoder.pw2.%d.weight", i);
    o->p2b = saan_tf(w, "decoder.pw2.%d.bias", i);
    o->cdw = saan_w(w, "decoder.cdown.%d.weight", i);
    o->cdb = saan_tf(w, "decoder.cdown.%d.bias", i);
    o->cuw = saan_w(w, "decoder.cup.%d.weight", i);
    o->cub = saan_tf(w, "decoder.cup.%d.bias", i);
    o->gm  = saan_tf(w, "decoder.gamma.%d", i);
    return SAAN_W_OK(o->dw) && SAAN_W_OK(o->p1w) && SAAN_W_OK(o->p2w)
        && SAAN_W_OK(o->cdw) && SAAN_W_OK(o->cuw) && o->gm != NULL;
}

/* 全部の重みを引く。**1 つでも欠けたら SAAN_ERR_MISSING**（step の中で初めて分かるより早い） */
static saan_status resolve_weights(saan_stream *st) {
    struct saan_stream_impl *im = (struct saan_stream_impl *)st->impl;
    const saan_weights *w = st->w;
    for (int i = 0; i < 3; ++i)
        if (!acblk_resolve(w, "acoustic.token", i, &im->tokw[i])) return SAAN_ERR_MISSING;
    for (int i = 0; i < 5; ++i)
        if (!acblk_resolve(w, "acoustic.frame", i, &im->acw[i])) return SAAN_ERR_MISSING;
    for (int i = 0; i < 5; ++i)
        if (!decblk_resolve(w, i, &im->decw[i])) return SAAN_ERR_MISSING;
    im->ow  = saan_w(w, "acoustic.out.weight");
    im->iw  = saan_w(w, "decoder.inp.weight");
    im->ib  = saan_tf(w, "decoder.inp.bias");
    im->hdw = saan_w(w, "decoder.hdown.weight");
    im->hdb = saan_tf(w, "decoder.hdown.bias");
    im->how = saan_w(w, "decoder.hout.weight");
    im->hob = saan_tf(w, "decoder.hout.bias");
    im->emb = saan_tf(w, "acoustic.emb.weight");
    im->pos = saan_tf(w, "acoustic.pos.weight");
    if (!SAAN_W_OK(im->ow) || !SAAN_W_OK(im->iw) || !SAAN_W_OK(im->hdw)
        || !SAAN_W_OK(im->how) || !im->emb || !im->pos) return SAAN_ERR_MISSING;
    return SAAN_OK;
}

/* GD の構成ビット（`saan_stream_arena_used_ex` の `gd` 引数）。1 = blob に GD / 2 = bf だけ */
int saan_stream_gd_flags(const saan_weights *w, const float *bf) {
    return saan_has_gd(w) ? 1 : (bf ? 2 : 0);
}

/* init が成功したときの a->used そのもの。**stream_init_body の確保と 1:1**（順序は和なので不問）。
 * ⚠️ 2 回書いている。arena_stress の §5 が実測と bit 一致で突き合わせる（ずれたら NG） */
static size_t pwnd_n_for(int gd) {
    return (gd & 1) && pwnd_n_gd() > PIPE_WND_N ? pwnd_n_gd() : PIPE_WND_N;
}

size_t saan_stream_arena_used(int32_t n_ids) {
    return saan_stream_arena_used_ex(n_ids, 0);
}

/* `gd` は構成のビット: 1 = blob に GD（acoustic.g.*）が在る / 2 = bf を渡す（GD 無し）。
 * 0 のときは GD 関連の確保が 1 バイトも無い（`saan_stream_arena_used` と同じ値）。
 * ⚠️ `saan_has_gd(w) ? 1 : (bf ? 2 : 0)` で作る（`saan_stream_gd_flags`）。 */
size_t saan_stream_arena_used_ex(int32_t n_ids, int gd) {
    const int maxC = DEC_W > AC_W ? DEC_W : AC_W;
    const int W_AC = 2 * 4 + CH;
    const size_t full_n = (size_t)AC_W * (W_AC - 4) > (size_t)DEC_W * CH
                        ? (size_t)AC_W * (W_AC - 4) : (size_t)DEC_W * CH;
    size_t s = 0;
    /* --- 発話長（ids 数）に比例する分（8 B/id） --- */
    s += SAAN_ALIGN16(sizeof(float) * (size_t)n_ids);            /* log_d */
    s += SAAN_ALIGN16(sizeof(int32_t) * (size_t)n_ids);          /* d_hat */
    /* --- 発話長に依存しない分（G3 の対象） --- */
    s += SAAN_ALIGN16(sizeof(struct saan_stream_impl));          /* ⚠️ ポインタ幅で変わる */
    s += SAAN_ALIGN16(sizeof(float) * PIPE_N(AC_W, 4)) * 5;       /* ac（MEM-6 でハロー） */
    s += SAAN_ALIGN16(sizeof(float) * PIPE_N(CD, 1));             /* dinp */
    s += SAAN_ALIGN16(sizeof(float) * PIPE_N(DEC_W, 3)) * 5;      /* dblk */
#if SAAN_MEM_PIPE_HALO
    s += SAAN_ALIGN16(sizeof(float) * pwnd_n_for(gd));            /* 共用窓（MEM-6。GD 有りなら pad 16 の段の窓） */
#endif
    s += SAAN_ALIGN16(sizeof(float) * CD * CRING_W);              /* cring（T4。旧 cdel 6 本 12,800 B） */
    /* S9: 作業領域は圧縮形 */
    s += SAAN_ALIGN16(sizeof(float) * full_n);                    /* w_full */
    s += SAAN_ALIGN16(sizeof(float) * (size_t)CD * CH);           /* w_c */
    s += SAAN_ALIGN16(sizeof(float) * (size_t)maxC * CH) * 2;     /* w_ch/2 */
    s += SAAN_ALIGN16(sizeof(float) * (size_t)E * CH);            /* w_e（re / im / frm もこの中。T4） */
    s += SAAN_ALIGN16(sizeof(float) * (size_t)SAAN_DEC_R * CH);   /* w_r */
    s += SAAN_ALIGN16(sizeof(float) * (size_t)DEC_W * CH);        /* w_g */
#if !SAAN_HEAD_EXT
    s += SAAN_ALIGN16(sizeof(float) * 1539 * HEAD_COLS);          /* o1539（MEM-5 で [1539][1]） */
#endif
    s += SAAN_ALIGN16(sizeof(float) * (size_t)SAAN_DEC_HEAD * CH);/* hr */
    s += SAAN_ALIGN16(sizeof(float) * (size_t)(SAAN_NFFT + 2 * SAAN_HOP)) * 2;   /* ola / olw */
    s += SAAN_ALIGN16(sizeof(float) * SAAN_NFFT);                 /* win */
    /* re / im / frm は w_e と共用（T4 = MEM-2）なので確保しない */
    /* obuf: 2·CH − (SAAN_LATENCY mod CH) hop（= CH+4。saanotts_stream.h の導出） */
    s += SAAN_ALIGN16(sizeof(float) * (size_t)SAAN_OBUF_HOPS * SAAN_HOP);
    /* S6（T3）: token パイプ 3 段 + リング（旧 tok_buf / tok_w1 / tok_w2 / tok_out 17,664 B → 12,288 B） */
    s += SAAN_ALIGN16(sizeof(float) * PIPE_N(AC_W, TOK_PAD)) * 3;  /* tok[0..2] */
    s += SAAN_ALIGN16(sizeof(float) * (size_t)TOK_G * AC_W * TOK_K);       /* tok_ring */
    if (gd) s += SAAN_ALIGN16(sizeof(struct saan_gdx));         /* GD / bf の状態（要るときだけ） */
#if SAAN_G_EXCITE
    s += SAAN_ALIGN16(sizeof(struct saan_exc));                 /* 倍音励起の状態 */
#endif
    if (gd & 1) {
        /* GD: 膨張 1/2/4/8/16 の 5 段のハロー + 残差用の h リング（stream_init_body と 1:1） */
        for (int s5 = 0; s5 < SAAN_GD_N; ++s5)
            s += SAAN_ALIGN16(sizeof(float) * PIPE_N(AC_W, gd_pad(s5)));
        s += SAAN_ALIGN16(sizeof(float) * (size_t)AC_W * GD_HR);
    }
    return s;
}

/* streaming の間に conv が arena の上に一時的に取る activation 作業領域の**最大**。
 *
 * ⚠️ **`saan_stream_arena_used()` はこれを含まない**（conv の中で確保してすぐ返すので
 *    init 後の `a->used` には出ない）。だが `a->peak` には出る。**下限は used ではなく
 *    used + これ**である（W8A8 で 2,464 B。W8A32 では 0）。
 * ⚠️ **MEM-7 の前はこの抜けが見えなかった** — 350 ids では duration の一時領域
 *    149,824 B が streaming の真のピーク 116,592 B より大きく、`arena_peak` が
 *    duration 側を返していたので**偶然に安全側**だった（[C-102](../docs/decisions.md#c-102)）。
 *    53 ids では当時から `a.peak` 114,224 > `arena_peak()` 111,760 とずれていた。
 * S9 で 1×1 conv は T=CH になったので候補の最大を取る: pw2（cin=E, T=CH）/
 * token の c1（cin=AC_W, T = 2·TOK_PAD + TOK_K）/ dw（ch=DEC_W, T=W_DEC）/ AC の c1。 */
static size_t stream_act_scratch_max(int gd) {
    size_t m = saan_act_scratch_needed(E, CH), v;
    if (gd & 1) { v = saan_act_scratch_needed(AC_W, 2 * GD_MAXPAD + CH); if (v > m) m = v; }   /* GD の d=16 の段 */
    v = saan_act_scratch_needed(AC_W, 2 * TOK_PAD + TOK_K); if (v > m) m = v;
    v = saan_act_scratch_needed(DEC_W, 2 * 3 + CH); if (v > m) m = v;
    v = saan_act_scratch_needed(AC_W, 2 * 4 + CH);  if (v > m) m = v;
    v = saan_act_scratch_needed(SAAN_DEC_HEAD, CH); if (v > m) m = v;   /* hout（G87 は cin=512） */
    return m;
}

/* init の途中も含めた**本当のピーク**（C-100 / C-102）。
 *
 * ⚠️ **`saan_stream_arena_used()` は init が終わった後の値**で、init の途中で
 *    `saan_run_duration` が取って返す一時領域を含まない。
 *    `arena_used` を下限だと思って arena を詰めると **長い文だけ init が失敗する**
 *    （実際に踏んだ: 120 KB にしたら `make -C csrc arena` が「300 ids は通るが
 *    350 ids で落ちる」と報告した）。
 * ⚠️ **MEM-7 の前は 350 ids で 134,400 B あり、streaming のバッファより大きかった**
 *    （= 下限の 89.7%）。窓分割したので今は `SAAN_DUR_K` で決まる定数に近い
 *    （K=128 なら最大 63,840 B）。**下限は streaming 側に移った。**
 * ⚠️ `saan_stream_arena_needed()` は和で足す**緩い上限**なので、詰めるには使えない。
 *    こちらは 2 フェーズの **max** = 実際に要る量。
 * ⚠️ **`saan_run_duration` の窓の取り方と 1:1。** 片方だけ変えると
 *    `make -C csrc arena` の §1/§6 が「関数が言う下限で init が落ちる」と報告する。 */
static size_t dur_window_bytes(int32_t n_ids) {
    /* 1 窓ぶん（h / t1 / t2 の 3 本 + W8A8 の act scratch）。最後の窓は短いので
     * **最大になるのは kk = min(n_ids, SAAN_DUR_K) の窓** */
    const int kk = (n_ids < SAAN_DUR_K) ? (int)n_ids : SAAN_DUR_K;
    const int Tw = kk + 2 * SAAN_DUR_HALO;
    return SAAN_ALIGN16(sizeof(float) * (size_t)SAAN_DUR_W * (size_t)Tw) * 3
         + saan_act_scratch_needed(SAAN_DUR_W, Tw);
}

size_t saan_stream_arena_peak(int32_t n_ids) {
    return saan_stream_arena_peak_ex(n_ids, 0);
}

size_t saan_stream_arena_peak_ex(int32_t n_ids, int gd) {
    /* フェーズ 1: log_d + d_hat + duration の 1 窓（MEM-7） */
    size_t dur = SAAN_ALIGN16(sizeof(float) * (size_t)n_ids)
               + SAAN_ALIGN16(sizeof(int32_t) * (size_t)n_ids)
               + dur_window_bytes(n_ids);
    /* フェーズ 2: streaming のバッファ（duration の分は a->used を巻き戻して返す）
     * ＋ conv が上に取る act 作業領域（C-102。**used だけでは 2,464 B 足りない**） */
    const size_t used = saan_stream_arena_used_ex(n_ids, gd) + stream_act_scratch_max(gd);
    return dur > used ? dur : used;
}

size_t saan_stream_arena_needed(int32_t n_ids) {
    return saan_stream_arena_needed_ex(n_ids, 0);
}

size_t saan_stream_arena_needed_ex(int32_t n_ids, int gd) {
    /* 確保の一覧は saan_stream_arena_used に 1 つだけ持つ。ここは緩い上限:
     * duration の一時領域（init の中で確保して返す。MEM-7 で 1 窓ぶん）を**和で**足す */
    size_t s = saan_stream_arena_used_ex(n_ids, gd);
    s += dur_window_bytes(n_ids);
    /* W8A8 のとき conv 1 本ぶんの activation 作業領域（上の helper と 1:1）。
     * W8A32（既定）では 0 が返るので G1/G3 の実測値は変わらない */
    s += stream_act_scratch_max(gd);
    return s + 8192;
}

/* --- c のリングの操作（T4）------------------------------------------------- */

/* c [CD][CH]（絶対時刻 [t_c, t_c + CH)）を末尾に押し込む（左へ CH シフト） */
static void cring_push(struct saan_stream_impl *im, const float *c, int32_t t_c) {
    SAAN_PROF_BEGIN(SAAN_PROF_PIPE);
    for (int ch = 0; ch < CD; ++ch) {
        float *row = im->cring + (size_t)ch * CRING_W;
        memmove(row, row + CH, sizeof(float) * (size_t)(CRING_W - CH));
        memcpy(row + CRING_W - CH, c + (size_t)ch * CH, sizeof(float) * CH);
    }
    im->cring_t0 = t_c + CH - CRING_W;
    SAAN_PROF_END(SAAN_PROF_PIPE);
}

/* 絶対時刻 [t_out, t_out + CH) の c を [CD][CH] として dst に取り出す */
static saan_status cring_center(const struct saan_stream_impl *im, int32_t t_out, float *dst) {
    const int32_t col = t_out - im->cring_t0;
    if (col < 0 || col + CH > CRING_W) return SAAN_ERR_SHAPE;
    SAAN_PROF_BEGIN(SAAN_PROF_PIPE);
    for (int ch = 0; ch < CD; ++ch)
        memcpy(dst + (size_t)ch * CH, im->cring + (size_t)ch * CRING_W + col,
               sizeof(float) * CH);
    SAAN_PROF_END(SAAN_PROF_PIPE);
    return SAAN_OK;
}

/* --- 各段の処理 ----------------------------------------------------------- */

/* 発話の外（時刻 < 0 または >= n_frames）の列をゼロにする。
 *
 * ⚠️ **これが無いと一括版と一致しない。** 一括版は段 i の入力の外側を厳密に
 * ゼロとみなすが、ストリーミングでは段 i-1 が**その位置にも bias 由来の
 * 非ゼロを出す**（conv の bias / LayerNorm の bias / 残差）。
 * 実測で全サンプルが max|Δ| 0.37 ずれた原因がこれだった。 */
static void zero_outside_n(float *x, int C, int stride, int n,
                           int32_t t0, int32_t n_frames) {
    for (int m = 0; m < n; ++m) {
        const int32_t t = t0 + m;
        if (t >= 0 && t < n_frames) continue;
        for (int c = 0; c < C; ++c) x[(size_t)c * stride + m] = 0.0f;
    }
}

static void zero_outside(float *x, int C, int32_t t0, int32_t n_frames) {
    zero_outside_n(x, C, CH, CH, t0, n_frames);
}

/* AcBlock 1 段（**frame 側と token 側で共有**。S6 / T3）。in [AC_W][CH] を p の窓
 * （W = 2·pad + CH）に押し込み、中央 CH を out へ。
 * 参照実装 `AcBlock.forward`: `x + LN(c2(relu(c1(x))))`
 * `k` は段の重み（acw[bi] か tokw[bi]）、`n_valid` は発話の長さ（frame 側 n_frames / token 側
 * n_ids）、`t_out` は中央先頭の絶対時刻（フレーム番号 / トークン番号）。
 * ⚠️ in と out は**同じ領域でもよい**（token パイプがそうする）: 先頭の pipe_push で窓に写した後、
 *    in は読まない（c1 も残差も p->buf を読む）。
 * ⚠️ c1 の作業領域は im->w_full [AC_W][W − 4]。frame 側は ac_step の中、token 側は make_hf の中で
 *    使い、同時には走らない（step_chunk_body の順序）。
 *
 * S9: 計算するのは要る列だけ。
 *   c1（k=5, pad 2）: c2 が中央 [pad, pad+CH) を出すのに要る列 [pad−2, pad+CH+2) = [2, W−2)
 *                    を圧縮して w_full [AC_W][W−4] に置く
 *   c2 / LN / 残差   : 中央 CH だけ。c2 の出力は `out` [AC_W][CH] に直接書く
 *                    （以前の w_full2 [AC_W][W] と pipe_center は要らない）
 * ⚠️ 陽性対照（計画 T2）: c1 の範囲を [3, W−2) に 1 列狭める（下の c1_lo を pad−1 にし、
 *    c2 の範囲を pad−c1_lo で追従させる）と、各チャンクの中央先頭フレームが c1 の 1 列を
 *    ゼロパディングで代用してしまい、stream G2 が落ちる（2026-09-03 に実際に落ちるのを見て戻した） */
static saan_status acblk_step(saan_stream *st, pipe_t *p, const saan_acblk_w *k,
                              const float *in, float *out, int32_t t_out,
                              int32_t n_valid) {
    struct saan_stream_impl *im = (struct saan_stream_impl *)st->impl;
    const saan_wref c1w = k->c1w, c2w = k->c2w;   /* init で解決済み（S1） */
    const float *c1b = k->c1b, *c2b = k->c2b, *ng = k->ng, *nb = k->nb;

    float *pb = PIPE_OPEN(im, p, in);
    const int W = p->W;
    /* buf の先頭が対応する絶対時刻。中央の先頭 t_out から pad 引いた位置 */
    const int32_t t_buf = t_out - p->pad;
    const int c1_lo = p->pad - 2, c1_hi = p->pad + CH + 2;   /* c1 の要る列 [2, W−2) */
    const int T1 = c1_hi - c1_lo;                            /* = W − 4 */

    SAAN_TRY(saan_conv1d_wr(im->w_full, pb, c1w, c1b, AC_W, AC_W, 5, W,
                            c1_lo, c1_hi, st->a));
    saan_relu(im->w_full, (size_t)AC_W * T1);
    /* ⚠️ **c1 の出力にもゼロクリアが要る。** 一括版では c1 の配列は [0,T) しかなく、
     * c2（k=5）がその外を参照するとゼロになる。ストリーミングは c1 を窓幅で
     * 計算するので、発話外にも **bias 由来の非ゼロ**が残る。
     * 実測: これが無いと先頭 pad フレームが max|Δ| 0.49 ずれた
     * （圧縮座標なので先頭の絶対時刻は t_buf + c1_lo） */
    zero_outside_n(im->w_full, AC_W, T1, T1, t_buf + c1_lo, n_valid);
    /* c2: 中央 [pad, pad+CH) = c1 の圧縮座標 [pad − c1_lo, pad − c1_lo + CH) = [2, 10) */
    SAAN_TRY(saan_conv1d_wr(out, im->w_full, c2w, c2b, AC_W, AC_W, 5, T1,
                            p->pad - c1_lo, p->pad - c1_lo + CH, st->a));
    saan_layernorm_c(out, ng, nb, AC_W, CH);
    for (int c = 0; c < AC_W; ++c)
        for (int m = 0; m < CH; ++m)
            out[(size_t)c * CH + m] += pb[(size_t)c * W + p->pad + m];
    PIPE_CLOSE(p, pb);
    zero_outside(out, AC_W, t_out, n_valid);
    return SAAN_OK;
}

/* frame 側の AcBlock（acoustic.frame.%d）。窓の外は n_frames で切る */
static saan_status ac_step(saan_stream *st, int bi, const float *in,
                           float *out, int32_t t_out) {
    struct saan_stream_impl *im = (struct saan_stream_impl *)st->impl;
    SAAN_PROF_BEGIN(SAAN_PROF_AC);
    const saan_status s = acblk_step(st, &im->ac[bi], &im->acw[bi], in, out, t_out,
                                     st->n_frames);
    SAAN_PROF_END(SAAN_PROF_AC);
    return s;
}

/* decoder の inp（k=3）。参照実装 `Decoder.forward` の 1 行目。
 * ⚠️ c の同期（旧 cdel[0]）はここではなく step_chunk_body の cring_push が 1 回で済ませる（T4） */
static saan_status dec_inp_step_body(saan_stream *st, const float *c_in,
                                     float *h_out, int32_t t_out) {
    struct saan_stream_impl *im = (struct saan_stream_impl *)st->impl;
    pipe_t *p = &im->dinp;
    const saan_wref iw = im->iw;   /* init で解決済み（S1） */
    const float *ib = im->ib;

    float *pb = PIPE_OPEN(im, p, c_in);   /* inp の入力は c そのもの（C=CD） */
    /* S9: 中央 [pad, pad+CH) だけを `h_out` [DEC_W][CH] に直接書く。入力は p->buf なので
     * 呼び出し側が `h_out` に `w_full` を渡してきても重ならない
     * （以前は w_g に [DEC_W][W] で出してから中央を memcpy していた。w_g が
     * [DEC_W][CH] に縮んだのでそこには入らない） */
    SAAN_TRY(saan_conv1d_wr(h_out, pb, iw, ib, CD, DEC_W, 3, p->W,
                            p->pad, p->pad + CH, st->a));
    PIPE_CLOSE(p, pb);
    zero_outside(h_out, DEC_W, t_out, st->n_frames);
    return SAAN_OK;
}

static saan_status dec_inp_step(saan_stream *st, const float *c_in, float *h_out,
                                int32_t t_out) {
    SAAN_PROF_BEGIN(SAAN_PROF_DINP);
    const saan_status s = dec_inp_step_body(st, c_in, h_out, t_out);
    SAAN_PROF_END(SAAN_PROF_DINP);
    return s;
}

/* dw ブロック 1 段。参照実装:
 *   g = cup(cdown(c));  h = h + gamma * pw2(gelu(pw1(dw(h) + g)))
 * ⚠️ **c は毎段とも元の入力**（h ではない）。時刻を h と揃えるため遅延させる
 *
 * S9（計画 §9 の訂正どおり）: **dw の入力（p->buf）だけ窓全部**で、dw の出力・cdown / cup・
 * pw1 / GELU / pw2・残差はすべて中央 CH だけ。1×1 conv と GELU はフレーム独立、
 * W8A8 の量子化も per-frame なので、中央だけ計算しても各要素の値は同じ。
 *   - c は先に中央を取り出し（c_out [CD][CH]）、cdown / cup をそこに T=CH で掛ける
 *   - dw は範囲版で中央 [pad, pad+CH) だけを w_full [DEC_W][CH] に出す（DW −43%）
 *   - pw2 の出力は h_out [DEC_W][CH] に直接書き、残差もそこで取る
 * 以前は dw / cdown / cup / pw1 / GELU / pw2 を窓 W=14 全部に掛けて 6 列を捨てていた */
static saan_status dec_step_body(saan_stream *st, int i, const float *h_in,
                                 float *h_out, int32_t t_out) {
    struct saan_stream_impl *im = (struct saan_stream_impl *)st->impl;
    pipe_t *p = &im->dblk[i];
    const saan_decblk_w *k = &im->decw[i];   /* init で解決済み（S1） */
    const saan_wref dw = k->dw, p1w = k->p1w, p2w = k->p2w, cdw = k->cdw, cuw = k->cuw;
    const float *p1b = k->p1b, *p2b = k->p2b, *cdb = k->cdb, *cub = k->cub, *gm = k->gm;
    float *c_out = im->w_c;   /* この段の c [CD][CH]（リングから取り出す。T4） */

    float *pb = PIPE_OPEN(im, p, h_in);
    const int W = p->W;
    /* 条件付けは 1x1 なので pad 不要。**c の中央 CH だけ**に掛ける（値は窓全部に掛けた
     * ときの中央列と同じ = per-frame）。c はこの段の出力時刻 t_out の位置をリングから読む */
    SAAN_TRY(cring_center(im, t_out, c_out));
    zero_outside(c_out, CD, t_out, st->n_frames);
    SAAN_TRY(saan_conv1d_w(im->w_r, c_out, cdw, cdb, CD, SAAN_DEC_R, 1, CH, st->a));
    SAAN_TRY(saan_conv1d_w(im->w_g, im->w_r, cuw, cub, SAAN_DEC_R, DEC_W, 1, CH, st->a));

    /* dw: 入力は窓全部（p->buf [DEC_W][W]）、出力は中央だけ圧縮して w_full [DEC_W][CH] */
#if SAAN_DEC_DWOC
    SAAN_TRY(saan_dwconv1d_wr_oc(im->w_full, pb, dw, DEC_W, 7, W,
                                 p->pad, p->pad + CH, st->a));
#else
    SAAN_TRY(saan_dwconv1d_wr(im->w_full, pb, dw, DEC_W, 7, W,
                              p->pad, p->pad + CH, st->a));
#endif
    for (size_t k = 0; k < (size_t)DEC_W * CH; ++k) im->w_full[k] += im->w_g[k];
#if SAAN_DEC_OC
    /* 出力チャネルで 2 つの実行者に割る（`saan_conv1d_wr_oc`。行ごとの演算は同じなので値も同じ） */
    SAAN_TRY(saan_conv1d_wr_oc(im->w_e, im->w_full, p1w, p1b, DEC_W, E, 1, CH, 0, CH, st->a));
    saan_gelu_oc(im->w_e, E, CH);
    SAAN_TRY(saan_conv1d_wr_oc(h_out, im->w_e, p2w, p2b, E, DEC_W, 1, CH, 0, CH, st->a));
#else
    SAAN_TRY(saan_conv1d_w(im->w_e, im->w_full, p1w, p1b, DEC_W, E, 1, CH, st->a));
    saan_gelu(im->w_e, (size_t)E * CH);
    SAAN_TRY(saan_conv1d_w(h_out, im->w_e, p2w, p2b, E, DEC_W, 1, CH, st->a));
#endif
    for (int c = 0; c < DEC_W; ++c)
        for (int m = 0; m < CH; ++m)
            h_out[(size_t)c * CH + m] =
                pb[(size_t)c * W + p->pad + m] + gm[0] * h_out[(size_t)c * CH + m];

    PIPE_CLOSE(p, pb);
    zero_outside(h_out, DEC_W, t_out, st->n_frames);
    return SAAN_OK;
}

static saan_status dec_step(saan_stream *st, int i, const float *h_in,
                            float *h_out, int32_t t_out) {
    SAAN_PROF_BEGIN(SAAN_PROF_DEC);
    const saan_status s = dec_step_body(st, i, h_in, h_out, t_out);
    SAAN_PROF_END(SAAN_PROF_DEC);
    return s;
}

/* --- iSTFT のリングバッファ -----------------------------------------------
 *
 * 一括版は全フレームを overlap-add してから `[N/2, N/2 + T*hop)` を切り出す。
 * ここでは **NFFT + HOP サンプルのリング**で同じことをする。
 * ⚠️ **窓の二乗和で割るのは、そのサンプルに寄与する全フレームが出そろってから。**
 * hop 256 / n_fft 1024 なので 4 フレーム分待つ必要がある。
 */
/* `stride` は mag / cosv / sinv の行ストライド（列数）。CH フレームまとめて
 * 計算する既定では CH、1 フレームずつの `SAAN_MEM_HEAD_PF` では 1。
 * ⚠️ **`CH` を直に書かないこと** — 書くと per-frame 版が黙って隣の bin を読む。 */
static void istft_ola(struct saan_stream_impl *im, int32_t abs_frame);
#if !SAAN_G_EXCITE
static void istft_push(struct saan_stream_impl *im, const float *mag,
                       const float *cosv, const float *sinv, int t, int stride,
                       int32_t abs_frame) {
    SAAN_PROF_BEGIN(SAAN_PROF_ISTFT);
    for (int k = 0; k < NB; ++k) {
        const float m = mag[(size_t)k * stride + t];
        im->re[k] = m * cosv[(size_t)k * stride + t];
        im->im[k] = m * sinv[(size_t)k * stride + t];
    }
    istft_ola(im, abs_frame);
    SAAN_PROF_END(SAAN_PROF_ISTFT);
}
#endif /* !SAAN_G_EXCITE */

/* im->re / im->im（513 bin）が埋まっている前提で、逆実 FFT → 窓 → overlap-add をする。
 * 時間の計測（SAAN_PROF_ISTFT）は呼び出し側が持つ。
 * 倍音励起つき（G_EXCITE）は re / im に「G の出力 × 励起スペクトル」を入れてからここを呼ぶ。 */
static void istft_ola(struct saan_stream_impl *im, int32_t abs_frame) {
    const int N = SAAN_NFFT;
    /* 逆実 FFT。一括版と**同じ関数**を使う（2 回書かない）。
     * `-DSAAN_USE_NAIVE_DFT` で naive に戻せる（検証基準として残してある） */
#ifdef SAAN_USE_NAIVE_DFT
    for (int n = 0; n < N; ++n) {
        double acc = (double)im->re[0];
        for (int k = 1; k < N / 2; ++k) {
            const double ang = 2.0 * M_PI * (double)k * (double)n / (double)N;
            acc += 2.0 * ((double)im->re[k] * cos(ang) - (double)im->im[k] * sin(ang));
        }
        acc += (double)im->re[N / 2] * cos(M_PI * (double)n);
        im->frm[n] = (float)(acc / (double)N);
    }
#else
    saan_irfft_1024(im->re, im->im, im->frm);
#endif
    const int L = OLA_LEN;
    /* ⚠️ **絶対フレーム番号で位置を決める。** ローカルのカウンタで数えると、
     * 時刻が負の（発話に存在しない）フレームまで数えてしまい、
     * 一括版と位置がずれる（実測でそうなった） */
    const int32_t base = abs_frame * SAAN_HOP;
    for (int i = 0; i < N; ++i) {
        const int j = (int)((base + i) % L);
        im->ola[j] += im->frm[i] * im->win[i];
        im->olw[j] += im->win[i] * im->win[i];
    }
    if (abs_frame + 1 > im->fpush) im->fpush = abs_frame + 1;
}

/* リングから HOP サンプル取り出して先へ進める。
 * ⚠️ **呼ぶのはそのサンプルへの寄与が出そろってから。** サンプル p に寄与する
 * 最大フレームは floor(p/hop) なので、一括版が切り出す `[N/2, ...)` の先頭
 * （= フレーム 2）まで push してからでないと値が違う。`istft_ready()` で判定する */
static void istft_pop(struct saan_stream_impl *im, float *out) {
    const int L = OLA_LEN;
    SAAN_PROF_BEGIN(SAAN_PROF_ISTFT);
    for (int i = 0; i < SAAN_HOP; ++i) {
        const int j = (int)((im->out_pos + i) % L);
        out[i] = im->olw[j] > 1e-11f ? im->ola[j] / im->olw[j] : 0.0f;
        im->ola[j] = 0.0f;
        im->olw[j] = 0.0f;
    }
    im->out_pos += SAAN_HOP;
    SAAN_PROF_END(SAAN_PROF_ISTFT);
}

/* 次の HOP サンプルを出せるか。`out_pos + HOP - 1` に寄与する最大フレームが
 * push 済みなら出せる。**発話の全フレームを push し終えたら残りは全部出せる** */
static int istft_ready(const struct saan_stream_impl *im, int32_t n_frames) {
    if (im->out_pos >= SAAN_NFFT / 2 + n_frames * SAAN_HOP) return 0;
    if (im->fpush >= n_frames) return 1;   /* 全フレーム push 済みなら残りを出せる */
    const int32_t last = im->out_pos + SAAN_HOP - 1;
    return im->fpush > last / SAAN_HOP;
}

/* push の直後に呼ぶ: 出せる hop を obuf に積む（先頭 N/2 は捨てる）。
 * ⚠️ obuf の深さは SAAN_OBUF_HOPS で閉じている（導出は saanotts_stream.h）。超えるなら隣のバッファを
 *    黙って壊すので、書く前に止める。 */
static saan_status istft_flush_ready(saan_stream *st, struct saan_stream_impl *im) {
    while (istft_ready(im, st->n_frames)) {
        if (im->skip_hops > 0) {          /* 一括版が捨てる先頭 N/2 に相当 */
            istft_pop(im, im->obuf + (size_t)im->ofill * SAAN_HOP);
            --im->skip_hops;
            continue;                     /* ofill は増やさない = 捨てる */
        }
        if (im->ofill >= SAAN_OBUF_HOPS) return SAAN_ERR_ARENA;
        istft_pop(im, im->obuf + (size_t)im->ofill * SAAN_HOP);
        ++im->ofill;
    }
    return SAAN_OK;
}

#if SAAN_G_EXCITE
/* --- 倍音励起つきの G（候補 A。`-DSAAN_G_EXCITE=1`）------------------------------------------------
 *
 * 仕様の正は scratchpad f0g/stage1/EXCITE_SPEC.md（学習形）と f0g/devform/devform.py（端末形 = これと同じ計算）。
 * 学習形からの変更は 3 つだけ: (1) 倍音の和を閉形式 Σsin(kφ) = sin(Hφ/2)·sin((H+1)φ/2)/sin(φ/2) に
 * (2) 無声フレームの F0 を「直前の有声を保つ」因果の規則に (3) 雑音を xorshift32 の整数乱数に。
 * 位相は uint32（1 周 = 2^32）で持つ。
 *
 * 流れ（1 チャンク = CH フレーム、h = 幹の出力 [DEC_W][CH]）:
 *   exc_f0_chunk : F0 ヘッドの conv は k=3 なので、直前 2 フレームを持ち越して
 *                  フレーム [t-1, t+CH-2] の log F0 と v を出す（1 フレーム遅れる）。
 *   exc_put_y    : フレーム tt の G の出力 Y = mag·(cos + i·sin) を 4 枠のリングに積む（励起が出るまで待つ）。
 *   exc_drain    : フレーム tt の励起 X_tt の窓（サンプル 256tt-512 .. 256tt+511）は F0 / v をフレーム tt+2 まで
 *                  要る。その F0 が出たフレームから順に e を作り、順 rFFT → Y との複素積 → istft_ola。
 * 発話の端: 先頭は e を反射（e[-j] = e[j]、torch.stft center=True と同じ）、末尾も反射（e[L-1+j] = e[L-1-j]）。
 * 最後のフレームの先読みは最後の値を保つ。 */
static const float *g_f0w;   /* 試験用の上書き（NULL = blob から） */
size_t saan_f0head_floats(void) { return (size_t)EXC_H * DEC_W * 3 + EXC_H + 2 * EXC_H + 2; }
void saan_stream_set_f0head(const float *w) { g_f0w = w; }
void saan_stream_exc_counts(const saan_stream *st, int32_t *n_voiced, int32_t *n_f0) {
    const struct saan_stream_impl *im = (const struct saan_stream_impl *)st->impl;
    *n_voiced = im->exc ? im->exc->n_voiced : 0;
    *n_f0 = im->exc ? im->exc->n_f0 : 0;
}

/* ビルド時の既定（任意）。全部そろったときだけ有効。一部だけだとコンパイルエラー */
#if defined(SAAN_F0_SCALE) || defined(SAAN_F0_CENTER_HZ) || defined(SAAN_F0_LO_HZ) || defined(SAAN_F0_HI_HZ)
#if !(defined(SAAN_F0_SCALE) && defined(SAAN_F0_CENTER_HZ) && defined(SAAN_F0_LO_HZ) && defined(SAAN_F0_HI_HZ))
#error "SAAN_F0_SCALE / SAAN_F0_CENTER_HZ / SAAN_F0_LO_HZ / SAAN_F0_HI_HZ は全部いっしょに渡すこと"
#endif
#define EXC_F0S_DEFAULT 1
#endif
#ifndef SAAN_F0_TAU_MS
#define SAAN_F0_TAU_MS 75.0
#endif

/* 検査して派生値を作る。不正なら何も変えずに SAAN_ERR_RANGE。
 * ⚠️ 派生値の計算は試験版（f0scale-trial.diff）と同じ式・同じ型（double の log / exp → float）にしてある
 *    = 試験版と PCM が bit 一致する */
static saan_status exc_f0shape_set(struct saan_exc *x, const saan_f0shape *p) {
    if (!(p->k >= 0.0) || !isfinite(p->k)) return SAAN_ERR_RANGE;
    if (p->k == 1.0) {                 /* 無効（元の 1 行を通す）。残りは検査しない */
        x->fs = *p; x->fs_on = 0;
        return SAAN_OK;
    }
    if (!(p->center_hz > 0.0) || !(p->lo_hz > 0.0) || !(p->lo_hz < p->center_hz) ||
        !(p->hi_hz > p->center_hz) || !(p->tau_ms > 0.0) ||
        !isfinite(p->center_hz) || !isfinite(p->hi_hz) || !isfinite(p->tau_ms)) return SAAN_ERR_RANGE;
    x->fs = *p;
    x->fs_lc = (float)log(p->center_hz);
    x->fs_hi = (float)log(p->hi_hz) - x->fs_lc;
    x->fs_lo = (float)log(p->lo_hz) - x->fs_lc;
    x->fs_alpha = 1.0f - (float)exp(-((double)SAAN_HOP / (double)SAAN_SR * 1000.0) / p->tau_ms);
    if (!x->fs_on) x->lp_init = 0;      /* 無効だった間 lp は更新していないので古い */
    x->fs_on = 1;
    return SAAN_OK;
}

static void exc_f0shape_default(struct saan_exc *x) {
#ifdef EXC_F0S_DEFAULT
    const saan_f0shape d = {(double)SAAN_F0_SCALE, (double)SAAN_F0_CENTER_HZ, (double)SAAN_F0_LO_HZ,
                            (double)SAAN_F0_HI_HZ, (double)SAAN_F0_TAU_MS};
#else
    const saan_f0shape d = {1.0, 0.0, 0.0, 0.0, (double)SAAN_F0_TAU_MS};
#endif
    x->fs_on = 0; x->lp_init = 0;
    (void)exc_f0shape_set(x, &d);       /* 既定が不正ならそのまま無効 */
}

static void exc_reset(struct saan_exc *x) {
    memset(x, 0, sizeof *x);
    x->known = -1; x->ymax = -1;
    x->rng = SAAN_EXC_SEED;
    x->ph = 0u;                         /* u = 0（位相の初期値に意味は無い。学習は乱数） */
    x->vnoise = SAAN_EXC_VNOISE;
    exc_f0shape_default(x);
}

saan_status saan_stream_set_vnoise(saan_stream *st, float v) {
    struct saan_stream_impl *im = (struct saan_stream_impl *)st->impl;
    if (!im || !im->exc) return SAAN_ERR_MISSING;       /* init の前 */
    if (!(v > 0.0f && v <= 1.0f)) return SAAN_ERR_RANGE;   /* NaN も弾く */
    im->exc->vnoise = v;
    return SAAN_OK;
}

saan_status saan_stream_get_vnoise(const saan_stream *st, float *v) {
    const struct saan_stream_impl *im = (const struct saan_stream_impl *)st->impl;
    if (!im || !im->exc || !v) return SAAN_ERR_MISSING;
    *v = im->exc->vnoise;
    return SAAN_OK;
}

saan_status saan_stream_set_f0shape(saan_stream *st, const saan_f0shape *p) {
    struct saan_stream_impl *im = (struct saan_stream_impl *)st->impl;
    if (!im || !im->exc) return SAAN_ERR_MISSING;       /* init の前 */
    if (!p) { exc_f0shape_default(im->exc); return SAAN_OK; }
    return exc_f0shape_set(im->exc, p);
}

saan_status saan_stream_get_f0shape(const saan_stream *st, saan_f0shape *p) {
    const struct saan_stream_impl *im = (const struct saan_stream_impl *)st->impl;
    if (!im || !im->exc || !p) return SAAN_ERR_MISSING;
    *p = im->exc->fs;
    return SAAN_OK;
}

/* F0 ヘッド。h [DEC_W][CH] はチャンクの先頭の絶対フレーム t */
static void exc_f0_chunk(struct saan_exc *x, const float *h, int32_t t, int32_t n_frames) {
    const int HW = CH + 2;
    for (int c = 0; c < DEC_W; ++c) {
        float *r = x->hx + (size_t)c * HW;
        r[0] = x->hp[c * 2]; r[1] = x->hp[c * 2 + 1];
        memcpy(r + 2, h + (size_t)c * CH, sizeof(float) * CH);
        x->hp[c * 2]     = h[(size_t)c * CH + CH - 2];
        x->hp[c * 2 + 1] = h[(size_t)c * CH + CH - 1];
    }
    const float *w1 = x->f0w1, *b1 = x->f0b1, *w2 = x->f0w2, *b2 = x->f0b2;
    for (int j = 0; j < CH; ++j) {
        const int32_t p = t - 1 + j;                 /* 中心フレーム。taps は hx の列 j, j+1, j+2 */
        if (p < 0 || p >= n_frames) continue;
        float hid[EXC_H];
        for (int o = 0; o < EXC_H; ++o) {
            const float *wo = w1 + (size_t)o * DEC_W * 3;
            const float *r = x->hx + j;
            float a0 = b1[o], a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
            int c = 0;
            for (; c + 4 <= DEC_W; c += 4) {
                const float *r0 = r + (size_t)c * HW, *r1 = r0 + HW, *r2 = r1 + HW, *r3 = r2 + HW;
                const float *w = wo + c * 3;
                a0 += w[0] * r0[0] + w[1] * r0[1] + w[2] * r0[2];
                a1 += w[3] * r1[0] + w[4] * r1[1] + w[5] * r1[2];
                a2 += w[6] * r2[0] + w[7] * r2[1] + w[8] * r2[2];
                a3 += w[9] * r3[0] + w[10] * r3[1] + w[11] * r3[2];
            }
            for (; c < DEC_W; ++c) {
                const float *rr = r + (size_t)c * HW, *w = wo + c * 3;
                a0 += w[0] * rr[0] + w[1] * rr[1] + w[2] * rr[2];
            }
            hid[o] = (a0 + a1) + (a2 + a3);
        }
        saan_gelu(hid, EXC_H);
        float o0 = b2[0], o1 = b2[1];
        for (int o = 0; o < EXC_H; ++o) { o0 += w2[o] * hid[o]; o1 += w2[EXC_H + o] * hid[o]; }
        if (x->fs_on) {
            /* 声の高さの整形（V4）。有声判定 o1 は変えない */
            const float raw = o0 + EXC_LOGREF;
            if (o1 > 0.0f) {
                if (!x->lp_init) { x->lp = raw; x->lp_init = 1; }
                else x->lp += x->fs_alpha * (raw - x->lp);
            }
            float u = (raw - x->fs_lc) + ((float)x->fs.k - 1.0f) * ((x->lp_init ? x->lp : raw) - x->fs_lc);
            u = u >= 0.0f ? x->fs_hi * (float)tanh((double)(u / x->fs_hi))
                          : x->fs_lo * (float)tanh((double)(u / x->fs_lo));
            x->lfr[p & (EXC_NF - 1)] = x->fs_lc + u;
        } else {
            x->lfr[p & (EXC_NF - 1)] = o0 + EXC_LOGREF;
        }
        x->vr[p & (EXC_NF - 1)] = (unsigned char)(o1 > 0.0f);
        x->known = p;
        x->n_f0 += 1; x->n_voiced += (o1 > 0.0f);
    }
}

/* Y = mag·(cos + i·sin) を積む */
static void exc_put_y(struct saan_exc *x, const float *mag, const float *cosv, const float *sinv,
                      int t, int stride, int32_t abs_frame) {
    float *yr = x->y + (size_t)(abs_frame & (EXC_NY - 1)) * 2 * NB, *yi = yr + NB;
    for (int k = 0; k < NB; ++k) {
        const float m = mag[(size_t)k * stride + t];
        yr[k] = m * cosv[(size_t)k * stride + t];
        yi[k] = m * sinv[(size_t)k * stride + t];
    }
    x->ymax = abs_frame;
}

/* 無声フレームの F0 を埋める（因果。直前の有声を保つ。発話頭は 0〜2 フレーム目の有声があればその値、無ければ 400 Hz）。
 * j まで（絶対フレーム）埋める。フレーム 0..2 の v / F0 は先に出ている */
static void exc_fill_upto(struct saan_exc *x, int32_t j, int32_t n_frames) {
    while (x->fill_next <= j) {
        const int32_t f = x->fill_next;
        const int s = f & (EXC_NF - 1);
        float v;
        if (x->vr[s]) { x->fcur = x->lfr[s]; x->have_cur = 1; v = x->fcur; }
        else if (x->have_cur) v = x->fcur;
        else {
            v = EXC_LOGREF;
            if (f < 2) {
                const int32_t gmax = n_frames - 1 < 2 ? n_frames - 1 : 2;
                for (int32_t g = f + 1; g <= gmax; ++g)
                    if (x->vr[g & (EXC_NF - 1)]) { v = x->lfr[g & (EXC_NF - 1)]; break; }
            }
        }
        x->ffl[s] = v;
        ++x->fill_next;
    }
}

/* sin の表引き。x は「1 周 = 2^33」の整数（mod 2^33 で使う）。上位 12 bit で引いて下位 21 bit で線形補間 */
static inline float exc_lut(uint64_t x) {
    x &= 0x1FFFFFFFFull;
    const uint32_t idx = (uint32_t)(x >> 22);
    const float fr = (float)(uint32_t)(x & 0x3FFFFFu) * (1.0f / 4194304.0f);
    const float t0 = saan_exc_sin_t[idx];
    return t0 + (saan_exc_sin_t[idx + 1] - t0) * fr;
}

/* e の count サンプル（ブロックの先頭から）。(a, va) = 左のフレームの (ln F, v)、(b, vb) = 右のフレーム。
 * F はブロックの中で ln F が線形 = 等比数列: F_i = exp(a)·r^i（r = exp((b-a)/256)）。サンプルごとの expf は無い。
 * 1/F も同じく等比で持ち、H = floor(11025·(1/F_i)) は割り算なし。倍音の和は閉形式で、3 つの sin は表引き。
 * ph / rng は呼び出し側の状態（先頭の反射の 512 番目のサンプルだけ、コピーで呼ぶ）。 */
static void exc_gen_run(uint32_t *phs, uint32_t *rngs, float vnoise, float a, float b, float va, float vb, int count, float *out) {
    const float d = (b - a) * (1.0f / 256.0f);
    /* 3 つの指数は double で 1 回ずつ（ブロックごと）。float の expf だと 1 ulp の差が r^255 で 3e-5 に伸び、位相が実装ごとにずれる */
    const float F0 = (float)exp((double)a), r = (float)exp((double)d), rinv = (float)exp(-(double)d);
    const float G0 = 1.0f / F0;
    float P = 1.0f, Q = 1.0f;
    uint32_t ph = *phs, s = *rngs;
    for (int i = 0; i < count; ++i) {
        const float w = (float)i * (1.0f / 256.0f);
        const float V = va * (1.0f - w) + vb * w;
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        const float nz = (float)((int)((s & 255u) + ((s >> 8) & 255u) + ((s >> 16) & 255u) + (s >> 24)) - 510)
                       * 0.00676587503f;                                 /* 1 / sqrt(4·(256²-1)/12) */
        const float F = F0 * P;
        P *= r;
#ifdef SAAN_EXC_TEST_PHASE_EXCL
        /* 陽性対照専用: 累積和が n 自身を含まない（1 サンプルずれる）。Python の端末形と比べて落ちなければならない */
        const uint32_t ph_use = ph;
        ph += (uint32_t)(F * 194783.094f + 0.5f);
#else
        ph += (uint32_t)(F * 194783.094f + 0.5f);                        /* 2^32 / 22050。1 周 = 2^32 */
        const uint32_t ph_use = ph;
#endif
        float e = ((vnoise * V + (1.0f - V)) * EXC_NOISE_SPEC) * nz;
        if (V != 0.0f) {
            const uint32_t Hi = (uint32_t)(11025.0f * (G0 * Q));     /* 正の数なので切り捨て = floor */
            const float H = (float)Hi;
            const uint64_t xa = (uint64_t)Hi * ph_use;
            const float sn = exc_lut(ph_use);
            float harm;
            if (fabsf(sn) < 1e-4f) {
                const float pw = (float)(int32_t)ph_use * 1.46291812e-9f;   /* 2π / 2^32。符号つき */
                harm = 0.5f * H * (H + 1.0f) * pw;
            } else {
                harm = exc_lut(xa) * exc_lut(xa + ph_use) / sn;
            }
            e += V * harm;
        }
        Q *= rinv;
        out[i] = e;
    }
    *phs = ph; *rngs = s;
}

static void exc_gen_block(struct saan_exc *x, int32_t b, int32_t n_frames) {
    const int32_t i1 = b + 1 < n_frames ? b + 1 : n_frames - 1;
    const int sa = b & (EXC_NF - 1), sb = i1 & (EXC_NF - 1);
    float *eb = x->eb + (size_t)(b & 3) * SAAN_HOP;
    exc_gen_run(&x->ph, &x->rng, x->vnoise, x->ffl[sa], x->ffl[sb], (float)x->vr[sa], (float)x->vr[sb], SAAN_HOP, eb);
    if (b * SAAN_HOP <= SAAN_NFFT / 2)
        for (int i = 0; i < SAAN_HOP; ++i)
            if (b * SAAN_HOP + i <= SAAN_NFFT / 2) x->ehead[b * SAAN_HOP + i] = eb[i];
    if (b == 1) {                      /* 先頭の反射が使う e[512]（フレーム 2 の値そのもの: w = 0） */
        uint32_t ph = x->ph, rng = x->rng;
        const int s2 = 2 & (EXC_NF - 1);
        exc_gen_run(&ph, &rng, x->vnoise, x->ffl[s2], x->ffl[s2], (float)x->vr[s2], (float)x->vr[s2], 1, &x->ehead[SAAN_NFFT / 2]);
    }
}

/* フレーム tt の励起 X_tt を作り、Y_tt との複素積を im->re / im->im に入れて istft_ola する */
static void exc_emit(saan_stream *st, struct saan_stream_impl *im, struct saan_exc *x, int32_t tt) {
    SAAN_PROF_BEGIN(SAAN_PROF_EXC);
    const int32_t L = st->n_frames;
    exc_fill_upto(x, tt + 2 < L ? tt + 2 : L - 1, L);
    const int32_t bmax = tt + 1 < L ? tt + 1 : L - 1;
    SAAN_PROF_BEGIN(SAAN_PROF_EXG);
    while (x->blk_next <= bmax) { exc_gen_block(x, x->blk_next, L); ++x->blk_next; }
    /* 窓 [256tt-512, 256tt+511] を反射つきで集める。window を掛けて frm に置く */
    float *xb = im->frm;
    const int32_t base = tt * SAAN_HOP - SAAN_NFFT / 2;
    const int32_t nend = L * SAAN_HOP;
    {
        const float *win = im->win;
        int m = 0;
        for (; m < SAAN_NFFT && base + m < 0; ++m) xb[m] = x->ehead[-(base + m)] * win[m];          /* 先頭の反射 */
        int mid_end = nend - base < SAAN_NFFT ? nend - base : SAAN_NFFT;
        while (m < mid_end) {                                    /* ブロックごとに連続で読む */
            const int32_t i = base + m;
            const float *src = x->eb + (size_t)((i >> 8) & 3) * SAAN_HOP;
            const int off = i & (SAAN_HOP - 1);
            int n = SAAN_HOP - off; if (n > mid_end - m) n = mid_end - m;
            for (int j = 0; j < n; ++j) xb[m + j] = src[off + j] * win[m + j];
            m += n;
        }
        for (; m < SAAN_NFFT; ++m) {                             /* 末尾の反射 */
            const int32_t i = 2 * (nend - 1) - (base + m);
            xb[m] = x->eb[(size_t)((i >> 8) & 3) * SAAN_HOP + (i & (SAAN_HOP - 1))] * win[m];
        }
    }
    SAAN_PROF_END(SAAN_PROF_EXG);
    SAAN_PROF_BEGIN(SAAN_PROF_EXF);
    saan_rfft_1024(xb, im->re, im->im);
    SAAN_PROF_END(SAAN_PROF_EXF);
    SAAN_PROF_BEGIN(SAAN_PROF_EXM);
    const float *yr = x->y + (size_t)(tt & (EXC_NY - 1)) * 2 * NB, *yi = yr + NB;
    for (int k = 0; k < NB; ++k) {
        const float xr = im->re[k], xi = im->im[k];
        im->re[k] = (yr[k] * xr - yi[k] * xi) * (1.0f / 256.0f);
        im->im[k] = (yr[k] * xi + yi[k] * xr) * (1.0f / 256.0f);
    }
    SAAN_PROF_END(SAAN_PROF_EXM);
    SAAN_PROF_END(SAAN_PROF_EXC);
    SAAN_PROF_BEGIN(SAAN_PROF_ISTFT);
    istft_ola(im, tt);
    SAAN_PROF_END(SAAN_PROF_ISTFT);
}

/* 出せるフレームを順に出す */
static saan_status exc_drain(saan_stream *st, struct saan_stream_impl *im, struct saan_exc *x) {
    const int32_t L = st->n_frames;
    while (x->next_x <= x->ymax) {
        const int32_t tt = x->next_x;
        const int32_t need = tt + 2 < L ? tt + 2 : L - 1;
        if (x->known < need) break;
        if (x->ymax - tt >= EXC_NY) return SAAN_ERR_ARENA;      /* Y のリングを越えた */
        exc_emit(st, im, x, tt);
        ++x->next_x;
        SAAN_TRY(istft_flush_ready(st, im));
    }
    return SAAN_OK;
}
#endif

/* --- 公開 API ------------------------------------------------------------- */

static saan_status stream_init_body(saan_stream *st, const saan_weights *w,
                                    saan_arena *a, const int32_t *ids,
                                    int32_t n_ids, float s_v, const float *bf) {
    memset(st, 0, sizeof *st);
    st->w = w; st->a = a; st->ids = ids; st->n_ids = n_ids; st->s_v = s_v;
    if (n_ids <= 0) return SAAN_ERR_SHAPE;

    st->log_d = (float *)saan_alloc(a, sizeof(float) * (size_t)n_ids);
    st->d_hat = (int32_t *)saan_alloc(a, sizeof(int32_t) * (size_t)n_ids);
    if (!st->log_d || !st->d_hat) return SAAN_ERR_ARENA;

    const size_t mark = a->used;
    saan_status s = saan_run_duration(w, a, ids, n_ids, st->log_d);
    if (s != SAAN_OK) return s;
    a->used = mark;

    int32_t T = 0;
    for (int i = 0; i < n_ids; ++i) {
        float v = roundf(s_v * expf(st->log_d[i]));
        if (v < SAAN_CLIP_LO) v = SAAN_CLIP_LO;
        if (v > SAAN_CLIP_HI) v = SAAN_CLIP_HI;
        st->d_hat[i] = (int32_t)v;
        T += st->d_hat[i];
    }
    st->n_frames = T;

    const int gdf = saan_stream_gd_flags(w, bf);
    /* impl と、要るときだけ gdx を**1 回の確保**で取る（gdx は ALIGN16(sizeof impl) の位置） */
    const size_t impl_sz = SAAN_ALIGN16(sizeof(struct saan_stream_impl));
    struct saan_stream_impl *im =
        (struct saan_stream_impl *)saan_alloc(a, gdf ? impl_sz + SAAN_ALIGN16(sizeof(struct saan_gdx))
                                                     : sizeof *im);
    if (!im) return SAAN_ERR_ARENA;
    memset(im, 0, gdf ? impl_sz + sizeof(struct saan_gdx) : sizeof *im);
    st->impl = im;
    SAAN_TRY(resolve_weights(st));   /* S1: 重みの検索はここで 1 回だけ */
    struct saan_gdx *gx = NULL;
    if (gdf) {
        im->gdx_off = (int32_t)impl_sz;
        gx = (struct saan_gdx *)((char *)im + impl_sz);
        gx->gd_on = gdf & 1;
        gx->tok_halo = TOK_HALO + (gx->gd_on ? SAAN_GD_RF : 0);
        gx->bf = bf;
        if (gx->gd_on) {
            for (int i = 0; i < SAAN_GD_N; ++i) {
                gx->gdw[i] = saan_w(w, "acoustic.g.convs.%d.weight", i);
                gx->gdb[i] = saan_tf(w, "acoustic.g.convs.%d.bias", i);
                if (!SAAN_W_OK(gx->gdw[i]) || !gx->gdb[i]) return SAAN_ERR_MISSING;
            }
        }
    }

    for (int i = 0; i < 5; ++i) if (!pipe_init(&im->ac[i], a, AC_W, 4)) return SAAN_ERR_ARENA;
    if (!pipe_init(&im->dinp, a, CD, 1)) return SAAN_ERR_ARENA;
    for (int i = 0; i < 5; ++i) if (!pipe_init(&im->dblk[i], a, DEC_W, 3)) return SAAN_ERR_ARENA;
    /* c のリング（T4）。旧 cdel 6 本（pad 1 / 3×5）の代わり。先頭はゼロパディング */
    im->cring = (float *)saan_alloc(a, sizeof(float) * (size_t)CD * CRING_W);
    if (!im->cring) return SAAN_ERR_ARENA;
    memset(im->cring, 0, sizeof(float) * (size_t)CD * CRING_W);
    im->cring_t0 = 0;

    /* ⚠️ **作業領域は段ごとの実寸で取る。** 以前は maxC(76) × maxW(16) を
     * 一律に確保していたが、acoustic は C=48/W=16、decoder は C=76/W=14 で、
     * 76×16 は**どちらにも要らない上限**だった。
     * S9 でさらに圧縮形になった（要る列だけ）。**saan_stream_arena_needed と 1:1**:
     *   w_full  max(AC_W × (W_AC − 4), DEC_W × CH)   c1 の出力 / dw の出力 + g / h
     *   w_c     CD × CH                              dec_step の c（リングから取り出す）
     *   w_e / w_r / w_g   E × CH / R × CH / DEC_W × CH（旧 W_DEC 幅から −9.4 KB）
     *   re / im / frm     w_e の中（T4 = MEM-2。確保しない） */
    const int W_AC = 2 * 4 + CH;    /* AcBlock の窓 */
    const size_t full_n = (size_t)AC_W * (W_AC - 4) > (size_t)DEC_W * CH
                        ? (size_t)AC_W * (W_AC - 4) : (size_t)DEC_W * CH;
    const int maxC = DEC_W > AC_W ? DEC_W : AC_W;
#if SAAN_MEM_PIPE_HALO
    /* MEM-6: 共用窓は「段ごとの C×W の最大」。**PIPE_WND_N と 1:1** */
    im->pwnd    = (float *)saan_alloc(a, sizeof(float) * pwnd_n_for(gdf));
    if (!im->pwnd) return SAAN_ERR_ARENA;
#endif
    im->w_full  = (float *)saan_alloc(a, sizeof(float) * full_n);
    im->w_c     = (float *)saan_alloc(a, sizeof(float) * (size_t)CD * CH);
    im->w_ch    = (float *)saan_alloc(a, sizeof(float) * (size_t)maxC * CH);
    im->w_ch2   = (float *)saan_alloc(a, sizeof(float) * (size_t)maxC * CH);
    im->w_e     = (float *)saan_alloc(a, sizeof(float) * (size_t)E * CH);
    im->w_r     = (float *)saan_alloc(a, sizeof(float) * (size_t)SAAN_DEC_R * CH);
    im->w_g     = (float *)saan_alloc(a, sizeof(float) * (size_t)DEC_W * CH);
#if SAAN_HEAD_EXT
    im->o1539   = (g_head_ext_n >= saan_stream_head_ext_bytes()) ? g_head_ext : NULL;
#else
    im->o1539   = (float *)saan_alloc(a, sizeof(float) * 1539 * HEAD_COLS);
#endif
    im->hr      = (float *)saan_alloc(a, sizeof(float) * (size_t)SAAN_DEC_HEAD * CH);
        im->ola     = (float *)saan_alloc(a, sizeof(float) * (size_t)OLA_LEN);
    im->olw     = (float *)saan_alloc(a, sizeof(float) * (size_t)OLA_LEN);
    im->win     = (float *)saan_alloc(a, sizeof(float) * SAAN_NFFT);
    /* re / im / frm は w_e と共用（生存期間の根拠は struct の注記）。w_e が NULL ならこれらも NULL */
    im->re      = im->w_e ? im->w_e + ISTFT_RE_OFF  : NULL;
    im->im      = im->w_e ? im->w_e + ISTFT_IM_OFF  : NULL;
    im->frm     = im->w_e ? im->w_e + ISTFT_FRM_OFF : NULL;

    /* ⚠️ **全部の確保を検査する。最後の 1 つだけでは足りない。**
     * saan_alloc は入らなければ NULL を返すが `used` を進めないので、
     * 途中で失敗しても**より小さい後続の確保は成功しうる**。
     * 以前ここが `if (!im->frm)` だけだったため、arena 175〜191 KB の
     * 15 サイズで **init が SAAN_OK を返したまま NULL を抱えて**
     * あとから落ちていた（検証で発覚）。 */
    if (!im->w_full || !im->w_c || !im->w_ch || !im->w_ch2 ||
        !im->w_e || !im->w_r || !im->w_g || !im->o1539 || !im->hr ||
        !im->ola || !im->olw || !im->win || !im->re || !im->im || !im->frm)
        return SAAN_ERR_ARENA;

    memset(im->ola, 0, sizeof(float) * (size_t)OLA_LEN);
    memset(im->olw, 0, sizeof(float) * (size_t)OLA_LEN);
    for (int i = 0; i < SAAN_NFFT; ++i)
        im->win[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)i / (float)SAAN_NFFT);
    im->fpush = 0;
    /* ⚠️ **out_pos は 0 から始める。** 一括版は `[N/2, ...)` を切り出すが、
     * リングでその先頭 N/2 サンプルを飛ばすと**その領域が pop されず
     * ゼロクリアもされない**ため、frame 3 の書き込みと衝突する（実測で
     * 出力サンプル 1025 以降が壊れた）。0 から pop して最初の
     * `N/2 / HOP` 回を捨てるのが正しい */
    im->out_pos = 0;
    im->skip_hops = SAAN_NFFT / 2 / SAAN_HOP;
    im->obuf = (float *)saan_alloc(a, sizeof(float) * (size_t)SAAN_OBUF_HOPS * SAAN_HOP);
    /* S6（T3）: token パイプ 3 段（frame 側の ac[] と同じ pipe_init。先頭はゼロパディング）と
     * 出力リング（saan_stream_arena_used と 1:1） */
    for (int i = 0; i < 3; ++i) if (!pipe_init(&im->tok[i], a, AC_W, TOK_PAD)) return SAAN_ERR_ARENA;
    im->tok_ring = (float *)saan_alloc(a, sizeof(float) * (size_t)TOK_G * AC_W * TOK_K);
    im->tok_pushed = 0;
    if (!im->obuf || !im->tok_ring) return SAAN_ERR_ARENA;
#if SAAN_G_EXCITE
    {
        if (st->n_frames < 3) return SAAN_ERR_SHAPE;      /* 先頭の反射が 512 サンプル先まで要る */
        im->exc = (struct saan_exc *)saan_alloc(a, sizeof(struct saan_exc));
        if (!im->exc) return SAAN_ERR_ARENA;
        exc_reset(im->exc);
        if (g_f0w) {                                       /* 試験用の上書き */
            im->exc->f0w1 = g_f0w;
            im->exc->f0b1 = im->exc->f0w1 + (size_t)EXC_H * DEC_W * 3;
            im->exc->f0w2 = im->exc->f0b1 + EXC_H;
            im->exc->f0b2 = im->exc->f0w2 + 2 * EXC_H;
        } else {
            /* ⚠️ blob に無ければ**止める**（黙って乱数や 0 にしない）。形も見る */
            static const char *const nm[4] = {"decoder.f0h.0.weight", "decoder.f0h.0.bias",
                                              "decoder.f0h.2.weight", "decoder.f0h.2.bias"};
            const uint64_t want[4] = {(uint64_t)EXC_H * DEC_W * 3 * 4, (uint64_t)EXC_H * 4, (uint64_t)2 * EXC_H * 4, 8};
            const float *got[4];
            for (int i = 0; i < 4; ++i) {
                uint32_t dt = 0; uint64_t nb = 0;
                const void *p = saan_tensor(w, nm[i], &dt, NULL, &nb);
                if (!p || dt != 0) return SAAN_ERR_MISSING;
                if (nb != want[i]) return SAAN_ERR_SHAPE;
                got[i] = (const float *)p;
            }
            im->exc->f0w1 = got[0]; im->exc->f0b1 = got[1]; im->exc->f0w2 = got[2]; im->exc->f0b2 = got[3];
        }
    }
#endif
    if (gx && gx->gd_on) {
        /* GD（`saan_stream_arena_used_ex` と 1:1）。先頭はゼロパディング / リングもゼロ */
        for (int i = 0; i < SAAN_GD_N; ++i)
            if (!pipe_init(&gx->gd[i], a, AC_W, gd_pad(i))) return SAAN_ERR_ARENA;
        gx->gd_h = (float *)saan_alloc(a, sizeof(float) * (size_t)AC_W * GD_HR);
        if (!gx->gd_h) return SAAN_ERR_ARENA;
        memset(gx->gd_h, 0, sizeof(float) * (size_t)AC_W * GD_HR);
    }
    im->ofill = 0;
    st->ofill_max = 0;
    st->peak_used = a->peak;
    return SAAN_OK;
}

saan_status saan_stream_init_bf(saan_stream *st, const saan_weights *w,
                                saan_arena *a, const int32_t *ids, int32_t n_ids,
                                float s_v, const float *bf) {
    SAAN_PROF_BEGIN(SAAN_PROF_INIT);
    const saan_status s = stream_init_body(st, w, a, ids, n_ids, s_v, bf);
    SAAN_PROF_END(SAAN_PROF_INIT);
    return s;
}

saan_status saan_stream_init(saan_stream *st, const saan_weights *w,
                             saan_arena *a, const int32_t *ids, int32_t n_ids,
                             float s_v) {
    return saan_stream_init_bf(st, w, a, ids, n_ids, s_v, NULL);
}

/* --- token レートのパイプ（S6 / T3）----------------------------------------
 *
 * ⚠️ **`tok_h` を発話全体で持つと ids に比例して RAM が増える**（実測 192 B/id、
 * 350 ids で 68 KB）。G1（200 KB）を満たすため、発話全体は持たない。
 * 旧 compute_tokens は「必要なトークン範囲 ± 12 を都度まるごと再計算」だった（TOK_HALO の
 * 説明を参照）。今はフレーム側と同じパイプで、1 回の advance が TOK_K トークンを 3 段通し、
 * 出力群をリングの (tok_pushed mod TOK_G) 番目に置く。
 *
 * 群 g（0 始まり）の入力はトークン [g·K, g·K + K)、出力は [g·K − TOK_HALO, g·K − TOK_HALO + K)。
 * 発話外の入力はゼロ（一括版では配列が無い = ゼロ）。ids は全部既知なので先読みに制約は無い。
 */
static saan_status tok_pipe_advance_body(saan_stream *st) {
    struct saan_stream_impl *im = (struct saan_stream_impl *)st->impl;
    const float *emb = im->emb;   /* init で解決済み（S1） */
    struct saan_gdx *gx = gdx_of(im);   /* NULL = GD も bf も無い（従来の経路） */
    const float *bfp = gx ? gx->bf : NULL;
    const int gd_on = gx && gx->gd_on;
    /* 段の入出力 [AC_W][TOK_K]。w_ch2 は make_hf の間は空いている（step_chunk_body は
     * make_hf の**後**で b = w_ch2 に ac_step の出力を書く）。in == out で回せる（acblk_step） */
    float *tmp = im->w_ch2;
    const int32_t g = im->tok_pushed;
    const int32_t i_in = g * TOK_K;

    for (int m = 0; m < TOK_K; ++m) {
        const int32_t i = i_in + m;
        if (i >= st->n_ids) {                  /* 発話外はゼロ（一括版と同じ） */
            for (int c = 0; c < AC_W; ++c) tmp[(size_t)c * TOK_K + m] = 0.0f;
            continue;
        }
        if (st->ids[i] < 0 || st->ids[i] >= SAAN_VOCAB) return SAAN_ERR_RANGE;
#ifdef SAAN_TEST_GD_STREAM_BF_SHIFT          /* 陽性対照専用: 加算ベクトルを 1 音素ずらす */
        const int32_t ib = i + 1 < st->n_ids ? i + 1 : i;
#else
        const int32_t ib = i;
#endif
        for (int c = 0; c < AC_W; ++c) {
            float v = emb[(size_t)st->ids[i] * AC_W + c];
            if (bfp) v += bfp[(size_t)ib * AC_W + c];
            tmp[(size_t)c * TOK_K + m] = v;
        }
    }

    /* 3 段。段を通るごとに TOK_PAD ずつ過去にずれる（frame 側の step_chunk_body と同じ簿記）。
     * 最終段の出力はリングの群 g の枠に直接書く（圧縮 [AC_W][TOK_K] がそのまま枠の形） */
    float *slot = im->tok_ring + (size_t)(g % TOK_G) * AC_W * TOK_K;
    int32_t t = i_in;
    for (int bi = 0; bi < 3; ++bi) {
        t -= im->tok[bi].pad;
        SAAN_TRY(acblk_step(st, &im->tok[bi], &im->tokw[bi], tmp,
                            bi == 2 && !gd_on ? slot : tmp, t, st->n_ids));
    }
    if (t != i_in - TOK_HALO) return SAAN_ERR_SHAPE;   /* 3 段 × TOK_PAD = TOK_HALO */
    if (gd_on) {
        /* h（token ブロック 3 段目の出力 [t, t + K)）を残差用リングの末尾に押し込む。
         * 押した直後の列 0 の時刻は t + K − GD_HR = t − 31 = 最終段の出力の先頭時刻 */
        for (int c = 0; c < AC_W; ++c) {
            float *row = gx->gd_h + (size_t)c * GD_HR;
            memmove(row, row + TOK_K, sizeof(float) * (size_t)(GD_HR - TOK_K));
            memcpy(row + GD_HR - TOK_K, tmp + (size_t)c * TOK_K, sizeof(float) * TOK_K);
        }
        for (int s5 = 0; s5 < SAAN_GD_N; ++s5) {
            t -= gd_pad(s5);
            pipe_t *p = &gx->gd[s5];
            float *pb = PIPE_OPEN(im, p, tmp);
            SAAN_TRY(saan_conv1d_wrd(tmp, pb, gx->gdw[s5], gx->gdb[s5], AC_W, AC_W, 3,
                                     gd_conv_dil(s5), p->W, p->pad, p->pad + TOK_K, st->a));
            PIPE_CLOSE(p, pb);
            if (s5 + 1 < SAAN_GD_N) saan_relu(tmp, (size_t)AC_W * TOK_K);   /* 最終段の後は relu 無し */
            zero_outside_n(tmp, AC_W, TOK_K, TOK_K, t, st->n_ids);          /* 発話外 = 一括版の配列の外 = 0 */
        }
        if (t != i_in - gx->tok_halo) return SAAN_ERR_SHAPE;                /* 12 + 31 */
        for (int c = 0; c < AC_W; ++c)
            for (int m = 0; m < TOK_K; ++m)
                slot[(size_t)c * TOK_K + m] = gx->gd_h[(size_t)c * GD_HR + m] + tmp[(size_t)c * TOK_K + m];
        zero_outside_n(slot, AC_W, TOK_K, TOK_K, t, st->n_ids);
    }
    im->tok_pushed = g + 1;
    return SAAN_OK;
}

static saan_status tok_pipe_advance(saan_stream *st) {
    SAAN_PROF_BEGIN(SAAN_PROF_TOKEN);
    const saan_status s = tok_pipe_advance_body(st);
    SAAN_PROF_END(SAAN_PROF_TOKEN);
    /* 要素 = 新しく計算した出力トークン数（旧: 再計算した窓の列数 i1 − i0 + 24） */
    SAAN_PROF_ADD(SAAN_PROF_TOKEN, (size_t)TOK_K);
    return s;
}

/* CH フレームぶんの `hf`（length regulator の出力）を作る。
 * 参照実装の `repeat_interleave` + 位置埋め込みと同じ。
 * `f0` は絶対フレーム番号。範囲外（発話末尾より後ろ）は**ゼロ**にする */
static saan_status make_hf_body(saan_stream *st, int32_t f0, float *out) {
    struct saan_stream_impl *im = (struct saan_stream_impl *)st->impl;
    const float *pos = im->pos;   /* init で解決済み（S1） */

    /* このチャンクが跨ぐトークン範囲を先に求める */
    int32_t tok_of[CH], within_of[CH];
    int32_t i0 = -1, i1 = -1;
    for (int k = 0; k < CH; ++k) {
        const int32_t f = f0 + k;
        if (f >= st->n_frames) { tok_of[k] = -1; continue; }
        int32_t acc = 0, i = 0;
        while (i < st->n_ids && acc + st->d_hat[i] <= f) { acc += st->d_hat[i]; ++i; }
        tok_of[k] = i;
        within_of[k] = f - acc;
        if (i0 < 0 || i < i0) i0 = i;
        if (i > i1) i1 = i;
    }
    if (i0 < 0) {                     /* 全部が発話外 */
        memset(out, 0, sizeof(float) * (size_t)AC_W * CH);
        return SAAN_OK;
    }
    /* S6: [i0, i1] が出そろうまで token パイプを進める。出そろい末尾（exclusive）は
     * tok_pushed·K − TOK_HALO。初回（i0 = 0）は 2 群（トークン −12〜3）を押すことになる */
    const struct saan_gdx *gxh = gdx_of(im);
    const int32_t tok_halo = gxh ? gxh->tok_halo : TOK_HALO;
    while (im->tok_pushed * TOK_K - tok_halo <= i1) SAAN_TRY(tok_pipe_advance(st));
    /* リングに残っている群は [tok_pushed − TOK_G, tok_pushed)。i0 がそれより古ければ
     * 設計（TOK_G の導出）が破れている。黙って別のトークンを読まない */
    if (i0 + tok_halo < (im->tok_pushed - TOK_G) * TOK_K) return SAAN_ERR_SHAPE;

    for (int k = 0; k < CH; ++k) {
        if (tok_of[k] < 0) {
            for (int c = 0; c < AC_W; ++c) out[(size_t)c * CH + k] = 0.0f;
            continue;
        }
        const int32_t u = tok_of[k] + tok_halo;       /* ≥ 0。群 u / K、列 u mod K */
        const float *slot = im->tok_ring + (size_t)((u / TOK_K) % TOK_G) * AC_W * TOK_K;
        const int col = (int)(u % TOK_K);
        const int pi = within_of[k] < SAAN_POS_MAX ? (int)within_of[k]
                                                   : SAAN_POS_MAX - 1;
        for (int c = 0; c < AC_W; ++c)
            out[(size_t)c * CH + k] =
                slot[(size_t)c * TOK_K + col] + pos[(size_t)pi * AC_W + c];
    }
    return SAAN_OK;
}

static saan_status make_hf(saan_stream *st, int32_t f0, float *out) {
    SAAN_PROF_BEGIN(SAAN_PROF_HF);
    const saan_status s = make_hf_body(st, f0, out);
    SAAN_PROF_END(SAAN_PROF_HF);
    return s;
}

/* パイプラインを CH フレーム進める。出力は `pcm`（CH * HOP サンプル） */
static saan_status step_chunk_body(saan_stream *st) {
    struct saan_stream_impl *im = (struct saan_stream_impl *)st->impl;
    const saan_wref ow = im->ow, hdw = im->hdw, how = im->how;   /* init で解決済み（S1） */
    const float *hdb = im->hdb, *hob = im->hob;

    float *a = im->w_ch, *b = im->w_ch2;
    /* 今回入力するフレームの先頭時刻。**段を通るごとに pad ぶん過去にずれる** */
    int32_t t = st->pushed;
    {
        /* ⚠️ make_hf の中で token パイプが w_ch2（= b）と w_full を作業領域に借りる（S6）。
         *    どちらもここから先で上書きされるので、make_hf は **b に触る前**に呼ぶこと */
        saan_status sh = make_hf(st, st->pushed, a);
        if (sh != SAAN_OK) return sh;
    }
    st->pushed += CH;

    for (int i = 0; i < 5; ++i) {
        t -= im->ac[i].pad;
        saan_status s = ac_step(st, i, a, b, t);
        if (s != SAAN_OK) return s;
        float *tmp = a; a = b; b = tmp;
    }
    /* acoustic.out は 1x1（bias 無し）。CH フレームに直接掛ける */
    float *c_cur = b;
    SAAN_TRY(saan_conv1d_w(c_cur, a, ow, NULL, AC_W, CD, 1, CH, st->a));

    if (st->dbg_c) {          /* デバッグ: c を絶対時刻で控える */
        for (int m = 0; m < CH; ++m) {
            const int32_t tt = t + m;   /* t は ac 5 段を通った後の出力時刻 */
            if (tt < 0 || tt >= st->dbg_cap) continue;
            for (int ch = 0; ch < CD; ++ch)
                st->dbg_c[(size_t)ch * st->dbg_cap + tt] = c_cur[(size_t)ch * CH + m];
        }
    }

    /* c をリングに 1 回だけ押し込む（T4。旧: dinp と 5 段がそれぞれ cdel に push）。
     * t はこの時点で c_cur の先頭の絶対時刻 */
    cring_push(im, c_cur, t);

    float *h = im->w_full;      /* [DEC_W][CH] として使い回す */
    t -= im->dinp.pad;
    saan_status s = dec_inp_step(st, c_cur, h, t);
    if (s != SAAN_OK) return s;

    /* 段ごとに h を進める。**c は毎段とも元の入力**（参照実装どおり）で、各段が自分の出力時刻の
     * 位置をリングから読む。
     * ⚠️ h は **in-place**（h_in == h_out == w_full。T4 = MEM-2 (c)）。以前は static h_tmp / c_tmp
     *    （3,712 B .bss）に出して memcpy で戻していた。in-place が安全な根拠（dec_step_body の順序）:
     *    先頭の pipe_push が h_in を窓に写した後、h_in は読まれない（dw も残差も p->buf を読む）。
     *    w_full は dw の出力 + g として使われ、pw1 が読み終わった時点で死ぬ。pw2 が h_out（= w_full）
     *    を書くのはその後で、pw2 の入力は w_e。残差は p->buf と h_out だけを読む。 */
    for (int i = 0; i < 5; ++i) {
        t -= im->dblk[i].pad;
        s = dec_step(st, i, h, h, t);
        if (s != SAAN_OK) return s;
    }

    SAAN_PROF_BEGIN(SAAN_PROF_HEAD);
#if SAAN_DEC_OC
    SAAN_TRY(saan_conv1d_wr_oc(im->hr, h, hdw, hdb, DEC_W, SAAN_DEC_HEAD, 1, CH, 0, CH, st->a));
    saan_gelu_oc(im->hr, SAAN_DEC_HEAD, CH);
#else
    SAAN_TRY(saan_conv1d_w(im->hr, h, hdw, hdb, DEC_W, SAAN_DEC_HEAD, 1, CH, st->a));
    saan_gelu(im->hr, (size_t)SAAN_DEC_HEAD * CH);
#endif

    /* ⚠️ **既定では hout を CH フレームまとめて計算する。** 1 フレームずつにすると
     * `o1539` が 49,248 B → 6,160 B に減る（`-DSAAN_MEM_HEAD_PF=1`）。
     * ⚠️ **ここに「全体が 35% 遅くなる（0.023 → 0.031 × RT）」と書いてあったが、
     *    それは fp32 時代の**ホスト**の値だった**（[C-055](../docs/decisions.md#c-055):
     *    ホストの時間は実機の内訳を予測しない）。**実機（CoreS3 / W8A8+PIE）で
     *    測り直した値は M-139 §6 にある（粒度 4 で xRT 0.483 / 粒度 1 で 0.700）。** */
    SAAN_PROF_END(SAAN_PROF_HEAD);

    /* HEAD_COLS 列ずつ hout を計算して iSTFT に流す。**既定は HEAD_COLS == CH = 1 群**で、
     * そのとき `saan_conv1d_wr(..., 0, CH, ...)` は `saan_conv1d_w(..., CH, ...)` の
     * 定義そのもの（saanotts_internal.h）なので、既定の経路は 1 命令も変わらない。 */
#if SAAN_G_EXCITE
    /* F0 ヘッド（h = 幹の出力）→ このチャンクで確定した F0 / v を足し、出せる X を出す */
    {
        SAAN_PROF_BEGIN(SAAN_PROF_F0H);
        exc_f0_chunk(im->exc, h, t, st->n_frames);
        SAAN_PROF_END(SAAN_PROF_F0H);
        SAAN_TRY(exc_drain(st, im, im->exc));
    }
#endif
    const float *mag = im->o1539;
    const float *cosv = im->o1539 + (size_t)513 * HEAD_COLS;
    const float *sinv = im->o1539 + (size_t)1026 * HEAD_COLS;
    for (int g0 = 0; g0 < CH; g0 += HEAD_COLS) {
    /* 群のフレームが全部「発話の外」なら hout を計算しない。出力は捨てられるので
     * 値は変わらない（下の m のループが同じ条件で continue する）。
     * ⚠️ HEAD_COLS == CH（既定）では 1 群しか無いので、効くのはプリロール中の
     *    t < 0 の step だけ。粒度を下げると効く群が増える。 */
    if (t + g0 + HEAD_COLS - 1 < 0 || t + g0 >= st->n_frames) continue;
    SAAN_PROF_BEGIN(SAAN_PROF_HEAD);
    SAAN_TRY(saan_conv1d_wr_oc(im->o1539, im->hr, how, hob, SAAN_DEC_HEAD, 1539, 1,
                               CH, g0, g0 + HEAD_COLS, st->a));
    SAAN_PROF_END(SAAN_PROF_HEAD);
    for (int m = g0; m < g0 + HEAD_COLS; ++m) {
        const int32_t tt = t + m;          /* decoder 最終段の出力の絶対時刻 */
        if (tt < 0 || tt >= st->n_frames) continue;   /* 発話に存在しない */
#if SAAN_G_EXCITE
        exc_put_y(im->exc, mag, cosv, sinv, m - g0, HEAD_COLS, tt);
        SAAN_TRY(exc_drain(st, im, im->exc));
#else
        istft_push(im, mag, cosv, sinv, m - g0, HEAD_COLS, tt);
        SAAN_TRY(istft_flush_ready(st, im));
#endif
    }
    }   /* HEAD_COLS 群 */
    return SAAN_OK;
}

static saan_status step_chunk(saan_stream *st) {
    SAAN_PROF_BEGIN(SAAN_PROF_STEP);
    const saan_status s = step_chunk_body(st);
    SAAN_PROF_END(SAAN_PROF_STEP);
    return s;
}

/* 前回 `saan_stream_pull_ptr` が返した分を obuf から退ける（MEM-8）。
 * ⚠️ **コピー版と同じ場所で同じことをする。** 2 回書かない。 */
static void obuf_retire(saan_stream *st, struct saan_stream_impl *im) {
    const int32_t n = im->opend;
    if (n <= 0) return;
    if (im->ofill > n)
        memmove(im->obuf, im->obuf + (size_t)n * SAAN_HOP,
                sizeof(float) * (size_t)(im->ofill - n) * SAAN_HOP);
    im->ofill -= n;
    st->emitted += n;
    im->opend = 0;
}

/* MEM-8: **obuf の中を指して返す。** 呼び出し側の CH×HOP の float バッファ
 * （ESP32 の雛形では `g_chunk` = 8,192 B）が丸ごと要らなくなる。
 *
 * ⚠️ **返ったポインタは次に `pull` / `pull_ptr` / `free` を呼ぶまでだけ有効。**
 *    端末側の消費者は 2 つとも即座に int16 へ変換してコピーするので問題ない
 *    （`saan_audio_write_f32` / `saan_audio_preroll_push`）。**保持する呼び出し側は
 *    コピー版 `saan_stream_pull` を使うこと。**
 * ⚠️ **`st->emitted` は「返した分」を含まない** — 次の呼び出しの先頭で進む。
 *    発話の最後は n=0 が返る呼び出しで退けるので、ループを抜けた後の値は正しい。 */
saan_status saan_stream_pull_ptr(saan_stream *st, const float **pcm, int32_t *n_out) {
    struct saan_stream_impl *im = (struct saan_stream_impl *)st->impl;
    *n_out = 0;
    *pcm = NULL;
    obuf_retire(st, im);                                  /* 前回返した分をここで退ける */
    if (st->emitted >= st->n_frames) return SAAN_OK;      /* 発話の終わり */

    /* CH フレームぶん溜まるまでパイプラインを進める。
     * warmup（遅延 38 フレーム）の間は obuf に何も溜まらない。
     * ⚠️ **まだ出していないフレームが残っている間だけ step する**（T1）。
     *    iSTFT は `istft_ready` で n_frames hop ちょうどで止まるので、
     *    `emitted + ofill == n_frames` になったら obuf に全フレームが出そろっている。
     *    そこで ofill < CH でも回し続けると、末尾の pull だけ出力に 1 sample も
     *    寄与しない step_chunk が 3 回走る（106 frames の 1 文で 21 → 18 step。
     *    出力サンプル列は不変 = stream G2 が bit 一致で示す）。
     *    ⚠️ 陽性対照は `- 1` では**落ちない**（実測）。pull の境目では常に
     *    `emitted + ofill = 8m + 2`、`fpush = 8m + 4` で、fpush が n_frames に届く step が
     *    残りを**全部一度に**吐く（istft_ready）ため、境目に残る r = n_frames − (8m + 2) が
     *    3..10 フレームの範囲でしか壊れない。demo_ids.h の prefix 1..53（n_frames mod 8 の
     *    8 残差すべて + T=2 / T=5 の短い発話）を一括版と memcmp した実測（fp32 / W8A8）:
     *      `- 1`  0/53 落ちない
     *      `- 3`  5/53: n_frames ≡ 5 (mod 8) の 4/4 **と** T=2（ループが一度も回らず出力 0）
     *      `- 8`  40/53: demo（106 ≡ 2）は 98 で切れるが **≡ 3, 4 (mod 8) は素通り**（r が 9〜10）
     *      `- 10` 53/53（= `- (CH+2)`。全残差で落ちる唯一の値）
     *    「1 フレーム早く」は検出できず `- 8` も 2 残差を見逃すので、条件を触るなら `- (CH+2)` で
     *    確かめること（`make stream` の多文 G2 は ≡ 3, 4 の文を含む = T2a）。 */
    while (im->ofill < CH && st->emitted + im->ofill < st->n_frames) {
        saan_status s = step_chunk(st);
        if (s != SAAN_OK) return s;
        /* ⚠️ `used` ではなく `peak` を見る。W8A8 の activation 作業領域は
         * conv の中で確保して**すぐ返す**ので、`used` では捕まらない */
        if (st->a->peak > st->peak_used) st->peak_used = st->a->peak;
        if (im->ofill > st->ofill_max) st->ofill_max = im->ofill;
        /* 入力を出し切ってなお足りないなら打ち切る（無限ループ防止）。
         * ⚠️ **真に必要な余剰は遅延 SAAN_LATENCY + iSTFT の 2 フレームだけ。**
         * 以前は `+ 4*CH + 16` の安全マージンを積んでいたが、それは
         * **48 フレーム（5 チャンク）ぶん出力に寄与しない純粋な無駄**で、
         * 外しても PCM は bit 一致する（short で −18.6%、D-3b の照合）。
         * チャンク境界で切り上がるぶんだけ余裕を持たせる */
        if (st->pushed > st->n_frames + SAAN_LATENCY + 2 + 2 * CH) break;
    }

    int32_t n = im->ofill < CH ? im->ofill : CH;
    if (st->emitted + n > st->n_frames) n = st->n_frames - st->emitted;
    *pcm = im->obuf;
    *n_out = n;
    im->opend = n;          /* 退けるのは次の呼び出し（呼び出し側が読み終わってから） */
    return SAAN_OK;
}

/* コピー版。**中身は pull_ptr に 1 本化してある**（同じ計算の 2 つ目の実装を持たない
 * = [C-103](../docs/decisions.md#c-103)）。保持したい呼び出し側・ホストのゲート・
 * wasm・Arduino はこちらを使う。 */
saan_status saan_stream_pull(saan_stream *st, float *pcm, int32_t *n_out) {
    const float *p = NULL;
    SAAN_TRY(saan_stream_pull_ptr(st, &p, n_out));
    if (*n_out > 0) {
        memcpy(pcm, p, sizeof(float) * (size_t)*n_out * SAAN_HOP);
        /* コピー版は「読み終わった」ので即座に退ける（ポインタを外に出さない） */
        obuf_retire(st, (struct saan_stream_impl *)st->impl);
    }
    return SAAN_OK;
}

#if !SAAN_G_EXCITE
/* 声の高さの整形は倍音励起つきの G の機能。励起の無いビルドでは何もしない */
saan_status saan_stream_set_f0shape(saan_stream *st, const saan_f0shape *p) { (void)st; (void)p; return SAAN_ERR_MISSING; }
saan_status saan_stream_get_f0shape(const saan_stream *st, saan_f0shape *p) { (void)st; (void)p; return SAAN_ERR_MISSING; }
saan_status saan_stream_set_vnoise(saan_stream *st, float v) { (void)st; (void)v; return SAAN_ERR_MISSING; }
saan_status saan_stream_get_vnoise(const saan_stream *st, float *v) { (void)st; (void)v; return SAAN_ERR_MISSING; }
#endif

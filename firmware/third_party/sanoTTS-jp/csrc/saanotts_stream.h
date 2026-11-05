/* ストリーミング推論（Phase D-2 / D-029）
 *
 * **目的: ESP32-S3 の SRAM (512 KB) に載せる。**
 * 一括版（`saan_synthesize`）は発話全体をバッファして 1.26 MB 使う。
 *
 * 方式: **ステート保持型**。各 conv 層が左コンテキスト (k-1) フレームを保持し、
 * チャンク間で繋ぐ。**ハローの再計算がゼロ**（再計算方式は同じメモリで計算 10 倍）。
 *
 * ⚠️ **代償はレイテンシ。** 出力は受容野ぶん遅れる:
 *   acoustic frame ±20 + decoder ±16 = **36 フレーム = 0.42 秒**
 *
 * 使い方:
 *   saan_stream_init(&st, &w, arena, &ids, n_ids, s_v);
 *   while (saan_stream_pull(&st, pcm, &n) == SAAN_OK && n > 0) { i2s_write(pcm, n); }
 *
 * ⚠️ **一括版と bit 一致することが受け入れ条件**（D-029 の G2）。
 * SNR では不可 — チャンク境界で 1 フレーム落としても 20 dB 出てしまう。
 */
#ifndef SAANOTTS_STREAM_H
#define SAANOTTS_STREAM_H

#include "saanotts.h"

/* 1 回に出すフレーム数。メモリと呼び出し回数の折衷（D-029）。
 * ⚠️ 最適化していない。実機で測ってから調整する */
#define SAAN_CHUNK 8

/* --- 倍音励起つきの G（候補 A。`-DSAAN_G_EXCITE=1`。**既定 0 = 従来の経路、1 命令も変わらない**）-----
 * G の幹の出力 h から F0 ヘッド（Conv1d 76→32 k3 → GELU → Conv1d 32→2）が log(F0/400) と有声の logit を出し、
 * F0 から作った倍音の励起スペクトル X（時間領域で e を作り、フレームごとに順 rFFT 1024）を、
 * G の複素ヘッドの出力 mag·(cos + i·sin) に掛けてから iSTFT する。F0 / v は 2 フレーム先まで要るので、
 * ストリームは 3 フレーム遅れる（conv の先読み 1 + X の窓 2）。F0 ヘッドの重みは blob の
 * `decoder.f0h.*`（fp32。scripts/export_c_weights.py が書く）から引き、無ければ init が SAAN_ERR_MISSING で止まる。
 * `saan_stream_set_f0head` はホストの試験用（blob 外の重みを渡す）。 */
#ifndef SAAN_G_EXCITE
#define SAAN_G_EXCITE 0
#endif

/* 受容野（実測 / D-029）。⚠️ **旧記述の acoustic ±10 は c1 だけ数えた誤り** */
#define SAAN_HALO_AC   20   /* (c1 k=5 + c2 k=5) × 5 段 */
#define SAAN_HALO_DEC  16   /* inp k=3 + dw k=7 × 5 段 */

typedef struct {
    const saan_weights *w;
    saan_arena *a;

    const int32_t *ids;
    int32_t n_ids;
    int32_t n_frames;      /* Σ d_hat */
    int32_t *d_hat;
    float *log_d;

    void *impl;            /* 内部状態（パイプ段と作業領域） */

    int32_t pushed;        /* acoustic に入れたフレーム数（ゼロ埋め含む） */
    int32_t emitted;       /* 呼び出し側に返したフレーム数 */

    float s_v;
    size_t peak_used;      /* arena の高水位（G1 の測定用） */
    int32_t ofill_max;     /* pull ごとの obuf 充填の最大（hop）。SAAN_OBUF_HOPS の検証用 */

    /* デバッグ: 非 NULL なら c-line を絶対時刻で書き込む（[CDIM][dbg_cap]）。
     * **段ごとに切り分けるため**。本番では NULL */
    float *dbg_c;
    int32_t dbg_cap;
} saan_stream;

/* パイプライン全体の遅延（フレーム）。**受容野の合計**（D-029）:
 * AcBlock pad=4 × 5 段 + decoder inp pad=1 + dw pad=3 × 5 段 = 36 */
#define SAAN_LATENCY (4 * 5 + 1 + 3 * 5)

/* 出力の詰め替えバッファ `obuf` の深さ（hop 数）= **pull ごとの ofill の最大値**。
 *
 *   SAAN_OBUF_HOPS = 2·CH − (SAAN_LATENCY mod CH)      （CH=8: 16 − 4 = 12 = CH+4）
 *
 * ofill は定常では 1 step で CH 増えて pull で CH 減るが、末尾の step は
 * 「fpush が n_frames に届いた瞬間に残りを全部吐く」（istft_ready）ので、
 * 発話の長さ次第で 1 step に CH を超える hop が積み上がる。実測（stream_test の
 * G2 多文、held-out 24 文、CH=8）: n_frames ≡ 4 (mod 8) の文で **12**、≡ 3 で 11、
 * それ以外は 10。上限 12 は 2·CH − (SAAN_LATENCY mod CH) に一致する。
 * ⚠️ **(CH+2) では足りない**（審査 2026-09-03）: ≡ 3・4 (mod 8) の文（held-out 24 文中 7 文）で
 *    1〜2 KB を隣のバッファに書き、1 文の G2（demo は 106 ≡ 2）でも QEMU checksum でも
 *    検出できない（陽性対照で確認: (CH+2) にするとその 7 文だけがコアのガードで落ち、
 *    1 文の G2 は通ったまま）。stream_test の
 *    G2 多文で「超えない **かつ** ちょうど届く」ことまで assert し、コアも
 *    書く前に `ofill >= SAAN_OBUF_HOPS` を見て SAAN_ERR_ARENA で止める。
 * ⚠️ CH を変えたらこの式で動く（CH=16 なら 32 − 4 = 28。S7 の注記）。式は CH=8 の
 *    実測に合っているが、CH=16 での 28 は**未実測**（S7 で G2 多文を回して確かめること） */
#if SAAN_G_EXCITE
/* 倍音励起つき: 12 + 7 = 19 hop。実測（f0g/devform/sweep.py: ids 9..137 個の 129 文、T mod 8 の 8 残差すべて）の ofill_max が
 * T≡0..7 で 16 17 18 19 15 15 15 15。最大は T≡3 の 19。導出はしていない（遅延が 3 フレーム増えたぶんの閉じた式は未）。 */
#ifndef SAAN_EXC_OBUF_EXTRA
#define SAAN_EXC_OBUF_EXTRA 7
#endif
#define SAAN_OBUF_HOPS (2 * SAAN_CHUNK - (SAAN_LATENCY % SAAN_CHUNK) + SAAN_EXC_OBUF_EXTRA)
#else
#define SAAN_OBUF_HOPS (2 * SAAN_CHUNK - (SAAN_LATENCY % SAAN_CHUNK))
#endif

/* 初期化。duration を走らせて d_hat と n_frames を確定させる */
saan_status saan_stream_init(saan_stream *st, const saan_weights *w,
                             saan_arena *a, const int32_t *ids, int32_t n_ids,
                             float s_v);

/* 音素ごとの 48 次元の加算ベクトル `bf` [n_ids][SAAN_AC_W]（NULL なら加算しない）つきの初期化。
 * `bf` は**呼び出し側が発話が終わるまで保持する**（コピーしない）。意味は `saan_synthesize_bf` と同じ。
 * `saan_stream_init` は NULL を渡す薄いラッパ。
 * GD（blob に `acoustic.g.*` が在る）かどうかは blob が決める（`saan_has_gd`）。 */
saan_status saan_stream_init_bf(saan_stream *st, const saan_weights *w,
                                saan_arena *a, const int32_t *ids, int32_t n_ids,
                                float s_v, const float *bf);

/* 次のチャンクを取り出す。`*n_out` が 0 になったら発話の終わり。
 * `pcm` は少なくとも SAAN_CHUNK * SAAN_HOP サンプルぶん必要 */
saan_status saan_stream_pull(saan_stream *st, float *pcm, int32_t *n_out);

/* MEM-8: **コピーせず出力リングの中を指して返す。**
 *
 * **なぜ在るか。** 呼び出し側の `float [SAAN_CHUNK * SAAN_HOP]`（**8,192 B**）は
 * `obuf` からの `memcpy` 先にしか使っていなかった（`step_chunk` は引数を
 * 受け取るだけで一度も触っていなかった）。ポインタで返せば**その 8,192 B が丸ごと要らない**。
 * ESP32 の雛形では `.bss` の `g_chunk` がこれで消える（[M-145](../docs/measurements.md#m-145)）。
 *
 * ⚠️ **返ったポインタは次に `pull` / `pull_ptr` を呼ぶまでだけ有効**（そこで詰め直す）。
 *    端末の消費者は 2 つとも即座に int16 へ変換してコピーするので問題ない。
 *    **保持したい呼び出し側はコピー版を使うこと。**
 * ⚠️ **`st->emitted` は返した分をまだ含まない**（次の呼び出しの先頭で進む）。
 *    n=0 が返る最後の呼び出しで退けるので、ループを抜けた後の値は正しい。
 * ⚠️ **サンプル列はコピー版と bit 一致する**（`make -C csrc pullptr` の G-PP1〜3）。 */
saan_status saan_stream_pull_ptr(saan_stream *st, const float **pcm, int32_t *n_out);

/* この発話で必要な arena のバイト数（**発話長に依存しない部分**と
 * ids に比例する部分の合計）。G3 の確認に使う */
size_t saan_stream_arena_needed(int32_t n_ids);

/* `saan_stream_init` が **成功したときの `a->used` の値そのもの**（T2 / S9 で導入）。
 * init が確保する全バッファ（log_d / d_hat / impl / パイプ / 作業領域 / iSTFT / obuf / token）の
 * ALIGN16 和で、duration の一時領域（init の中で返す）は含まない。
 *
 * 用途は雛形の二重防御（esp32/main/main.c）: init が SAAN_OK を返した後に `a.used` がこれと
 * **一致しなければ**確保が黙って抜けている。以前は定数 SAAN_ARENA_USED_FLOOR（ホストで測った
 * 値）を置いていたが、`sizeof(struct saan_stream_impl)` がポインタ幅で変わる（ホスト 64 bit /
 * Xtensa 32 bit で 768 B 違う。T2 の QEMU で実際に「a.used が 179,296 B しかない」と拒否された）ので、
 * 各ターゲットで自分の sizeof から計算する関数にした。
 * ⚠️ **確保の一覧を 2 回書いている**（stream_init_body とこの関数）。片方だけ変えると
 *    `make -C csrc arena` の §5（実測 a.used との bit 一致、陽性対照つき）が落ちる。 */
size_t saan_stream_arena_used(int32_t n_ids);

/* init の途中も含めた**本当のピーク**
 * = max(duration フェーズ, `saan_stream_arena_used()` + conv の act 作業領域)。
 *
 * ⚠️ **arena を詰めるときに見るのはこちら**（[C-100](../docs/decisions.md#c-100)）。
 *    `saan_stream_arena_used()` は init 後の値で、**2 つを含まない**:
 *      (1) init の途中で duration net が取って返す一時領域
 *          （⚠️ **MEM-7 の前は 350 ids で 134,400 B。いまは窓分割したので `SAAN_DUR_K` で
 *          決まる定数に近い** = K=128 で最大 63,840 B。[M-144](../docs/measurements.md#m-144)）
 *      (2) **conv が arena の上に取ってすぐ返す activation 作業領域**（W8A8 で 2,464 B）
 *          — ⚠️ **これは `arena_peak` にも入っていなかった**（[C-102](../docs/decisions.md#c-102)）。
 *          MEM-7 の前は (1) が大きくて偶然に安全側だったので出なかった。
 * ⚠️ **実機のログで確かめるなら「結果」ブロックの「arena 高水位（発話後）」を見る。**
 *    init 直後に出る `peak` は **pull を 1 度も通っていない**ので (2) を含まない。
 * ⚠️ **`make -C csrc dur` の G-DUR4 が 6 点 × 2 レーンで実測の `a.peak` と突き合わせる。**
 *    ⚠️ **1 点だけで突き合わせないこと** — 長さで支配項が入れ替わると、
 *    支配していない方の誤りが隠れる（C-102 がまさにそれ）。 */
size_t saan_stream_arena_peak(int32_t n_ids);

/* GD つきの値。`gd` は構成のビット（`saan_stream_gd_flags(w, bf)` で作る）:
 * 1 = blob に GD（acoustic.g.*）が在る / 2 = bf だけ渡す / 0 = どちらも無し（上の 3 関数と同じ値）。
 * ⚠️ **GD の blob を載せるなら必ずこちらを使うこと** — 上の関数は GD 無しの量を返すので、
 * arena を詰めた構成では init が SAAN_ERR_ARENA になる（`make -C csrc gd` の G-GD5）。
 * ⚠️ GD も bf も使わない構成では、arena は GD 追加前と 1 バイトも変わらない（`gd = 0`。G-GD4）。 */
int saan_stream_gd_flags(const saan_weights *w, const float *bf);
size_t saan_stream_arena_used_ex(int32_t n_ids, int gd);
size_t saan_stream_arena_peak_ex(int32_t n_ids, int gd);
size_t saan_stream_arena_needed_ex(int32_t n_ids, int gd);

/* `-DSAAN_HEAD_EXT=1` のときだけ効く: hout の出力（`saan_stream_head_ext_bytes()` バイト、16 B 境界）を
 * arena の外の `buf` に置く。init より前に 1 回呼ぶ。`SAAN_HEAD_EXT=0`（既定）では何もしない。 */
size_t saan_stream_head_ext_bytes(void);
void saan_stream_set_head_ext(float *buf, size_t bytes);

/* `-DSAAN_G_EXCITE=1` のときだけ在る: F0 ヘッドの重み（fp32）。レイアウトは PyTorch の state_dict の並び:
 *   w1 [32][SAAN_DEC_W][3] / b1 [32] / w2 [2][32] / b2 [2]   （W=76 で 7,394 個）
 * 渡したポインタは発話が終わるまで保持する（コピーしない）。 */
#if SAAN_G_EXCITE
size_t saan_f0head_floats(void);
void saan_stream_set_f0head(const float *w);
/* 測定用: これまでに F0 ヘッドが出したフレーム数と、そのうち有声（logit > 0）のフレーム数 */
void saan_stream_exc_counts(const saan_stream *st, int32_t *n_voiced, int32_t *n_f0);
#endif

/* --- 声の高さの整形（F0 の幅を広げる。V4）-----------------------------------------------
 * 倍音励起つきの G（`SAAN_G_EXCITE=1`）で、F0 ヘッドが予測した ln F0 を声の中心のまわりで広げる。
 *   c = center_hz、raw = F0 ヘッドの ln F0、lp = 有声フレームだけで更新する因果の一次低域通過（最初の有声フレームで raw に初期化）
 *   u = (raw - ln c) + (k - 1)·(lp - ln c)
 *   hi = ln(hi_hz) - ln c、lo = ln(lo_hz) - ln c
 *   u ← u ≥ 0 ? hi·tanh(u/hi) : lo·tanh(u/lo)       （原点の傾き 1 で滑らかに飽和）
 *   ln F0 = ln c + u
 * 有声判定は変えない。lp の係数は 1 - exp(-(256/22050·1000 ms) / tau_ms)。
 *
 * ⚠️ **`k == 1.0` は「無効」で、そのとき元の 1 行（ln F0 = raw）を通す = PCM は 1 bit も変わらない。** 既定も無効。
 * ⚠️ **設定は 1 本のストリームの中の状態。** `saan_stream_init*` の後、最初の `saan_stream_pull*` の前に呼ぶ。
 *    発話ごとに init し直す呼び出し側は、**発話ごとに呼び直すこと**（init が既定に戻す）。
 *    pull の途中で呼んでも安全で、**次に F0 ヘッドが出すチャンクから**効く（すでに出たフレームは変わらない）。
 * ⚠️ **`SAAN_G_EXCITE=0` のビルドでは何もせず `SAAN_ERR_MISSING` を返す**（取得も同じ）。
 *    呼び出し側は戻り値を見ること。 */
typedef struct {
    double k;         /* 倍率（≥ 0）。1.0 = 無効。female2 は 1.5、man2 は 2.0（試聴で決めた値） */
    double center_hz; /* 声の中心 c（Hz, > 0）。k ≠ 1 のとき必須 */
    double lo_hz;     /* 下側の飽和 p1（Hz, 0 < lo < center） */
    double hi_hz;     /* 上側の飽和 p99（Hz, hi > center） */
    double tau_ms;    /* lp の時定数（ms, > 0）。75 */
} saan_f0shape;     /* ⚠️ double なのは設定値の丸めを呼び出し側に出さないため（log は double で取る）。毎フレームの計算は float */

/* 設定。`p == NULL` はビルド時の既定（`-DSAAN_F0_SCALE` ほか。無ければ無効）に戻す。
 * 戻り値: SAAN_OK / 値が不正なら SAAN_ERR_RANGE（何も変えない）/ 励起の無いビルドは SAAN_ERR_MISSING。
 * `k == 1.0` のときは center 以降を検査しない。 */
saan_status saan_stream_set_f0shape(saan_stream *st, const saan_f0shape *p);
/* 取得（今効いている値。無効のときは k = 1.0）。`SAAN_G_EXCITE=0` では SAAN_ERR_MISSING */
saan_status saan_stream_get_f0shape(const saan_stream *st, saan_f0shape *p);

/* 有声区間の雑音比（倍音励起の有声部に足す白色雑音の比。学習形と同じ値でなければならない）。
 * 既定は `SAAN_EXC_VNOISE`（0.03f。`-DSAAN_EXC_VNOISE=0.3f` で変えられる）。`saan_stream_init*` が既定に戻す。
 * 声ごとに学習時の値（spec.gExcite.noise）が違うので、**発話ごとに init の直後・最初の pull の前に呼ぶ**。
 * 戻り値: SAAN_OK / 0 < v <= 1 でなければ SAAN_ERR_RANGE（何も変えない）/ 励起の無いビルドは SAAN_ERR_MISSING。 */
saan_status saan_stream_set_vnoise(saan_stream *st, float v);
saan_status saan_stream_get_vnoise(const saan_stream *st, float *v);

/* ビルド時の既定（任意）: `-DSAAN_F0_SCALE=1.5 -DSAAN_F0_CENTER_HZ=402.6174 -DSAAN_F0_LO_HZ=232.979
 * -DSAAN_F0_HI_HZ=661.401 [-DSAAN_F0_TAU_MS=75]` を**全部**渡すと、init 直後の設定がその値になる
 * （一部だけ渡すとコンパイルエラー）。渡さなければ無効。 */

#endif /* SAANOTTS_STREAM_H */

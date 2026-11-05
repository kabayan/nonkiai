/* 出力 PCM の変換と統計（float → int16 / FNV-1a / |max| / Σx² / クリップ数）。
 *
 * ⚠️ **ここが唯一の実装。** 音声出力の実装（saan_i2s.c = DevKit の I2S 直叩き /
 *    boards/m5unified/main/saan_audio_m5.cpp = M5.Speaker）はどちらも
 *    `saan_f32_to_i16()` を呼ぶだけで、変換も統計も自分では持たない。
 *    2 か所に書くと「片方だけ丸めが違う」形で checksum の突き合わせが黙って無意味になる
 *    （csrc/saanotts_internal.h が「同じカーネルを 2 回書かない」と言うのと同じ理由）。
 *
 * ⚠️ **正規化しない。** 発話ごとに音量が変わると決定性が壊れる。クリップは数えて出す。
 *
 * --- checksum の読み方（移植の検証用）----------------------------------------
 * `saan_f32_to_i16()` を通った**すべての** int16 サンプルの FNV-1a 64
 * （リトルエンディアンの 2 バイトを順に食う）。プリロールも定常ループも同じ関数を
 * 通るので、**スピーカーに出た列そのもの**。
 * ⚠️ 「音が鳴った」は移植が正しい証拠にならない。QEMU の記録（M-62）と同じ値が出れば
 *    ターゲット上の全経路が bit 単位で一致していると言える。
 * ⚠️ **ホストとターゲットは bit 一致しない。それは正常**（float の丸めが違う）。
 *    そのときは |max| と Σx² の**大きさ**で「丸め差」か「経路が壊れている」かを分ける。
 *    bit 一致を主張してよいのは**同じターゲット上の 2 構成**を比べたときだけ。
 *
 * 純 C99。ESP-IDF に依存しないのでホスト stub でもそのままコンパイルされる。 */
#ifndef SAAN_PCM_H
#define SAAN_PCM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 軟リミッタ単体（純関数。統計は触らない）。|x| <= SAAN_SOFT_LIMIT（既定 0.8）は x をそのまま返す
 * （bit 一致）。超えたら sign(x)*(T+(1-T)*tanh((|x|-T)/(1-T)))。無効（0）なら常に x。
 * ⚠️ float のまま外へ出す経路（web の再生バッファ）はこれを通した値を使うこと。
 *    limit_count を数えるのは saan_f32_to_i16 だけ（同じ sample に両方を呼んでも 2 重にならない） */
float    saan_soft_limit(float x);

/* float → int16。|x| が SAAN_SOFT_LIMIT（既定 0.8）を超えたときだけ tanh の軟リミッタを通し、
 * lrintf(x * 32767) を飽和させ、統計を更新する。閾値以下は従来と bit 一致 */
int16_t  saan_f32_to_i16(float x);

uint32_t saan_pcm_clip_count(void);
/* 軟リミッタ（|x| > SAAN_SOFT_LIMIT、既定 0.8）がかかった sample 数。無効（0）なら常に 0 */
uint32_t saan_pcm_limit_count(void);
uint64_t saan_pcm_checksum(void);
uint32_t saan_pcm_samples(void);
int32_t  saan_pcm_absmax(void);
uint64_t saan_pcm_sqsum(void);

/* 統計を発話の頭に戻す。**対話モードで 2 発話目以降を測るのに要る。**
 * ⚠️ これが無いと 2 発話目の checksum が「1 + 2 発話目」になり、しかも値は出るので
 *    突き合わせて「合わない」と悩むまで気づけない。
 * ⚠️ 起動直後の 1 発話目は呼んでも呼ばなくても同じ値（初期値 = FNV-1a のオフセット基底）。
 *    **M-62 の記録値は変わらない。** */
void     saan_pcm_reset(void);

#ifdef __cplusplus
}
#endif
#endif /* SAAN_PCM_H */

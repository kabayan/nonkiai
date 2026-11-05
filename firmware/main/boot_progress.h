#pragma once
// 起動の進み具合（AtomS3R の画面のプログレスバー）。label は短い ASCII、pct は 0〜100。
#ifdef __cplusplus
extern "C" {
#endif
void nk_boot_voice_name(const char *text);
void nk_boot_progress(const char *label, int pct);
#ifdef __cplusplus
}
#endif

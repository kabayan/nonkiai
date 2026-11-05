/* hout を 2 コアで割る（DLS-205）。`-DSAAN_DUALCORE=1` のときだけビルドされる（既定は無効）。
 *
 * C コア（csrc/）は FreeRTOS を知らない。コアが公開する `saan_set_oc_par()` フックに、
 * ここで「別コアのタスクに後半を任せて合流する」実装を差し込む。 */
#pragma once

#include <stdbool.h>

/* 作業タスク（コア 1 に固定）を作り、フックを差し込む。合成を始める前に 1 回だけ呼ぶ。
 * 呼び出し側（合成タスク）はコア 0 に固定されていること。 */
bool saan_dualcore_start(void);

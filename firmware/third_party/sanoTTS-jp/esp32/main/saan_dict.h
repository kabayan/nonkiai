/* K-7: 辞書パーティションを mmap して K-1 の blob を開く。
 *
 * ⚠️ **`model` と同じ理由で 64 KB 境界が要る**（partitions_16mb.csv）。
 * ⚠️ 辞書は 12 MB 以上ある。**RAM には読み込まない** — flash から直接引く。
 */
#ifndef SAAN_DICT_H
#define SAAN_DICT_H

#include <stdbool.h>
#include "jdict.h"

/* dict パーティションを mmap して開く。失敗したら false。 */
bool saan_dict_open(jdict_t *d);
void saan_dict_close(void);

/* DLS-308: ユーザー辞書（パーティション `userdict`、subtype 0x42）を貼って開く。
 * 領域は先頭 64 B のヘッダ（"SUD1" / blob 長 u32 LE / SHA-256 32 B / 予約）+ K1D1 の blob。
 * 開けたら true。**失敗してもユーザー辞書だけを使わないだけ**で、理由をログに出して false を返す
 * （本辞書・漢字経路には影響しない）。main_d は開いていること。 */
bool saan_userdict_open(jdict_t *u, const jdict_t *main_d);

#endif /* SAAN_DICT_H */

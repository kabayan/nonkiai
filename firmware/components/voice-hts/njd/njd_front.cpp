/* ----------------------------------------------------------------- */
/* DLS Phase E 文脈正規化: NJD フロントアダプタ実装                    */
/*                                                                   */
/* 4パスは Open JTalk 1.11 (Modified BSD, Copyright (c) 2008-2016      */
/* Nagoya Institute of Technology) を upstream/ に無改変 vendoring。    */
/* 出典・ライセンスは ./LICENSE.open_jtalk / ./UPSTREAM.md を参照。     */
/* 本ファイルは規則を一切持たず、正準なパス順で upstream 関数を呼ぶ。  */
/* ----------------------------------------------------------------- */
#include "njd_front.h"

#include "njd_set_accent_phrase.h" /* upstream */
#include "njd_set_accent_type.h"   /* upstream */
#include "njd_set_unvoiced_vowel.h"/* upstream */
#include "njd_set_long_vowel.h"    /* upstream */

void njd_front_normalize_context(NJD *njd)
{
   /* pyopenjtalk run_frontend (openjtalk.pyx:216-219) と同順。
    * 順序は load-bearing: long_vowel は unvoiced_vowel の後でなければ
    * 無声化マーカーの位置がずれる。 */
   njd_set_accent_phrase(njd);
   njd_set_accent_type(njd);
   njd_set_unvoiced_vowel(njd);
   njd_set_long_vowel(njd);
}

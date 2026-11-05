#pragma once
// DLS-096 (supersedes DLS-092) + DLS-099 (play_text lookup): 145 文字 + ぢ系 5 alias
// v10/scripts/voice_char_map.py CHAR_MAP + HIRAGANA_DI_ALIASES から自動生成、 手動編集禁止
// 再生成: python3 v10/scripts/voice_char_map.py --emit-header > v10/src/atom-fw/include/voice_char_map_fw.h

#include <cstddef>

// VOICE_CHAR_KEYS[i] (utf8 NUL 終端) は VOICE_WAV_FILES[i] と 1:1 対応 (DLS-099)。
// play_text は input utf8 を先頭から longest-match スキャンして該当 wav を順次再生する。
static const char* const VOICE_CHAR_KEYS[] = {
    "あ",
    "い",
    "う",
    "え",
    "お",
    "か",
    "き",
    "く",
    "け",
    "こ",
    "さ",
    "し",
    "す",
    "せ",
    "そ",
    "た",
    "ち",
    "つ",
    "て",
    "と",
    "な",
    "に",
    "ぬ",
    "ね",
    "の",
    "は",
    "ひ",
    "ふ",
    "へ",
    "ほ",
    "ま",
    "み",
    "む",
    "め",
    "も",
    "や",
    "ゆ",
    "よ",
    "ら",
    "り",
    "る",
    "れ",
    "ろ",
    "わ",
    "を",
    "ん",
    "が",
    "ぎ",
    "ぐ",
    "げ",
    "ご",
    "ざ",
    "じ",
    "ず",
    "ぜ",
    "ぞ",
    "だ",
    "ぢ",
    "づ",
    "で",
    "ど",
    "ば",
    "び",
    "ぶ",
    "べ",
    "ぼ",
    "ぱ",
    "ぴ",
    "ぷ",
    "ぺ",
    "ぽ",
    "きゃ",
    "きゅ",
    "きょ",
    "しゃ",
    "しゅ",
    "しょ",
    "ちゃ",
    "ちゅ",
    "ちょ",
    "にゃ",
    "にゅ",
    "にょ",
    "ひゃ",
    "ひゅ",
    "ひょ",
    "みゃ",
    "みゅ",
    "みょ",
    "りゃ",
    "りゅ",
    "りょ",
    "ぎゃ",
    "ぎゅ",
    "ぎょ",
    "じゃ",
    "じゅ",
    "じょ",
    "びゃ",
    "びゅ",
    "びょ",
    "ぴゃ",
    "ぴゅ",
    "ぴょ",
    "ゃ",
    "ゅ",
    "ょ",
    "っ",
    "ー",
    "0",
    "1",
    "2",
    "3",
    "4",
    "5",
    "6",
    "7",
    "8",
    "9",
    "A",
    "B",
    "C",
    "D",
    "E",
    "F",
    "G",
    "H",
    "I",
    "J",
    "K",
    "L",
    "M",
    "N",
    "O",
    "P",
    "Q",
    "R",
    "S",
    "T",
    "U",
    "V",
    "W",
    "X",
    "Y",
    "Z",
    "ぢ",  // (alias)
    "づ",  // (alias)
    "ぢゃ",  // (alias)
    "ぢゅ",  // (alias)
    "ぢょ",  // (alias)
};

static const char* const VOICE_WAV_FILES[] = {
    "/voice/hiragana_a.wav",  // 'あ'
    "/voice/hiragana_i.wav",  // 'い'
    "/voice/hiragana_u.wav",  // 'う'
    "/voice/hiragana_e.wav",  // 'え'
    "/voice/hiragana_o.wav",  // 'お'
    "/voice/hiragana_ka.wav",  // 'か'
    "/voice/hiragana_ki.wav",  // 'き'
    "/voice/hiragana_ku.wav",  // 'く'
    "/voice/hiragana_ke.wav",  // 'け'
    "/voice/hiragana_ko.wav",  // 'こ'
    "/voice/hiragana_sa.wav",  // 'さ'
    "/voice/hiragana_shi.wav",  // 'し'
    "/voice/hiragana_su.wav",  // 'す'
    "/voice/hiragana_se.wav",  // 'せ'
    "/voice/hiragana_so.wav",  // 'そ'
    "/voice/hiragana_ta.wav",  // 'た'
    "/voice/hiragana_chi.wav",  // 'ち'
    "/voice/hiragana_tsu.wav",  // 'つ'
    "/voice/hiragana_te.wav",  // 'て'
    "/voice/hiragana_to.wav",  // 'と'
    "/voice/hiragana_na.wav",  // 'な'
    "/voice/hiragana_ni.wav",  // 'に'
    "/voice/hiragana_nu.wav",  // 'ぬ'
    "/voice/hiragana_ne.wav",  // 'ね'
    "/voice/hiragana_no.wav",  // 'の'
    "/voice/hiragana_ha.wav",  // 'は'
    "/voice/hiragana_hi.wav",  // 'ひ'
    "/voice/hiragana_fu.wav",  // 'ふ'
    "/voice/hiragana_he.wav",  // 'へ'
    "/voice/hiragana_ho.wav",  // 'ほ'
    "/voice/hiragana_ma.wav",  // 'ま'
    "/voice/hiragana_mi.wav",  // 'み'
    "/voice/hiragana_mu.wav",  // 'む'
    "/voice/hiragana_me.wav",  // 'め'
    "/voice/hiragana_mo.wav",  // 'も'
    "/voice/hiragana_ya.wav",  // 'や'
    "/voice/hiragana_yu.wav",  // 'ゆ'
    "/voice/hiragana_yo.wav",  // 'よ'
    "/voice/hiragana_ra.wav",  // 'ら'
    "/voice/hiragana_ri.wav",  // 'り'
    "/voice/hiragana_ru.wav",  // 'る'
    "/voice/hiragana_re.wav",  // 'れ'
    "/voice/hiragana_ro.wav",  // 'ろ'
    "/voice/hiragana_wa.wav",  // 'わ'
    "/voice/hiragana_wo.wav",  // 'を'
    "/voice/hiragana_n.wav",  // 'ん'
    "/voice/hiragana_ga.wav",  // 'が'
    "/voice/hiragana_gi.wav",  // 'ぎ'
    "/voice/hiragana_gu.wav",  // 'ぐ'
    "/voice/hiragana_ge.wav",  // 'げ'
    "/voice/hiragana_go.wav",  // 'ご'
    "/voice/hiragana_za.wav",  // 'ざ'
    "/voice/hiragana_ji.wav",  // 'じ'
    "/voice/hiragana_zu.wav",  // 'ず'
    "/voice/hiragana_ze.wav",  // 'ぜ'
    "/voice/hiragana_zo.wav",  // 'ぞ'
    "/voice/hiragana_da.wav",  // 'だ'
    "/voice/hiragana_ji.wav",  // 'ぢ'
    "/voice/hiragana_zu.wav",  // 'づ'
    "/voice/hiragana_de.wav",  // 'で'
    "/voice/hiragana_do.wav",  // 'ど'
    "/voice/hiragana_ba.wav",  // 'ば'
    "/voice/hiragana_bi.wav",  // 'び'
    "/voice/hiragana_bu.wav",  // 'ぶ'
    "/voice/hiragana_be.wav",  // 'べ'
    "/voice/hiragana_bo.wav",  // 'ぼ'
    "/voice/hiragana_pa.wav",  // 'ぱ'
    "/voice/hiragana_pi.wav",  // 'ぴ'
    "/voice/hiragana_pu.wav",  // 'ぷ'
    "/voice/hiragana_pe.wav",  // 'ぺ'
    "/voice/hiragana_po.wav",  // 'ぽ'
    "/voice/hiragana_kya.wav",  // 'きゃ'
    "/voice/hiragana_kyu.wav",  // 'きゅ'
    "/voice/hiragana_kyo.wav",  // 'きょ'
    "/voice/hiragana_sha.wav",  // 'しゃ'
    "/voice/hiragana_shu.wav",  // 'しゅ'
    "/voice/hiragana_sho.wav",  // 'しょ'
    "/voice/hiragana_cha.wav",  // 'ちゃ'
    "/voice/hiragana_chu.wav",  // 'ちゅ'
    "/voice/hiragana_cho.wav",  // 'ちょ'
    "/voice/hiragana_nya.wav",  // 'にゃ'
    "/voice/hiragana_nyu.wav",  // 'にゅ'
    "/voice/hiragana_nyo.wav",  // 'にょ'
    "/voice/hiragana_hya.wav",  // 'ひゃ'
    "/voice/hiragana_hyu.wav",  // 'ひゅ'
    "/voice/hiragana_hyo.wav",  // 'ひょ'
    "/voice/hiragana_mya.wav",  // 'みゃ'
    "/voice/hiragana_myu.wav",  // 'みゅ'
    "/voice/hiragana_myo.wav",  // 'みょ'
    "/voice/hiragana_rya.wav",  // 'りゃ'
    "/voice/hiragana_ryu.wav",  // 'りゅ'
    "/voice/hiragana_ryo.wav",  // 'りょ'
    "/voice/hiragana_gya.wav",  // 'ぎゃ'
    "/voice/hiragana_gyu.wav",  // 'ぎゅ'
    "/voice/hiragana_gyo.wav",  // 'ぎょ'
    "/voice/hiragana_ja.wav",  // 'じゃ'
    "/voice/hiragana_ju.wav",  // 'じゅ'
    "/voice/hiragana_jo.wav",  // 'じょ'
    "/voice/hiragana_bya.wav",  // 'びゃ'
    "/voice/hiragana_byu.wav",  // 'びゅ'
    "/voice/hiragana_byo.wav",  // 'びょ'
    "/voice/hiragana_pya.wav",  // 'ぴゃ'
    "/voice/hiragana_pyu.wav",  // 'ぴゅ'
    "/voice/hiragana_pyo.wav",  // 'ぴょ'
    "/voice/hiragana_ya_small.wav",  // 'ゃ'
    "/voice/hiragana_yu_small.wav",  // 'ゅ'
    "/voice/hiragana_yo_small.wav",  // 'ょ'
    "/voice/hiragana_tsu_small.wav",  // 'っ'
    "/voice/hiragana_long.wav",  // 'ー'
    "/voice/num_0.wav",  // '0'
    "/voice/num_1.wav",  // '1'
    "/voice/num_2.wav",  // '2'
    "/voice/num_3.wav",  // '3'
    "/voice/num_4.wav",  // '4'
    "/voice/num_5.wav",  // '5'
    "/voice/num_6.wav",  // '6'
    "/voice/num_7.wav",  // '7'
    "/voice/num_8.wav",  // '8'
    "/voice/num_9.wav",  // '9'
    "/voice/ank_A.wav",  // 'A'
    "/voice/ank_B.wav",  // 'B'
    "/voice/ank_C.wav",  // 'C'
    "/voice/ank_D.wav",  // 'D'
    "/voice/ank_E.wav",  // 'E'
    "/voice/ank_F.wav",  // 'F'
    "/voice/ank_G.wav",  // 'G'
    "/voice/ank_H.wav",  // 'H'
    "/voice/ank_I.wav",  // 'I'
    "/voice/ank_J.wav",  // 'J'
    "/voice/ank_K.wav",  // 'K'
    "/voice/ank_L.wav",  // 'L'
    "/voice/ank_M.wav",  // 'M'
    "/voice/ank_N.wav",  // 'N'
    "/voice/ank_O.wav",  // 'O'
    "/voice/ank_P.wav",  // 'P'
    "/voice/ank_Q.wav",  // 'Q'
    "/voice/ank_R.wav",  // 'R'
    "/voice/ank_S.wav",  // 'S'
    "/voice/ank_T.wav",  // 'T'
    "/voice/ank_U.wav",  // 'U'
    "/voice/ank_V.wav",  // 'V'
    "/voice/ank_W.wav",  // 'W'
    "/voice/ank_X.wav",  // 'X'
    "/voice/ank_Y.wav",  // 'Y'
    "/voice/ank_Z.wav",  // 'Z'
    "/voice/hiragana_ji.wav",  // 'ぢ' (alias)
    "/voice/hiragana_zu.wav",  // 'づ' (alias)
    "/voice/hiragana_ja.wav",  // 'ぢゃ' (alias)
    "/voice/hiragana_ju.wav",  // 'ぢゅ' (alias)
    "/voice/hiragana_jo.wav",  // 'ぢょ' (alias)
};

static const size_t VOICE_WAV_COUNT = 150;

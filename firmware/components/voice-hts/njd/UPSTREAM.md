# voice-hts/njd upstream provenance

DLS Phase E「文脈正規化」用に取り込んだ Open JTalk の NJD モジュール
(連結リスト + 文脈正規化4パス) の出自記録。

## 取得元
- パッケージ: pyopenjtalk 0.4.1 の sdist 内蔵 Open JTalk
  (`src/lib/open_jtalk/src/`)
- Open JTalk バージョン: **1.11** (jpcommon と同一コードベース)
- 取得日: 2026-06-24 (Phase E 文脈正規化)
- pyopenjtalk の `run_frontend` (`openjtalk.pyx:214-219`) が呼ぶ実体と同一コード。

## ライセンス
- **Modified BSD (3条項BSD)** — Copyright (c) 2008-2016 Nagoya Institute of Technology,
  Department of Computer Science. All rights reserved.
- 全文: `LICENSE.open_jtalk` (= 純正 `src/lib/open_jtalk/src/COPYING`、jpcommon と同一)
- 各 `.c/.h` 冒頭のライセンスヘッダも保持する。

## 配置 (`upstream/` = 無改変 pristine)
- `upstream/njd.h` … NJDNode / NJD 構造体・全アクセサ・I/O API 宣言
- `upstream/njd.c` … NJD ラッパー (push / load_from_fp / fprint / refresh / clear)
- `upstream/njd_node.c` … NJDNode のアクセサ・load・fprint
- `upstream/njd_set_digit.{c,h}` … 数字→漢数字読み変換 (DLS-232 CP4 追加)。連続数字ノード
  (pos_group1=数) を検出し string を漢数字化・位ノード (十/百/千/万/億) を rule から挿入・
  助数詞別の連濁/促音便/特殊マージ (四月→シガツ 等) を適用。桁の素の読み(pron)は合成せず、
  入力ノードが持つ基底カナを前提とする (供給は front_synth.h の tier1_digit/counter.tsv side-table)。
- `upstream/njd_set_digit_rule_utf_8.h` … 数詞/位/助数詞の読み規則表 (自己完結・NJD-CSV 定義)
- `upstream/njd_set_accent_phrase.{c,h}` … chain_flag (0/1) 設定 (品詞ベース Rule 01-18)
- `upstream/njd_set_accent_type.{c,h}` … chain_flag==1 時の核位置再計算 (chain_rule F/C/P*)
- `upstream/njd_set_unvoiced_vowel.{c,h}` … 母音無声化「’」マーカー挿入 (3モーラ先読み)
- `upstream/njd_set_long_vowel.{c,h}` … 長音「ー」処理 (無声化の後に走る)
- `upstream/njd_set_*_rule_utf_8.h` … 各パスの UTF-8 規則表

CHARSET は UTF-8 固定でビルドする (`-DCHARSET_UTF_8`)。各 `.c` は ascii/euc/sjis の
rule ヘッダも `#ifdef` で参照するが、UTF-8 ブランチのみ有効なので utf_8 のみ取り込む
(jpcommon と同方針)。md5 は `git` 履歴で追跡。

## パス適用順 (load-bearing)
`run_frontend` 実順 = pronunciation → digit → **accent_phrase → accent_type →
unvoiced_vowel → long_vowel**。本プロジェクトは pronunciation を Trie/Viterbi が担い、
**digit は raw-text 経路 (`front_synthesize_labels`) でのみ `njd_set_digit` を4パスの前に適用**
(DLS-232 CP4)、後半4パスを `njd_front_normalize_context()` で適用する。golden oracle ノードを
直接投入する経路 (frontend_parity C0/C1/C2) は digit を適用しない (二重変換回避)。

## 取り込まなかったもの
- `njd_set_pronunciation` … read→pron 変換。Trie が pron を直接保持するため不要。
- `mecab2njd` / mecab 辞書 … 形態素解析は Trie/Viterbi が代替。NJD 各パスは mecab に非依存
  (njd.h + 自パスの rule のみ) であることを include 走査で確認済。
- `text2mecab` **本体**は非取込だが、**半角→全角の数字関連サブセット** (ASCII 0-9→全角・
  数字隣接の . , % - の全角化) のみ `front_synth.h::normalize_fullwidth_digits` に再現した
  (DLS-232 CP4)。njd_set_digit の numeral_list は全角キーのみで ASCII を食えないため。
- `：/・` 等の記号→pause 正規化 (`njd_set_pronunciation` 由来) は未取込 = 既知残差 (CP4 と直交)。
- `njd_set_*_rule_ascii_* / *_euc_jp / *_shift_jis` … UTF-8 のみ使用。
- ビルドスクリプト … host CMake / PlatformIO で独自ビルド。

## 改変方針
- `upstream/` 配下は **無改変**。再取得時はそのまま上書きできる。
- 本プロジェクト固有の薄いアダプタ (`njd_front.{h,cpp}`) は `upstream/` の外に置き、
  正準パス順のみを定義する (規則は持たない)。

## 関連
- jpcommon (NJD → full-context label) の出自は `../jpcommon/UPSTREAM.md` を参照。
- 入力 NJD の CSV 形式は upstream `NJD_load_from_fp` / `NJD_fprint` が規定する
  (`string,pos,pos_group1..3,ctype,cform,orig,read,pron,acc/mora_size,chain_rule,chain_flag`)。

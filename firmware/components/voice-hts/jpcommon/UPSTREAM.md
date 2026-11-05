# voice-hts/jpcommon upstream provenance

DLS-174 HTS フロント整備 第1CP (JPCommon Lite host de-risk) 用に取り込んだ
Open JTalk の jpcommon / njd2jpcommon ソースの出自記録。

## 取得元
- パッケージ: pyopenjtalk 0.4.1 の sdist (`pyopenjtalk-0.4.1.tar.gz`) 内蔵 Open JTalk
- Open JTalk バージョン: **1.11** (`src/lib/open_jtalk/src/CMakeLists.txt` の `set(OPEN_JTALK_VERSION 1.11)`)
- 取得日: 2026-06-23 (DLS-174 フロント整備 第1CP)
- pyopenjtalk の合成経路 (`extract_fullcontext` / `make_label`) が使う実体と同一コードベース。

## ライセンス
- **Modified BSD (3条項BSD)** — Copyright (c) 2008-2016 Nagoya Institute of Technology, Department of Computer Science. All rights reserved.
- 全文: `LICENSE.open_jtalk` (= 純正 `src/lib/open_jtalk/src/COPYING`)
- BSD 著作権表示保持義務のため、各 `.c/.h` 冒頭のライセンスヘッダも保持する。

## 配置 (`upstream/` = 無改変 pristine)
- `upstream/jpcommon.h` … JPCommon / JPCommonNode / JPCommonLabel の公開構造体・API 宣言
- `upstream/jpcommon.c` … JPCommon ラッパー (push / make_label / clear)
- `upstream/jpcommon_node.c` … JPCommonNode のアクセサ
- `upstream/jpcommon_label.c` … **full-context label 生成本体** (push_word の木構築 + JPCommonLabel_make の A:〜K: 書式化)
- `upstream/jpcommon_rule_utf_8.h` … モーラ分割表 / 無声化表 / pos・ctype・cform 最終コード表
- `upstream/njd2jpcommon_rule_utf_8.h` … NJD の pos(4部品)・ctype・cform → JPCommon 中間文字列 変換表

いずれも md5 を `git` 履歴で追跡。CHARSET は UTF-8 固定でビルドする (`-DCHARSET_UTF_8`)。

## 取り込まなかったもの
- `njd2jpcommon.c` 本体 … `njd.h` (NJDNode 構造体 + 全 njd モジュール) に依存するため取り込まず、
  3つの変換関数 (`convert_pos` / `convert_ctype` / `convert_cform`) のみを `jpcommon_front.cpp` に
  移植 (BSD ヘッダ + 出自コメント保持)。njd モジュール全体 (~10ファイル) の取り込みを回避。
- `jpcommon_rule_ascii_*` / `*_euc_jp` / `*_shift_jis` … UTF-8 のみ使用。
- ビルドスクリプト (CMakeLists.txt 等) … 本プロジェクトの host CMake / PlatformIO で独自ビルド。

## 改変方針 (DLS-174)
- `upstream/` 配下は **無改変**。再取得時はそのまま上書きできる。
- 本プロジェクト固有の薄いアダプタ (`jpcommon_front.{h,cpp}`) は `upstream/` の外に置く。
- 第1CP では exact 再現 (pyopenjtalk `extract_fullcontext` と文字列ビット一致) が目的。PDF が将来求める
  struct-Lite 軽量化 (メモリ最適化) は再現性証明に不要なため後段 (Phase G) で検討する。

## 関連
- HTS Engine (バック) の出自は `../UPSTREAM.md` を参照。
- 音響モデル `mei_normal.htsvoice` (CC-BY3.0) のライセンスは sdist の `LICENSE_mei_normal.htsvoice` を参照。

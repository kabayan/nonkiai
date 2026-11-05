# sanoTTS-jp の重みと声レコード

female2（既定）と man2 の bundle は model-builder の検証済み ZIP をバイト単位でそのままコピーしたもの。再梱包しない。各 ZIP の内容は `model-int8.bin`、`build-flags.json`、`README.txt`。重み `.bin` はその ZIP から抽出している。

### female2

原本 run: `voice-tail-female2-v2-excite-20261029`。表示名: `female2-v2-excite-bundle`。

| ファイル | SHA-256 |
|---|---|
| `female2-v2-excite-int8.bin` | `a4ca57246302c98d2c72c26f519b85953a3dab09a2f16aa7a27054a77c999eb7` |
| `female2-v2-excite-bundle.zip` | `e2e12fedfeac323e53b4f04b8cecd7295eaa96745ddb8099c68ea55b51b39535` |
| `female2-v2-excite-record.bin` | `932e5d91d103b53214bdb6629bc8c3166453c01b511ae1d6a8cebf2ef8c0d499` |

音声値: k=1.5, centerHz=402.6174, loHz=232.97919617, hiHz=661.40134277, tauMs=75, vnoise=0.03（記録時は float32）。重みは 1,476,992 B。

### man2

原本 run: `voice-tail-man2-v2-excite-full-20261029`。表示名: `man2-v2-excite-bundle`。

| ファイル | SHA-256 |
|---|---|
| `man2-v2-excite-int8.bin` | `a0d91a04266765258d0ee6fb9b4972c6ebaf419d9373c126f25e656941713b44` |
| `man2-v2-excite-bundle.zip` | `6f9a9c33f3e27265e73cdee127abf93046f6baee999b76ac5b21b9f1dcfa67ed` |
| `man2-v2-excite-record.bin` | `9af40a64daccd5527c437cdb73d70042a55cf73c95b5650ddf308492dc19710d` |

音声値: k=2.0, centerHz=186.22861, loHz=124.31815735, hiHz=305.44101379, tauMs=75, vnoise=0.03（記録時は float32）。重みは 1,476,992 B。

## レコードの生成と検証

リポジトリのルートで実行する。生成器と `--check` は Python 標準ライブラリのみを使う。検証テストには Node.js と C コンパイラ（`cc`）も必要:

```sh
python3 tools/make_voice_record.py
python3 tools/make_voice_record.py --check
python3 tools/test_voice_record.py
```

任意の bundle は `python3 tools/make_voice_record.py BUNDLE.zip --output RECORD.bin`。`--check` は一切書かず、不一致・欠落なら失敗する。生成は決定的。bundle 内のモデルサイズ・SHA を検証し、WebUI core と同じ f0Shape / vnoise の既定値・数値変換を使う。表示名は WebUI の手元のファイル選択と同じく、bundle ファイル名から最後の拡張子を除いた名前を使う。ASCII 1〜31 字でなければ NKVN を省略し、manifest の voice / label / run へはフォールバックしない。

レコードは 4096 B。先頭 96 B は NKVP v1（モデルサイズ・SHA、k / centerHz / loHz / hiHz / tauMs の float64、vnoise の float32、CRC32）、続く 40 B は任意の NKVN 表示名と CRC32。残りは 0xff。テストは同じ bundle を JS validateManifest / fileDisplayName / encodeVoiceSector に渡して全バイトを照合し、ファームの C nk_vp_decode / nk_vn_decode で全フィールドを検証する。

## ビルドと書き込み

`idf.py flash` は model（0x1d0000、0x170000 B）の重みと末尾セクタ（0x33f000）のレコード、dict、stt32w を書く。レコード位置は CMake がパーティションの offset + size - 4096 から計算する。重みは末尾セクタに侵入できない。

`SANO_MODEL_BIN` の `-int8.bin` を `-record.bin` に置換した既存ファイルを configure のたびに推定する。man2 に切り替えても female2 のレコードをキャッシュしない。`-DSANO_MODEL_RECORD_BIN=...` で明示指定、空値で無効化できる。レコード欠落時は警告する。

```sh
cd firmware
idf.py -DSANO_MODEL_BIN="$PWD/data/sanotts/man2-v2-excite-int8.bin" build
# 既定に戻す
idf.py -DSANO_MODEL_BIN="$PWD/data/sanotts/female2-v2-excite-int8.bin" build
```

ブラウザの `webui/model-flasher.html` からは各 `*-bundle.zip` を選ぶ。重みと音声値を一緒に書く。素の重みだけを手動で書くとレコードが古いままになるため、対応するレコードも末尾セクタへ書くこと。

モデルフラグは両方とも SAAN_DEC_HEAD=512、SAAN_G_EXCITE=1、SAAN_PHRASE_RULE_SBV2=1、SAAN_TAIL_PAUSE=1、SAAN_INT8_ACT=1。ファームとそろえる。

## 辞書とライセンス

`k1_dict_4mb.bin`: 3,013,752 B、dict（0x340000）。SHA-256 `b7bb311bd99d5a67e24ccd4c40e927f8bf4e13dc79ed786c7b67c865f6dc73ac`。NAIST-JDIC 派生、修正 BSD（`COPYING.naist-jdic`）。モデル・bundle・レコードは MIT（`LICENSE`）。

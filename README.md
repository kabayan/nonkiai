# nonkiai

[English](README.en.md) | 日本語

nonkiai（のんきちゃんai）は、M5Stack ATOMS3R（ESP32-S3）とボイスベースだけで、ぬいぐるみやおもちゃを声で遊べるようにするファームウェアです。「のんきちゃん」と呼ぶか本体をたたくと、言われたことの意図を聞きとり、声で返事をします。聞きとりも音声合成もマイコンの中で動き、ネットにはつながりません。

このリポジトリは、開発用のリポジトリから同期したスナップショットです。履歴は持ちません。

## 何を解決するか

おもちゃに AI の機能を足したい人のためのものです。ぬいぐるみやおもちゃに「話しかけたら返事をしてほしい」「声で動かしたい」と思っても、多くの方法はクラウドやスマホにつなぐ必要があります。nonkiai は、部品代 5,000 円以下の ATOMS3R とボイスベースだけで、呼びかけの検出・言われたことの意図の聞きとり・声での返事をマイコンの中で行います。

## 主な機能

- 聞きとりの始め方は 2 つです。本体をたたくか、「のんきちゃん」と呼びます。
- 言われた言葉を文字にせず、「何をしてほしいか」（意図）として聞きとります。意図は 17 種類です（前進・後退・右・左・名前・天気・なんじ・元気・ほめる・はい・いいえ・おはよう・おやすみ・上・下・停止・開始）。
- 認識モードは 2 つです。ぬいぐるみモードはおしゃべり、おもちゃモードは前進・後退・右・左を赤外線で送ります。
- 返事は端末の中の音声合成で話します。声のモデルは female2 と man2 の 2 つを同梱しています。声のモデルは 10 秒ほどの声の録音から作れます（学習のコードはこのリポジトリにはありません）。生成速度は約 0.7（話す時間より短い時間で作る）で、作りながら順に再生します。
- 赤外線の送り先は、内蔵の IR LED か、Grove につないだ外付けの IR ユニットから選べます。
- 画面は 2 つです（PC の Chrome / Edge）。設定の画面（Web Bluetooth）は名前・音量・認識モード・IR のテスト送信と、時刻と東京の天気を端末へ送ります。書き込みの画面（USB）はアプリ・声のモデル・辞書の書き換えと、全体の書き込みをします。

モデルの大きさは、すべて合わせて約 6 MB です（聞きとり 約 1.6 MB、音声合成 約 1.5 MB、読みの辞書 約 3.0 MB。いずれも INT8）。マイコンのフラッシュメモリに入っています。

## 使っている技術・必要な環境

| 項目 | 内容 |
|---|---|
| ハードウェア | M5Stack ATOMS3R（ESP32-S3）、Atomic Voice Base、USB-C ケーブル |
| ファームウェア | ESP-IDF 5.4.4（`firmware/dependencies.lock`）、M5Unified、LittleFS、esp-dsp |
| 聞きとり | stt32（ソースの写しと重みを同梱） |
| 音声合成 | sanoTTS-jp の写し（`firmware/third_party/sanoTTS-jp`）と、漢字の辞書（NAIST-JDIC の派生） |
| 画面 | HTML / JavaScript（Web Bluetooth・Web Serial、esptool-js） |
| 中継サーバ | Python 3（`tools/model_flasher_server.py`） |
| ブラウザ | Chrome / Edge |

天気は [Open-Meteo](https://open-meteo.com/) から、画面を開いたブラウザが取得します。API キーは要りません。

## デモ動画

準備中

## ビルド

必要なもの:

- M5Stack ATOMS3R と Atomic Voice Base、USB-C ケーブル
- ESP-IDF 5.4.4（版は `firmware/dependencies.lock` に固定されています）

ESP-IDF の `export.sh` を読み込んだシェルで実行します。

```sh
cd firmware
idf.py set-target esp32s3
idf.py build
```

ソースと重みはリポジトリに入っています。ビルドのときにネットから取ってくるのは、ESP-IDF の依存コンポーネントだけです。

## 実機への書き込み

### ビルドせずに書く（いちばん手軽）

ESP-IDF が無くても、ビルド済みの一式（`firmware/prebuilt/`）をそのまま書けます。中身は `idf.py flash` が書くものと同じです（ブートローダー・パーティション表・アプリ・LittleFS と、`firmware/data/` の声のモデル female2・声の値の記録・辞書・音声認識の重み）。どのソースのコミットからビルドしたかは `firmware/prebuilt/build-info.json` に書いてあります。

どちらの方法でも ATOMS3R の今の中身は上書きされ、元には戻せません。

**ブラウザで書く**（Python 3 と Chrome / Edge だけ）

1. リポジトリのルートで `python3 tools/model_flasher_server.py` を実行し、http://localhost:8095/ を開きます。
2. 「端末に USB で接続」で ATOMS3R のポートを選びます。
3. 「全体を書く（idf.py flash と同じ）」で「中継サーバの一式を取得」を押し、「全体を書く」を押します。

くわしくは下の「ブラウザから書き込む」を見てください。

**esptool で書く**（コマンド 1 行）

```sh
pip install esptool
cd firmware/prebuilt
esptool.py --chip esp32s3 -p <PORT> write_flash @flash_args
```

`<PORT>` は ATOMS3R のシリアルポートです（Linux なら `/dev/ttyACM0`、Windows なら `COM3` など）。esptool v5 では `esptool --chip esp32s3 -p <PORT> write-flash @flash_args` とも書けます。書き終わると ATOMS3R が再起動します。

### ファイルの場所

書き込むファイルは、リポジトリの中の次の場所にあります。

| ファイル | 中身 | 使う場面 |
|---|---|---|
| `firmware/data/sanotts/female2-v2-excite-int8.bin` | 声のモデル female2（既定） | `idf.py flash` |
| `firmware/data/sanotts/man2-v2-excite-int8.bin` | 声のモデル man2 | `idf.py flash`（`-DSANO_MODEL_BIN` で指定） |
| `firmware/data/sanotts/female2-v2-excite-record.bin` | female2 の声の値の記録（4 KB。`idf.py flash` が重みと一緒に書く） | `idf.py flash` |
| `firmware/data/sanotts/man2-v2-excite-record.bin` | man2 の声の値の記録 | `idf.py flash`（man2 を指定すると自動で選ばれる） |
| `firmware/data/sanotts/female2-v2-excite-bundle.zip` | female2 の重み・フラグ・声の値（上の `.bin` と同じ重み） | ブラウザから書き込む |
| `firmware/data/sanotts/man2-v2-excite-bundle.zip` | man2 の重み・フラグ・声の値（上の `.bin` と同じ重み） | ブラウザから書き込む |
| `firmware/data/sanotts/k1_dict_4mb.bin` | 漢字の辞書 | `idf.py flash`・ブラウザ |
| `firmware/data/stt32/weights_partition.bin` | 音声認識の重み | `idf.py flash` |
| `firmware/build/nonkiact2_sampler.bin` | アプリ（ビルドすると作られる） | `idf.py flash`・ブラウザ |
| `firmware/prebuilt/` | ビルド済みのブートローダー・パーティション表・アプリ・LittleFS と、書き込み先の一覧（`flash_args`・`flasher_args.json`） | ビルドせずに書く |

### USB で書き込む（初回）

```sh
idf.py -p <PORT> flash
```

`<PORT>` は ATOMS3R のシリアルポートです（Linux なら `/dev/ttyACM0` など）。初回はアプリ・声のモデル（female2）とその声の値の記録・辞書・音声認識の重み・空の LittleFS をまとめて書き込みます。2 回目以降でアプリだけを変えたときは、`idf.py -p <PORT> app-flash` で足ります。

声を man2 にするときは、ビルドのときに重みを指定します。同じ場所の `man2-v2-excite-record.bin` が自動で選ばれます。

```sh
idf.py -DSANO_MODEL_BIN="$PWD/data/sanotts/man2-v2-excite-int8.bin" build
idf.py -p <PORT> flash
```

### ブラウザから書き込む

PC の Chrome / Edge から USB（Web Serial）で ATOMS3R に書き込めます。ほかのサーバーやネットのサービスは要りません（画面が読み込む esptool-js だけはネットから取ります）。初めての ATOMS3R（出荷時のファームなど、別のファームが入っているもの）には「全体を書く」を、一度書いた ATOMS3R にはアプリ・声のモデル・辞書の書き換えを使います。

1. リポジトリのルートで次を実行し、http://localhost:8095/ を開きます。Web Serial が localhost でしか動かないので、手元で画面を配ります。

   ```sh
   python3 tools/model_flasher_server.py
   ```

2. 「端末に USB で接続」で ATOMS3R のポートを選びます。別のファームが入っていると「端末に nonkiact2 のパーティション表がありません」と出ますが、接続はそのまま残ります。下の「全体を書く」に進んでください。
3. 書くものを選びます。
   - 声のモデル: 「手元のモデル bundle（.zip）を選択」で、`firmware/data/sanotts/female2-v2-excite-bundle.zip` か `man2-v2-excite-bundle.zip` を選びます。素の `.bin` は選べません。bundle に入っているフラグを、画面が端末と照合します。
   - 辞書: `firmware/data/sanotts/k1_dict_4mb.bin`
   - アプリ: `firmware/build/nonkiact2_sampler.bin`（ビルドした場合）
4. 「照合」で違うものを確かめ、「書き込む」を押します。

書き込みが途中で切れたときは、もう一度つないで「照合」からやり直してください。アプリが起動しなくなったときは、下の「全体を書く」か `idf.py -p <PORT> flash` で書き直せます。

#### 全体を書く（`idf.py flash` と同じ一式）

画面は `firmware/build/flasher_args.json` が並べるファイルを、`idf.py flash` と同じアドレスに書きます。ビルドしていなければ（`firmware/build/` が無ければ）、ビルド済みの `firmware/prebuilt/flasher_args.json` を使います。

1. 接続したあと、「全体を書く（idf.py flash と同じ）」の「中継サーバの一式を取得」を押します。ブートローダー・パーティション表・アプリ・声のモデルと声の値の記録・辞書・音声認識の重み・LittleFS が一覧に出ます。
2. 「全体を書く」を押し、確認で OK を押します。全部で約 8 MB あるので数分かかります。書いたあとは、領域ごとに端末の中身を照合してから端末をリセットします。

今の中身は保存しないので、元のファームには戻せません。別のファームが入っていた ATOMS3R では、一式に含まれない設定の領域（nvs・phy_init・userdict）も空にします。nonkiact2 を書いたことのある ATOMS3R では、設定（nvs）とユーザー辞書（userdict）は残ります。

#### model-builder から声のモデルを取る（任意）

声のモデルを学習する model-builder を手元で動かしている場合は、「モデル」の欄の「model-builder の URL」に、その URL を入力します。「model-builder の一覧を取得」で一覧が出て、選んだ重みを取得できます。起動時に `--builder <URL>` を付けるか、`tools/model_builder_url.txt` に URL を 1 行書いておくと、それが欄の既定値になります。一度取った重みは `~/.cache/nonkiact2/model-flasher` に保存されます。次からは、同じものを取りに行きません（置き場は `--cache-dir` で変えられます）。model-builder が無ければ欄は空のままにして、手元の bundle（.zip）を選んでください。

## 設定の画面を開く

1. リポジトリのルートで `python3 tools/model_flasher_server.py` を実行します。
2. http://localhost:8095/index.html を Chrome / Edge で開き、「つなぐ」で ATOMS3R を選びます（Bluetooth）。

## ライセンス

ルートの `LICENSE`（MIT）がかかるのは、このリポジトリの自作のコードだけです。同梱物はそれぞれのライセンスに従います。

| 対象 | ライセンス |
|---|---|
| 自作のコード | MIT（`LICENSE`） |
| 声のモデル female2 / man2（`.bin` と bundle の `.zip`） | MIT（`firmware/data/sanotts/LICENSE`） |
| 音声認識 stt32 のソースの写しと重み（`firmware/third_party/stt32`・`firmware/data/stt32`） | Apache-2.0（Copyright 2026 adlib ltd.）。第三者の表記は `firmware/third_party/stt32/firmware/components/stt32_wakehubert/NOTICE` |
| 漢字の辞書 `k1_dict_4mb.bin` | NAIST-JDIC の派生（修正 BSD、`firmware/data/sanotts/COPYING.naist-jdic`） |
| sanoTTS-jp のソースの写し | `firmware/third_party/sanoTTS-jp` の `LICENSE` / `NOTICE.md` |
| 姿勢推定 x-io Fusion の写し | MIT（`firmware/third_party/Fusion/LICENSE.md`） |
| Open JTalk 由来のコード | 各 `COPYING` / `LICENSE.open_jtalk` |

声のモデル female2 / man2 の作り方は次のとおりです。

1. 参照音声と学習用の音声を、Irodori-TTS（MIT）で作りました。実在の人の録音は使っていません。
2. その音声で、sanoTTS-jp-2 を使って声のモデルを作りました。sanoTTS-jp-2 は、sanoTTS-jp を元に Style-Bert-VITS2 で学習したものです。

学習用の音声の文には、ROHAN4600（CC0-1.0）・ITA コーパス（パブリックドメイン / Unlicense）・AICA コーパス（CC0-1.0）・COEIROINK コーパス（パブリックドメイン / Unlicense）を使っています。COEIROINK コーパスを使ったのは man2 だけです。

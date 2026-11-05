# sanotts_embed

nonkiact2 の音声合成を HTS から sanoTTS-jp に置き換えるためのつなぎ（`sano_voice.h`）。

- sanoTTS-jp の C ファイルは `SANOTTS_DIR`（`firmware/CMakeLists.txt`）から読む。既定はリポジトリ内の写し `firmware/third_party/sanoTTS-jp`。
- 写したコミット: `387ef319b9a1a5afd5c8cdc83444dc2b5cec3b81`（sanoTTS-jp-2 の main）
- 必要なパーティション: `model`（type 0x40、重み blob）、`dict`（0x41、漢字辞書）、`userdict`（0x42、ユーザー辞書）。`firmware/partitions.csv` を見る。
- 重みは起動時に PSRAM へ写して使う（`sano_model_copy.c`）。
- 計測用の関数（G2P probe、分割上限の変更、xRT・アンダーランの計測）は載せていない。計測用は sano-measure の作業ツリーに残してある。

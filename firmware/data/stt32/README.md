# stt32 の生成済み重み

元: stt32 `feature/nonkiact2-integration`、ソースのコミット
`30d878c1da083ed8016a548e8a7936595de45bf0`。
重みは z（18 クラス、17 意図＋起動語 のんきちゃん、stt32 DLS-1073 / stt32 DLS-1082）。fw-eq 作業ツリーの
`generated/`（md5 `d754c19556912c118f4e54352f748837`）から写した。
生成物は upstream の git 管理外なので、このディレクトリに保存する。

| ファイル | サイズ (B) | SHA-256 |
|---|---:|---|
| 元 `firmware/components/stt32_wakehubert/generated/weights.bin` | 1,649,216 | `ef5f7442ace5d1feac9e01cdb09dfeac5878eaf3c0a4e8b40bbfce19350ca404` |
| 元と同じ `qlayout.h` | 4,827 | `505cd7b88dec4f990ffdb22bf8a09b277c808342bf42b430b0060fde7051cede` |
| `weights_partition.bin` | 1,649,232 | `744dfbe8d2b191cecbdddfe3bd35823a49a7a88d5b31bc14333c425f75f2b8a5` |

作り方（Python 標準ライブラリのみ。出力先は nonkiact2）:

```sh
python3 /path/to/stt32/tools/regen_wakehubert_weights.py --pack \
  /path/to/stt32/firmware/components/stt32_wakehubert/generated/weights.bin \
  firmware/data/stt32/weights_partition.bin
cp /path/to/stt32/firmware/components/stt32_wakehubert/generated/qlayout.h firmware/data/stt32/
```

イメージの先頭は 16 B（`WKH1`・little-endian u32 の blob 長・予約 8 B）、
その後が元の blob。`stt32w` は 0x630000 / 0x193000 B で、ヘッダ込みで収まる（残り 1,456 B）。
`idf.py flash` にはこのイメージを含め、`app-flash` では書かない。
ビルドは `STT32_WAKEHUBERT_EMBED_WEIGHTS=OFF`、生成物の参照先はこのディレクトリ。

## ライセンス

重み（`weights_partition.bin`・`qlayout.h`）は stt32 の出荷物で、Apache-2.0（Copyright 2026 adlib ltd.）。
上流の重み（wakehubert-mel-bigru・hubert-base-ls960、Apache-2.0）と蒸留データ（LibriSpeech・MSWC、CC BY 4.0）の表記は
`firmware/third_party/stt32/firmware/components/stt32_wakehubert/NOTICE` にある（stt32 DLS-1094）。

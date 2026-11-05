# stt32 の写し

stt32 `feature/nonkiact2-integration` の
`92885b3da29a7668680004cc029058f79d800a67` から、`FILES.txt` の 2 component（20 ファイル。ライセンス文 LICENSE（Apache-2.0）と第三者の表記 NOTICE を含む。補正の `Kconfig.projbuild`・`eq_fir_coeffs.h` を含む）
だけを写したもの。`host/`・診断アプリ・音声 HAL は含まない。
ここにある upstream ファイルを直接編集しない。

更新はリポジトリのルートで実行する（未コミットの変更は取り込まない）。

```sh
python3 tools/sync_stt32.py /path/to/stt32 92885b3da29a7668680004cc029058f79d800a67
```

生成物は `firmware/data/stt32/README.md` を参照。重みはアプリへ埋め込まず、
`stt32w` パーティションからラベルで読み取る（subtype は任意、ここでは data 0x43）。

# sanoTTS-jp（写し）

nonkiact2 のビルドが読む sanoTTS-jp-2 のファイルだけを写したもの。別のマシンで sanoTTS-jp-2 を clone しなくてもビルドできるようにするため（DLS-151）。

- 写した元: sanoTTS-jp-2（`git@github.com:kabayan/sanoTTS-jp-2.git`）のコミット `42e78bbe4e4e478e5f4110f7f8b77e201fb988b5`
- 写したファイル: ビルド（`ninja -t deps` と `compile_commands.json`）が実際に読んだソースとヘッダ、`esp32/components/saanotts_core/CMakeLists.txt`、ライセンス（`LICENSE`、`NOTICE.md`、`csrc/openjtalk/COPYING`・`PROVENANCE.md`）
- ディレクトリの形は元と同じ（`saanotts_core/CMakeLists.txt` が `../../../csrc` を相対で読むため）
- **ここを直接直さない。** 直すときは sanoTTS-jp-2 で直してから `python3 tools/sync_sanotts.py <sanoTTS-jp-2 の場所> <コミット>` で写し直す
- 手元の sanoTTS-jp-2 を直接使うときは `idf.py -DSANOTTS_DIR=<絶対パス> ...`

ライセンス: コードは MIT（`LICENSE`）。`csrc/openjtalk/` は Open JTalk の修正 BSD（`csrc/openjtalk/COPYING`）。

# word23 fixed recognizer

DLS-136 の端末内聞き取り用 component。stt32 commit
`e1624dec702e5cd26a09fe2b96510c5a6d6a90e3`（v0.41 の trim 経路）から
F2 特徴量、M2t exact-dequant 推論、S3 SIMD dot、energy trim をバイトコピーした。
stream 経路・I2S/audio HAL・BLE・旧判定 policy は含めない。
ファームウェアのビルドと実行は stt32 リポジトリに依存しない。

固定モデルは v0.4、元 `model_int8.npz` の SHA-256 は
`48bb5ff9af9ade3eac7895265a561995e7a5053ba54bf4fc79bcfc8b4cec8843`。
生成4ヘッダは Git 管理外の stt32 artifact
`artifacts/word23_v0_4_accel_20261001_01/projects/integration_stream_ring64/main/generated/`
からコピーした。tracked source は上記 commit の Git blob から取得した。
コピー元 path・SHA・tracked の区別は [upstream_manifest.json](upstream_manifest.json)
にも保存する。変更が必要な場合は元ファイルを直接変更せず、この移植版の来歴を新たに記録する。

## API と判定

[公開 API](word23_recognizer.h) の `w23_recognize(pcm, samples, &result)` は
16 kHz mono signed int16 PCM（400〜51,200 sample）を受け取る。PCM は呼び出し元が所有し、
処理中も書き換えない。ライブ入力は48,000 sample。結果には10クラスの softmax 確率、
speech_detected、trim 統計、各段の時間、arena の配置・サイズ、error を返す。
無音は成功した判定として true を返すが、scores は全0、feature/model は実行しない。
失敗は false と error、scores 全0を返す。

クラス順は `前進, 後退, 右, 左, 名前, 天気, なんじ, 元気, ほめる, 不明`。
`w23_decide(scores, speech, ok)` が返す intent は先頭9クラスの0〜8、
不採用は `W23_NONE=255`。不明以外の top1 が0.90以上なら採用する。
top1 が不明の場合だけ、top2 の `score2/(1-score不明)` が0.60以上なら採用する。
無音・失敗・それ以外は不採用。等確率なら固定クラス index の小さい順を保つ。
`w23_rank_scores` は診断用の raw1/raw2 を同じ順で返す。
コピーした `word23_runtime_config.h` の旧0.95は元artifactの来歴資料で、判定には使わない。

## 資源と算術

発話を検出してから arena を16-byte alignmentで一時確保する。内部RAMを優先し、
確保できなければ PSRAM へ切り替える。F2 と M2t の領域は共有するため、
必要量は両者の最大値。48,000 sample の arena は95,360 B、
M2t 単体は29,728 B。feature 10,880 B は別の一時heap領域に置き、
PSRAM を優先し内部RAMへ fallbackする。成功・失敗とも呼び出し終了時に解放する。
無音ではどちらも確保しない。

コピーされた F2 の window/FFT/sparse mel tables は14,744 Bで、初回に構築され共有される。
FFT と統計計算には約14 KB の task stack を別途確保する必要がある。
元の `FE_TONEAREST` 算術契約を維持し、component とホストを
`-fno-fast-math -ffp-contract=off` でコンパイルする。
端末では S3 assembly dot を使い、host は固定モデルの scalar oracle を使う。

## ホスト照合と PCM ダンプ

```sh
python3 tools/test_word23_recognizer.py
python3 tools/test_word23_recognizer.py --pcm /path/recording.s16le
python3 tools/test_word23_recognizer.py --wav /path/recording.wav
```

既定テストだけが stt32 の fresh15 WAV を読み込む。参照位置は sibling `../stt32`、
`STT32_ROOT` または `--stt32-root` で変更できる。固定 fixture の WAV path は相対 path。
PCM/WAV 診断は stt32 を必要とせず、scores と trim・時間・判定をJSONで出力する。
Pythonからは `build(executable: Path)` と `infer(executable, pcm_file)` を共用できる。

fresh15 合否参照は、移植前の stt32 C++ native frozen golden の trim15行を保存した
[tests/fresh15_native_golden.json](tests/fresh15_native_golden.json)。元の inputs.jsonl、
golden_predictions.jsonl、Python診断JSONの SHA と native index を併記する。
移植kernelを直接実行して15件すべての確率を許容1e-5で照合し、public API は
発話12件の確率と無音3件の推論・割り当て省略を検証する。90%/60%の境界、
無音/失敗、NaN等の不正値、caller PCMの保全、内部RAM/PSRAM fallback、
割り当て失敗時の解放も確認する。

承認計画にあった `postlock_fresh15_diagnostic.json` は Python/scipy float32 特徴量を
使う別経路であり、元端末用 C++ double FFT と確率1e-5以内で一致する参照ではない。
この差は全15件で別診断し、一致という扱いにはしない。移植後の照合結果は
native golden 最大差4.71e-13（JSON出力丸め）、Python参照との差は最大0.00767317414、
top1差0/15、90%/60% policy の採否差0/15。無音3件は public API では常に不採用。
ホスト一致は assembly 実行・録音ゲイン・実マイク精度を証明しないため、実機ゲートは別に行う。

## コピーしたファイルの SHA-256

| ローカルファイル | SHA-256 |
| --- | --- |
| `upstream/word23_accel_feature_f2.cpp` | `5ffcd1e8186485668f5da5c5eab16860dc699f08c81fdfe43d9f85467cb4554e` |
| `upstream/word23_dot_s8_s3.S` | `7ac1ef106408ca3cac4ca8c55d3dfff4ec1e8f2f58b097d6e3fab792b26cd386` |
| `upstream/word23_accel_m2t_model.cpp` | `c11e8080b302074689b1a5f9bc9e238c941d45a13f5912bf5ac3ffc048767f04` |
| `upstream/word23_accel_feature_f2.h` | `b1a652c482b386346dcf0436530dd02c62a19aed39d80b0f0dfb8905a83fc360` |
| `upstream/word23_accel_feature_f2_internal.h` | `41a8ccad56a05253b64a5e9101438b3865b0ddbbb8029a90ceed43da314dae33` |
| `upstream/word23_accel_internal.h` | `31b5f36d656621562cb66a4e2c0175e1dd21a74dda5fd073eeeb4d1cee5c7e10` |
| `upstream/word23_accel_m2_detail.h` | `debcb257d1d7c92149441bfe6ac8e8459b6cd2cd635b6a7121902efc71c5ccb6` |
| `upstream/word23_accel_m2_model.h` | `f15af002164fe959de4a983ce56e7d4abaacfbc59743140cfc47445c71e4659d` |
| `upstream/word23_accel_m2t_model.h` | `c1f01d1ba9b5c1d930b85fdaf38e4762e1c5035aaffa0ea31f9a2351ba0dada8` |
| `upstream/word23_accel_model.h` | `bb46b0f4691b6ff4104fb459a53f3a3211faca489584687b942164799bd12a26` |
| `upstream/log_mel_feature.h` | `caca96c1dc4339b4821f18f1ce5d6c921fff9e86dbd00351609759b8acff46a0` |
| `upstream/sts_int8_model.h` | `a815ed9201c6581f323ad24380477a949781f8a69e2c09dbd6d0722500c03cb5` |
| `upstream/energy_trim.h` | `63ef03ede87ebc6e79eab1a38bc15fd6196a3f9c2e5e509cf58baa6fb31d0ea6` |
| `generated/sts_int8_model_config.h` | `acd816feb8ca5160854270fc365758ebfdf7e2187811c13aa8874ef4579ce54d` |
| `generated/sts_int8_model_data.h` | `5c73b42e279f0f82bbdd9a0621039f4bf0e7eb07217c0b45a4697fefa04e3672` |
| `generated/sts_feature_norm.h` | `36a5c4d50fe6702fff5fbfe82d1a3fd22c420242c27c2a1bc382251bc64b6361` |
| `generated/word23_runtime_config.h` | `b68bfc0fe1854fb8bc645c3ba6dbd543a946dc2ccced72af25560af7cfba4449` |
| `tests/fresh15_native_golden.json`（既存参照の抜粋） | `68ad2e404f634e3e838359d2c56f770b1d49669063f1825b2641b4f7114f9d2f` |

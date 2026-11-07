# nonkiai

English | [日本語](README.md)

nonkiai (Nonki-chan AI) is firmware that lets you play with plush toys and other toys by voice, using only an M5Stack ATOMS3R (ESP32-S3) and a Voice Base. Call it "Nonki-chan" or tap the body, and it works out the intent of what you said and answers in a voice. Both speech recognition and speech synthesis run on the microcontroller; it does not connect to the internet.

This repository is a snapshot synced from the development repository. It does not carry that history.

## What it solves

It is for people who want to add AI features to their toys. If you want a plush toy to answer when you talk to it, or a toy to move when you give a voice command, most approaches need a cloud service or a smartphone. nonkiai does wake-up detection, intent recognition and spoken replies on the microcontroller, with only an ATOMS3R and a Voice Base (parts cost under 5,000 yen).

## Main features

- Two ways to start listening: tap the body, or call "Nonki-chan".
- It does not transcribe what you say; it recognizes what you want (the intent). There are 17 intents: forward, backward, right, left, name, weather, time, energy, praise, yes, no, good morning, good night, up, down, stop, start.
- Two recognition modes. Plush mode is for conversation; toy mode sends forward, backward, right and left by infrared.
- Replies are spoken by on-device speech synthesis. Two voice models are included: female2 and man2. A voice model can be made from about 10 seconds of recorded voice (the training code is not in this repository). Generation speed is about 0.7 (it takes less time than the speech lasts), and audio is played back while it is being generated.
- The infrared output can be the built-in IR LED or an external IR unit on the Grove port.
- Two web pages (Chrome / Edge on a PC). The settings page (Web Bluetooth) sets the name, volume, recognition mode and IR test sends, and sends the time and the Tokyo weather to the device. The flashing page (USB) rewrites the app, voice model and dictionary, and can write the whole image.

All models together take about 6 MB (recognition about 1.6 MB, speech synthesis about 1.5 MB, reading dictionary about 3.0 MB; all INT8). They live in the microcontroller's flash memory.

## Technology and requirements

| Item | Details |
|---|---|
| Hardware | M5Stack ATOMS3R (ESP32-S3), Atomic Voice Base, USB-C cable |
| Firmware | ESP-IDF 5.4.4 (`firmware/dependencies.lock`), M5Unified, LittleFS, esp-dsp |
| Speech recognition | stt32 (source copy and weights included) |
| Speech synthesis | A copy of sanoTTS-jp (`firmware/third_party/sanoTTS-jp`) and a kanji dictionary (derived from NAIST-JDIC) |
| Web pages | HTML / JavaScript (Web Bluetooth, Web Serial, esptool-js) |
| Relay server | Python 3 (`tools/model_flasher_server.py`) |
| Browser | Chrome / Edge |

The weather is fetched from [Open-Meteo](https://open-meteo.com/) by the browser that opens the page. No API key is needed.

## Demo video

Coming soon.

## Build

You need:

- M5Stack ATOMS3R, Atomic Voice Base and a USB-C cable
- ESP-IDF 5.4.4 (the version is pinned in `firmware/dependencies.lock`)

Run in a shell where ESP-IDF's `export.sh` has been sourced:

```sh
cd firmware
idf.py set-target esp32s3
idf.py build
```

The sources and weights are in the repository. The only things fetched from the network at build time are ESP-IDF's dependency components.

## Flashing the device

### Flash without building (easiest)

Even without ESP-IDF, you can flash the prebuilt set (`firmware/prebuilt/`) as it is. It holds the same things `idf.py flash` writes (bootloader, partition table, app, LittleFS, plus the female2 voice model, its voice value record, the dictionary and the speech recognition weights from `firmware/data/`). `firmware/prebuilt/build-info.json` records which source commit it was built from.

Either method overwrites what is on the ATOMS3R now, and it cannot be restored.

**From the browser** (only Python 3 and Chrome / Edge)

1. In the repository root, run `python3 tools/model_flasher_server.py` and open http://localhost:8095/.
2. Click 「端末に USB で接続」 (connect over USB) and pick the ATOMS3R's port.
3. Under 「全体を書く（idf.py flash と同じ）」 (write the whole image), click 「中継サーバの一式を取得」 (fetch the set from the relay server), then 「全体を書く」 (write all).

See "Flashing from the browser" below for details.

**With esptool** (one command)

```sh
pip install esptool
cd firmware/prebuilt
esptool.py --chip esp32s3 -p <PORT> write_flash @flash_args
```

`<PORT>` is the ATOMS3R's serial port (`/dev/ttyACM0` on Linux, `COM3` etc. on Windows). With esptool v5 you can also write `esptool --chip esp32s3 -p <PORT> write-flash @flash_args`. The ATOMS3R restarts when writing finishes.

### Where the files are

The files to flash are at these places in the repository.

| File | Contents | Used by |
|---|---|---|
| `firmware/data/sanotts/female2-v2-excite-int8.bin` | Voice model female2 (default) | `idf.py flash` |
| `firmware/data/sanotts/man2-v2-excite-int8.bin` | Voice model man2 | `idf.py flash` (set with `-DSANO_MODEL_BIN`) |
| `firmware/data/sanotts/female2-v2-excite-record.bin` | female2's voice value record (4 KB; `idf.py flash` writes it with the weights) | `idf.py flash` |
| `firmware/data/sanotts/man2-v2-excite-record.bin` | man2's voice value record | `idf.py flash` (picked automatically when man2 is set) |
| `firmware/data/sanotts/female2-v2-excite-bundle.zip` | female2's weights, flags and voice values (same weights as the `.bin` above) | Flashing from the browser |
| `firmware/data/sanotts/man2-v2-excite-bundle.zip` | man2's weights, flags and voice values (same weights as the `.bin` above) | Flashing from the browser |
| `firmware/data/sanotts/k1_dict_4mb.bin` | Kanji dictionary | `idf.py flash`, browser |
| `firmware/data/stt32/weights_partition.bin` | Speech recognition weights | `idf.py flash` |
| `firmware/build/nonkiact2_sampler.bin` | App (created by a build) | `idf.py flash`, browser |
| `firmware/prebuilt/` | Prebuilt bootloader, partition table, app and LittleFS, plus the list of write addresses (`flash_args`, `flasher_args.json`) | Flash without building |

### Flashing over USB (first time)

```sh
idf.py -p <PORT> flash
```

`<PORT>` is the ATOMS3R's serial port (`/dev/ttyACM0` etc. on Linux). The first time, this writes the app, the voice model (female2) and its voice value record, the dictionary, the speech recognition weights and an empty LittleFS together. Later, when only the app has changed, `idf.py -p <PORT> app-flash` is enough.

To use the man2 voice, specify its weights at build time. The `man2-v2-excite-record.bin` next to it is picked automatically.

```sh
idf.py -DSANO_MODEL_BIN="$PWD/data/sanotts/man2-v2-excite-int8.bin" build
idf.py -p <PORT> flash
```

### Flashing from the browser

You can flash the ATOMS3R over USB (Web Serial) from Chrome / Edge on a PC. No other server or online service is needed (only esptool-js, which the page loads, comes from the network). For an ATOMS3R that has never been flashed with this firmware (factory firmware or other firmware), use 「全体を書く」 (write the whole image); for one flashed before, rewrite the app, voice model or dictionary.

1. In the repository root, run the following and open http://localhost:8095/. Web Serial only works on localhost, so the page is served locally.

   ```sh
   python3 tools/model_flasher_server.py
   ```

2. Click 「端末に USB で接続」 and pick the ATOMS3R's port. If other firmware is on it, the page shows 「端末に nonkiact2 のパーティション表がありません」 (the device has no nonkiact2 partition table), but stays connected. Go on to "Write the whole image" below.
3. Choose what to write.
   - Voice model: with 「手元のモデル bundle（.zip）を選択」 (choose a local model bundle), pick `firmware/data/sanotts/female2-v2-excite-bundle.zip` or `man2-v2-excite-bundle.zip`. A bare `.bin` cannot be chosen. The page checks the flags in the bundle against the device.
   - Dictionary: `firmware/data/sanotts/k1_dict_4mb.bin`
   - App: `firmware/build/nonkiact2_sampler.bin` (if you built it)
4. Click 「照合」 (compare) to see what differs, then 「書き込む」 (write).

If writing is cut off, reconnect and start again from 「照合」. If the app no longer starts, you can rewrite it with "Write the whole image" below or with `idf.py -p <PORT> flash`.

#### Write the whole image (the same set as `idf.py flash`)

The page writes the files listed in `firmware/build/flasher_args.json` to the same addresses as `idf.py flash`. If you have not built (there is no `firmware/build/`), it uses the prebuilt `firmware/prebuilt/flasher_args.json`.

1. After connecting, click 「中継サーバの一式を取得」 under 「全体を書く（idf.py flash と同じ）」. The bootloader, partition table, app, voice model and its voice value record, dictionary, speech recognition weights and LittleFS appear in the list.
2. Click 「全体を書く」 and confirm with OK. It is about 8 MB in all, so it takes a few minutes. After writing, the page compares each region with the device, then resets the device.

The current contents are not saved, so you cannot go back to the original firmware. On an ATOMS3R that had other firmware, the settings regions not in the set (nvs, phy_init, userdict) are also erased. On an ATOMS3R that has had nonkiact2 before, the settings (nvs) and user dictionary (userdict) are kept.

#### Getting a voice model from model-builder (optional)

If you run model-builder (which trains voice models) yourself, enter its URL in 「model-builder の URL」 in the model section. 「model-builder の一覧を取得」 (get the model-builder list) shows the list, and you can fetch the chosen weights. Passing `--builder <URL>` at start-up, or writing the URL on one line in `tools/model_builder_url.txt`, makes it the field's default. Fetched weights are saved in `~/.cache/nonkiact2/model-flasher`, and the same ones are not fetched again (change the location with `--cache-dir`). If you have no model-builder, leave the field empty and choose a local bundle (.zip).

## Opening the settings page

1. In the repository root, run `python3 tools/model_flasher_server.py`.
2. Open http://localhost:8095/index.html in Chrome / Edge, click 「つなぐ」 (connect) and pick the ATOMS3R (Bluetooth).

## License

The root `LICENSE` (MIT) covers only the original code in this repository. Bundled material follows its own license.

| What | License |
|---|---|
| Original code | MIT (`LICENSE`) |
| Voice models female2 / man2 (`.bin` and bundle `.zip`) | MIT (`firmware/data/sanotts/LICENSE`) |
| Copy of the stt32 speech recognition source and its weights (`firmware/third_party/stt32`, `firmware/data/stt32`) | Apache-2.0 (Copyright 2026 adlib ltd.). Third-party notices are in `firmware/third_party/stt32/firmware/components/stt32_wakehubert/NOTICE` |
| Kanji dictionary `k1_dict_4mb.bin` | Derived from NAIST-JDIC (modified BSD, `firmware/data/sanotts/COPYING.naist-jdic`) |
| Copy of the sanoTTS-jp source | `LICENSE` / `NOTICE.md` in `firmware/third_party/sanoTTS-jp` |
| Copy of the x-io Fusion orientation filter | MIT (`firmware/third_party/Fusion/LICENSE.md`) |
| Code derived from Open JTalk | Each `COPYING` / `LICENSE.open_jtalk` |

The voice models female2 / man2 were made as follows.

1. The reference voices and training audio were generated with Irodori-TTS (MIT). No recordings of real people were used.
2. From that audio, the voice models were made with sanoTTS-jp-2. sanoTTS-jp-2 is sanoTTS-jp retrained with Style-Bert-VITS2.

The sentences in the training audio come from ROHAN4600 (CC0-1.0), the ITA corpus (public domain / Unlicense), the AICA corpus (CC0-1.0) and the COEIROINK corpus (public domain / Unlicense). Only man2 used the COEIROINK corpus.

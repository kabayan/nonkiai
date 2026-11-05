# sanoTTS-jp の C99 推論コアを ESP-IDF の component として登録する。
#
# ⚠️ **csrc/ をコピーもシンボリックリンクもしない。** 相対パスで直接参照する。
#    同じものを 2 か所に置くと、片方だけ直したときに golden test（bit 一致）が
#    どちらを見ているのか分からなくなる。csrc/saanotts_internal.h が
#    「同じカーネルを 2 回書かない」と書いているのと同じ理由。

get_filename_component(SAAN_CSRC "${CMAKE_CURRENT_LIST_DIR}/../../../csrc" ABSOLUTE)

if(NOT EXISTS "${SAAN_CSRC}/saanotts.c")
    message(FATAL_ERROR
        "csrc が見つからない: ${SAAN_CSRC}\n"
        "esp32/ はリポジトリ直下に置くこと（esp32/../csrc が推論コア）")
endif()

# ⚠️ **コア 4 ファイルすべてが要る。** saanotts_int8.c には saan_w / saan_conv1d_w /
#    saan_dwconv1d_w / saan_act_scratch_needed があり、saanotts.c と
#    saanotts_stream.c の両方がこれを参照する。3 ファイルではリンクできない
#    （csrc/Makefile の CORE と同じ並び。片方だけ増やさないこと）。
#
# ⚠️ **g2p.c はコアではなく独立した翻訳単位**（推論コアを 1 行も参照しない）が、
#    esp32/main/ がこれらを呼ぶのでここに並べる。csrc/Makefile の CORE には
#    入れない（golden_test 等はリンクしないため）。**非対称なのは意図的。**
#      g2p.c  … main.c が saan_g2p() を呼ぶ
#      line.c … saan_console.c が saan_line_feed() を呼ぶ（行編集ステートマシン）
# --- K トラック（端末で漢字を扱う）------------------------------------------
#
# ⚠️ **既定では入れない。** 辞書リーダ + 取り込んだ Open JTalk で app が
#    大きくなり、辞書パーティション（13.5 MB）も要る。かな入力だけの
#    出荷構成（v0.1.1）はこれ無しで動く。
#
#     idf.py -DSAAN_KANJI=1 -DPARTITION_TABLE=partitions_16mb.csv build
#
# ⚠️ **取り込んだ Open JTalk は改変しない**（csrc/openjtalk/PROVENANCE.md）。
#    `-DCHARSET_UTF_8` が要る（無いと `#error CHARSET is not specified`）。
#
# --- T10(b): 取り込んだ Open JTalk の一時ヒープを PSRAM に向ける -------------
#
# ⚠️ **ソースは 1 バイトも変えない。** `-include oj_heap_psram.h` を
#    `csrc/openjtalk/*.c` **だけ**に当てて calloc/strdup/free を差し替える
#    （K-5 の `-include oj_heap_probe.h` と同じ手口。`k4b_vendor.py --check` は通る）。
# ⚠️ **実装ファイル `oj_heap_psram.c` には当てない**（当てると無限再帰）。
# ⚠️ **label_ids.c にも当てない** — あれは malloc を 1 つも使わない。
#
#     idf.py -DSAAN_OJ_PSRAM=0 ...    ← 陽性対照（内部 DRAM が減るのを見る）
set(SAAN_K_SRCS "")
if(SAAN_KANJI)
    file(GLOB SAAN_OJT "${SAAN_CSRC}/openjtalk/*.c")
    set(SAAN_K_SRCS
        "${SAAN_CSRC}/jdict.c"
        "${SAAN_CSRC}/accent.c"
        "${SAAN_CSRC}/njd_rules.c"
        "${SAAN_CSRC}/label_ids.c"
        ${SAAN_OJT})
    if(NOT DEFINED SAAN_OJ_PSRAM)
        set(SAAN_OJ_PSRAM 1)
    endif()
    if(SAAN_OJ_PSRAM)
        list(APPEND SAAN_K_SRCS "${CMAKE_CURRENT_LIST_DIR}/oj_heap_psram.c")
    endif()
endif()

idf_component_register(
    SRCS "${SAAN_CSRC}/saanotts.c"
         "${SAAN_CSRC}/saanotts_stream.c"
         "${SAAN_CSRC}/fft.c"
         "${SAAN_CSRC}/saanotts_int8.c"
         "${SAAN_CSRC}/g2p.c"
         "${SAAN_CSRC}/line.c"
         ${SAAN_K_SRCS}
    INCLUDE_DIRS "${SAAN_CSRC}" "${SAAN_CSRC}/openjtalk"
    REQUIRES ""
)

if(SAAN_KANJI)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC CHARSET_UTF_8 SAAN_KANJI=1)
    message(STATUS "sanoTTS: **漢字対応を有効**（辞書 13.7 MB のパーティションが要る）")

    # ⚠️ **絶対パスで当てる。** `-include oj_heap_psram.h` だとヘッダ探索順に
    #    依存して、見つからなくても **警告なしで素の calloc のまま**になる
    #    （= 効いていないのに気づけない）。
    if(SAAN_OJ_PSRAM)
        set_source_files_properties(${SAAN_OJT} PROPERTIES
            COMPILE_OPTIONS "-include;${SAAN_CSRC}/oj_heap_psram.h")
        message(STATUS "sanoTTS: Open JTalk の一時ヒープを **PSRAM 優先**に"
                       "（-include oj_heap_psram.h）。陽性対照は -DSAAN_OJ_PSRAM=0")
    else()
        message(STATUS "sanoTTS: Open JTalk の一時ヒープは素の calloc（**陽性対照**）")
    endif()

    # K-7 のトークン表を .bss ではなく合成 arena から借りる（T10(a)）。
    # ⚠️ **saan_kanji.c が label_ids_set_scratch() を必ず呼ぶ前提**。呼ばずに
    #    label_ids_convert() を叩くと LABEL_IDS_ERR_ARG で**止まる**（黙って動かない）。
    target_compile_definitions(${COMPONENT_LIB} PUBLIC LABEL_IDS_EXTERNAL_SCRATCH=1)

    # トークン表の本数。⚠️ **既定 640 は端末には過大**（M-79 の 3.6 倍の余裕）。
    # 入力が 44 形態素に縛られた今（SAAN_KANJI_MAX_INPUT_TOK。M-98）、
    # 実際に要るのは 200 前後で、M-79 は 256 でも held-out 298/298 一致・
    # **128 で 273/298 に落ちる**ことを陽性対照として測ってある。
    # 256 = 4,096 B（640 の 10,240 B から −6,144。M-140）。
    # ⚠️ **溢れても黙って切り詰めない**（M-79 で K7_ERR_OVERFLOW を返すようにした）。
    if(NOT DEFINED SAAN_LABEL_IDS_MAX_TOKENS)
        set(SAAN_LABEL_IDS_MAX_TOKENS 256)
    endif()
    target_compile_definitions(${COMPONENT_LIB} PUBLIC
                               LABEL_IDS_MAX_TOKENS=${SAAN_LABEL_IDS_MAX_TOKENS})
    message(STATUS "sanoTTS: K-7 トークン表 ${SAAN_LABEL_IDS_MAX_TOKENS} 本"
                   "（× 16 B。既定 640 から詰めてある）")
endif()

# ℹ️ **かつて「-std=c99 を足すな」と書いていたが、もう地雷ではない。**
#    newlib の M_PI が __STRICT_ANSI__ の下で隠れる問題は実在し、実際に
#    `xtensa-esp32s3-elf-gcc -std=c99` で saanotts.c:395 が落ちた（M-54）。
#    **csrc/saanotts_internal.h で M_PI を定義して解決済み**なので、
#    厳密 -std=c99 でも 5 ファイルすべてコンパイルが通る（実測）。
#    IDF 既定の gnu17 のままにしておくが、足しても壊れない。
#
# ⚠️ **SAAN_FFT_DOUBLE と SAAN_USE_NAIVE_DFT は定義しない。**
#    ESP32-S3 の FPU は単精度のみ。naive DFT は double の cos/sin を使うが、
#    -O2 では未定義のままデッドコード除去される（`nm -u` で _cos/_sin が
#    出ないことを手元で確認済み）。定義するとソフト浮動小数に落ちる。
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)

# --- プラットフォームの注入点（T5-G2）------------------------------------------
#
# csrc/saanotts_internal.h は `SAAN_PORT_HEADER` が定義されていればそれを include する。
# ここでは saan_port_esp32.h（このディレクトリ）を渡し、SAAN_HOT_DATA → DRAM_ATTR /
# SAAN_HOT_CODE → IRAM_ATTR にする。erf 表 1,032 B（csrc/erf_table.h）が内部 DRAM に載る。
# ⚠️ esp_attr.h は esp_common（IDF の共通依存）から解決されるので REQUIRES は増やさない。
# ⚠️ **ホストのビルドは無変更**（未定義 → 空。csrc/Makefile と check_esp32_template.sh はこの構成）。
# ⚠️ 配置だけの変更なので QEMU checksum は不変（T5-G2 で確認）。.map の kSaanErf* が
#    `.dram1.*` に入っていれば効いている。
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_compile_definitions(${COMPONENT_LIB} PRIVATE "SAAN_PORT_HEADER=\"saan_port_esp32.h\"")

# --- 段別プロファイラ（csrc/saan_prof.h）-------------------------------------
#
#     idf.py -DSAAN_PROFILE=1 build
#
# 実機で「1 チャンクの時間がどこに行っているか」を CCOUNT（サイクル）で段・カーネル
# ごとに出す。main.c が発話のあとに表を ESP_LOGI する。**既定は無効**で、
# 無効ならコアは 1 バイトも変わらない（マクロが `((void)0)` に展開される）。
# ⚠️ **計測自体にコストがある**（出力チャネルごとに CCOUNT を 2 回読む）。
#    速度を報告するときは SAAN_PROFILE=0 で測ること。内訳を見るときだけ 1。
# ⚠️ **QEMU で取った値はサイクルではない**（`-icount` なら命令数に比例した仮想時間）。
if(SAAN_PROFILE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_PROFILE=1)
    message(STATUS "sanoTTS: 段別プロファイラ **有効**（速度の報告には使わない）")
endif()

# --- G87（幅512 の hdown/hout）: hdown の出力幅 SAAN_DEC_HEAD -------------------
#
#     idf.py -DSAAN_DEC_HEAD=512 -DSAAN_MODEL_BLOB=<G87 の int8 blob> build
#
# ⚠️ **PUBLIC で定義する。** arena の確保・`saan_stream_arena_used()/peak()` がこの値で決まるので、
#    main.c（SAAN_ARENA_BYTES の既定）もコアと同じ値を見る必要がある。PRIVATE にすると
#    main 側だけ 48 の見積りになる（SAAN_MEM_HEAD_PF と同じ形）。
# ⚠️ **既定はここに持たない。** 既定 48 は csrc/saanotts.h の `#ifndef` にある（定義しなければ出荷構成のまま）。
# ⚠️ blob と食い違うと saan_conv1d_w が SAAN_ERR_SHAPE で止める（黙って誤読しない。DLS-199）。
if(DEFINED SAAN_DEC_HEAD)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC SAAN_DEC_HEAD=${SAAN_DEC_HEAD})
    message(STATUS "sanoTTS: SAAN_DEC_HEAD=${SAAN_DEC_HEAD}（hdown/hout の幅。出荷は 48）")
endif()

# --- decoder の幹の幅（SAAN_DEC_W / SAAN_DEC_E。既定 76 / 304 は csrc/saanotts.h）-------------
#     idf.py -DSAAN_DEC_W=228 -DSAAN_DEC_E=912 build
# ⚠️ PUBLIC: arena の見積り（main.c）もコアと同じ値を見る必要がある（SAAN_DEC_HEAD と同じ）。
if(DEFINED SAAN_DEC_W)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC SAAN_DEC_W=${SAAN_DEC_W})
    message(STATUS "sanoTTS: SAAN_DEC_W=${SAAN_DEC_W}（decoder の幹の幅。出荷は 76）")
endif()
if(DEFINED SAAN_DEC_E)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC SAAN_DEC_E=${SAAN_DEC_E})
    message(STATUS "sanoTTS: SAAN_DEC_E=${SAAN_DEC_E}（pw1 の出力幅。出荷は 304）")
endif()

# --- decoder の pw1 / pw2 / hdown と GELU も 2 コアに割る（既定 0）。idf.py -DSAAN_DEC_OC=1 build
if(SAAN_DEC_OC)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_DEC_OC=1)
    message(STATUS "sanoTTS: decoder の pw1/pw2/hdown と GELU を出力チャネルで割る（SAAN_DEC_OC=1）")
endif()

# 同じく depthwise（SAAN_DEC_DWOC）と、分割する conv の活性化の量子化（SAAN_OC_QPAR）も割る（既定 0）
if(SAAN_DEC_DWOC)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_DEC_DWOC=1)
    message(STATUS "sanoTTS: decoder の depthwise を 2 コアに割る（SAAN_DEC_DWOC=1）")
endif()
if(SAAN_OC_QPAR)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_OC_QPAR=1)
    message(STATUS "sanoTTS: 分割する conv の活性化の量子化もフレームで割る（SAAN_OC_QPAR=1）")
endif()

# --- 長さのスケール s_v（SAAN_S_V）---------------------------------------------
#
#     idf.py -DSAAN_S_V=1.0f build
#
# ⚠️ **PUBLIC で定義する。** main.c も `SAAN_S_V` を読む（saan_stream_init に渡す）。
#    ここで渡した値をコアと main が同じに見る必要がある。
# ⚠️ 既定は csrc/saanotts.h の `#ifndef`（1.2187f = D-019）。渡さなければ出荷構成のまま。
#    値は C のリテラルとして渡す（`1.0f`）。
if(DEFINED SAAN_S_V)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC SAAN_S_V=${SAAN_S_V})
    message(STATUS "sanoTTS: SAAN_S_V=${SAAN_S_V}（長さのスケール。既定 1.2187f）")
endif()

# --- MEM-5: hout の計算粒度（既定 4 = csrc/saanotts_stream.c）-----------------
#
#     idf.py -DSAAN_MEM_HEAD_PF=0 build   # 元に戻す（arena も 180224 に戻すこと）
#
# ⚠️ **PUBLIC で定義する。** `saan_stream_arena_used()` の値が変わるので、
#    雛形（main.c）の二重防御がコアと同じ値を見る必要がある。
#    PRIVATE にすると main 側だけ旧い値を期待して「a.used が足りない」で止まる。
# 速度の代償は実機で測ってある（M-139 §6。粒度 4 が折衷点）。
# ⚠️ **値をそのまま渡すこと。** `=1` と書き潰すと `-DSAAN_MEM_HEAD_PF=2` が
#    黙って 1 として効き、**arena も速度も 1 列版のまま**になる（実際に踏んだ。
#    起動ログの `arena used` が粒度で変わらないので気づけた）。
# ⚠️ **既定はここに持たない。** 既定 4 は `csrc/saanotts_stream.c` の `#ifndef` にある。
#    ESP-IDF / wasm / Arduino の 3 経路が全部その 1 行を見るので、**ここに既定を置くと
#    wasm と Arduino だけ別の値になる**（C-099 と同じ形）。ここは `-D` の中継だけ。
#    0 を渡すと元の「CH 列まとめて」に戻る（`-DSAAN_ARENA_BYTES=180224` も一緒に戻すこと。
#    main.c が起動時に検査して足りなければ止める）。
# --- hout の出力 o1539 を arena の外（PSRAM）に置く（既定 0）。PUBLIC: main.c が PSRAM に確保して渡す ---
#     idf.py -DSAAN_HEAD_EXT=1 build
if(SAAN_HEAD_EXT)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC SAAN_HEAD_EXT=1)
    message(STATUS "sanoTTS: hout の出力 o1539 を arena の外（PSRAM）に置く（SAAN_HEAD_EXT=1）")
endif()

# --- 倍音励起つきの G（候補 A。既定 0）。PUBLIC: main.c が F0 ヘッドの重みを渡す ---
#     idf.py -DSAAN_G_EXCITE=1 [-DSAAN_EXC_TEST_VBIAS=0.5f] build
#     ⚠️ arena が +33.6 KB（励起の状態 26.4 KB + obuf 7 hop）要る。-DSAAN_ARENA_BYTES を上げること。
if(SAAN_G_EXCITE)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC SAAN_G_EXCITE=1)
    message(STATUS "sanoTTS: 倍音励起つきの G（SAAN_G_EXCITE=1）")
endif()
# --- 端末の句境界を SBV2 の規則にする / 文末が句読点なら末尾に _ を足す（既定 0。DLS-302）---
#     idf.py -DSAAN_PHRASE_RULE_SBV2=1 -DSAAN_TAIL_PAUSE=1 build
if(SAAN_PHRASE_RULE_SBV2)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_PHRASE_RULE_SBV2=1)
    message(STATUS "sanoTTS: 句境界を SBV2 の規則にする（SAAN_PHRASE_RULE_SBV2=1）")
endif()
if(SAAN_TAIL_PAUSE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_TAIL_PAUSE=1)
    message(STATUS "sanoTTS: 文末が句読点なら末尾に _ を足す（SAAN_TAIL_PAUSE=1）")
endif()
# FFT の twiddle 表 / 励起の sin 表を内部 DRAM に置く（既定 0 = .rodata）
if(SAAN_FFT_TAB_RAM)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_FFT_TAB_RAM=1)
endif()
if(SAAN_EXC_TAB_RAM)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_EXC_TAB_RAM=1)
endif()
if(DEFINED SAAN_EXC_TEST_VBIAS)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC SAAN_EXC_TEST_VBIAS=${SAAN_EXC_TEST_VBIAS})
endif()

if(DEFINED SAAN_MEM_HEAD_PF)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC SAAN_MEM_HEAD_PF=${SAAN_MEM_HEAD_PF})
    message(STATUS "sanoTTS: MEM-5 — hout を ${SAAN_MEM_HEAD_PF} 列ずつ（arena を詰める）")
else()
    message(STATUS "sanoTTS: MEM-5 は csrc の既定（4 列）。変えるなら -DSAAN_MEM_HEAD_PF=<列数>")
endif()

# --- PIE（ESP32-S3 の整数 SIMD）-------------------------------------------
#
# ⚠️ **現在このファームは PIE を使っていない。** 有効にするには次の 2 つを
#    **両方**定義する（片方だけでは効かない）:
#
#      SAAN_INT8_ACT=1  … activation も int8 にする（W8A8）。PIE の前提。
#                         W8A32（既定）は積和が fp32 なので整数 SIMD が効かない
#      SAAN_PIE=1       … saan_conv1d_i8a の内積を ee.vmulas.s8.accx にする
#
#    正しさは QEMU で検証済み（MAC の 99.40% を PIE 化、bit 完全一致。M-57 / M-58）。
#
# ⚠️ **有効化には判断が要る**（未決）:
#      - SNR が fp32 比 25.88 → 23.24 dB に落ちる
#        （⚠️ ただし **SCOREQ では差が無い**。陽性対照つきで確認済み = M-55）
#      - arena ピークが 196.9 → 201.1 KB になり、**自主基準 200 KB を 1.1 KB 超える**
#        （SRAM 512 KB の 39%。当時の g_arena 208 KB には収まった。T2〜T4 後は 163.0 KB / g_arena 176 KB）
#      - ⚠️ **速度は一度も測っていない。** QEMU はサイクル精度ではない。実機が要る
#
# 切り替えは**ビルド時のフラグ 1 本**にしてある（既定は無効 = W8A32・移植可能 C）:
#
#     idf.py -DSAAN_ENABLE_PIE=1 -DSAAN_MODEL_BLOB=<int8 blob> build
#
# ⚠️ **2 つを別々に定義できないようにしてある。** `SAAN_PIE=1` だけを定義しても
#    PIE カーネルは W8A8 経路（`saan_conv1d_i8a`）の中にしか無いので 1 命令も出ない。
#    「片方だけ定義して効かない」を構造的に潰す。
# --- D-048: **ESP32-S3 では W8A8 + PIE を既定にする**（2026-09-03）-----------------
#
# 根拠: (1) **これ無しでは実時間に間に合わない** — W8A32 は CoreS3 で xRT 4.3〜4.6（M-83 / M-86）、
#       W8A8+PIE は **0.494**（M-89。要件 RTF ≤ 0.5）。(2) **知覚的に無料** — held-out 24 文の
#       SCOREQ 差 +0.0049 で CI が 0 を含む（M-55）。(3) arena の増分 4.2 KB は 176 KB に収まる（M-89）。
# ⚠️ **ESP32-S3 以外（ESP32 / C3 …）では PIE 命令が無いので自動的に W8A32 に落ちる**（下の分岐）。
#    明示的に W8A32 で測りたいときは `-DSAAN_ENABLE_PIE=0`。
if(NOT DEFINED SAAN_ENABLE_PIE)
    idf_build_get_property(_saan_default_target IDF_TARGET)
    if(_saan_default_target STREQUAL "esp32s3")
        set(SAAN_ENABLE_PIE 1)
        message(STATUS "sanoTTS: ESP32-S3 なので W8A8 + PIE を**既定で有効**にする（D-048）"
                       " — W8A32 で測るなら -DSAAN_ENABLE_PIE=0")
    endif()
endif()

if(SAAN_ENABLE_PIE)
    # ⚠️ **PIE は ESP32-S3 だけ。** ESP32（M5Stack Core2 / Basic）に渡すと、コアの
    #    `#if defined(__XTENSA__) && SAAN_PIE` は通るが `ee.*` がアセンブラで落ちる。
    #    落ち方が分かりにくいので CMake で止める（黙って W8A8 スカラに落ちない）。
    idf_build_get_property(_saan_target IDF_TARGET)
    if(NOT _saan_target STREQUAL "esp32s3")
        message(FATAL_ERROR "SAAN_ENABLE_PIE=1 は ESP32-S3 専用。IDF_TARGET=${_saan_target} では"
                            " -DSAAN_ENABLE_PIE=0（W8A32 / 移植可能 C）にすること")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_INT8_ACT=1 SAAN_PIE=1)
    message(STATUS "sanoTTS: W8A8 + PIE **有効**（SNR 低下と arena +4.2 KB を許容する構成）")
elseif(SAAN_W8A8_NOPIE)
    # ⚠️ **陰性対照専用。出荷構成ではない。**
    #    「PIE カーネルがスカラ実装と bit 一致するか」を**同じターゲット上で**
    #    確かめるために、W8A8 のままスカラに落とす。ホストと比べても意味は無い
    #    （アーキテクチャが違えば float の丸めが変わるので必ずずれる）。
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_INT8_ACT=1)
    message(STATUS "sanoTTS: W8A8 / PIE 無し（**陰性対照**。出荷構成ではない）")
else()
    message(STATUS "sanoTTS: W8A8 + PIE 無効（W8A32 / 移植可能 C）。"
                   "ESP32-S3 では既定が有効なので、これは -DSAAN_ENABLE_PIE=0 を明示したか S3 以外の板（D-048）")
endif()

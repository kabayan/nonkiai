# sanoTTS-jp（端末の日本語音声合成）を nonkiact2 に取り込むつなぎ。
# sanoTTS-jp-2 の esp32/main の C ファイル（漢字経路・辞書・2 コア・PCM 変換）は SANOTTS_DIR から読む。
# SANOTTS_DIR は firmware/CMakeLists.txt が決める（既定: firmware/third_party/sanoTTS-jp の写し。-DSANOTTS_DIR= で上書き）。
# 写したコミット: 3abaaaba5d9a464fb1c1682dd332c25f46e494c8
if(NOT DEFINED SANOTTS_DIR)
    set(SANOTTS_DIR "${CMAKE_CURRENT_LIST_DIR}/../../third_party/sanoTTS-jp")
endif()
set(SANO_MAIN "${SANOTTS_DIR}/esp32/main")

idf_component_register(
    SRCS
        "sano_voice.c"
        "${SANO_MAIN}/saan_kanji.c"
        "${SANO_MAIN}/saan_dict.c"
        "sano_model_copy.c"
        "${SANO_MAIN}/saan_dualcore.c"
        "${SANO_MAIN}/saan_pcm.c"
    INCLUDE_DIRS "include" "${SANO_MAIN}"
    REQUIRES saanotts_core esp_partition esp_mm esp_timer esp_psram heap mbedtls nvs_flash
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_KANJI=1 CHARSET_UTF_8)
if(SAAN_ENABLE_PIE OR SAAN_W8A8_NOPIE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_INT8_ACT=1)
endif()
if(SAAN_DUALCORE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_DUALCORE=1)
endif()
if(SAAN_PHRASE_RULE_SBV2)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_PHRASE_RULE_SBV2=1)
endif()
if(SAAN_TAIL_PAUSE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_TAIL_PAUSE=1)
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_ARENA_BYTES=${SAAN_ARENA_BYTES})

# 参照されない目印もリンカの節の削除で落とさない（ESP-IDF のカスタムアプリ記述）。
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u nk_app_model_flags")

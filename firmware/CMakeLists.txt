cmake_minimum_required(VERSION 3.16)

# AtomS3R (ESP32-S3-PICO-1-N8R8) 固定。機種分岐は持たない。
set(IDF_TARGET "esp32s3" CACHE STRING "IDF target" FORCE)

# ---- sanoTTS-jp（端末の日本語音声合成）----
# sanoTTS-jp のソースの置き場所。既定はリポジトリ内の写し third_party/sanoTTS-jp（DLS-151。ビルドが読むファイルだけ）。
# 手元の sanoTTS-jp-2 を直接使うときは -DSANOTTS_DIR=<絶対パス> を渡す。
# 写したコミット: 42e78bbe4e4e478e5f4110f7f8b77e201fb988b5（写し直しは tools/sync_sanotts.py）。
set(SANOTTS_DIR "${CMAKE_CURRENT_LIST_DIR}/third_party/sanoTTS-jp" CACHE PATH "sanoTTS-jp sources")
list(APPEND EXTRA_COMPONENT_DIRS "${SANOTTS_DIR}/esp32/components/saanotts_core")

# stt32 a259132 (weights 30d878c): components are copied with tools/sync_stt32.py.
set(STT32_DIR "${CMAKE_CURRENT_LIST_DIR}/third_party/stt32/firmware/components")
list(APPEND EXTRA_COMPONENT_DIRS "${STT32_DIR}/stt32" "${STT32_DIR}/stt32_wakehubert")
set(STT32_WAKEHUBERT_EMBED_WEIGHTS OFF CACHE BOOL "Read stt32 weights from stt32w" FORCE)
set(STT32_WAKEHUBERT_GENERATED_DIR "${CMAKE_CURRENT_LIST_DIR}/data/stt32")
# IDF otherwise builds every local component, even without a REQUIRES edge.
list(APPEND EXCLUDE_COMPONENTS word23)

# sanoTTS-jp-2 を AtomS3R 向けに使う構成（sano-measure の計測ビルド build_sano6 と同じ組み合わせ）。
# -D で上書きできる。コアの各フラグの意味は saanotts_core/CMakeLists.txt を見る。
foreach(_kv
        "SAAN_KANJI=1" "SAAN_ENABLE_PIE=1" "SAAN_DUALCORE=1"
        "SAAN_ARENA_BYTES=174080" "SAAN_MEM_HEAD_PF=8" "SAAN_HEAD_EXT=1"
        "SAAN_DEC_HEAD=512" "SAAN_DEC_OC=1" "SAAN_DEC_DWOC=1" "SAAN_OC_QPAR=1"
        "SAAN_G_EXCITE=1" "SAAN_FFT_TAB_RAM=1" "SAAN_EXC_TAB_RAM=1" "SAAN_OJ_PSRAM=1"
        "SAAN_PHRASE_RULE_SBV2=1" "SAAN_TAIL_PAUSE=1" "SAAN_S_V=1.0f")
    string(REGEX MATCH "^[^=]+" _k "${_kv}")
    string(REGEX REPLACE "^[^=]+=" "" _v "${_kv}")
    if(NOT DEFINED ${_k})
        set(${_k} "${_v}" CACHE STRING "sanoTTS-jp: ${_k}")
    endif()
endforeach()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nonkiact2_sampler)

# model（重み blob）と dict（漢字辞書 blob）のパーティションの中身。idf.py flash が一緒に書く（data/sanotts/README.md）。
# 別の声を書くときは -DSANO_MODEL_BIN=<絶対パス>、書かないときは -DSANO_MODEL_BIN= のように空を渡す。
set(SANO_MODEL_BIN "${CMAKE_CURRENT_LIST_DIR}/data/sanotts/female2-v2-excite-int8.bin" CACHE FILEPATH "model パーティションに書く重み blob")
# A normal variable is deliberate: infer again on every configure, never cache
# the female record when SANO_MODEL_BIN changes. An explicit -D override wins.
if(NOT DEFINED SANO_MODEL_RECORD_BIN)
    set(SANO_MODEL_RECORD_BIN "")
    if(SANO_MODEL_BIN MATCHES "-int8\\.bin$")
        string(REGEX REPLACE "-int8\\.bin$" "-record.bin" _record "${SANO_MODEL_BIN}")
        if(EXISTS "${_record}")
            set(SANO_MODEL_RECORD_BIN "${_record}")
        endif()
    endif()
endif()
set(SANO_DICT_BIN "${CMAKE_CURRENT_LIST_DIR}/data/sanotts/k1_dict_4mb.bin" CACHE FILEPATH "dict パーティションに書く辞書 blob")
set(STT32_WEIGHTS_BIN "${CMAKE_CURRENT_LIST_DIR}/data/stt32/weights_partition.bin" CACHE FILEPATH "stt32w partition image")

function(nonki_flash_data_image partition image)
    if(NOT EXISTS "${image}")
        message(FATAL_ERROR "${partition} image is missing: ${image}")
    endif()
    partition_table_get_partition_info(_size "--partition-name ${partition}" "size")
    if(partition STREQUAL "model")
        math(EXPR _size "${_size} - 4096")
    endif()
    # esptool_py_flash_to_partition only reads the offset, not the image size.
    # Check on every full build and flash, including changed model overrides.
    add_custom_target(nonki_check_${partition} ALL
        COMMAND ${CMAKE_COMMAND} "-DIMAGE=${image}" "-DPARTITION_SIZE=${_size}"
                -P "${CMAKE_CURRENT_LIST_DIR}/cmake/check_partition_image.cmake"
        VERBATIM)
    add_dependencies(flash nonki_check_${partition})
    esptool_py_flash_to_partition(flash "${partition}" "${image}")
endfunction()

foreach(_part model dict)
    string(TOUPPER "${_part}" _up)
    if(SANO_${_up}_BIN)
        nonki_flash_data_image("${_part}" "${SANO_${_up}_BIN}")
    else()
        message(WARNING "SANO_${_up}_BIN が未指定。idf.py flash は ${_part} パーティションを書かない（書き込み済みの板だけで動く）")
    endif()
endforeach()
nonki_flash_data_image(stt32w "${STT32_WEIGHTS_BIN}")

if(SANO_MODEL_BIN AND SANO_MODEL_RECORD_BIN AND EXISTS "${SANO_MODEL_RECORD_BIN}")
    file(SIZE "${SANO_MODEL_RECORD_BIN}" _record_size)
    if(NOT _record_size EQUAL 4096)
        message(FATAL_ERROR "Model record must be exactly 4096 bytes")
    endif()
    partition_table_get_partition_info(_model_offset "--partition-name model" "offset")
    partition_table_get_partition_info(_model_size "--partition-name model" "size")
    math(EXPR _record_offset "${_model_offset} + ${_model_size} - 4096" OUTPUT_FORMAT HEXADECIMAL)
    esptool_py_flash_target_image(flash model_record "${_record_offset}" "${SANO_MODEL_RECORD_BIN}")
elseif(SANO_MODEL_BIN)
    message(WARNING "Model record is absent; flash will not update voice parameters/name: ${SANO_MODEL_RECORD_BIN}")
endif()

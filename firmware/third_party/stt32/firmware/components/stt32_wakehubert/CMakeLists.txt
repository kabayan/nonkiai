# 17-intent recognition core (DLS-242/243): wakehubert int8 + 10 classifiers.
# Weights: generated/weights.bin + generated/qlayout.h, written by
# tools/regen_wakehubert_weights.py (or copied from the padfix_inputs backup).
#   -DSTT32_WAKEHUBERT_GENERATED_DIR=<dir>   where weights.bin / qlayout.h are (default: ./generated)
#   -DSTT32_WAKEHUBERT_EMBED_WEIGHTS=OFF     do not link the blob; the app must pass cfg->weights_partition
if(CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_register(REQUIRES esp_partition heap log freertos esp_timer)
    return()
endif()

if(NOT DEFINED STT32_WAKEHUBERT_GENERATED_DIR)
    set(STT32_WAKEHUBERT_GENERATED_DIR "${CMAKE_CURRENT_LIST_DIR}/generated")
endif()
if(NOT DEFINED STT32_WAKEHUBERT_EMBED_WEIGHTS)
    set(STT32_WAKEHUBERT_EMBED_WEIGHTS ON)
endif()
if(NOT EXISTS "${STT32_WAKEHUBERT_GENERATED_DIR}/qlayout.h")
    message(FATAL_ERROR "stt32_wakehubert: ${STT32_WAKEHUBERT_GENERATED_DIR}/qlayout.h is missing. "
                        "Run tools/regen_wakehubert_weights.py (see its --help).")
endif()

# class count of the weights: STT32_WH_NC in qlayout.h (written by the exporter) -> -DSTT32_WH_CLASSES
file(STRINGS "${STT32_WAKEHUBERT_GENERATED_DIR}/qlayout.h" wh_nc_line REGEX "^#define STT32_WH_NC [0-9]+")
if(wh_nc_line MATCHES "STT32_WH_NC ([0-9]+)")
    set(STT32_WH_NC_VALUE "${CMAKE_MATCH_1}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${STT32_WAKEHUBERT_GENERATED_DIR}/qlayout.h")

set(srcs "wakehubert.cpp" "stt32_backend_wakehubert.cpp" "dot_s8_s3.S")
if(STT32_WAKEHUBERT_EMBED_WEIGHTS)
    set(WEIGHTS_BIN "${STT32_WAKEHUBERT_GENERATED_DIR}/weights.bin")
    if(NOT EXISTS "${WEIGHTS_BIN}")
        message(FATAL_ERROR "stt32_wakehubert: ${WEIGHTS_BIN} is missing. Run tools/regen_wakehubert_weights.py.")
    endif()
    configure_file(${CMAKE_CURRENT_LIST_DIR}/weights.S.in ${CMAKE_CURRENT_BINARY_DIR}/weights.S @ONLY)
    list(APPEND srcs "${CMAKE_CURRENT_BINARY_DIR}/weights.S")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${WEIGHTS_BIN}")
    # weights.S only .incbin's the blob, so the compiler does not know the dependency:
    # rebuild the object when weights.bin changes.
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/weights.S" PROPERTIES OBJECT_DEPENDS "${WEIGHTS_BIN}")
endif()

# stt32_backend.h / stt32.h are in the stt32 component; reached by path so the two components
# do not require each other.
# The FFT / int16-FIR corrections use esp-dsp (a managed component; only linked in when selected).
idf_component_register(SRCS ${srcs}
    INCLUDE_DIRS "include" "../stt32" "../stt32/include"
    PRIV_INCLUDE_DIRS "." "${STT32_WAKEHUBERT_GENERATED_DIR}"
    REQUIRES esp_partition heap log freertos esp_timer)
if(CONFIG_STT32_WAKEHUBERT_EQ_IMPL_FFT OR CONFIG_STT32_WAKEHUBERT_EQ_IMPL_S16)
    # not in REQUIRES (the early-expansion list is fixed): link the managed component by name
    idf_component_get_property(wh_dsp_lib espressif__esp-dsp COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${wh_dsp_lib})
endif()
if(DEFINED STT32_WH_NC_VALUE)
    # PUBLIC: stt32_wakehubert.h (STT32_WH_CLASSES) is included by components that use the backend
    target_compile_definitions(${COMPONENT_LIB} PUBLIC STT32_WH_CLASSES=${STT32_WH_NC_VALUE})
endif()
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-fast-math -ffp-contract=off -Wno-misleading-indentation)
if(STT32_WAKEHUBERT_EMBED_WEIGHTS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE STT32_WH_EMBED_WEIGHTS=1)
endif()

file(GLOB HTS_ENGINE_SRCS "lib/*.c")
file(GLOB NJD_SRCS "njd/*.cpp" "njd/upstream/*.c")
file(GLOB JPCOMMON_SRCS "jpcommon/*.cpp" "jpcommon/upstream/*.c")

idf_component_register(
    SRCS
        "voice_hts_engine.cpp"
        "wakati.cpp"
        ${HTS_ENGINE_SRCS}
        ${NJD_SRCS}
        ${JPCOMMON_SRCS}
    INCLUDE_DIRS
        "include"
        "lib"
        "tmpl"
        "njd"
        "njd/upstream"
        "jpcommon"
        "jpcommon/upstream"
        "."
    REQUIRES
        joltwallet__littlefs
        esp_timer
    PRIV_REQUIRES
        esp_psram
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CHARSET_UTF_8
    HTS_STREAM0_MLPG_SKIP
    HTS_IDF_TIMING
)
target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -fno-math-errno -Wno-error -Wno-restrict -Wno-return-type -Wno-unused-variable)


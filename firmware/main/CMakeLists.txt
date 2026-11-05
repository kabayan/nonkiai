idf_component_register(
    SRCS
        "main.cpp"
        "display_orb.cpp"
        "board.cpp"
        "imu_sampler.cpp"
        "audio_cue.cpp"
        "ble_stream.cpp"
        "model_writer.cpp"
        "serial_say.cpp"
        "device_clock.cpp"
        "device_weather.cpp"
        "direction_ir.cpp"
        "gesture_shadow.cpp"
        "gesture_shadow_window.cpp"
        "gesture_svm.cpp"
        "madgwick_ahrs.cpp"
        "../third_party/Fusion/FusionAhrs.c"
        "motion_detector.cpp"
        "motion_interaction.cpp"
        "voice_player.cpp"
        "voice_listen.cpp"
        "voice_config.cpp"
        "ima_adpcm_decoder.c"
    INCLUDE_DIRS "." "../third_party/Fusion"
    REQUIRES
        bt
        esp_timer
        esp_driver_rmt
        nvs_flash
        m5stack__m5unified
        joltwallet__littlefs
        json
        sanotts_embed
        esp_partition
        esp_driver_usb_serial_jtag
        mbedtls
        stt32
)

# Match FusionMath inline normalization in the public wrapper header.
target_compile_definitions(${COMPONENT_LIB} PUBLIC FUSION_USE_NORMAL_SQRT)

option(NONKI_LISTEN_DIAGNOSTICS "Enable developer-only listen/PCM diagnostic CTRL" OFF)
if(NONKI_LISTEN_DIAGNOSTICS)
    set_source_files_properties("ble_stream.cpp" PROPERTIES COMPILE_DEFINITIONS NONKI_LISTEN_DIAGNOSTICS=1)
endif()

set(NONKI_LITTLEFS_STAGE "${CMAKE_BINARY_DIR}/littlefs_stage")
# DLS-194: Keep an empty LittleFS image and the existing partition layout; fillers are synthesized.
add_custom_target(nonki_littlefs_stage
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${NONKI_LITTLEFS_STAGE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${NONKI_LITTLEFS_STAGE}"
    VERBATIM)
littlefs_create_partition_image(littlefs "${NONKI_LITTLEFS_STAGE}"
                                FLASH_IN_PROJECT DEPENDS nonki_littlefs_stage)

idf_component_register(
    SRCS
        "word23_recognizer.cpp"
        "upstream/word23_accel_feature_f2.cpp"
        "upstream/word23_accel_m2t_model.cpp"
        "upstream/word23_dot_s8_s3.S"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "upstream" "generated"
    REQUIRES esp_timer
)

# Preserve the frozen feature and exact-dequant model arithmetic on host/device.
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-fast-math -ffp-contract=off)

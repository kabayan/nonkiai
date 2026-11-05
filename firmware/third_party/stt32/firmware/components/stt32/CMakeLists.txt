# stt32 recognition service (DLS-179 contract, DLS-239, DLS-242/243). Owns no
# audio hardware: the host passes PCM through read_pcm. The backend
# (stt32_wakehubert) only runs inference on a 3 s window. On the linux target it
# is left out so tests/stt32_service_host can drive the lifecycle with a fake one.
if(IDF_TARGET STREQUAL "linux")
    idf_component_register(
        SRCS "stt32_service.c" "stt32_policy.c"
        INCLUDE_DIRS "include"
        PRIV_INCLUDE_DIRS "."
    )
    target_link_libraries(${COMPONENT_LIB} PRIVATE m) # pow() of the end detection margin
else()
    idf_component_register(
        SRCS "stt32_service.c" "stt32_policy.c"
        INCLUDE_DIRS "include"
        PRIV_INCLUDE_DIRS "."
        PRIV_REQUIRES stt32_wakehubert esp_timer heap
    )
endif()

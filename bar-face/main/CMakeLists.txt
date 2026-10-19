idf_component_register(
    SRCS
        "bar-face.cpp"
        "session_manager_service.cpp"
        "opus_codec.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        embed embed_core cogitor_iot
        audio_bus voice_detection display
        udp_audio esp-opus
        esp_timer log freertos
        esp_app_format esp_system
    PRIV_REQUIRES
        esp_ringbuf cjson
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-exceptions)
# Ensure libhal.a is linked after libmbedcrypto.a to resolve sha_hal_* symbols
# that mbedtls hardware SHA acceleration requires.
target_link_libraries(${COMPONENT_LIB} INTERFACE idf::hal)

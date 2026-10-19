idf_component_register(
    SRCS
        "src/voice_detection_service.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        embed
        embed_core
        audio_bus
        log
        freertos
    PRIV_REQUIRES
        esp-sr
        esp_psram
        esp_driver_gpio
        esp_driver_uart
        esp_driver_spi
        esp_driver_i2c
        esp_timer
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-exceptions)


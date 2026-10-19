idf_component_register(
    SRCS
        "src/audio_bus_service.cpp"
        "src/es8311_codec.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        embed
        embed_core
    PRIV_REQUIRES
        esp_driver_i2s
        esp_driver_i2c
        esp_driver_gpio
        log
        freertos
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-exceptions)

idf_component_register(
    SRCS
        "src/udp_audio_transport.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        log
        freertos
        esp_netif
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-exceptions)

idf_component_register(
    SRCS src/pe_platform.c src/pe_store.c
    INCLUDE_DIRS include
    REQUIRES lock_core nvs_flash
    PRIV_REQUIRES ln_store esp_timer esp_hw_support esp_security mbedtls log
)

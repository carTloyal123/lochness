idf_component_register(
    SRCS src/ln_store.c
    INCLUDE_DIRS include
    REQUIRES esp_common
    PRIV_REQUIRES nvs_flash bootloader_support esp_hw_support log
)

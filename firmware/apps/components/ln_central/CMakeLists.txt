idf_component_register(SRCS src/ln_central.c
                       INCLUDE_DIRS include
                       REQUIRES esp_common
                       PRIV_REQUIRES bt link_proto freertos log)

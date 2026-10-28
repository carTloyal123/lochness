idf_component_register(
    SRCS main.c keypad_task.c ble_central.c input.c feedback.c console.c
    INCLUDE_DIRS .
    PRIV_REQUIRES keypad_core link_proto secure_link ln_store bt console
                  esp_driver_gpio esp_driver_ledc esp_timer freertos log
)
